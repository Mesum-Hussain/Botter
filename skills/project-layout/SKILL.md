---
name: project-layout
description: Reference for the folder layout and file formats of an agent project, and what ends up inside the .bot. Read it when unsure where a file goes.
---

# Agent project layout

The project root is the folder the user started botter in. The built file is named after that folder: <root folder name>.bot

```
<agent>/
  AGENT.md             persona and behaviour = the agent's system prompt
  config.json          how to build and run the agent: name, version, internet, skills ... (always write it; see "config.json" below)
  FLOW.lua             OPTIONAL: code-driven agents: flow(input) gets every job and runs the procedure (functions for exact steps, bot.llm for judgement); see write-functions
  functions/<name>.lua  OPTIONAL: agent-wide exact work in Lua (calculations, dates, data files, checks, HTTP calls), always loaded; each documented function is a tool (see write-functions)
  functions/tests/      tests for the functions, run by the build, never embedded
  skills/<name>/        a skill: a playbook plus what it needs
    SKILL.md            frontmatter (name = folder name, description = what it does and when to use it), then the procedure
    references/         OPTIONAL: longer documents SKILL.md tells the agent to read when needed
    scripts/            OPTIONAL: programs the skill runs (shell, Python, Node, native); see write-scripts
    assets/             OPTIONAL: templates and files copied into output
    functions/<name>.lua  OPTIONAL: Lua tools that exist only while the skill is loaded
  secrets.vault        OPTIONAL: the agent's keys, encrypted with the user's password. Written only by vault_seal (see build-agent)
  agents/<name>/AGENT.md   OPTIONAL sub-agents: a frontmatter (--- description: one sentence ---) and then that sub-agent's persona
  evals/                OPTIONAL: test cases for ./<name>.bot --eval, never embedded
  artifacts/            build-time inputs (not embedded), e.g. a cloned repo you wrap
```

## What is embedded in the .bot (read-only)
AGENT.md, FLOW.lua, config.json, secrets.vault, functions/*.lua, skills/** (without functions/tests), agents/**; when config.json lists "skills", only those skills

## What is NOT embedded
functions/tests, skills/*/functions/tests, evals, artifacts, hidden files (names starting with a dot), symlinks, everything else (README, .git, notes, test data).

## Rules
- AGENT.md is the system prompt. If missing, the agent runs with a generic default prompt (build warns). The agent's model drives every conversation from it: how the agent works, step by step, goes in AGENT.md (or in a skill for a longer procedure).
- The old layout stops the build with a message saying where things go: agent.md (now AGENT.md), tools/bin and tools/doc (programs are skill scripts now), FLOW.fwl.
- There is no manifest: at start-up the runtime lists the skills (name + frontmatter description) in the system prompt; the model loads one with the skill tool, which gives it SKILL.md, the real path of the skill's files, and the skill's Lua tools. Tool schemas come from the functions' doc comments. So skill and function descriptions must be accurate and short.
- What the whole agent depends on (its core functions, a hook such as on_turn_end, access to its data store) goes in the top-level functions/, always loaded. A skill is an on-demand capability. Keep the top-level documented functions few: each is a tool the model sees on every request.
- The build checks every skill: name = folder, a description under 1024 characters, links in SKILL.md that resolve, references/ files that something points to, no placeholder words ("TODO", "FIXME", "TBD"), SKILL.md over 500 lines is a warning.
- Sub-agents (agents/<name>/AGENT.md) are for a job that needs its own persona and a fresh conversation (a researcher, a reviewer, a writer). A sub-agent has the agent's tools and model, sees only the task it is handed, cannot ask the user anything and cannot delegate further; only its final reply comes back. The model hands it work with the delegate tool; a function can with bot.agent("name", task). For a recurring task the agent does itself, write a skill instead: it is cheaper.
- A built agent also answers over three standard protocols, with nothing to add to the project: ./<name>.bot --mcp as an MCP server with one tool, ask, for any MCP client that starts a command (a coding agent such as Claude Code or opencode; --mcp 8080 serves it over HTTP, with MCP_TOKEN as the token); ./<name>.bot --acp inside an editor that speaks the Agent Client Protocol (Zed and others start it themselves), and ./<name>.bot --a2a 8080 to be called by other agents (A2A; put A2A_TOKEN=<secret> in .env, required when it listens beyond this machine: --a2a 0.0.0.0:8080). There is no user to type there: the model connection is the saved one or BOTTER_BASE_URL / BOTTER_API_KEY / BOTTER_MODEL in .env, and risky actions are asked through the editor (ACP) or refused (A2A, MCP).
- Batch mode: ./<name>.bot --run "task" does one task and prints only the answer; ./<name>.bot --run < tasks.txt does one task per line, each in a fresh conversation, and prints one JSON line per task ({"input", "reply"}); token usage goes to stderr at the end. Nobody is there to ask: risky actions are refused. For a job over many records (a CSV), write AGENT.md so one line names one record, and let functions read and write the files.
- Function names: [A-Za-z0-9_-]{1,64}, never a built-in tool's name.
- Skill folder names: lowercase letters, digits and dashes, e.g. summarize-logs.
- An agent with only AGENT.md and config.json is a complete agent.

