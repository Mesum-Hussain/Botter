-- The job and the design, recorded by Botter's workers through these tools and checked on the spot
-- (progress.md §12.9). FLOW.lua reads BOTTER.job and BOTTER.design; .botter/ holds the readable views.

BOTTER = BOTTER or {}

local KINDS = { chat = true, batch = true, service = true, scheduled = true, producer = true }
local BUILTIN = { fs_list = true, fs_read = true, fs_write = true, fs_edit = true, shell_exec = true,
  image_view = true, note = true, recall = true, skill = true, cron_set = true, cron_list = true,
  cron_delete = true, get_time = true, delegate = true, vault_seal = true }

function botter_reset_job()
  BOTTER.job = { goal = "", inputs = {}, outputs = {}, rules = {}, examples = {}, success = {}, edges = {}, skipped = {} }
  BOTTER.design = { kind = nil, why = "", functions = {}, model_steps = {}, skills = {} }
end

if not BOTTER.job then
  botter_reset_job()
end

local function trim(text)
  return (tostring(text or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function need(text, what)
  text = trim(text)
  if text == "" then
    error(what .. " is empty")
  end
  return text
end

local function exists(path)
  return path == "" or bot.fs.exists(path)
end

--- Record the goal of the agent to build: what it must achieve, for whom, in one or two sentences.
---@param text string  the goal
---@return string  confirmation
function job_goal(text)
  BOTTER.job.goal = need(text, "the goal")
  return "ok: goal recorded"
end

--- Record one input the agent works from: a data file or folder (with its key columns or fields), or what a user types.
---@param path string  the file or folder, relative to the project; "" for input a user types
---@param description string  what it holds: its columns or fields and what they mean
---@return string  confirmation
function job_input(path, description)
  path = trim(path)
  if not exists(path) then
    error(path .. " does not exist: give the path exactly as fs_list shows it")
  end
  table.insert(BOTTER.job.inputs, { path = path, description = need(description, "the description") })
  return "ok: input " .. #BOTTER.job.inputs .. " recorded"
end

--- Record one output the agent must produce: a file (with its columns in order and value formats) or a reply.
---@param path string  the file it writes, relative to the project; "" for a reply in the conversation
---@param columns string  its column or field names in order, comma-separated, exactly as the brief writes them; "" for free text
---@param description string  the format: how each value is written, one row per what
---@return string  confirmation
function job_output(path, columns, description)
  local cols = {}
  for c in tostring(columns or ""):gmatch("[^,%s|]+") do table.insert(cols, c) end
  table.insert(BOTTER.job.outputs, { path = trim(path), columns = cols, description = need(description, "the description") })
  return "ok: output " .. #BOTTER.job.outputs .. " recorded" .. (#cols > 0 and (" with " .. #cols .. " columns") or "")
end

local RULE_KINDS = { exact = true, judgement = true, hybrid = true }

-- A rule's words, for spotting one recorded twice (case, punctuation and "(R3)" references ignored).
local function words(text)
  local set, n = {}, 0
  for w in tostring(text):lower():gsub("%(r%d+%)", ""):gmatch("[%w_]+") do
    if not set[w] then set[w], n = true, n + 1 end
  end
  return set, n
end

local function same_rule(a, b)
  local wa, na = words(a)
  local wb, nb = words(b)
  if na == 0 or nb == 0 then return false end
  local common = 0
  for w in pairs(wa) do
    if wb[w] then common = common + 1 end
  end
  return common / (na + nb - common) >= 0.8
end

--- Record one rule of the brief: a calculation, a limit, an order, a format, a tie-break, a decision. One rule per call, in the brief's own words where possible, with its numbers. A rule already recorded is not added again.
---@param text string  the rule
---@param source string  where the brief states it (file and section)
---@param kind "exact"|"judgement"|"hybrid"  exact = arithmetic, dates, limits, joins, orders, formats (code does it); judgement = reading free text or images, wording, tone (the model does it); hybrid = the model reads facts that code then decides on
---@return string  the rule's id (R1, R2 ...)
function job_rule(text, source, kind)
  text = need(text, "the rule")
  if not RULE_KINDS[kind] then
    error("kind must be exact, judgement or hybrid")
  end
  source = trim(source)
  for _, r in ipairs(BOTTER.job.rules) do
    if same_rule(r.text, text) then
      if source ~= "" and not r.source:lower():find(source:lower(), 1, true) then
        r.source = r.source ~= "" and (r.source .. "; " .. source) or source -- the same rule, stated in another section
      end
      return "already recorded as " .. r.id .. " (not added again); " .. #BOTTER.job.rules .. " rules so far"
    end
  end
  local id = "R" .. (#BOTTER.job.rules + 1)
  table.insert(BOTTER.job.rules, { id = id, text = text, source = source, kind = kind })
  return "ok: " .. id
end

--- Record worked examples or expected answers the brief gives: a file of examples, or examples in the brief's text.
---@param path string  the file that holds them, relative to the project; "" when they are in the brief's text
---@param description string  what one example is and which fields are expected
---@return string  confirmation
function job_examples(path, description)
  path = trim(path)
  if not exists(path) then
    error(path .. " does not exist: give the path exactly as fs_list shows it")
  end
  table.insert(BOTTER.job.examples, { path = path, description = need(description, "the description") })
  return "ok: examples recorded"
end

--- Say that a section of the brief holds no rule for the agent (e.g. how to submit, the file list already recorded with job_input), so job_check stops asking for it.
---@param section string  the section's heading, as the brief writes it
---@param why string  why it holds no rule
---@return string  confirmation
function job_skip_section(section, why)
  section = need(section, "the section")
  need(why, "the reason")
  table.insert(BOTTER.job.skipped, section)
  return "ok: " .. section .. " skipped"
end

-- The brief: Markdown files in the folder and one level down (not the agent's own AGENT.md or Botter's files).
function botter_brief_files()
  local found = {}
  local function look(dir, depth)
    local ok, names = pcall(bot.fs.list, dir)
    if not ok then return end
    for _, name in ipairs(names) do
      local path = dir == "." and name or (dir .. "/" .. name)
      if name:sub(-1) == "/" then
        if depth < 1 and name ~= "functions/" and name ~= "skills/" and name ~= ".botter/" and name ~= "agents/" then
          look(path:sub(1, -2), depth + 1)
        end
      elseif name:lower():match("%.md$") and name ~= "AGENT.md" then
        table.insert(found, path)
      end
    end
  end
  look(".", 0)
  return found
end

-- Headings of the brief (## and ###) that no rule names in its source and that were not skipped.
local function uncovered_sections()
  local said = {}
  for _, r in ipairs(BOTTER.job.rules) do table.insert(said, r.source:lower()) end
  for _, sk in ipairs(BOTTER.job.skipped or {}) do table.insert(said, sk:lower()) end
  local text = table.concat(said, "\n")
  local out = {}
  for _, file in ipairs(botter_brief_files()) do
    for line in (bot.fs.read(file) .. "\n"):gmatch("([^\n]*)\n") do
      local heading = line:match("^###?%s+(.-)%s*$")
      if heading and heading ~= "" and not text:find(heading:lower(), 1, true) then
        table.insert(out, file .. ": " .. heading)
      end
    end
  end
  return out
end

--- Record how success is measured: the score, the checks, what "done" means.
---@param text string  one criterion
---@return string  confirmation
function job_success(text)
  table.insert(BOTTER.job.success, need(text, "the criterion"))
  return "ok: criterion recorded"
end

--- Record an edge case and what the agent must do then (missing data, bad input, a service down ...).
---@param text string  the case and the expected behaviour
---@return string  confirmation
function job_edge(text)
  table.insert(BOTTER.job.edges, need(text, "the edge case"))
  return "ok: edge case recorded"
end

--- Say what kind of agent this is.
---@param kind "chat"|"batch"|"service"|"scheduled"|"producer"  chat = talks with a person; batch = one task per input line (--run), e.g. one record of a CSV; service = answers other programs; scheduled = runs on a timer; producer = writes output files
---@param why string  one sentence
---@return string  confirmation
function design_kind(kind, why)
  if not KINDS[kind] then
    error("kind must be chat, batch, service, scheduled or producer")
  end
  BOTTER.design.kind, BOTTER.design.why = kind, trim(why)
  return "ok: " .. kind
end

local function rule_ids(text)
  local ids = {}
  for id in tostring(text or ""):gmatch("R%d+") do
    table.insert(ids, id)
  end
  return ids
end

function list_units()
  local units = {}
  for _, f in ipairs(BOTTER.design.functions) do table.insert(units, f) end
  for _, s in ipairs(BOTTER.design.model_steps) do table.insert(units, s) end
  for _, s in ipairs(BOTTER.design.skills or {}) do table.insert(units, s) end
  return units
end

local function known_rule(id)
  for _, r in ipairs(BOTTER.job.rules) do
    if r.id == id then
      return true
    end
  end
  return false
end

--- Plan one Lua function of the agent: exact work (rules, arithmetic, dates, joins, filters, formats, reading and writing files). The agent's model calls it like a tool.
---@param name string  snake_case, e.g. forecast_balance
---@param params string  its parameters with types, e.g. "request_id string, horizon_days integer"
---@param returns string  what it returns, e.g. "table {amount, status}"
---@param purpose string  what it does, step by step
---@param rules string  the ids of the rules it implements, e.g. "R1, R4"
---@param uses string?  other planned functions it calls, comma-separated
---@return string  confirmation
function design_function(name, params, returns, purpose, rules, uses)
  name = trim(name)
  if not name:match("^[a-z][a-z0-9_]*$") or #name > 64 then
    error("the name must be snake_case: lowercase letters, digits and _")
  end
  if BUILTIN[name] or name:match("^job_") or name:match("^design_") or name == "agent_build" then
    error(name .. " is already a tool's name")
  end
  for _, id in ipairs(rule_ids(rules)) do
    if not known_rule(id) then
      error(id .. " is not a recorded rule")
    end
  end
  for i, f in ipairs(BOTTER.design.functions) do
    if f.name == name then
      table.remove(BOTTER.design.functions, i) -- planned again: the new plan replaces it
      break
    end
  end
  table.insert(BOTTER.design.functions, { name = name, params = trim(params), returns = need(returns, "returns"),
    purpose = need(purpose, "the purpose"), rules = rule_ids(rules), uses = trim(uses) })
  return "ok: function " .. name .. " planned"
end

--- Plan one step the agent's model does itself: reading text or images, judging, explaining. Only what code cannot do exactly.
---@param name string  a short name
---@param purpose string  what the model does, and which function facts it works from
---@param rules string  the ids of the rules it covers, e.g. "R7"
---@return string  confirmation
function design_model_step(name, purpose, rules)
  for _, id in ipairs(rule_ids(rules)) do
    if not known_rule(id) then
      error(id .. " is not a recorded rule")
    end
  end
  name = need(name, "the name")
  for i, s in ipairs(BOTTER.design.model_steps) do
    if s.name == name then
      table.remove(BOTTER.design.model_steps, i) -- planned again: the new plan replaces it
      break
    end
  end
  table.insert(BOTTER.design.model_steps, { name = name, purpose = need(purpose, "the purpose"), rules = rule_ids(rules) })
  return "ok: model step " .. name .. " planned"
end

--- Add rules to a function or model step you already planned (when a rule is not implemented anywhere yet).
---@param name string  the planned function or model step
---@param rules string  the rule ids to add, e.g. "R10, R15"
---@return string  confirmation
function design_cover(name, rules)
  local ids = rule_ids(rules)
  for _, id in ipairs(ids) do
    if not known_rule(id) then
      error(id .. " is not a recorded rule")
    end
  end
  for _, unit in ipairs(list_units()) do
    if unit.name == trim(name) then
      for _, id in ipairs(ids) do
        local has = false
        for _, r in ipairs(unit.rules) do has = has or r == id end
        if not has then table.insert(unit.rules, id) end
      end
      return "ok: " .. unit.name .. " implements " .. table.concat(unit.rules, ", ")
    end
  end
  error(trim(name) .. " is not planned: plan it with design_function or design_model_step first")
end

--- Remove a function or model step from the plan (to merge it into another one, or when it is not needed).
---@param name string  the planned function or model step
---@return string  confirmation
function design_drop(name)
  name = trim(name)
  for _, group in ipairs({ BOTTER.design.functions, BOTTER.design.model_steps, BOTTER.design.skills }) do
    for i, unit in ipairs(group) do
      if unit.name == name then
        table.remove(group, i)
        return "ok: " .. name .. " removed; its rules need another function or model step"
      end
    end
  end
  error(name .. " is not planned")
end

--- Plan one skill of the agent: a capability the model loads on demand (a procedure with its references, scripts and templates), for open-ended work a conversation needs. Not for exact rules: those are functions.
---@param name string  lowercase letters, digits and -, e.g. write-report
---@param description string  what it does and when to use it (the model picks skills by this)
---@param files string  the files besides SKILL.md, one per line as "path: what it holds", e.g. "references/style.md: the house style" or "scripts/render.py: renders the slides"; "" for none
---@param rules string  the ids of the rules it covers, e.g. "R3, R9"
---@return string  confirmation
function design_skill(name, description, files, rules)
  name = trim(name)
  if not name:match("^[a-z0-9][a-z0-9-]*$") or #name > 64 then
    error("a skill name is lowercase letters, digits and -")
  end
  for _, id in ipairs(rule_ids(rules)) do
    if not known_rule(id) then
      error(id .. " is not a recorded rule")
    end
  end
  local list = {}
  for line in tostring(files or ""):gmatch("[^\n;]+") do
    local path, what = line:match("^%s*([%w_./-]+)%s*:%s*(.-)%s*$")
    if path then
      if path:match("^/") or path:match("%.%.") or path == "SKILL.md" then
        error(path .. ": give a path inside the skill folder (references/..., scripts/..., assets/...)")
      end
      table.insert(list, { path = path, outline = what })
    end
  end
  for i, s in ipairs(BOTTER.design.skills) do
    if s.name == name then table.remove(BOTTER.design.skills, i) break end
  end
  table.insert(BOTTER.design.skills, { name = name, description = need(description, "the description"),
    files = list, rules = rule_ids(rules) })
  return "ok: skill " .. name .. " planned with " .. #list .. " file(s) besides SKILL.md"
end

-- What the job still lacks (nil when it is complete enough to design from).
function job_problems()
  local j, p = BOTTER.job, {}
  if j.goal == "" then table.insert(p, "no goal (job_goal)") end
  if #j.inputs == 0 then table.insert(p, "no input (job_input)") end
  if #j.outputs == 0 then table.insert(p, "no output (job_output)") end
  if #j.rules == 0 then table.insert(p, "no rule (job_rule, one per rule of the brief)") end
  if #j.success == 0 then table.insert(p, "no success criterion (job_success)") end
  local open = uncovered_sections()
  if #open > 0 then
    table.insert(p, "sections of the brief no rule comes from: " .. table.concat(open, "; ")
      .. ". Read each one again and record its rules with job_rule, its source naming the section (e.g. \""
      .. open[1]:match("^[^:]+") .. ": " .. open[1]:match(": (.*)$") .. "\"), or job_skip_section when it holds no rule for the agent")
  end
  return #p > 0 and table.concat(p, "; ") or nil
end

local MAX_FUNCTIONS = 8

-- Rules one function may implement: 7, more when the job has so many that 8 functions could not hold them.
local function max_rules()
  return math.max(7, math.ceil(#BOTTER.job.rules / (MAX_FUNCTIONS - 2)))
end

-- A column of an output comes from a function's returns, or a model step names it (a text the model writes).
local function column_made(col)
  local function names(text)
    return (" " .. tostring(text or ""):lower():gsub("[^%w_]", " ") .. " "):find(" " .. col:lower() .. " ", 1, true)
  end
  for _, f in ipairs(BOTTER.design.functions) do
    if names(f.returns) then return true end
  end
  for _, s in ipairs(BOTTER.design.model_steps) do
    if names(s.purpose) then return true end
  end
  return false
end

-- What the design still lacks (nil when it can be built).
function design_problems()
  local d, j, p = BOTTER.design, BOTTER.job, {}
  if not d.kind then table.insert(p, "no kind (design_kind)") end
  if #d.functions == 0 and #d.model_steps == 0 and #d.skills == 0 then
    table.insert(p, "no function, model step or skill")
  end
  if #d.functions > MAX_FUNCTIONS then
    table.insert(p, #d.functions .. " functions: plan at most " .. MAX_FUNCTIONS
      .. " (each is a tool the model sees on every request)")
  end
  for _, f in ipairs(d.functions) do
    if #f.rules > max_rules() then -- one function that does everything cannot be tested or fixed piece by piece
      table.insert(p, f.name .. " implements " .. #f.rules .. " rules: at most " .. max_rules()
        .. "; split it into functions of one calculation or decision each")
    end
  end
  local covered, names, in_code = {}, {}, {}
  for _, f in ipairs(d.functions) do
    names[f.name] = true
    for _, id in ipairs(f.rules) do covered[id], in_code[id] = true, true end
  end
  for _, s in ipairs(d.model_steps) do
    for _, id in ipairs(s.rules) do covered[id] = true end
  end
  for _, s in ipairs(d.skills) do
    for _, id in ipairs(s.rules) do covered[id] = true end
  end
  local missing = {}
  for _, r in ipairs(j.rules) do
    if not covered[r.id] then table.insert(missing, r.id) end
  end
  if #missing > 0 then
    table.insert(p, "rules no function or model step implements: " .. table.concat(missing, ", "))
  end
  local exact = {} -- a model cannot do arithmetic, dates or orderings reliably: code must
  for _, r in ipairs(j.rules) do
    if covered[r.id] and not in_code[r.id] and (r.kind == "exact" or r.kind == "hybrid") then
      table.insert(exact, r.id)
    end
  end
  if #exact > 0 then
    table.insert(p, "rules that are exact or hybrid but only a model step or skill covers: " .. table.concat(exact, ", ")
      .. ": a function must implement them (a model step may read facts for them, not decide them)")
  end
  for _, f in ipairs(d.functions) do
    for used in f.uses:gmatch("[%w_]+") do
      if not names[used] then
        table.insert(p, f.name .. " uses " .. used .. ", which is not planned")
      end
    end
  end
  for _, o in ipairs(j.outputs) do
    local file, made = o.path:match("([^/]+)$"), false
    for _, f in ipairs(d.functions) do
      if file and (f.purpose .. " " .. f.returns):find(file, 1, true) then made = true end
    end
    if file and not made then
      table.insert(p, "no function says it writes " .. o.path .. " (name the file in its purpose)")
    end
    local missing_cols = {}
    for _, c in ipairs(o.columns or {}) do
      if not column_made(c) then table.insert(missing_cols, c) end
    end
    if #missing_cols > 0 then
      table.insert(p, (o.path ~= "" and o.path or "the reply") .. ": no function returns (and no model step writes) "
        .. table.concat(missing_cols, ", ") .. ": name each column in the returns of the function that computes it")
    end
  end
  return #p > 0 and table.concat(p, "; ") or nil
end

function job_markdown()
  local j = BOTTER.job
  local out = { "# Job", "", "Goal: " .. j.goal, "", "## Inputs" }
  for _, i in ipairs(j.inputs) do table.insert(out, "- " .. (i.path ~= "" and i.path or "(typed)") .. ": " .. i.description) end
  table.insert(out, "\n## Outputs")
  for _, o in ipairs(j.outputs) do
    table.insert(out, "- " .. (o.path ~= "" and o.path or "(reply)") .. ": "
      .. (#(o.columns or {}) > 0 and ("columns " .. table.concat(o.columns, ",") .. ". ") or "") .. o.description)
  end
  table.insert(out, "\n## Rules")
  for _, r in ipairs(j.rules) do
    table.insert(out, r.id .. ". " .. (r.kind and ("[" .. r.kind .. "] ") or "") .. r.text
      .. (r.source ~= "" and (" (" .. r.source .. ")") or ""))
  end
  if #j.edges > 0 then
    table.insert(out, "\n## Edge cases")
    for _, e in ipairs(j.edges) do table.insert(out, "- " .. e) end
  end
  if #(j.skipped or {}) > 0 then
    table.insert(out, "\n## Sections of the brief without rules")
    for _, sk in ipairs(j.skipped) do table.insert(out, "- " .. sk) end
  end
  if #j.examples > 0 then
    table.insert(out, "\n## Examples")
    for _, e in ipairs(j.examples) do table.insert(out, "- " .. (e.path ~= "" and e.path or "(in the brief)") .. ": " .. e.description) end
  end
  table.insert(out, "\n## Success")
  for _, s in ipairs(j.success) do table.insert(out, "- " .. s) end
  return table.concat(out, "\n") .. "\n"
end

function design_function_line(f)
  return f.name .. "(" .. f.params .. ") -> " .. f.returns .. ": " .. f.purpose
    .. (#f.rules > 0 and (" Rules: " .. table.concat(f.rules, ", ") .. ".") or "")
    .. (f.uses ~= "" and (" Uses: " .. f.uses .. ".") or "")
end

function design_markdown()
  local d = BOTTER.design
  local out = { "# Design", "", "Kind: " .. tostring(d.kind) .. (d.why ~= "" and (" (" .. d.why .. ")") or ""), "",
    "## Functions (exact work: Lua, tested)" }
  for _, f in ipairs(d.functions) do table.insert(out, "- " .. design_function_line(f)) end
  if #d.skills > 0 then
    table.insert(out, "\n## Skills (loaded on demand)")
    for _, s in ipairs(d.skills) do
      table.insert(out, "- " .. s.name .. ": " .. s.description .. (#s.rules > 0 and (" Rules: " .. table.concat(s.rules, ", ") .. ".") or ""))
      for _, f in ipairs(s.files) do table.insert(out, "  - " .. f.path .. ": " .. f.outline) end
    end
  end
  if #d.model_steps > 0 then
    table.insert(out, "\n## Model steps (judgement)")
    for _, s in ipairs(d.model_steps) do
      table.insert(out, "- " .. s.name .. ": " .. s.purpose .. (#s.rules > 0 and (" Rules: " .. table.concat(s.rules, ", ") .. ".") or ""))
    end
  end
  return table.concat(out, "\n") .. "\n"
end

--- Check the job you recorded: what is still missing, or "complete". Call it before you reply.
---@return string  the gaps, or "complete"
function job_check()
  return job_problems() or ("complete: " .. #BOTTER.job.rules .. " rules")
end

--- Check the plan: rules no unit implements, too many functions or rules per function, outputs or columns nobody makes; or "complete". Call it before you reply.
---@return string  the gaps, or "complete"
function design_check()
  return design_problems() or "complete"
end
