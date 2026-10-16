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
