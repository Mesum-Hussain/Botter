# Lua in a Botter agent

Lua 5.4. All Lua files (FLOW.lua, functions/*.lua, skills/*/functions/*.lua) share one state: everything can
call everything, and module-level variables live as long as the agent runs (use them to load data once).
Not available: io, os, require, dofile, load, package, debug, coroutine, math.random. `print` goes to stderr.
Limits: 512 MB for all Lua; a function the model calls as a tool may run 60 s; flow() and --call have no
time limit (Ctrl-C stops them).

## A tool the model can call

```lua
--- Days from one date to another (negative if "to" is earlier). Use it for any date difference.
---@param from string  first date, YYYY-MM-DD
---@param to string  second date, YYYY-MM-DD
---@return integer  the number of days
function days_between(from, to)
  return bot.date.diff_days(from, to)
end
```

- A `---` comment directly above `function name(...)` at column 0 makes it a tool. First lines: what it does
  and when to call it. `---@param name type description` for every parameter; `---@return`.
- Types: string number integer boolean table any, lists `string[]`, choices `"low"|"high"`; optional `name?`.
- No `---` comment, or `local function`: a helper the model never sees. Keep tools few: each one is sent with
  every model request.
- Return a table (the model gets JSON) or a short text. Fail with `error("what went wrong, how to fix it")`.
- End each top-level function with `end` at column 0.

## The bot module

Data
- `bot.json.encode(v)`, `bot.json.decode(text)` (object keys are written sorted)
- `bot.csv.parse(text)` -> list of rows keyed by header; every column present, "" when empty.
  `bot.csv.parse(text, {header = false})` -> list of lists.
- `bot.csv.encode(rows, {"col1", "col2"})` -> text with a header line, in that column order
- `bot.date.add_days(d, n)`, `bot.date.add_months(d, n)` (end of month clamps), `bot.date.diff_days(a, b)`,
  `bot.date.weekday(d)` (1 = Monday), `bot.date.is_valid(d)`; dates are "YYYY-MM-DD" strings, which also
  compare correctly with `<` and `==`
- `bot.today()` -> "YYYY-MM-DD" (BOTTER_TODAY=YYYY-MM-DD pins it)

Files (inside the working folder only)
- `bot.fs.read(path)`, `bot.fs.write(path, text)`, `bot.fs.append(path, text)`, `bot.fs.exists(path)`,
  `bot.fs.list(dir)`

The model
- `bot.llm(prompt)` -> text: one request to the agent's own model.
- `bot.llm{prompt = ..., system = ..., schema = {...}, images = {"path.png"}}` -> a Lua table. `schema`
  describes the JSON object you want, field by field, e.g.
  `{topic = "billing|delivery|refund|other", amount = "number or null", due = "YYYY-MM-DD or null"}`.
  The runtime asks for exactly that object, checks every field is present, asks once more if not, and
  errors after that. Images are files in the working folder (PNG, JPEG, WEBP, GIF). Validate the values in
  code (types, enums, dates) before using them.
- `bot.worker{task = ..., tools = {"fs_read", ...}, writes = {"out/"}, skills = {...}, max_rounds = 20}`
  -> text: a fresh model conversation with tools for an open-ended part (research, writing a document).
  `writes` limits what it may change. Workers cannot start workers.
- `bot.usage()` -> {calls, input_tokens, output_tokens} so far (for a usage report).

People and progress
- `bot.say(text)`: a progress note for whoever runs the agent (stderr in batch; `botter eval --case` shows it).
- `bot.ask(question)` -> the person's answer, or nil when nobody is there (batch, MCP, A2A).

Outside the agent
- `bot.http.get(url [, headers])`, `bot.http.post(url, body [, headers])` -> {status, body, json}: https,
  or http to this machine; refused when the agent is offline.
- `bot.secret("NAME")` -> a value from `.env` in the working folder or the sealed vault. Never return it,
  log it or put it in an error.
- `bot.shell(command [, timeout_s])` -> output, exit code: sandboxed like shell_exec; risky commands ask the
  person (refused in batch). Quote every argument.
- `bot.mcp(server, tool, args)` -> text; `bot.agent(name, task)` -> text (sub-agent or remote agent);
  `bot.tool(name, args)` -> text (any of the agent's tools, as the model would call it).
- `bot.skill_dir(name)` -> the real folder of one of the agent's skills (to run its scripts/).
- `bot.test([path])` -> {passed, failed, report}: run the project's tests from inside the agent.

## Tests (functions/tests/*.lua, run by botter check and botter build)

```lua
function test_days_between()
  assert(days_between("2026-09-07", "2026-12-06") == 90)
  assert(days_between("2026-03-01", "2026-02-28") == -1, "a day earlier is -1")
end
```

- Every global `test_*` function runs with all the agent's Lua loaded; a failing `assert` or `error` stops
  the build. A `score_*` function may return passed, total [, details]: reported, never a failure.
- In tests `bot.fs.read` reads files relative to the project folder (data and fixtures work);
  `bot.fs.write` stays in memory (later reads in that test file see it; the folder never changes).
- Not available in tests: bot.llm, workers, http, shell, mcp, agents. So keep model reading in its own small
  function, and test the code that uses its results by passing facts in directly. `botter eval` runs
  solve_case end to end, model included.
- Use real ids and rows from the data, never invented ones. Each test calls the function it tests and
  asserts on what it returns. Test the rules from the brief and their edge cases (empty data, a blank field,
  the last day of a month, a tie).

## Lua habits that avoid bugs here

- `tonumber("")` is nil: check blanks before arithmetic (`local a = tonumber(row.amount)`; `if not a then`).
- CSV values are strings: convert once, at load time, into a typed table.
- Money: keep full precision while computing; round only when writing (`string.format("%.2f", x)`), and
  never format with `%d` a value that may have cents. `math.floor(x * 100 + 1e-9) / 100` rounds down to cents.
- Tables used as lists: `#t` counts from 1 with no holes; use `table.insert` and `ipairs`.
- Sort with a total order (tie-break on an id) so results are the same every run.
