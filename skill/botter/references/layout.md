# The project folder

```
<agent>/
  AGENT.md                  the system prompt: who the agent is, how it works, firm rules
  config.json               name, version, description, skills, requires, ... (always write it)
  FLOW.lua                  OPTIONAL: flow(input) handles every job in code (batch agents); absent = chat loop
  functions/<name>.lua      OPTIONAL: Lua, always loaded; a function with a --- doc comment is a tool
  functions/tests/<n>.lua   tests: every test_* function runs at check/build; never embedded
  skills/<name>/SKILL.md    OPTIONAL: the agent's own playbooks (frontmatter: name = folder, description)
  skills/<name>/references/ documents SKILL.md points to
  skills/<name>/scripts/    programs the skill runs (#! scripts or static x86-64 binaries)
  skills/<name>/assets/     templates and files the skill copies into output
  skills/<name>/functions/  Lua tools that exist only while that skill is loaded
  agents/<name>/AGENT.md    OPTIONAL: a sub-agent (frontmatter: --- description: one sentence ---)
  evals/, data, anything else  never embedded
```

`botter build` writes `./<name>` (name from config.json). Embedded, read-only: AGENT.md, FLOW.lua,
config.json, functions/*.lua, skills/** (only the skills listed in config.json, without their tests),
agents/**, secrets.vault. Not embedded: tests, data files, hidden files, symlinks, everything else. So data
the agent reads at run time (a dataset) stays next to it in the working folder.

## config.json

```json
{
  "name": "ticket-triage",
  "display_name": "Ticket Triage",
  "version": "0.1.0",
  "description": "Reads support tickets from a CSV and writes a priority, team and reply draft for each.",
  "internet": true,
  "skills": [],
  "requires": [],
  "temperature": 0
}
```

- name: lowercase letters, digits, `- _ .` (1-64); it becomes the file name. display_name: as people see it.
- version: MAJOR.MINOR.PATCH, start at 0.1.0; bump PATCH for fixes, MINOR for new abilities.
- description: one sentence, what it does and for whom.
- internet: false only for an offline agent (shell, scripts and bot.http get no network).
- skills: the agent's own skills (folder names) to build in. A skill not listed is not embedded.
- requires: every program the agent needs on the machine (python3, sqlite3, git ...). Empty = runs anywhere.
- temperature: 0 for batch and anything scored (as repeatable as the provider allows). seed: optional integer.
- reasoning: optional "low" | "medium" | "high".
- mcp: MCP servers the agent uses: `{"docs": {"url": "https://host/mcp", "token": "DOCS_TOKEN"}}`, or a
  command `{"files": {"command": ["npx", "-y", "pkg"], "env": ["SOME_TOKEN"]}}` (put npx in requires).
  Their tools appear as `docs_search`; Lua calls `bot.mcp("docs", "search", {...})`.
- agents: remote A2A agents it may hand work to: `{"billing": {"url": "https://host", "token": "BILLING_TOKEN",
  "description": "Answers billing questions."}}`.
- A "token" is the NAME of a secret, never the secret: the value comes from `.env` in the working folder or
  the sealed vault.

## How a built agent runs

- `./<name>`: chat in the terminal. First run asks for an OpenAI-compatible provider, key and model; the
  key is kept in the kernel keyring (RAM) until reboot. Nothing is written to disk.
- `./<name> --run "task"`: one job, prints only the reply. `./<name> --run < jobs.txt`: one job per line,
  one JSON line out per job (`{"input","reply"}`). Token use on stderr. Exit 1 if a job failed.
- `./<name> --call solve_case < cases.jsonl`: each JSON line to a Lua function, one JSON line out
  (`{"input","result"}` or `"error"`). This is what `botter eval` uses.
- `./<name> --mcp` (MCP server over stdio, one tool `ask`), `--mcp 8080` (HTTP), `--a2a 8080`, `--acp`.
- `./<name> --about`: name, version, requires. `--version`.
- Without a person (batch, MCP, A2A) the model connection comes from the keyring or from `.env`:
  `BOTTER_BASE_URL`, `BOTTER_API_KEY`, `BOTTER_MODEL`. Risky actions are refused.
- Sandbox: free inside the working folder; writes elsewhere and destructive commands need approval (refused
  in batch). The kernel enforces it (Landlock).

## Built-in tools the model always has

fs_list, fs_read (a big CSV comes back as a profile: rows, columns, types, first rows), fs_write, fs_edit,
image_view, shell_exec (sandboxed), recall (older tool results are archived; recall brings one back),
get_time, skill (loads one of the agent's skills), delegate (sub-agents and remote agents, when any exist).
Your function names must not clash with these.
