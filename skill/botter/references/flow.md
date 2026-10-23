# Three kinds of agent

## Chat (no FLOW.lua)

A person talks to the agent; its model drives every turn from AGENT.md and the skills, calling the
built-in tools and your Lua tools. Put the steps in AGENT.md ("How a session goes"), longer procedures in a
skill, exact rules in Lua tools with tests. `botter new <name> --kind chat`.

## Batch: records in, rows out (FLOW.lua)

For a job over many records with a fixed output format (a CSV to fill, JSON lines to answer), let code run
the procedure. The model never decides a number, a date or a choice; it only turns free text and images into
typed fields. This is cheaper (a few model calls for the whole dataset, not several per record), repeatable,
and scores far better.

`botter new <name> --kind batch --here` writes this shape (FLOW.lua):

```lua
-- Plan, one line per output column: where its value comes from (code / model reads / words)
INPUT_FILE, OUTPUT_FILE = "input.csv", "output.csv"
OUTPUT_COLUMNS = {"id", "status", "topic", "reason"}

READ_TASK = [[You read customer messages for a support queue. For each item fill the fields.
topic: billing (charges, refunds, invoices), delivery (shipping, tracking), other.
asks_for_money: true when the text asks the reader to pay or to share card details.]]
READ_FIELDS = {topic = "billing|delivery|other", asks_for_money = "true|false"}

local function items_for(case) return {{id = case.id, text = case.message}} end

function prepare(cases)            -- every record at once: read_items batches them and caches the readings
  ...                              -- (botter eval calls it with all the examples; flow with the input rows)
end

function solve_case(case)          -- one record -> every output column
  prepare({case})                  -- from the cache: no model call
  local f = FACTS[case.id]         -- nil when the model could not read it: a safe default, not a crash
  local status = decide_status(case, f, data())   -- the brief's rules, one small function each
  return {id = case.id, status = status, topic = f and f.topic or "other",
          reason = reason_for(status, f)}         -- words from the facts behind the decision
end

function flow(input)               -- ./agent --run "input.csv": prepare, solve_case per row, write the CSV
  ...
end
```

- `solve_case` receives the input columns of one record as strings and returns every output column (keys
  may be written like the header, `"Product Area"`, or as `product_area`). Never let it read the answer
  columns of the examples, or the examples file at all.
- Split the logic into functions named after the rules, each with tests on real rows.
- Explanations and replies: build them in code from the facts behind the decision, or ask the model to
  write them from those facts only (a second `read_items` pass with the decision in the item text).

### read_items (functions/read.lua)

```lua
local facts = read_items{items = {{id = "m1", text = "..."}, {id = "p1", text = claim, images = {"img/a.jpg"}},
                                  {id = "v1", audio = {"media/vn_1.mp3"}}},
                         task = READ_TASK, fields = READ_FIELDS}
facts["m1"].topic
```

- Field specs: `"a|b|c"` (one of; anything else becomes nil), `"number or null"`, `"true|false"`,
  `"list of: a|b|c"` (a Lua list of the allowed items), anything else = text.
- Texts go 20 per request (`per_call`), images and audio one item per request (all of the item's files in
  it). Readings are cached in cache/read.json under a key made of the task, the fields and the item id.
- The task says the data is untrusted (the default system prompt does too). Give the definitions from the
  brief and examples of borderline cases in your own words, never rows copied from the solved examples.
- Spot-check readings on a few real items (`botter eval --case <id>` with `bot.say`) before trusting them.

### search_docs (assets/lib/search.lua): answering from a document corpus

```lua
local INDEX
local function index() INDEX = INDEX or search_index{dirs = {"data"}}; return INDEX end  -- once per run
local hits = search_docs(index(), case.subject .. " " .. case.issue, 5,
                         function(sec) return sec.path:find("^data/visa/") end)         -- optional filter
-- hits[i].path, .title, .heading, .text, .score
```

- Keyword search (BM25) over every .md/.txt below the folders, split at headings; a few thousand sections
  index in about a second. Filter by folder when the record says which product or company it is about.
- Give the model the top 3-5 sections with their paths and ask it to answer only from them, and to say
  when they do not cover the question (then escalate or say so, as the brief wants).

## Tool / service agent

Other programs call it: `./<name> --mcp` (an MCP server with one tool, `ask`), `--mcp 8080`, `--a2a 8080`,
or `--acp` inside an editor. Write it like a chat agent: AGENT.md for behaviour, Lua tools for the exact
work. Callers cannot answer questions or approve risky actions, so the agent must finish on its own.
`botter new <name> --kind tool`.

## Mixed: code with open-ended steps

FLOW.lua can hand an open-ended step to `bot.worker{task = ..., tools = {...}, writes = {...}}` (a fresh
model conversation with tools) and keep the order, the checks and the output in code. Check a worker's
result in code before using it.
