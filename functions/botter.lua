-- Botter keeps working until the job is really done. After every turn botcore calls
-- on_turn_end with the reply; a text it returns goes back to the model.

local UNFINISHED = { "todo", "fixme", "placeholder", "simplified", "for demo", "demo only", "not implemented",
  "stub", "mock", "dummy", "hard-coded", "hardcoded", "for this test", "in a real", "implement later",
  "to be implemented", "under construction" }

-- The unticked "- [ ]" lines of PLAN.md in the project folder.
local function open_items()
  if not bot.fs.exists("PLAN.md") then
    return {}
  end
  local open = {}
  for line in bot.fs.read("PLAN.md"):gmatch("[^\n]+") do
    if line:match("^%s*[-*] %[ %]") then
      table.insert(open, line)
    end
  end
  return open
end

-- Comment lines in functions/*.lua that say the code is not done ("simplified", "placeholder" ...).
local function unfinished_code()
  local found = {}
  local files = {}
  for _, dir in ipairs({ "functions", "functions/tests" }) do
    if bot.fs.exists(dir) then
      for _, name in ipairs(bot.fs.list(dir)) do
        if name:match("%.lua$") then
          table.insert(files, (dir == "functions" and "" or "tests/") .. name)
        end
      end
    end
  end
  for _, name in ipairs(files) do
    do
      local number = 0
      for line in (bot.fs.read("functions/" .. name) .. "\n"):gmatch("([^\n]*)\n") do
        number = number + 1
        local comment = line:lower():match("%-%-(.*)")
        for _, word in ipairs(UNFINISHED) do
          if comment and comment:find(word, 1, true) then
            table.insert(found, "functions/" .. name .. ":" .. number .. ": " .. line:match("^%s*(.-)%s*$"))
            break
          end
        end
      end
    end
  end
  return found
end

-- Files whose names say they hold worked examples or expected answers (in the folder, two levels down).
local function example_files()
  local found = {}
  local function look(dir, depth)
    for _, name in ipairs(bot.fs.list(dir)) do
      local path = dir == "." and name or (dir .. "/" .. name)
      if name:sub(-1) == "/" then
        if depth < 2 and name ~= "functions/" and name ~= "media/" then
          look(path:sub(1, -2), depth + 1)
        end
      elseif name:lower():find("sample") or name:lower():find("example") or name:lower():find("expected")
          or name:lower():find("golden") or name:lower():find("ground_truth") then
        table.insert(found, path)
      end
    end
  end
  look(".", 0)
  return found
end

-- How many examples the files hold: data rows of a CSV, items of a JSON list, else one per file.
local function count_examples(files)
  local count = 0
  for _, path in ipairs(files) do
    local text = bot.fs.read(path)
    if path:match("%.csv$") then
      local ok, rows = pcall(bot.csv.parse, text)
      count = count + (ok and #rows or 0)
    elseif path:match("%.json$") then
      local ok, value = pcall(bot.json.decode, text)
      count = count + ((ok and type(value) == "table" and #value > 0) and #value or 1)
    else
      count = count + 1
    end
  end
  return count
end

-- Agent code (not its tests) that names an example file: the agent would be reading its answer key.
local function reads_answer_key(files)
  local leaks = {}
  if not bot.fs.exists("functions") then
    return leaks
  end
  for _, name in ipairs(bot.fs.list("functions")) do
    if name:match("%.lua$") then
      local code = bot.fs.read("functions/" .. name)
      for _, path in ipairs(files) do
        local base = path:match("([^/]+)$")
        if code:find(base, 1, true) then
          table.insert(leaks, "functions/" .. name .. " reads " .. path)
        end
      end
    end
  end
  return leaks
end

-- How the agent does on the worked examples, across the turns of this session.
local scoring = { best = nil, rounds = 0, stalled = 0 }
local MAX_SCORE_ROUNDS = 6

-- The score the build reports for the examples: passed, total, and the report.
local function examples_score()
  local ok, report = pcall(bot.tool, "agent_build", { dry_run = true })
  if not ok then
    return nil, nil, tostring(report)
  end
  local passed, total = report:match("packer: score [%w_]+: ([%d.]+)/([%d.]+)")
  return tonumber(passed), tonumber(total), report
end

function on_turn_end(reply)
  if reply:match("%?%s*$") then
    return nil -- the reply asks the user something: let them answer
  end
  local open, unfinished = open_items(), unfinished_code()
  if #open > 0 then
    return "PLAN.md still has " .. #open .. " open item(s):\n" .. table.concat(open, "\n") .. "\n"
      .. "Continue with the next one now. Tick each item (- [x]) in PLAN.md as soon as it is really done. "
      .. "If one truly cannot be done, tick it, write why after it, and say so in your report."
  end
  if #unfinished > 0 then
    return "The plan is ticked, but this code says it is not finished:\n" .. table.concat(unfinished, "\n") .. "\n"
      .. "Write the real logic there (every rule of the brief, exact and tested), remove the note, "
      .. "check with agent_build (dry_run) and build again."
  end

  -- worked examples are the one judge the model cannot talk round: measure, then improve
  local examples = example_files()
  if #examples == 0 or scoring.rounds >= MAX_SCORE_ROUNDS or scoring.stalled >= 2 then
    return nil
  end
  local leaks = reads_answer_key(examples)
  if #leaks > 0 then -- an agent that looks up the expected answers scores 100% and solves nothing
    return "The agent reads its answer key:\n" .. table.concat(leaks, "\n") .. "\n"
      .. "Those files hold the expected answers; only functions/tests may read them. The agent must work the "
      .. "answers out from the inputs, by the rules of the brief, for requests it has never seen. Remove every "
      .. "use of these files from functions/*.lua and write the real logic, then score again."
  end
  scoring.rounds = scoring.rounds + 1
  local passed, total, report = examples_score()
  if not passed then
    return "The brief has worked examples (" .. table.concat(examples, ", ") .. "), and nothing measures the agent "
      .. "against them yet. In functions/tests/examples.lua write score_examples(): run your functions on every "
      .. "example, compare each field with the expected answer, and return passed, total and a text listing what "
      .. "differs (what came out, what was expected). Never copy the expected answers into the agent. Then run "
      .. "agent_build with dry_run and improve the functions where they differ."
  end
  local expected = count_examples(examples)
  if total < expected then -- a score is only worth something when it covers every example
    return "score_examples reports " .. passed .. "/" .. total .. ", but the examples hold " .. expected
      .. " cases (" .. table.concat(examples, ", ") .. "). Score every one of them: run your functions on each "
      .. "example and compare every field with its expected answer. Then improve the functions where they differ."
  end
  if passed >= total then
    return nil
  end
  if scoring.best and passed <= scoring.best then
    scoring.stalled = scoring.stalled + 1
  else
    scoring.stalled = 0
  end
  scoring.best = math.max(scoring.best or 0, passed)
  if scoring.stalled >= 2 then
    return nil -- two rounds without progress: report where it stands
  end
  return "The examples score " .. passed .. "/" .. total .. " (best so far " .. scoring.best .. "). From the build:\n"
    .. report:sub(1, 6000) .. "\n"
    .. "Find the rule behind these differences and fix the functions (never the expected answers, never by copying "
    .. "them), check with agent_build (dry_run), then build again. Report the final score."
end
