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
  if not bot.fs.exists("functions") then
    return {}
  end
  local found = {}
  for _, name in ipairs(bot.fs.list("functions")) do
    if name:match("%.lua$") then
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
  return nil
end
