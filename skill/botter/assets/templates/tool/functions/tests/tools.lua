function test_word_count()
  assert(word_count("one two  three") == 3)
  assert(word_count("") == 0, "an empty text has no words")
end
