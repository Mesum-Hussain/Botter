# Writing AGENT.md and the agent's own skills

## AGENT.md

The system prompt, read on every request: short and firm, second person, plain text.

```
You are <name>, <one-line role>.

Purpose
- <what you do, for whom, 2-4 lines>

How a session goes
1. <first step, naming the tool, function or skill it uses>
2. <next step; what to check before going on>

Rules
- <hard limits: what you never do>
- <when you do not know: say so, do not guess>
```

- One role per agent. Two jobs: two agents, or one becomes a skill.
- Name tools and functions exactly. Exact rules (limits, sums, dates) are functions; AGENT.md says when to
  call them.
- Say what the agent cannot do: no memory between runs unless it writes files; no internet if offline.
- No secrets: AGENT.md is embedded in readable form.
- For a FLOW.lua batch agent, AGENT.md is still the persona for chat and for any bot.worker; keep it short.

## The agent's own skills (skills/<name>/SKILL.md)

A playbook for one recurring task. The agent sees only the `description` until it loads the skill, so the
description says what it does AND when to use it.

```
---
name: <same as the folder>
description: <what it does and when to use it, one or two sentences>
---

# <Title>

Steps
1. <first concrete action, naming the tool and its arguments>
2. <next>
3. <how to check the result>

Notes
- <formats, pitfalls, an example of good output>
```

- Under ~60 lines; longer material in references/ files the steps name by path.
- Every file in references/ must be named by SKILL.md (or by a file it names); links must resolve; no
  "TODO"/"TBD"/"FIXME" text. `botter check` enforces this.
- List the skill in config.json `"skills"` or it is not built in.

## Sub-agents (agents/<name>/AGENT.md)

For a job that needs its own persona and a fresh conversation (a researcher, a critic). Frontmatter
`--- description: one sentence ---`, then its persona. It has the agent's tools, sees only the task it is
given, cannot ask the person, cannot delegate. The model hands work over with the delegate tool; Lua with
`bot.agent("name", task)`. For a task the agent does itself, a skill is cheaper.
