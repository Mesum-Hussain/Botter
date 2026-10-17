-- How Botter builds an agent (progress.md §12.3-12.9). Code owns the order, the context and the verdicts;
-- the model does one narrow task at a time, each in a fresh worker that sees only what that task needs.
--
--   understand -> job.md -> design -> CHECKPOINT -> tests, then code, per function -> examples score
--   -> AGENT.md + config.json -> build -> report (with the measured score)
--
-- Every input first goes to the lead (AGENT.md): it answers questions itself and hands a build to
-- start_build. State lives in .botter/ (state.json, job.md, design.md, log.md), so the owner can read it.

BOTTER = BOTTER or {}

local DIR = ".botter"
local TRIES = 3            -- attempts per task before the stuck policy
local SCORE_ROUNDS = 5     -- fix rounds on the examples score
local READ = { "fs_list", "fs_read", "shell_exec", "note", "recall" }
local CODE = { "fs_list", "fs_read", "shell_exec", "fs_write", "fs_edit", "note", "recall" }

local history = {} -- the last exchanges with the person, for the lead

local function list(...)
  local out = {}
  for _, l in ipairs({ ... }) do
    for _, v in ipairs(l) do table.insert(out, v) end
  end
  return out
end

local function log(text)
  bot.fs.append(DIR .. "/log.md", "- " .. text .. "\n")
end

local function save()
  bot.fs.write(DIR .. "/job.md", job_markdown())
  bot.fs.write(DIR .. "/design.md", design_markdown())
  bot.fs.write(DIR .. "/state.json", bot.json.encode({ request = BOTTER.request, job = BOTTER.job,
    design = BOTTER.design, tasks = BOTTER.tasks, limits = BOTTER.limits }))
end

local function usage()
  local ok, u = pcall(bot.usage)
  return ok and u or { calls = 0, input_tokens = 0, output_tokens = 0 }
end

-- Run one phase and log what it cost the model (calls, tokens).
local function phase(name, fn, ...)
  local before = usage()
  local res = table.pack(pcall(fn, ...))
  local after = usage()
  log(string.format("cost of %s: %d calls, %d input tokens, %d output tokens", name, after.calls - before.calls,
    after.input_tokens - before.input_tokens, after.output_tokens - before.output_tokens))
  if not res[1] then error(res[2], 0) end
  return table.unpack(res, 2, res.n)
end

local function first_lines(text, n)
  local out, count = {}, 0
  for line in (tostring(text or "") .. "\n"):gmatch("([^\n]*)\n") do
    count = count + 1
    if count > n then
      table.insert(out, "...")
      break
    end
    table.insert(out, line)
  end
  return table.concat(out, "\n")
end

-- Run a worker until check(reply) returns nil, at most `tries` times; each retry is told what failed.
-- Returns true, reply or false, why.
local function attempt(name, spec, check, tries)
  local why
  for i = 1, tries or TRIES do
    local s = {}
    for k, v in pairs(spec) do s[k] = v end
    if why then
      s.task = spec.task .. "\n\nYour last attempt did not pass the check:\n" .. first_lines(why, 40)
        .. "\nFix exactly that."
    end
    local ok, reply = pcall(bot.worker, s)
    if not ok and tostring(reply):find("HTTP 429", 1, true) then -- retried for a minute already: the quota is spent
      log(name .. ": the model refused (HTTP 429)")
      error("the model refused more requests (HTTP 429: quota or rate limit). Everything done so far is saved: "
        .. "say \"continue the build\" when the quota is back.", 0)
    end
    if not ok then
      why = tostring(reply)
    else
      why = check(reply)
      if not why then
        log(name .. ": done" .. (i > 1 and (" (attempt " .. i .. ")") or ""))
        return true, reply
      end
    end
    log(name .. ": attempt " .. i .. " failed: " .. first_lines(why, 3):gsub("\n", " "))
  end
  return false, why
end

local function job_context()
  return bot.fs.read(DIR .. "/job.md")
end

local function full_context()
  return job_context() .. "\n" .. bot.fs.read(DIR .. "/design.md")
end

-- ---- 1. understand: the brief and the data -> the job ----------------------------------------------

local ANALYST = [[You are Botter's analyst. You read a request, the brief and the data in the project folder, and you record the job precisely with the job_ tools, so that others can build the agent from your record alone.
- fs_list the folder (and its sub-folders). Read every brief in full (README, problem statement, notes). Read every data file with fs_read: big CSV files come back as a profile (rows, columns, types, values).
- Record: job_goal once; job_input for every input file with its key columns; job_output for every output with its exact column names in order and their formats; job_rule for EVERY rule of the brief, once each, with its numbers and its exact wording (formulas, limits, priorities, tie-breaks, rounding, date handling, allowed values) and its kind (exact, judgement or hybrid), its source naming the file and the section heading; job_edge for each special case; job_examples for any file or text of worked examples or expected answers; job_success for how the result is judged.
- Go through the brief section by section: every section either gives rules (record them) or holds none for the agent (job_skip_section, with why). A rule you leave out will be missing from the agent.
- Do not write any file. When everything is recorded, call job_check and fill what it reports, then reply with one line: how many rules you recorded.]]

