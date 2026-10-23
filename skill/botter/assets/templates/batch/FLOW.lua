-- {{name}}: records in, rows out. Code decides; the model only reads (functions/read.lua), in batches.
-- Plan, one line per output column (where its value comes from), e.g.:
--   status   code: rule R1 from the brief, on the fields the model read
--   topic    model: read_items reads the message into one of the allowed values
--   reason   code: words built from the facts behind the decision
-- Run:   ./{{name}} --run "input.csv"           (or: botter run --run "input.csv")
-- Score: botter eval <solved-examples.csv>     (prepare(all cases), then solve_case for each)

INPUT_FILE = "input.csv"            -- the records to answer
OUTPUT_FILE = "output.csv"
OUTPUT_COLUMNS = {"id"}             -- every column of the output, in order

local DATA       -- the data files, parsed once and kept for every case
local FACTS = {} -- what the model read, by item id

local function load_data()
  local d = {}
  -- d.users = {}; for _, r in ipairs(bot.csv.parse(bot.fs.read("data/users.csv"))) do d.users[r.user_id] = r end
  return d
end

local function data()
  if not DATA then
    DATA = load_data()
  end
  return DATA
end

-- What the model reads, in the brief's words: each field's meaning and its allowed values.
READ_TASK = [[You read <what> for <purpose>. For each item fill the fields below.
<the brief's definitions and rules for each field, copied, not paraphrased>]]
READ_FIELDS = {} -- e.g. {topic = "billing|delivery|other", amount = "number or null", flags = "list of: a|b|none"}

-- The items the model must read for one record: texts, images or audio, each with a stable id.
local function items_for(case)
  return {} -- e.g. {{id = case.id, text = case.message}} or {{id = case.id, text = case.note, images = {path}}}
end

-- Every record at once, so the model reads them in batches (cached in cache/read.json).
-- botter eval calls it with all the examples before solve_case; flow calls it with the input rows.
function prepare(cases)
  local items = {}
  for _, c in ipairs(cases) do
    for _, it in ipairs(items_for(c)) do
      table.insert(items, it)
    end
  end
  if #items > 0 then
    for id, f in pairs(read_items{items = items, task = READ_TASK, fields = READ_FIELDS}) do
      FACTS[id] = f
    end
  end
  return #items .. " item(s) read"
end

-- One record (its input columns, as strings) -> a table with every output column.
function solve_case(case)
  local d = data()
  prepare({case}) -- from the cache when prepare already read it: no model call
  error("solve_case is not written yet: apply the rules of the brief to case " .. tostring(case.id))
end

-- The whole job: every record of the input file -> OUTPUT_FILE.
function flow(input)
  local path = (input ~= nil and input ~= "") and input or INPUT_FILE
  local rows = bot.csv.parse(bot.fs.read(path))
  prepare(rows)
  local out, failed = {}, 0
  for i, row in ipairs(rows) do
    local ok, res = pcall(solve_case, row)
    if not ok then
      failed = failed + 1
      bot.say("case " .. i .. " failed: " .. tostring(res))
      res = {id = row.id} -- a safe, valid row: never a guess presented as an answer
    end
    table.insert(out, res)
  end
  bot.fs.write(OUTPUT_FILE, bot.csv.encode(out, OUTPUT_COLUMNS))
  local u = bot.usage()
  return string.format("wrote %s: %d rows (%d failed); model calls %d, tokens %d in / %d out",
    OUTPUT_FILE, #out, failed, u.calls, u.input_tokens, u.output_tokens)
end