## Every agent also has these built-in tools (no work needed)
fs_list, fs_read, fs_write, fs_edit (files in the working directory; fs_read of a big CSV returns a profile: rows, columns, types, first rows), image_view (shows a PNG/JPEG/GIF/WEBP to the model: scans, receipts, screenshots), shell_exec (sandboxed; online unless the agent is offline), note and recall (working memory: older tool results are archived as the conversation grows), cron_set / cron_list / cron_delete (schedules that live only while the agent runs), get_time, and skill (loads one of the agent's skills; only when it has skills).

## Runtime facts the agent author should know
- The agent connects to an OpenAI-compatible LLM endpoint chosen by the user at every start (provider, API key, model). Nothing is saved to disk: no config, no logs, no memory between runs.
- The agent is online by default: its shell and tools can reach the internet and localhost services.
- With config.json "internet": false it is offline except for the LLM connection: its shell and scripts cannot reach the network (not even localhost services), and bot.http refuses.
- For a completely offline agent, the user can also pick "Ollama (local)" as provider, so not even the LLM call leaves the machine.
- It is free inside its working directory and asks the user before touching anything outside it or doing something destructive. The kernel enforces this for shell commands and tools (Landlock): writes only in the working directory, /tmp and package caches; the rest of $HOME is hidden unless the user approves a command that names the path.

## config.json
How Botter builds and runs the agent. Every agent has one, and it holds only this (nothing about who made it or which LLM it uses: the person running the agent chooses the provider). The build checks it: bad JSON, wrong types, a bad name or version, or a listed skill that does not exist are errors.

{
  "name": "lead-outreach",
  "display_name": "Lead Outreach",
  "version": "0.1.0",
  "description": "Finds local businesses that need the user's service and pitches each one, with approval.",
  "internet": true,
  "skills": ["research-lead", "pitching"],
  "requires": ["python3"]
}

- name: lowercase letters, digits, - _ . (1-64); the project folder's name. display_name: the spelling people see (shown at start-up).
- version: MAJOR.MINOR.PATCH. A new agent starts at "0.1.0". When you change an existing agent, bump it before building: PATCH for fixes, MINOR for new abilities, MAJOR when it behaves differently for its users.
- description: one sentence, what it does and for whom (shown at start-up; the agent knows it).
- internet: false only when the user asks for an offline agent (see write-scripts, section Network). Default true.
- skills: the skills (folder names) compiled into the agent. List every one you wrote; a skill left off the list stays in the folder but is not built in.
- requires: every program the agent's scripts or shell steps need on the machine (python3, node, git, ffmpeg ...). The build warns when a script's interpreter is missing from it; at start-up the agent checks the machine and tells both the user and its own model which ones are not installed.
- builder: true only for agents that build agents (Botter itself).
- reasoning: OPTIONAL "low" | "medium" | "high": how much the model thinks before answering (sent as reasoning_effort; dropped if the provider refuses it).
- temperature, seed: OPTIONAL, sent with every model request. "temperature": 0 makes answers as repeatable as the provider allows (use it for batch jobs and anything scored); "seed" is a whole number (dropped if the provider refuses it).
- mcp: OPTIONAL, only when the user names an MCP server to use: {"docs": {"url": "https://host/mcp", "token": "DOCS_TOKEN"}}. Its tools join the agent's own as <server>_<tool> (docs_search) for the model; a function calls one with bot.mcp("docs", "search", {...}). By URL, botcore makes one HTTPS request per call and starts nothing. A server that is run as a program is given as a command instead: {"files": {"command": ["npx", "-y", "@modelcontextprotocol/server-filesystem", "."], "env": ["SOME_TOKEN"]}}. botcore starts it with the agent, inside the tools' sandbox (it sees the working folder, not the rest of the home directory), and stops it with the agent; put its program (npx, uvx, python3 ...) in "requires", and in "env" the NAMES of the secrets it needs. Prefer a URL when the server offers one.
- agents: OPTIONAL, other people's agents this one may hand work to (A2A): {"billing": {"url": "https://host", "token": "BILLING_TOKEN", "description": "Answers billing questions."}}. The model uses the delegate tool, exactly like a sub-agent; a function uses bot.agent("billing", task).
- "token" in mcp / agents is a NAME, never the secret: the value comes from the agent's sealed vault (vault_seal asks the user for it) or, without a vault, from a line of that name in the .env file of the working directory (DOCS_TOKEN=...): config.json is embedded in the .bot for anyone to read. Tell the user which lines to put in .env. Leave "token" out for a server that needs none.
