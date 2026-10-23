-- Tests for search.lua (copy next to it: functions/tests/search.lua).

local DOCS = {
  {path = "data/billing/refunds.md", text = "---\ntitle: \"Refunds and cancellations\"\n---\n# Refunds\n\n"
    .. "## Ask for a refund\nOpen Billing, choose the charge and press Refund. Refunds take 5-10 days.\n\n"
    .. "## Cancel a plan\nCancelling stops the next renewal; you keep access until the period ends.\n"},
  {path = "data/account/password.md", text = "# Reset your password\n\nUse Forgot password on the sign-in page. "
    .. "A reset link is emailed to you and expires after one hour.\n"},
}

function test_search_finds_the_right_section()
  local idx = search_index{docs = DOCS}
  local hits = search_docs(idx, "how do I get my money back, I want a refund", 3)
  assert(hits[1].path == "data/billing/refunds.md" and hits[1].heading == "Ask for a refund", hits[1].heading)
  assert(hits[1].title == "Refunds and cancellations")
  local pw = search_docs(idx, "forgot password reset link", 1)
  assert(pw[1].path == "data/account/password.md")
end

function test_search_filter_and_no_match()
  local idx = search_index{docs = DOCS}
  local only = search_docs(idx, "refund", 5, function(sec) return sec.path:find("^data/account/") end)
  assert(#only == 0, "the filter keeps only account docs, none mention refunds")
  assert(#search_docs(idx, "zebra", 5) == 0)
end

function test_search_words()
  local w = search_words("The Refunds, and the PASSWORDS!")
  assert(w[1] == "refund" and w[2] == "password", table.concat(w, ","))
end