local function understand(request)
  bot.say("Reading the brief and the data")
  local ok, why = attempt("understand", {
    system = ANALYST,
    task = "The request: " .. request .. "\n\nStudy the folder and record the job.",
    tools = list(READ, { "job_goal", "job_input", "job_output", "job_rule", "job_examples", "job_success", "job_edge", "job_skip_section", "job_check" }),
    writes = {},
  }, function() return job_problems() end)
  save()
  if not ok then
    error("I could not work out the job from the brief: " .. why)
  end
end

-- ---- 2. design: the job -> functions and model steps ------------------------------------------------

local DESIGNER = [[You are Botter's designer. From the job below you plan the agent: which work is EXACT (rules, arithmetic, dates, joins, filters, formats, reading and writing files) and becomes a Lua function, and which needs JUDGEMENT (reading free text or images, choosing words, explaining) and stays with the agent's model.
- design_kind first. Then design_function for each function, smallest useful units: one that loads and joins the data for one case, one per calculation or decision of the brief, one that writes the output file in the exact columns. Name the rules (R1, R2 ...) each one implements: every rule must be implemented somewhere. Name the output file in the purpose of the function that writes it.
- design_model_step only for what code cannot do exactly (reading free text or images, wording). A model step works from the facts a function returns. Rules of kind exact or hybrid must be implemented by a function: forecasts, safety checks, amounts, dates, rankings and formats are code.
- design_skill for a capability the model loads on demand in a conversation: a procedure (SKILL.md) with the references, scripts and templates it needs, each file planned with an outline. Exact rules stay in functions.
- One function usually implements several rules: list them all in its rules (or add them later with design_cover). Before you reply, call design_check and fix everything it reports.
- At most 8 functions, each implementing at most 7 rules. Plain parameters (ids, dates, numbers), not whole tables, so each can be tested alone.
- Every column of every output must come from somewhere: name it in the returns of the function that computes it (e.g. "table {amount_safe_to_pay, affordability_status}"), or in the purpose of the model step that writes it.
- When the worked examples are input rows with their expected answers in a file of their own (e.g. sample_requests.csv beside requests.csv), the examples' cases are NOT in the input file: functions must take the case's own fields (user id, date, amount ...), not look the case up by its id in the input file, so the examples run through the same code. Only tests may read the examples file.
- For a batch agent over records (CSV in, CSV out): one input line is one case; a function gathers that case's facts from the data by its fields; the model reads what needs reading (messages, images); functions decide by the rules and append the result row; a last function writes the final output file in the required order and columns.
- Write no file. When the plan is complete, reply with one line.]]

local function design(correction)
  bot.say("Designing the agent")
  local task = "Plan the agent for this job."
  if correction then
    task = task .. "\n\nThe owner looked at the plan and said: " .. correction
      .. "\nChange the plan accordingly (planning a function again replaces it); record missing rules with job_rule."
  end
  local ok, why = attempt("design", {
    system = DESIGNER,
    context = job_context(),
    task = task,
    tools = list({ "fs_list", "fs_read", "note" },
      { "design_kind", "design_function", "design_model_step", "design_skill", "design_cover", "design_drop",
        "design_check", "job_rule" }),
    writes = {},
  }, function()
    local p = design_problems()
    return p and (p .. "\n\nThe plan so far (it is kept: add to it, design_cover to give a unit more rules, "
      .. "design_drop to remove one):\n" .. design_markdown()) or nil
  end)
  save()
  if not ok then
    error("I could not design the agent: " .. first_lines(why, 3))
  end
end

-- ---- 3. checkpoint: the owner confirms or corrects, once ------------------------------------------

