--- Count the words in a text. Use it whenever a request needs an exact word count.
---@param text string  the text to count
---@return integer  the number of words
function word_count(text)
  local n = 0
  for _ in text:gmatch("%S+") do
    n = n + 1
  end
  return n
end
