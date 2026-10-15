---
name: write-functions
description: How to write an agent's Lua functions (functions/<name>.lua) for exact, repeatable work, with their tests, and when a shell or Python tool is the better choice. Read before writing anything under functions/.
---

# Functions (Lua)

The agent's model talks to the user and decides what to do. A function does one exact job the model is bad at, the same way every time: arithmetic, dates, joining and filtering data, checking output against rules, reading and writing CSV or JSON files, calling an HTTP API in fixed steps. The model calls it like a tool and gets its result back.

## Which kind of tool (in this order)
1. Lua function in functions/: runs inside the agent, needs nothing installed. The default.
2. Shell script in tools/bin (see write-tool): when the job needs a program on the machine (sqlite3, psql, ffmpeg, git). List each program in config.json "requires".
3. Python or Node script in tools/bin: last resort, only for a library with no other way (PDF parsing, a vendor SDK). The interpreter goes in "requires".

## A function file
```
--- Days from one date to another (negative if "to" is earlier).
---@param from string  first date, YYYY-MM-DD
---@param to string  second date, YYYY-MM-DD
---@return integer  the number of days
function days_between(from, to)
  return bot.date.diff_days(from, to)
end
```
- A `---` comment directly above `function name(...)` (at column 0) makes it a tool: the first lines say what it does and WHEN the model should call it; each parameter gets `---@param name type description`; `---@return` says what comes back.
- Types: string, number, integer, boolean, table, any, a list `string[]`, a choice of words `"buy"|"wait"`. An optional parameter is `name?`.
- Functions without a `---` comment, and `local function`s, are helpers: the model never sees them.
- End each top-level function with `end` at column 0. Names must not be a tool's name (fs_read, shell_exec ...).
- Return a table (the model gets JSON) or a short text. To fail, `error("what went wrong and how to fix it")`: the model gets the message with its file and line.

## Readable code
Clear names (`balance_after`, not `b2`), short functions, plain loops and ifs, a comment for each rule that comes from the user ("-- pending credits do not count until they settle"). Someone who reads the request should recognise every rule in the code.

## What Lua can reach (the `bot` module, nothing else: no io, os or require)
- bot.json.encode(value) / bot.json.decode(text)
- bot.csv.parse(text) -> list of rows keyed by the header; empty fields are "" (never nil)
- bot.csv.encode(rows, {"col1", "col2"}) -> text with a header line
- bot.date.add_days(d, n), add_months(d, n), diff_days(from, to), weekday(d) (1 = Monday), is_valid(d); dates are "YYYY-MM-DD" texts
- bot.today() -> today's date (BOTTER_TODAY=YYYY-MM-DD in the environment pins it)
- bot.fs.read(path), write(path, text), append(path, text), list(dir), exists(path): inside the working directory only; writes are refused in Plan mode
- bot.http.get(url [, headers]) / bot.http.post(url, body [, headers]) -> {status, body, json}: https only (http:// for this machine); refused when the agent is offline
- bot.secret(name) -> a key from .env or the sealed vault, for HTTP headers. Never return it or put it in an error
- bot.shell(command) -> output, exit code: the same rules as shell_exec (risky commands ask the user, or are refused when nobody is there)
- bot.mcp(server, tool, args) -> text: a tool of an MCP server in config.json
- bot.agent(name, task) -> text: hand a task to a sub-agent or a remote agent

## Rules
- Never parse what a person wrote (intent, names, keywords): that is the model's job. The model passes clean arguments.
- Never call an LLM, never return mock or hard-coded results.
- Keep results small (over 100 KB is cut): return what the model needs, write the rest to a file and return its name.
- A call may run 60 seconds at most.

## Tests (run by the build)
functions/tests/<name>.lua is never embedded. Every global function whose name starts with test_ runs at build time with all the functions loaded; a failing assert stops the build.
```
function test_days_between()
  assert(days_between("2026-09-07", "2026-12-06") == 90)
  assert(days_between("2026-03-01", "2026-02-28") == -1)
end
```
In tests, bot.fs.read reads files relative to the project folder (put sample data in functions/tests/), and writes, the network, commands and other agents are not available. Test the rules from the user's request, including the edge cases (empty data, a missing field, the last day of a month).
