-- {{name}}: records in, rows out. Code decides; the model only reads text and images (bot.llm with a schema).
-- Run:   ./{{name}} --run "input.csv"           (or: botter run --run "input.csv")
-- Score: botter eval <solved-examples.csv>     (calls solve_case for each example)

INPUT_FILE = "input.csv"            -- the records to answer
OUTPUT_FILE = "output.csv"
OUTPUT_COLUMNS = {"id"}             -- every column of the output, in order

local DATA -- the data files, parsed once and kept for every case

local function load_data()
  local d = {}
  -- d.things = bot.csv.parse(bot.fs.read("data/things.csv"))   -- then convert and index by id
  return d
end

local function data()
  if not DATA then
    DATA = load_data()
  end
  return DATA
end

-- One record (its input columns, as strings) -> a table with every output column.
function solve_case(case)
  local d = data()
  error("solve_case is not written yet: apply the rules of the brief to case " .. tostring(case.id))
end

-- The whole job: every record of the input file -> OUTPUT_FILE.
function flow(input)
  local path = (input ~= nil and input ~= "") and input or INPUT_FILE
  local rows = bot.csv.parse(bot.fs.read(path))
  local out, failed = {}, 0
  for _, row in ipairs(rows) do
    local ok, res = pcall(solve_case, row)
    if not ok then
      failed = failed + 1
      bot.say("case " .. tostring(row.id) .. " failed: " .. tostring(res))
      res = {id = row.id}
    end
    table.insert(out, res)
  end
  bot.fs.write(OUTPUT_FILE, bot.csv.encode(out, OUTPUT_COLUMNS))
  local u = bot.usage()
  return string.format("wrote %s: %d rows (%d failed); model calls %d, tokens %d in / %d out",
    OUTPUT_FILE, #out, failed, u.calls, u.input_tokens, u.output_tokens)
end
