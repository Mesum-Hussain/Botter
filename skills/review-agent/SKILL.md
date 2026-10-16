---
name: review-agent
description: Before building, read every file of the agent project and list each requirement of the request and plan that the files do not implement yet. Reply with a JSON list of gaps.
---

# Reviewing an agent project against its requirements

The build only checks formats. This review checks that the files do what the user asked. Do not fix anything in this step: only find the gaps.

1. Read every project file: AGENT.md, config.json, each skills/*/SKILL.md, each functions/*.lua and functions/tests/*.lua, each skills/*/scripts/* and references/*. Read them; do not rely on memory of what you wrote.
2. List the requirements: every input, behaviour, rule, limit, state file and notification in the request, the user's answers and the plan (e.g. "replies are answered in the same thread", "opted-out leads are never contacted", "daily cap of 10 counted from data/sent.json", "TEST_RECIPIENT redirects every lead email").
3. For each requirement, find the exact place that implements it: a function or tool action in the code, a step in AGENT.md, a skill rule. An exact rule (a limit, a calculation, a date) only described in prose, without a function that applies it and a test that checks it, is a gap.
4. Also a gap:
   - an action, parameter or output named in a function's doc comment, a SKILL.md or the plan but not implemented in the code (or the reverse);
   - a tool that would crash on real data (an e-mail with several parts, an empty file, a missing data/ folder, a key missing from .env) or prints more than ~32KB (trim the fields, save the full data to a file);
   - steps in AGENT.md or a skill that never use a function, tool or skill the plan needs;
   - placeholder or hard-coded content where real content is needed.
   - a function or tool that interprets people's text (splitting, keyword matching, regex for intent): that is the model's job;
   - for every script and function, compare the actions it offers with every action the plan or the skills use (send, reply, check ...): each one must exist in the code.

Reply with only a JSON list, one object per gap, most important first:
[{"file": "skills/mail/scripts/gmail.py", "gap": "no 'send' action: first emails cannot be sent", "fix": "add action send: to, subject, body; apply TEST_RECIPIENT"}]
Reply [] if every requirement is implemented.