local function checkpoint()
  local d = BOTTER.design
  local summary = { "Here is what I will build (details in .botter/job.md and .botter/design.md):",
    "- " .. #BOTTER.job.rules .. " rules from the brief; a " .. tostring(d.kind) .. " agent." }
  for _, f in ipairs(d.functions) do table.insert(summary, "- function " .. f.name .. ": " .. f.purpose) end
  for _, s in ipairs(d.model_steps) do table.insert(summary, "- the model: " .. s.purpose) end
  table.insert(summary, "Reply ok to build it, or say what to change.")
  local answer = bot.ask(table.concat(summary, "\n"))
  if answer and not answer:lower():match("^%s*o?k?a?y?%s*$") and not answer:lower():match("^%s*y") then
    log("checkpoint: the owner said: " .. answer)
    design(answer)
  end
end

-- ---- 4. tasks: tests first (another worker), then the code, per function ---------------------------

local TESTER = [[You are Botter's tester. You write the tests for ONE function before it exists, from the job's rules, so the code can be checked against the brief and not against itself.
- Use only real cases: ids, dates and rows that exist in the data files (look them up with fs_read, or shell_exec with grep). Never invent ids such as user1 or req1: the check rejects ids no data file holds.
- When the job has worked examples with expected answers, test against them first (their cases may exist only in the examples file: pass the example row's fields to the function): where this function's result can be read from an example's expected fields, take the expected value from there (the test may read the example file). Otherwise work out the expected values BY HAND from the rules, and show the working in a comment above each assert.
- Write global test_ functions with assert(value == expected, "what was expected and why"); compare numbers with a tolerance when the brief rounds. Test the normal case, each rule the function implements, and the edge cases.
- Write only the test file. Do not write the function.]]

local CODER = [[You are Botter's coder. You write ONE Lua function of the agent so that its tests pass, implementing the brief's rules exactly.
- Read the test file first, then the data it uses. Write the function in the file you are given, with its --- doc comment (what it does and when the model calls it, ---@param for every parameter, ---@return).
- Implement every rule named in the design for this function, the general rule, never the test's numbers: the function must work for cases nobody has seen. No stubs, no placeholder values, no copying expected answers.
- After writing, the build runs the tests; you see their result in the write's Lua check and in the next task if they fail.]]

local function snapshot(paths)
  local snap = {}
  for _, p in ipairs(paths) do snap[p] = bot.fs.exists(p) and bot.fs.read(p) or false end
  return snap
end

local function put_back(snap)
  for p, content in pairs(snap) do
    if content or bot.fs.exists(p) then bot.fs.write(p, content or "") end
  end
end

local function test_file(f) return "functions/tests/" .. f.name .. ".lua" end
local function code_file(f) return "functions/" .. f.name .. ".lua" end

-- Errors loading the project or the tests, else nil.
local function load_errors(report)
  local lines = {}
  for line in report:gmatch("[^\n]+") do
    if line:match(":%d+: error:") or line:match(": cannot read:") then table.insert(lines, line) end
  end
  return #lines > 0 and table.concat(lines, "\n") or nil
end

local function data_paths()
  local paths = {}
  for _, i in ipairs(BOTTER.job.inputs) do
    if i.path ~= "" then table.insert(paths, i.path) end
  end
  for _, e in ipairs(BOTTER.job.examples) do
    if e.path ~= "" then table.insert(paths, e.path) end
  end
  return paths
end

-- Quoted ids in a test file that no data file holds: a test built on made-up ids (user1, req1) proves
-- nothing, and the code bends to fit it. An id here = letters and digits, like user_03 or event_253.
function botter_invented_ids(path)
  local paths = data_paths()
  if #paths == 0 or not bot.fs.exists(path) then return {} end
  local ids, seen = {}, {}
  for lit in bot.fs.read(path):gmatch("[\"']([%w_%-]+)[\"']") do
    if not seen[lit] and #lit >= 3 and #lit <= 40 and lit:find("%a") and lit:find("%d")
      and not lit:match("^%d%d%d%d%-%d") and not lit:match("^R%d+$") and not lit:match("^test_") then
      seen[lit] = true
      table.insert(ids, lit)
    end
    if #ids >= 40 then break end
  end
  if #ids == 0 then return {} end
  local cmd = { "grep -rIhowF" }
  for _, id in ipairs(ids) do table.insert(cmd, "-e '" .. id .. "'") end
  table.insert(cmd, "--")
  for _, p in ipairs(paths) do table.insert(cmd, "'" .. p:gsub("'", "") .. "'") end
  local ok, out, code = pcall(bot.shell, table.concat(cmd, " "))
  if not ok or (code ~= 0 and code ~= 1) then return {} end -- grep itself failed: no verdict
  local found, missing = {}, {}
  for line in tostring(out):gmatch("[^\n]+") do found[line] = true end
  for _, id in ipairs(ids) do
    if not found[id] then table.insert(missing, id) end
  end
  return missing
end

local function check_tests(f)
  return function()
    local path = test_file(f)
    if not bot.fs.exists(path) then return path .. " was not written" end
    if not bot.fs.read(path):find("function test_") then return path .. " has no test_ function" end
    local invented = botter_invented_ids(path)
    if #invented > 0 then
      return path .. " uses ids that no data file holds: " .. table.concat(invented, ", ")
        .. ". Use real ids from " .. table.concat(data_paths(), ", ") .. " (look them up first)."
    end
    local r = bot.test(path)
    for line in r.report:gmatch("[^\n]+") do
      if line:find(path, 1, true) and line:match(":%d+: error:") then return line end
    end
    return nil
  end
end

local function check_code(f)
  return function()
    local path = code_file(f)
    if not bot.fs.exists(path) then return path .. " was not written" end
    local src = bot.fs.read(path)
    if not src:find("function " .. f.name .. "%(") then return path .. " does not define " .. f.name .. "(...)" end
    local unfinished = {}
    for _, line in ipairs(botter_unfinished_code()) do
      if line:find(path, 1, true) then table.insert(unfinished, line) end
    end
    if #unfinished > 0 then
      return "the code says it is not finished:\n" .. table.concat(unfinished, "\n") .. "\nWrite the real logic."
    end
    local r = bot.test(test_file(f))
    local errors = load_errors(r.report)
    if errors then return errors end
    if r.failed > 0 then return r.report end
    return nil
  end
end

local REVIEWER = [[You are Botter's reviewer. A function keeps failing its tests. You decide whether the TESTS are wrong: check each failing expectation against the job's rules and the data, by hand.
- If an expected value is wrong, fix that assert in the test file and say in a comment why.
- If the test is right, change nothing.
- Reply with one line per failing test: "test wrong: <why>" or "test right".]]

local function build_function(f)
  local spec = "The function: " .. design_function_line(f)
  local rules = {}
  for _, id in ipairs(f.rules) do
    for _, r in ipairs(BOTTER.job.rules) do
      if r.id == id then table.insert(rules, r.id .. ". " .. r.text) end
    end
  end
  if #rules > 0 then spec = spec .. "\nIts rules:\n" .. table.concat(rules, "\n") end
  local examples = {}
  for _, e in ipairs(BOTTER.job.examples) do
    if e.path ~= "" then table.insert(examples, e.path) end
  end
  bot.say("Writing tests for " .. f.name)
  local ok, why = attempt("tests " .. f.name, {
    system = TESTER, context = full_context(), skills = { "write-functions" }, tools = CODE,
    writes = { test_file(f) },
    task = spec .. "\n\nWrite " .. test_file(f) .. "."
      .. (#examples > 0 and ("\nWorked examples with expected answers: " .. table.concat(examples, ", ") .. ".") or ""),
  }, check_tests(f))
  if not ok then
    table.insert(BOTTER.limits, f.name .. ": no usable tests (" .. first_lines(why, 1) .. ")")
  end
  bot.say("Writing " .. f.name)
  local coder = {
    system = CODER, context = full_context(), skills = { "write-functions" }, tools = CODE,
    writes = { code_file(f) },
    task = spec .. "\n\nWrite " .. code_file(f) .. " so that " .. test_file(f) .. " passes.",
  }
  ok, why = attempt(f.name, coder, check_code(f))
  if not ok and bot.fs.exists(test_file(f)) then -- stuck: maybe the test is wrong (the planner wrote it)
    bot.say("Checking the tests of " .. f.name)
    local snap = snapshot({ test_file(f) })
    if not attempt("review " .. f.name, {
      system = REVIEWER, context = full_context(), skills = { "write-functions" }, tools = CODE, writes = { test_file(f) },
      task = spec .. "\n\nThe tests in " .. test_file(f) .. " fail:\n" .. first_lines(why, 40),
    }, check_tests(f), 1) then
      put_back(snap) -- a review that leaves the tests unloadable or on invented ids is worse than none
      log("review " .. f.name .. ": undone")
    end
    ok, why = attempt(f.name .. " (after review)", coder, check_code(f), 2)
  end
  BOTTER.tasks[f.name] = ok and "done" or "failed"
  if not ok then
    table.insert(BOTTER.limits, f.name .. ": its tests still fail: " .. first_lines(why, 3))
  end
  save()
end

-- Functions in the order they can be written: what a function uses comes first.
local function ordered_functions()
  local byname, done, out = {}, {}, {}
  for _, f in ipairs(BOTTER.design.functions) do byname[f.name] = f end
  local function visit(f, depth)
    if done[f.name] or depth > 10 then return end
    done[f.name] = true
    for used in f.uses:gmatch("[%w_]+") do
      if byname[used] then visit(byname[used], depth + 1) end
    end
    table.insert(out, f)
  end
  for _, f in ipairs(BOTTER.design.functions) do visit(f, 0) end
  return out
end

-- ---- 4b. skills: SKILL.md, then each of its files, checked by the build's skill validator -----------

local SKILLWRITER = [[You are Botter's skill writer. You write one file of one skill of the agent.
- SKILL.md: a frontmatter (name: the folder's name; description: what it does and when to use it), then the procedure as short numbered steps, naming each file of the skill (references/..., scripts/..., assets/...) with when to read or run it, by its relative path.
- A reference: the knowledge the procedure needs, complete and concrete. A script: a real program with a #! line, arguments on the command line, result on stdout, exit 0 on success; never mock output.
- Write only the file you are given. No placeholder words: finish every part.]]

-- The build's lines about one skill (its validator), and whether any is an error.
local function skill_report(name)
  local report, lines, errors = agent_build(true), {}, false
  for line in report:gmatch("[^\n]+") do
    if line:find("skills/" .. name .. "/", 1, true) then
      table.insert(lines, line)
      errors = errors or line:find("error", 1, true) ~= nil
    end
  end
  return table.concat(lines, "\n"), errors
end

local function build_skill(s)
  local dir = "skills/" .. s.name .. "/"
  local outline = "The skill " .. s.name .. ": " .. s.description .. "\nIts files besides SKILL.md:"
  for _, f in ipairs(s.files) do outline = outline .. "\n- " .. f.path .. ": " .. f.outline end
  bot.say("Writing the skill " .. s.name)
  local files = { { path = "SKILL.md", outline = "the procedure" } }
  for _, f in ipairs(s.files) do table.insert(files, f) end
  for _, f in ipairs(files) do
    local path = dir .. f.path
    local ok, why = attempt(path, {
      system = SKILLWRITER, context = full_context(), skills = { "write-agent-md-and-skills", "write-scripts" },
      tools = CODE, writes = { path },
      task = outline .. "\n\nWrite " .. path .. " (" .. f.outline .. ").",
    }, function()
      if not bot.fs.exists(path) then return path .. " was not written" end
      local report, errors = skill_report(s.name)
      return errors and report or nil
    end)
    if not ok then table.insert(BOTTER.limits, path .. ": " .. first_lines(why, 2)) end
  end
  local report = skill_report(s.name)
  BOTTER.tasks["skill " .. s.name] = report == "" and "done" or "warnings"
  if report ~= "" then table.insert(BOTTER.limits, "skill " .. s.name .. ": " .. first_lines(report, 3)) end
  save()
end

-- ---- 5. examples: the honest score --------------------------------------------------------------

local SCORER = [[You are Botter's scorer. You write functions/tests/examples.lua with one function, score_examples(), that runs the agent's functions on EVERY worked example and compares each expected field.
- Read the example files and the functions' doc comments. For each example: call the functions the way the agent would, compare every expected field (numbers with a tolerance when the brief rounds), and count it as passed only when all fields match.
- Run each example inside pcall: an error in one counts that example as failed (with the error), the others still run.
- Return passed, total (the number of examples, each counted once), and a text listing each failing example: its id, each differing field, what came out and what was expected.
- Never copy expected answers into the agent's functions. Only this test file reads the example files.]]

local FIXER = [[You are Botter's fixer. The agent's functions get some worked examples wrong. You find the rule behind ONE difference and fix the function that implements it: the general rule from the brief, never the example's numbers.
- Read the job's rules again, the function, and the data of the failing example. Work the example out by hand, find where the function goes wrong, fix it.
- Do not change the tests or the examples. Keep every other test passing.]]

local function examples_check(expected, files)
  return function()
    local path = "functions/tests/examples.lua"
    if not bot.fs.exists(path) then return path .. " was not written" end
    local r = bot.test(path)
    local errors = load_errors(r.report)
    if errors then return errors end
    local s = r.scores[1]
    if not s then return "score_examples() did not return passed, total:\n" .. r.report end
    local src, reads = bot.fs.read(path), false
    for _, f in ipairs(files) do
      reads = reads or src:find(f:match("([^/]+)$"), 1, true) ~= nil
    end
    if #files > 0 and not reads then
      return path .. " never reads the examples (" .. table.concat(files, ", ")
        .. "): score the cases whose expected answers they hold"
    end
    if expected > 0 and s.total ~= expected then
      return "score_examples reports " .. s.passed .. "/" .. s.total .. ", but the examples hold " .. expected
        .. " cases: total is the number of examples (each counts once, passed when all its fields match)"
    end
    return nil
  end
end

local function score()
  local r = bot.test("functions/tests/examples.lua")
  local s = r.scores[1]
  return s and s.passed, s and s.total, s and s.details or r.report
end

-- The function tests that fail now (not the examples score): "file: test name", or "file (does not load)".
local function failing_tests()
  local set = {}
  local r = bot.test("functions/tests")
  for line in r.report:gmatch("[^\n]+") do
    local file, name = line:match("^(%S+%.lua): test (%S+) FAILED")
    local key = file and (file .. ": " .. name)
      or (line:match(":%d+: error:") and (line:match("^([^:]+)") .. " (does not load)"))
    if key and not key:find("tests/examples.lua", 1, true) then set[key] = true end
  end
  return set
end

local function newly_failing(before)
  local out = {}
  for key in pairs(failing_tests()) do
    if not before[key] then table.insert(out, key) end
  end
  table.sort(out)
  return out
end

local function remove_answer_key(files, writes)
  local leaks = botter_reads_answer_key(files)
  if #leaks == 0 then return end -- an agent that reads the expected answers scores 100% and solves nothing
  attempt("remove answer key", { system = FIXER, context = full_context(), tools = CODE, writes = writes,
    task = "The agent reads its answer key:\n" .. table.concat(leaks, "\n")
      .. "\nOnly functions/tests may read those files. Remove every use of them and compute the answers from the inputs." },
    function() return #botter_reads_answer_key(files) > 0 and "it still reads them" or nil end)
end

local function examples()
  local files = botter_example_files()
  if #BOTTER.job.examples == 0 and #files == 0 then
    return nil
  end
  local expected = #files > 0 and botter_count_examples(files) or 0
  bot.say("Measuring the agent against the brief's examples")
  local ok, why = attempt("examples score", {
    system = SCORER, context = full_context(), skills = { "write-functions" }, tools = CODE,
    writes = { "functions/tests/examples.lua" },
    task = "Write functions/tests/examples.lua. Example files: " .. (#files > 0 and table.concat(files, ", ") or "see the job")
      .. (expected > 0 and (" (" .. expected .. " examples)") or "") .. ".",
  }, examples_check(expected, files))
  if not ok then
    table.insert(BOTTER.limits, "no score against the examples: " .. first_lines(why, 2))
    return nil
  end
  local writes = {}
  for _, f in ipairs(BOTTER.design.functions) do table.insert(writes, code_file(f)) end
  remove_answer_key(files, writes)
  local passed, total, details = score()
  if not passed then return nil end
  log("score: " .. passed .. "/" .. total)
  BOTTER.score = { passed = passed, total = total }
  save()
  local stalled = 0
  for round = 1, SCORE_ROUNDS do
    if passed >= total or stalled >= 2 then break end
    bot.say("Examples: " .. passed .. "/" .. total .. "; fixing a difference")
    -- a fix is kept only when every function test that passed still passes and the score does not drop
    local before, snap = failing_tests(), snapshot(writes)
    local fixed = attempt("fix examples " .. round, {
      system = FIXER, context = full_context(), skills = { "write-functions" }, tools = CODE, writes = writes,
      task = "The examples score " .. passed .. "/" .. total .. ". Differences:\n" .. first_lines(details, 30)
        .. "\n\nPick the first difference, find its rule and fix it. Every function test that passes now must still pass.",
    }, function()
      local broken = newly_failing(before)
      return #broken > 0 and ("your change broke tests that passed before:\n" .. table.concat(broken, "\n")
        .. "\nFix the difference without breaking them.") or nil
    end, 2)
    if fixed then remove_answer_key(files, writes) end
    local p, t, d = score()
    if not fixed or not p or p < passed then
      put_back(snap)
      log("fix examples " .. round .. ": undone (" .. (not fixed and "it broke passing tests"
        or ("the score fell to " .. tostring(p) .. "/" .. tostring(t))) .. ")")
      stalled = stalled + 1
    else
      stalled = p > passed and 0 or stalled + 1
      passed, total, details = p, t, d
      log("score after fix " .. round .. ": " .. passed .. "/" .. total)
      BOTTER.score = { passed = passed, total = total }
      save()
    end
  end
  return BOTTER.score
end

-- ---- 6. AGENT.md, config.json, build ----------------------------------------------------------------

local WRITER = [[You are Botter's writer. You write the agent's AGENT.md: its system prompt, plain text, in the second person.
- Who it is and what it does (from the job's goal), its firm rules, and "How a session goes": a short numbered list naming the functions it calls, in order, and what the model itself does (the model steps).
- For a batch agent: one input line names one case (an id); say exactly how to process one case and what to reply.
- Short and firm: under 60 lines. No secrets.]]

local function documented_functions()
  local names = {}
  for _, f in ipairs(BOTTER.design.functions) do table.insert(names, f.name) end
  return names
end

local function write_agent_md()
  bot.say("Writing AGENT.md")
  local ok, why = attempt("AGENT.md", {
    system = WRITER, context = full_context(), skills = { "write-agent-md-and-skills" }, tools = CODE,
    writes = { "AGENT.md" },
    task = "Write AGENT.md for this agent. Its functions: " .. table.concat(documented_functions(), ", ") .. ".",
  }, function()
    if not bot.fs.exists("AGENT.md") then return "AGENT.md was not written" end
    local text, missing = bot.fs.read("AGENT.md"), {}
    for _, name in ipairs(documented_functions()) do
      if BOTTER.tasks[name] == "done" and not text:find(name, 1, true) then table.insert(missing, name) end
    end
    if #missing > 0 then return "AGENT.md never names these functions: " .. table.concat(missing, ", ") end
    if #text > 12000 then return "AGENT.md has " .. #text .. " characters: keep it under 8000" end
    return nil
  end)
  if not ok then table.insert(BOTTER.limits, "AGENT.md: " .. first_lines(why, 2)) end
end

local function folder_name()
  local out = bot.shell("pwd")
  return (out:match("([^/\n]+)%s*$") or "agent"):lower():gsub("[^a-z0-9_.-]", "-")
end

local function write_config()
  local config = {}
  if bot.fs.exists("config.json") then
    local ok, old = pcall(bot.json.decode, bot.fs.read("config.json"))
    if ok and type(old) == "table" then config = old end
  end
  config.name = config.name or folder_name()
  config.version = config.version or "0.1.0"
  config.description = config.description or BOTTER.job.goal:sub(1, 300)
  if config.internet == nil then config.internet = true end
  local kind = BOTTER.design.kind
  if (kind == "batch" or kind == "producer") and config.temperature == nil then config.temperature = 0 end
  if #BOTTER.design.skills > 0 then
    config.skills = {}
    for _, s in ipairs(BOTTER.design.skills) do table.insert(config.skills, s.name) end
  end
  bot.fs.write("config.json", bot.json.encode(config))
end

-- Tests that still fail would stop the build: they move to .botter/failing-tests/ and the report lists them,
-- so the rest of the agent ships with its limits stated (stuck-task policy step 3, §12.7).
local function set_aside_failing_tests()
  local ok, names = pcall(bot.fs.list, "functions/tests")
  if not ok then return end
  for _, name in ipairs(names) do
    local path = "functions/tests/" .. name
    if name:match("%.lua$") then
      local r = bot.test(path)
      if r.failed > 0 then
        bot.fs.write(DIR .. "/failing-tests/" .. name, bot.fs.read(path))
        bot.fs.write(path, "-- These tests failed; they are kept in " .. DIR .. "/failing-tests/" .. name
          .. " and listed in the build report.\n")
        table.insert(BOTTER.limits, path .. " failed (" .. r.failed .. " failing); moved to " .. DIR
          .. "/failing-tests/: " .. first_lines(load_errors(r.report) or r.report, 2):gsub("\n", " "))
        log("set aside " .. path .. ": it fails")
      end
    end
  end
end

-- What stopped a build: its error lines, without the score details that can run to hundreds of lines.
local function build_errors(report)
  local lines = {}
  for line in report:gmatch("[^\n]+") do
    if line:find("FAILED", 1, true) or line:find("error", 1, true) or line:find("nothing was built", 1, true) then
      table.insert(lines, line)
    end
  end
  return #lines > 0 and table.concat(lines, "\n") or report
end

local function build_agent()
  if not bot.fs.exists("AGENT.md") then
    error("AGENT.md was not written, so the agent would have no instructions; nothing was built. "
      .. "Say \"continue the build\" to try again.", 0)
  end
  set_aside_failing_tests()
  bot.say("Building the agent")
  local report = agent_build(false)
  if report:find("packer: wrote", 1, true) then return report end
  local writes = { "AGENT.md", "config.json" }
  for _, f in ipairs(BOTTER.design.functions) do table.insert(writes, code_file(f)) end
  local before, snap = failing_tests(), snapshot(writes)
  local ok, why = attempt("build", {
    system = FIXER, context = full_context(), skills = { "write-functions" }, tools = CODE, writes = writes,
    task = "The build failed:\n" .. first_lines(build_errors(report), 40)
      .. "\nFix what it reports. Every function test that passes now must still pass.",
  }, function()
    local broken = newly_failing(before)
    if #broken > 0 then
      return "your change broke tests that passed before:\n" .. table.concat(broken, "\n")
    end
    report = agent_build(false)
    return not report:find("packer: wrote", 1, true) and build_errors(report) or nil
  end, 2)
  if not ok then
    put_back(snap)
    log("build fixes undone")
    set_aside_failing_tests()
    report = agent_build(false)
    if not report:find("packer: wrote", 1, true) then
      error("the build still fails:\n" .. first_lines(build_errors(report), 20))
    end
  end
  return report
end

-- ---- the report: facts the code measured, not the model's opinion -------------------------------

local function final_report(build)
  local out = {}
  local file = build:match("packer: wrote (%S+)")
  local size = build:match("total (%d+) bytes")
  table.insert(out, "Built " .. tostring(file) .. (size and (" (" .. math.floor(tonumber(size) / 1024) .. " KB)") or "") .. ".")
  local done, total = 0, 0
  for _, f in ipairs(BOTTER.design.functions) do
    total = total + 1
    if BOTTER.tasks[f.name] == "done" then done = done + 1 end
  end
  table.insert(out, "Functions passing their tests: " .. done .. "/" .. total .. ".")
  if #BOTTER.design.skills > 0 then
    local names = {}
    for _, s in ipairs(BOTTER.design.skills) do table.insert(names, s.name .. " (" .. tostring(BOTTER.tasks["skill " .. s.name]) .. ")") end
    table.insert(out, "Skills: " .. table.concat(names, ", ") .. ".")
  end
  if BOTTER.score then
    table.insert(out, "Score on the brief's examples: " .. BOTTER.score.passed .. "/" .. BOTTER.score.total .. ".")
  end
  if BOTTER.design.kind == "batch" then
    table.insert(out, "Run it: ./" .. tostring(file):match("([^/]+)$") .. " --run < ids.txt (one case per line).")
  end
  if #BOTTER.limits > 0 then
    table.insert(out, "Known limitations:")
    for _, l in ipairs(BOTTER.limits) do table.insert(out, "- " .. l) end
  end
  local u = usage()
  table.insert(out, string.format("Model use for this build: %d calls, %d input tokens, %d output tokens (per phase in .botter/log.md).",
    u.calls - (BOTTER.usage0 and BOTTER.usage0.calls or 0), u.input_tokens - (BOTTER.usage0 and BOTTER.usage0.input_tokens or 0),
    u.output_tokens - (BOTTER.usage0 and BOTTER.usage0.output_tokens or 0)))
  table.insert(out, "The plan and a log of every step are in .botter/.")
  return table.concat(out, "\n")
end

-- The build recorded in .botter/state.json, to continue it: true when there was one.
local function restore()
  local ok, st = pcall(function() return bot.json.decode(bot.fs.read(DIR .. "/state.json")) end)
  if not ok or type(st) ~= "table" or type(st.job) ~= "table" then
    return false
  end
  botter_reset_job()
  for k, v in pairs(st.job) do BOTTER.job[k] = v end
  for k, v in pairs(st.design or {}) do BOTTER.design[k] = v end
  BOTTER.request, BOTTER.tasks, BOTTER.limits = st.request, st.tasks or {}, {}
  return true
end

local function build(request, resume)
  BOTTER.usage0 = usage()
  if resume and restore() then
    log("resumed: " .. request)
    bot.say("Continuing the build recorded in .botter/")
  else
    BOTTER.request, BOTTER.tasks, BOTTER.limits, BOTTER.score = request, {}, {}, nil
    botter_reset_job()
    bot.fs.write(DIR .. "/log.md", "# Build log\n\n- request: " .. request .. "\n")
  end
  if job_problems() then phase("understand", understand, BOTTER.request) end
  if design_problems() then
    phase("design", design)
    checkpoint()
  end
  write_config() -- early, so the build's checks see the planned skills
  for _, f in ipairs(ordered_functions()) do
    if BOTTER.tasks[f.name] ~= "done" then phase(f.name, build_function, f) end
  end
  for _, s in ipairs(BOTTER.design.skills) do
    if BOTTER.tasks["skill " .. s.name] ~= "done" then phase("skill " .. s.name, build_skill, s) end
  end
  phase("examples", examples)
  phase("AGENT.md", write_agent_md)
  write_config()
  local report = phase("build", build_agent)
  save()
  return final_report(report)
end

-- ---- every input: the lead answers, or hands a build to the pipeline ------------------------------

function flow(input)
  BOTTER.start, BOTTER.resume = nil, nil
  local context = #history > 0 and ("Earlier in this conversation:\n" .. table.concat(history, "\n")) or nil
  local ok, reply = pcall(bot.worker, {
    task = input, context = context,
    tools = list(READ, { "start_build", "agent_build", "agent_inspect", "vault_seal", "skill" }),
  })
  if not ok then
    if tostring(reply):find("HTTP 429", 1, true) then
      return "The model refused more requests (HTTP 429: quota or rate limit). Try again when the quota is back."
    end
    error(reply, 0)
  end
  if BOTTER.start then
    if reply ~= "" then bot.say(reply) end
    -- the person's own words are the request (a paraphrase can add ideas they never had)
    local request = input
    if #history > 0 then request = table.concat(history, "\n") .. "\nUser: " .. input end
    local ok, result = pcall(build, request, BOTTER.resume)
    reply = ok and result or ("The build stopped: " .. tostring(result) .. "\nWhat was done so far is in .botter/.")
  end
  table.insert(history, "User: " .. first_lines(input, 5))
  table.insert(history, "Botter: " .. first_lines(reply, 5))
  while #history > 12 do table.remove(history, 1) end
  return reply
end
