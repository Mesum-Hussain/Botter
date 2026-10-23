-- Tests for read.lua: a stand-in model (bot.llm is not available in tests).

function test_read_check_kinds()
  assert(read_check("billing|delivery|other", " Billing ") == "billing")
  assert(read_check("billing|delivery|other", "refund") == nil, "not one of the choices")
  assert(read_check("number or null", "1,250.5") == 1250.5)
  assert(read_check("number or null", "n/a") == nil)
  assert(read_check("true|false", "yes") == true)
  assert(read_check("true|false", false) == false)
  local l = read_check("list of: late|damaged|none", "late; Damaged; bogus")
  assert(#l == 2 and l[1] == "late" and l[2] == "damaged")
  assert(read_check("string", "  text  ") == "text")
  assert(read_check("string", nil) == nil)
end

function test_read_items_batches_and_retries()
  local calls = 0
  local function fake(spec)
    calls = calls + 1
    local items = bot.json.decode(spec.prompt:match("Items %(JSON%):\n(.-)\n\nReply"))
    local out = {}
    for _, it in ipairs(items) do
      if not (calls == 1 and it.id == "b") then -- the first request skips b: it is asked again alone
        table.insert(out, {id = it.id, topic = it.text:find("refund") and "refund" or "other"})
      end
    end
    return {items = out}
  end
  local got = read_items{items = {{id = "a", text = "I want a refund"}, {id = "b", text = "hello"}},
    task = "Read the topic.", fields = {topic = "refund|other"}, llm = fake, cache = "cache/test.json"}
  assert(got.a.topic == "refund" and got.b.topic == "other")
  assert(calls == 2, "one batch, then b alone; calls = " .. calls)
  read_items{items = {{id = "a", text = "I want a refund"}}, task = "Read the topic.",
    fields = {topic = "refund|other"}, llm = fake, cache = "cache/test.json"}
  assert(calls == 2, "the second read comes from the cache")
end

function test_read_items_media_one_per_call()
  local seen = {}
  local function fake(spec)
    table.insert(seen, spec.images and spec.images[1] or spec.audio[1])
    return {damage = "dent"}
  end
  local got = read_items{items = {{id = "p1", images = {"a.jpg"}}, {id = "v1", audio = {"v.mp3"}}},
    task = "Read the damage.", fields = {damage = "dent|scratch|none"}, llm = fake, cache = "cache/m.json"}
  assert(#seen == 2 and seen[1] == "a.jpg" and seen[2] == "v.mp3")
  assert(got.p1.damage == "dent" and got.v1.damage == "dent")
end
