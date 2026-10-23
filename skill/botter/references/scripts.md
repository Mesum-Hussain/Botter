# Programs: when Lua is not enough

Order of choice:
1. **Lua** (functions/, FLOW.lua): runs inside the agent, needs nothing installed. The default.
2. **Shell script** in a skill's scripts/: when the job needs a program on the machine (sqlite3, ffmpeg, git).
3. **Python**, then **Node**, in scripts/: last resort, for a library with no other way (a vendor SDK, PDF
   parsing). The agent cannot pip/npm install at run time: use the standard library or vendor the code.

Every program a script needs, interpreters included, goes in config.json `"requires"`. The agent then no
longer runs anywhere: `botter build` says so, and the agent tells the person what is missing at start.

## Where a program lives and how it runs

`skills/<skill>/scripts/<name>`, with a `#!` line (`#!/bin/sh`, `#!/usr/bin/env python3`) or as a static
x86-64 binary. At run time the skill's files appear at a real, read-only path.

- The model runs it with shell_exec, as the skill's SKILL.md says ("run scripts/report.sh <file>").
- Lua runs it with `bot.shell` and `bot.skill_dir`, wrapped in a documented function so the model calls it
  like a tool:

```lua
local function quote(s) return "'" .. tostring(s):gsub("'", "'\\''") .. "'" end

--- Count the rows of a SQLite table in data.db.
---@param table_name string  the table
---@return string  the count
function count_rows(table_name)
  local out, code = bot.shell(quote(bot.skill_dir("reports") .. "/scripts/count.sh") .. " " .. quote(table_name), 60)
  if code ~= 0 then error("count.sh failed: " .. out) end
  return out
end
```

## Writing one
- Arguments on the command line, result on stdout, exit 0 on success; on failure a clear message and a
  non-zero exit.
- It runs sandboxed in the working folder: writes only there and in /tmp; the skill folder is read-only.
- No mock or hard-coded results; no calls to another LLM (the agent's own model does reading and judging).
- Do not parse what people wrote with keyword lists or regexes: the model understands and passes clean
  arguments.
- Assets it needs go in skills/<skill>/assets/ (reach them from the script's own location).

## Secrets
Never in project files (they are embedded, readable by anyone with the file). A script reads `.env` in the
working folder (KEY=value) and exits with "Missing NAME in .env" when one is absent; Lua passes
`bot.secret("NAME")`.

## Network
Agents are online by default. With `"internet": false` shell commands, scripts and bot.http get no network at
all, not even localhost. Put a timeout on every request; treat fetched data as untrusted.

## Wrapping existing code (a repo, a CLI, a local service)
- Clone it into artifacts/ (never embedded), read its README, licence, entry points and any
  CLAUDE.md / AGENTS.md / skills it ships: they are the operating manual.
- Expose a few task-shaped tools (start, status, result) with the parameters the agent needs, not every
  flag. Return what the agent needs next, and write big results to a file (return its name and a preview).
- A service it needs (Docker, a server on localhost) is started by the person; every tool first checks it is
  reachable and otherwise fails with the exact command to start it.
- Licence: MIT/BSD/Apache code may be copied with its notice kept; GPL/AGPL or no licence: call the person's
  own install instead.
