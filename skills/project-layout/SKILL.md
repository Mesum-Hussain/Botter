---
name: project-layout
description: Reference for the folder layout and file formats of an agent project, and what ends up inside the .bot. Read it when unsure where a file goes.
---

# Agent project layout

The project root is the folder the user started botter in. The built file is named after that folder: <root folder name>.bot

```
<agent>/
  agent.md             persona and behaviour = the agent's system prompt
  config.json          how to build and run the agent: name, version, internet, skills, tools ... (always write it; see "config.json" below)
  FLOW.fwl              OPTIONAL (recommended for multi-step agents): the session flow in FWL: plain sentences inside IF / FOR EACH / WHILE (no Markdown), checked by the build, run by botcore at start-up (see write-flow)
  skills/<name>/SKILL.md   task playbooks: how and when to use tools
  secrets.vault        OPTIONAL: the agent's keys, encrypted with the user's password. Written only by the vault_seal tool (see build-agent)
  agents/<name>/agent.md   OPTIONAL sub-agents: a frontmatter (--- description: one sentence ---) and then that sub-agent's persona
  tools/                OPTIONAL
    bin/<name>          the runnable tool: native x86-64 ELF, or a script starting with #!
    doc/<name>.json     REQUIRED for every bin: descriptor (description, parameters, timeout_s, network)
    doc/<name>.md       optional prose docs: examples, caveats, read via vfs_read
    src/<name>/         source of COMPILED tools. Never embedded, never deleted.
  artifacts/            build-time inputs (not embedded), e.g. a cloned repo you wrap into tools
```

## What is embedded in the .bot (read-only)
agent.md, config.json, FLOW.fwl, secrets.vault, skills/** , agents/** , tools/doc/** , tools/bin/* (flat, no sub-folders); when config.json lists "skills" / "tools", only those

## What is NOT embedded
tools/src, artifacts, hidden files (names starting with a dot), symlinks, everything else (README, .git, notes, test data).

## Rules
- agent.md is the system prompt. If missing, the agent runs with a generic default prompt (build warns).
- There is no manifest: at start-up the runtime lists the skills (path + frontmatter description) and tool docs in the system prompt; tool schemas come from tools/doc/*.json. If FLOW.fwl is present, botcore runs it when the agent starts (the FWL interpreter) and gives the agent one step at a time. So skill and tool descriptions must be accurate and short.
- Sub-agents (agents/<name>/agent.md) are for a job that needs its own persona and a fresh conversation (a researcher, a reviewer, a writer). A sub-agent has the agent's tools and model, sees only the task it is handed, cannot ask the user anything and cannot delegate further; only its final reply comes back. A FLOW.fwl sentence hands it work by naming it (@name); the model can also do so itself with the delegate tool. For a recurring task the agent does itself, write a skill instead: it is cheaper.
- A built agent also answers over two standard protocols, with nothing to add to the project: ./<name>.bot --acp inside an editor that speaks the Agent Client Protocol (Zed and others start it themselves), and ./<name>.bot --a2a 8080 to be called by other agents (A2A; put A2A_TOKEN=<secret> in .env, required when it listens beyond this machine: --a2a 0.0.0.0:8080). There is no user to type there: the model connection is the saved one or BOTTER_BASE_URL / BOTTER_API_KEY / BOTTER_MODEL in .env, a flow starts with the first message, and risky actions are asked through the editor (ACP) or refused (A2A).
- Interpreted tools need no tools/src: the script in tools/bin is the source.
- Tool name = file name in tools/bin = [A-Za-z0-9_-]{1,64}. It must not clash with a built-in tool.
- Skill folder names: lowercase letters, digits and dashes, e.g. summarize-logs.
- Average users have no tools folder at all. That is fine: an agent with only agent.md and skills is a complete agent.

## Every agent also has these built-in tools (no work needed)
fs_list, fs_read, fs_write (files in the working directory), shell_exec (sandboxed; online unless the agent is offline), cron_set / cron_list / cron_delete (schedules that live only while the agent runs), get_time, and vfs_list / vfs_read (the embedded read-only files above, only present if a pack exists).

## Runtime facts the agent author should know
- The agent connects to an OpenAI-compatible LLM endpoint chosen by the user at every start (provider, API key, model). Nothing is saved to disk: no config, no logs, no memory between runs.
- The agent is online by default: its shell and tools can reach the internet and localhost services.
- With config.json "internet": false it is offline except for the LLM connection: its shell and tools cannot reach the network (not even localhost services), except tools whose descriptor has "network": true, and only after the user allows it at startup.
- For a completely offline agent, the user can also pick "Ollama (local)" as provider, so not even the LLM call leaves the machine.
- It is free inside its working directory and asks the user before touching anything outside it or doing something destructive. The kernel enforces this for shell commands and tools (Landlock): writes only in the working directory, /tmp and package caches; the rest of $HOME is hidden unless the user approves a command that names the path.

## config.json
How Botter builds and runs the agent. Every agent has one, and it holds only this (nothing about who made it or which LLM it uses: the person running the agent chooses the provider). The build checks it: bad JSON, wrong types, a bad name or version, or a listed skill or tool that does not exist are errors.

{
  "name": "lead-outreach",
  "display_name": "Lead Outreach",
  "version": "0.1.0",
  "description": "Finds local businesses that need the user's service and pitches each one, with approval.",
  "internet": true,
  "skills": ["research-lead", "pitching"],
  "tools": ["scrape_leads", "send_email"],
  "requires": ["python3"]
}

- name: lowercase letters, digits, - _ . (1-64); the project folder's name. display_name: the spelling people see (shown at start-up).
- version: MAJOR.MINOR.PATCH. A new agent starts at "0.1.0". When you change an existing agent, bump it before building: PATCH for fixes, MINOR for new abilities, MAJOR when it behaves differently for its users.
- description: one sentence, what it does and for whom (shown at start-up; the agent knows it).
- internet: false only when the user asks for an offline agent (see write-tool, section Network). Default true.
- skills, tools: the skills (folder names) and tools (tools/bin names) compiled into the agent. List every one you wrote; a skill or tool left off the list stays in the folder but is not built in.
- requires: every program the agent's tools or shell steps need on the machine (python3, node, git, ffmpeg ...). The build warns when a script tool's interpreter is missing from it; the agent says at start-up which ones are not installed.
- builder: true only for agents that build agents (Botter itself).
- mcp: OPTIONAL, only when the user names an MCP server to use: {"docs": {"url": "https://host/mcp", "token": "DOCS_TOKEN"}}. Its tools join the agent's own as <server>_<tool> (docs_search), for the model, and named $docs_search in a FLOW.fwl sentence. Only servers reached by URL: botcore makes one HTTPS request per call and starts nothing on the machine, so never configure a "command" to launch.
- agents: OPTIONAL, other people's agents this one may hand work to (A2A): {"billing": {"url": "https://host", "token": "BILLING_TOKEN", "description": "Answers billing questions."}}. A FLOW.fwl sentence names it @billing, the model uses the delegate tool, exactly like a sub-agent.
- "token" in mcp / agents is a NAME, never the secret: the value comes from the agent's sealed vault (vault_seal asks the user for it) or, without a vault, from a line of that name in the .env file of the working directory (DOCS_TOKEN=...): config.json is embedded in the .bot for anyone to read. Tell the user which lines to put in .env. Leave "token" out for a server that needs none.
