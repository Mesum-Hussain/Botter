-- Botter's own functions: the build tools (they run skills/build-agent/scripts/agent_tools, the packer),
-- start_build (the lead hands a build to FLOW.lua), and the checks FLOW.lua runs on the agent's code.

local function quote(text)
  return "'" .. text:gsub("'", "'\\''") .. "'"
end

-- Run the build program with these arguments; its report is the result.
local function agent_tools(args)
  local dir = bot.skill_dir("build-agent")
  if not dir then
    error("the build-agent skill is missing from Botter, so it cannot build")
  end
  local out, code = bot.shell(quote(dir .. "/scripts/agent_tools") .. " " .. args, 120)
  if code ~= 0 and not out:find("error", 1, true) then
    out = out .. "\n(the build program stopped with exit code " .. tostring(code) .. ")"
  end
  return out
end

--- Check an agent project and build it into one executable, <project folder>.bot: the runtime plus AGENT.md,
--- config.json, functions/, skills/ and agents/. The check covers config.json, every skill, the Lua functions
--- and their tests (functions/tests), and prints scores. Fix every error and warning it reports.
---@param dry_run boolean? only check the project and run its tests, write nothing (default false)
---@param dir string? the project folder, inside the working directory (default: the working directory)
---@param output string? where to write the .bot, inside the working directory (default: ./<folder>.bot)
---@return string the build report
function agent_build(dry_run, dir, output)
  local args = "build"
  if dry_run then
    args = args .. " --dry-run"
  end
  if output and output ~= "" then
    args = args .. " --output " .. quote(output)
  end
  return agent_tools(args .. " " .. quote(dir and dir ~= "" and dir or "."))
end

--- Start building (or changing) an agent in this folder from the user's request. Botter's build pipeline takes over: it studies the brief and the data, designs the agent, writes and tests it, and builds the .bot.
---@param request string  what the user wants, in their words, with everything they said that matters
---@param resume boolean?  true to continue the unfinished build recorded in .botter/ (when the user asks to continue) instead of starting over
---@return string  confirmation
function start_build(request, resume)
  BOTTER.start, BOTTER.resume = request, resume and bot.fs.exists(".botter/state.json")
  return "ok: the build starts when you reply. Reply with one short line saying you are starting."
end

--- List what a built .bot contains (files and runnable programs with their sizes) and verify its checksum.
---@param file string the .bot file, inside the working directory
---@return string the listing
function agent_inspect(file)
  return agent_tools("inspect " .. quote(file))
end

local UNFINISHED = { "todo", "fixme", "placeholder", "simplified", "for demo", "demo only", "not implemented",
  "stub", "mock", "dummy", "hard-coded", "hardcoded", "for this test", "in a real", "implement later",
  "to be implemented", "under construction" }

-- Comment lines in functions/*.lua that say the code is not done ("simplified", "placeholder" ...).
function botter_unfinished_code()
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
function botter_example_files()
  local found = {}
  local function look(dir, depth)
    for _, name in ipairs(bot.fs.list(dir)) do
      local path = dir == "." and name or (dir .. "/" .. name)
      if name:sub(-1) == "/" then
        if depth < 2 and name ~= "functions/" and name ~= "media/" and name ~= ".botter/" then
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
function botter_count_examples(files)
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
function botter_reads_answer_key(files)
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

--- Run the tests of the project's functions and see the result: which tests pass, which fail and why, and any score. The shell cannot run them (it has no bot module and does not load the functions): use this.
---@param path string?  a test file (e.g. functions/tests/add_row.lua) or folder; default: every test
---@return string  the test report
function run_tests(path)
  local r = bot.test(path ~= "" and path or nil)
  local lines, n = {}, 0
  for line in r.report:gmatch("[^\n]+") do
    n = n + 1
    if n > 60 then
      table.insert(lines, "...")
      break
    end
    table.insert(lines, line)
  end
  return r.passed .. " passed, " .. r.failed .. " failed\n" .. table.concat(lines, "\n")
end

-- A column holds free text (an explanation, a message) when most of its values run to 6 words or more:
-- such text is the model's judgement, compared by nobody.
local function free_text(rows, col)
  local long, filled = 0, 0
  for _, r in ipairs(rows) do
    local v = tostring(r[col] or "")
    if v:find("%S") then
      filled = filled + 1
      local words = 0
      for _ in v:gmatch("%S+") do words = words + 1 end
      if words >= 6 then long = long + 1 end
    end
  end
  return filled > 0 and long * 2 >= filled
end

-- Worked examples that are CSV rows holding a case's fields and its expected answers: for each such file,
-- {path, id (the first column that is not an answer), answers (columns of an output that no input holds:
-- removed from the case), scored (answers compared, free text left out), count}. Botter scores the agent on these by code.
function botter_answer_examples()
  local outputs = {}
  for _, o in ipairs(BOTTER.job.outputs) do
    for _, c in ipairs(o.columns or {}) do outputs[c] = true end
  end
  for _, i in ipairs(BOTTER.job.inputs) do -- a column an input holds too (an id) is a case field, not an answer
    if i.path:match("%.csv$") and bot.fs.exists(i.path) then
      local ok, raw = pcall(bot.csv.parse, bot.fs.read(i.path):match("^[^\n]*"), { header = false })
      for _, col in ipairs(ok and raw[1] or {}) do outputs[col] = nil end
    end
  end
  local paths, seen, found = {}, {}, {}
  for _, e in ipairs(BOTTER.job.examples) do table.insert(paths, e.path) end
  local ok, files = pcall(botter_example_files)
  for _, p in ipairs(ok and files or {}) do table.insert(paths, p) end
  for _, path in ipairs(paths) do
    path = path:gsub("^%./", "")
    if not seen[path] and path:match("%.csv$") and bot.fs.exists(path) then
      seen[path] = true
      local text = bot.fs.read(path)
      local okh, raw = pcall(bot.csv.parse, text, { header = false })
      local okr, rows = pcall(bot.csv.parse, text)
      if okh and okr and raw[1] and #rows > 0 then
        local ex = { path = path, answers = {}, scored = {}, count = #rows }
        for _, col in ipairs(raw[1]) do
          if outputs[col] then
            table.insert(ex.answers, col)
            if not free_text(rows, col) then table.insert(ex.scored, col) end
          elseif not ex.id then
            ex.id = col
          end
        end
        if #ex.scored > 0 and ex.id then table.insert(found, ex) end
      end
    end
  end
  return found
end
