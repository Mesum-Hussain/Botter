-- search.lua: keyword search (BM25) over a folder of Markdown or text documents, in plain Lua.
-- Copy it into functions/ when the agent must answer from a corpus (help articles, policies, manuals).
--
--   local index = search_index{dirs = {"data"}}            -- every .md/.txt below data/, split at headings
--   local hits = search_docs(index, "reset my password", 5) -- best sections first
--   hits[1].path, hits[1].title, hits[1].heading, hits[1].text, hits[1].score
--   search_docs(index, query, 5, function(sec) return sec.path:find("^data/visa/") end) -- only some docs
--
-- Build the index once per run (keep it in a module variable). Give the model only the top few sections
-- and ask it to answer from them (and to say when they do not cover the question).

local STOP = {}
for w in ("a an and are as at be by can do does for from has have how i if in is it its my of on or our so "
  .. "that the their then there this to was we what when where which who why will with you your"):gmatch("%S+") do
  STOP[w] = true
end

-- Words of a text: lowercase letters and digits (other UTF-8 letters kept as they are), stop words out,
-- a plain plural "s" dropped.
function search_words(text)
  local out = {}
  for w in text:lower():gmatch("[%w\128-\255]+") do
    if #w > 1 and not STOP[w] then
      if #w > 3 and w:sub(-1) == "s" and w:sub(-2) ~= "ss" then
        w = w:sub(1, -2)
      end
      table.insert(out, w)
    end
  end
  return out
end

local function list_files(dir, exts, out)
  for _, name in ipairs(bot.fs.list(dir)) do
    local path = dir .. "/" .. name
    if name:sub(-1) == "/" then
      list_files(path:sub(1, -2), exts, out)
    else
      for _, e in ipairs(exts) do
        if name:sub(-#e):lower() == e then
          table.insert(out, path)
        end
      end
    end
  end
  return out
end

-- One document -> sections: the title (front matter "title:" or the first # line), then one section per
-- heading, long sections cut at about `size` characters.
local function sections(path, text, size)
  local title = text:match("^%-%-%-.-\ntitle:%s*\"?([^\n\"]+)") or text:match("\n#%s+([^\n]+)")
    or text:match("^#%s+([^\n]+)") or path:match("([^/]+)%.%w+$")
  text = text:gsub("^%-%-%-\n.-\n%-%-%-\n", "") -- front matter out
  local out, heading, buf = {}, title, {}
  local function flush()
    local body = table.concat(buf, "\n"):gsub("^%s+", ""):gsub("%s+$", "")
    while #body > 0 do
      local cut = #body > size and (body:find("\n", size, true) or size) or #body
      table.insert(out, {path = path, title = title, heading = heading, text = body:sub(1, cut)})
      body = body:sub(cut + 1):gsub("^%s+", "")
    end
    buf = {}
  end
  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    local h = line:match("^#+%s+(.+)")
    if h then
      flush()
      heading = h
    else
      table.insert(buf, line)
    end
  end
  flush()
  return out
end

-- The index: opts.dirs (folders, searched below), or opts.docs ({path, text} given directly);
-- opts.ext (default {".md", ".txt"}), opts.size (characters per section, default 1500).
function search_index(opts)
  local docs = opts.docs or {}
  if opts.dirs then
    for _, d in ipairs(opts.dirs) do
      for _, path in ipairs(list_files(d, opts.ext or {".md", ".txt"}, {})) do
        table.insert(docs, {path = path, text = bot.fs.read(path)})
      end
    end
  end
  local idx = {secs = {}, df = {}, total = 0}
  for _, doc in ipairs(docs) do
    for _, sec in ipairs(sections(doc.path, doc.text, opts.size or 1500)) do
      local tf, n = {}, 0
      -- the title and heading count twice: they say what the section is about
      local words = search_words(sec.title .. " " .. sec.heading .. " " .. sec.title .. " " .. sec.heading
        .. " " .. sec.path:gsub("[/_%-%.]", " ") .. " " .. sec.text)
      for _, w in ipairs(words) do
        if not tf[w] then
          idx.df[w] = (idx.df[w] or 0) + 1
        end
        tf[w] = (tf[w] or 0) + 1
        n = n + 1
      end
      sec.tf, sec.len = tf, n
      idx.total = idx.total + n
      table.insert(idx.secs, sec)
    end
  end
  idx.avg = #idx.secs > 0 and idx.total / #idx.secs or 1
  return idx
end

-- The k best sections for a query (BM25), optionally only those keep(sec) accepts. Ties: path order.
function search_docs(idx, query, k, keep)
  local q, seen = {}, {}
  for _, w in ipairs(search_words(query)) do
    if not seen[w] and idx.df[w] then
      seen[w] = true
      table.insert(q, w)
    end
  end
  local n, hits = #idx.secs, {}
  for _, sec in ipairs(idx.secs) do
    if not keep or keep(sec) then
      local s = 0
      for _, w in ipairs(q) do
        local f = sec.tf[w]
        if f then
          local idf = math.log(1 + (n - idx.df[w] + 0.5) / (idx.df[w] + 0.5))
          s = s + idf * f * 2.2 / (f + 1.2 * (0.25 + 0.75 * sec.len / idx.avg))
        end
      end
      if s > 0 then
        table.insert(hits, {path = sec.path, title = sec.title, heading = sec.heading, text = sec.text, score = s})
      end
    end
  end
  table.sort(hits, function(a, b)
    if a.score ~= b.score then
      return a.score > b.score
    end
    return a.path .. a.heading < b.path .. b.heading
  end)
  local top = {}
  for i = 1, math.min(k or 5, #hits) do
    top[i] = hits[i]
  end
  return top
end
