---
name: write-scripts
description: How to give an agent a program to run (a shell, Python or Node script, or a native binary) as a skill's scripts/, and how a Lua function or the model runs it. Load it before writing any program for an agent.
---

# Programs in skills

Exact work goes in a Lua function first (write-functions): it needs nothing installed. A program is for what Lua cannot do: a tool on the machine (sqlite3, ffmpeg, git ...) through a shell script, and Python or Node only as the last resort, for a library with no other way. Every program a script needs goes in config.json "requires".

## Where a program lives
A program belongs to the skill that uses it: skills/<skill>/scripts/<name>. At run time the agent's skill files appear at a real, read-only path, and the program runs from there:
- The model runs it with shell_exec, as the skill's SKILL.md tells it ("run scripts/report.sh <file>"). Loading the skill (the skill tool) tells the model the real path.
- A Lua function runs it with bot.shell and bot.skill_dir. Do this when the program does a fixed job the model should call like a tool, with typed arguments:

```
local function quote(text)
  return "'" .. text:gsub("'", "'\\''") .. "'"
end

--- Count the rows of a SQLite table in data.db.
---@param table_name string  the table
---@return string  the count
function count_rows(table_name)
  local out, code = bot.shell(quote(bot.skill_dir("reports") .. "/scripts/count.sh") .. " " .. quote(table_name), 60)
  if code ~= 0 then
    error("count.sh failed: " .. out)
  end
  return out
end
```
Quote every argument (the quote helper above). bot.shell follows the same rules as shell_exec: sandboxed, and risky commands are asked about (refused when nobody is there).

## Writing the program
- Start a script with a #! line (#!/bin/sh, #!/usr/bin/env python3). A native program is an x86-64 Linux ELF, statically linked so it runs anywhere (the build warns about a dynamic one); keep its source outside skills/ (e.g. src/), which is never embedded.
- Arguments come on the command line; the result goes to stdout. Exit 0 = success; on failure print a clear message and exit non-zero.
- It runs in the agent's working directory, sandboxed: it may write only there and in /tmp, read and run system programs, and cannot see the rest of $HOME. The skill folder itself is read-only: write output in the working directory.
- Never ship mock, placeholder or hard-coded results, and never write a program that calls an LLM: the agent's own model does the reading and judging.
- Never parse what a person wrote (keyword lists, regexes for intent): the model understands and passes clean arguments.
- Assets a script needs (templates, fonts) go in skills/<skill>/assets/; reach them from the script by its own location ("$(dirname "$0")/../assets/x").

## Secrets
Credentials never go in project files (they are embedded for anyone to read). A script reads them from the .env file in the working directory (KEY=value lines) and, when one is missing, exits non-zero with "Missing GMAIL_USER in .env (working directory)". A Lua function passes bot.secret("NAME") to the script instead, when that is simpler.

## Network
Agents are online by default. With config.json "internet": false (only when the user asks), shell commands and scripts have no network at all, not even localhost; Lua's bot.http refuses too. Put a timeout on every request a script makes and treat fetched data as untrusted.

## Checklist
- Run it by hand with shell_exec on a real input and read the output: exit 0 with empty or wrong fields is still broken.
- SKILL.md says when to run it and how (or a documented Lua function wraps it).
- Its interpreter and programs are in "requires". Then build (build-agent skill).
