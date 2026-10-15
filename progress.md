# BOTCORE + BOTTER: SPEC + STATUS (AI-consumed; terse by design)

Updated: 2026-10-15 | Version 0.2.0 (released; binary refreshed in place) | Lang: C (gnu11) | Target: x86_64 Linux (dev: Fedora 44, kernel 7.2, SELinux enforcing, Landlock + userns on, musl-gcc)
State: sub-agents, MCP (both directions, URL + command), A2A, ACP and the sealed vault are done and pushed. DONE 2026-10-15 (not yet committed): Lua functions replaced FWL (§3), batch mode --run, image_view, token usage, temperature/seed. NEXT: the HackerRank Orchestrate practice run (§9).
Legend: [D] = owner decided

## 1. PRODUCT [D]
- Pitch: the Markdown-first agent framework; every agent ships as one portable executable. Not "no framework".
- botcore = single-binary agent runtime. Agent `<name>.bot` = static botcore ELF + appended read-only pack (text files + tool executables).
- Botter (binary `botter`) = the agent that builds agents: `mkdir x && cd x && botter` -> designs with the user, writes the files, builds `./<folder>.bot`. Botter is itself an agent project (repo root).
- packer = the separate program that appends a project to botcore. Only Botter carries it. [D] Do NOT merge it into botcore: ordinary agents must not be able to build agents.

## 2. AGENT PROJECT LAYOUT [D]
```
agent.md                 persona -> system prompt
functions/<n>.lua        OPTIONAL: exact work in Lua (§3); functions/tests/<n>.lua run at build time, never embedded
config.json              name, display_name, version, description, internet, skills, tools, requires, builder, mcp, agents
skills/<n>/SKILL.md      playbooks (frontmatter name + description)
agents/<n>/agent.md      OPTIONAL sub-agents (frontmatter description, then persona)
tools/bin/<name>         ELF x86-64 or #! script; tools/doc/<name>.json REQUIRED {description, parameters, timeout_s<=600, network, example}
tools/doc/<name>.md      optional; tools/src/ = compiled-tool source, never embedded
secrets.vault            OPTIONAL: keys encrypted with a password, written only by vault_seal (§5)
artifacts/               build inputs, never embedded
```
Embedded: agent.md config.json secrets.vault functions/*.lua skills/** agents/** tools/doc/** tools/bin/* (only listed skills/tools when config.json lists them). Output named after the folder.
config.json: `requires` = programs on PATH (build warns for unlisted #! interpreters and MCP commands; at start botcore tells the user AND the model which are missing). `builder: true` = Botter only (vault_seal). `temperature`, `seed`: sent with every model request. A FLOW.fwl / FLOW.md / SQNC.md in a project is a build error that says where its content goes. `mcp`, `agents`: §4.

## 3. FUNCTIONS (Lua) [D 2026-10-15; replaces FWL]
Why [D]: Lua is deterministic and an existing standard for embedded scripting; no home-made language to maintain. The model drives the conversation from agent.md + skills (the harness loop, chat.c); there is no flow file and no second loop.
- Three tiers, chosen in this order [D]: 1 Lua `functions/*.lua` (inside botcore; glue, processing, validation, files, later HTTP/MCP calls; needs nothing on the machine) -> 2 shell scripts in tools/bin (programs such as sqlite3/ffmpeg, listed in "requires") -> 3 Python/Node in tools/bin (last resort, interpreter in "requires").
- A function is a tool when a `---` doc comment sits directly above `function name(...)` at column 0. `---@param name type desc` (optional: `name?` or `type?`; types string number integer boolean table any, `T[]`, `"a"|"b"` = enum), `---@return type desc`. Undocumented or `local` functions are helpers. Top-level functions end with `end` at column 0.
- Runtime: Lua 5.4.7 vendored (lib/lua, MIT). Libraries: base (no dofile/loadfile/load; print -> stderr), string, table, math, utf8; no io/os/package/debug/coroutine. All functions/*.lua share one state. Limits per call: instructions, memory (allocator cap), 60 s, Ctrl-C.
- `bot` module: bot.json.encode/decode, bot.csv.parse(text) -> rows keyed by header / bot.csv.encode(rows, columns), bot.date.parse/format/add_days/diff_days/weekday (pure), bot.today() (clock), bot.fs.read/list/exists/write/append: paths inside the working directory only (no prompt; outside = error), write refused in Plan mode. bot.http.get/post -> {status, body, json} (https, http only to loopback, refused offline), bot.secret(name) (vault/.env), bot.shell(cmd) -> output, exit code (= shell_exec: classifier, permission, sandbox), bot.mcp(server, tool, args), bot.agent(name, task) (= delegate). Calls nest (bot.agent -> sub-agent -> a function): the stack and time limit are per call.
- Arguments arrive as JSON -> Lua values in @param order; a string result goes back as is, tables/numbers/booleans as JSON, nil as "(no result)", an error as "ERROR: functions/x.lua:LINE: msg".
- Pure = uses none of bot.fs/today (and later http/shell/mcp/agent); packer reports each function as pure or what it uses.
- Tests: functions/tests/*.lua (not embedded). The packer loads the functions and runs every global test_* function in each test file; a failure stops the build. In tests bot.fs reads relative to the project folder.
- packer: functions/*.lua embedded (not tests/); syntax errors, duplicate names, names that clash with built-in tools, and bad annotations are build errors.
- Batch mode `--run`: `--run "task"` = one task, reply on stdout; `--run` alone = one task per stdin line, each in a fresh conversation, one JSON line out per task {"input","reply"} (reply null + "error" on failure). Nobody to ask: risky actions refused, model connection from keyring/vault/.env. Token usage summary on stderr at the end.
- config.json "temperature", "seed": sent with every model request (seed dropped if the endpoint refuses it). Token usage counted per model (usage from responses; streaming asks include_usage).
- image_view {path}: shows a PNG/JPEG/WEBP/GIF from the working directory to the model for one turn (attached as a user message after the tool results; replaced by a text note when the turn ends).
- FWL is gone (fwl.c, fwl_run.c, tool_review.c/fwl_review, chat_ask, packer checks, write-flow skill, FWL tests, TUI highlighter). Botter's session procedure moved from FLOW.fwl into agent.md ("How a session goes"); skills teach functions (write-functions) and the three tiers.
- Measured: runtime 542 KB (was 403 KB with FWL), minimal agent 532 KB; start 0.45 ms either way without functions, 0.9 ms with; RSS ~0.44 MB.

## 4. PROTOCOLS [D 2026-10-11]
All in botcore, in C; no bridge. Tested against the peers in tests/ plus the real cases noted.
- MCP client (remote.c): config.json `"mcp": {"n": {"url", "token"}}` = Streamable HTTP, stateless 2026-07-28 first, session handshake (2025-06-18) when the server wants it; or `{"command": [...], "env": [NAMES]}` = server started by the agent over stdio, inside the tools' sandbox, killed with the agent. Tools appear as `<server>_<tool>`. Tools only (no resources/prompts; server-to-client requests declined). Bearer token only (no OAuth). Real test: npx @modelcontextprotocol/server-filesystem, 14 tools, used by the model.
- MCP server (front.c): `--mcp` (stdio; the client starts the agent) or `--mcp [HOST:]PORT` (HTTP, MCP_TOKEN). One tool, `ask` {message, new_conversation}. No cancel handling. Not yet tried from Claude Code / opencode.
- A2A client (remote.c): config.json `"agents": {"n": {"url", "token", "description"}}`; agent card, JSON-RPC 1.0 (SendMessage/GetTask) and 0.3; text only; polls a working task.
- A2A server (front.c): `--a2a [HOST:]PORT`; card at /.well-known/agent-card.json; SendMessage / message/send answered with a Message. No tasks, no streaming, plain HTTP, A2A_TOKEN bearer (required off-loopback).
- ACP agent (front.c): `--acp`; initialize, session/new, session/prompt, session/update (chunks, tool_call, tool_call_update), session/request_permission, session/cancel (SIGIO).
- Remote modes (--acp/--a2a/--mcp/--run): no /commands from the caller; one conversation in RAM (a new session/context sends an internal /new); risky actions asked through ACP, refused in A2A/MCP/--run; model connection = keyring, sealed vault, or .env BOTTER_BASE_URL / BOTTER_API_KEY / BOTTER_MODEL.
- Sub-agents: agents/<n>/agent.md; own chat in the same process, agent's tools and model, cannot ask the user or delegate. Reached by the model's `delegate` tool or a function's bot.agent. Not parallel.

## 5. KEYS + UX [D]
- Default: provider -> key (no echo) -> model -> validate -> "Connected". Key in the kernel user keyring until reboot (keystore.c; BOTCORE_NO_KEYRING=1 off) and mlock'd in the process; never argv/env. /provider, /forget, /new, /plan, /build, /run, /details, /help.
- Sealed vault [D opt-in]: Botter asks before building (agent.md step 5); `vault_seal` (in main.c, never through the model) asks for the LLM key (or reuses Botter's connection), each MCP/agent token by name with its purpose, the A2A serving token, then a password twice (>= 10 chars). vault.c: PBKDF2-HMAC-SHA256 600k + ChaCha20-Poly1305 -> secrets.vault. A sealed agent asks only for the password; headless modes use the key remembered in the keyring after one terminal login. env_get() reads the vault first, then ./.env. Tool credentials in a tool's own .env are NOT in the vault.
- Presets: Gemini (default gemini-3.1-flash-lite), OpenAI, OpenRouter, Groq, Ollama, Custom URL.
- Botter TUI only for botter; agents use the plain REPL. Users see status words, not tool names. ASK choices render as a numbered list everywhere (no picker in the TUI yet).

## 6. SAFETY MODEL
Free inside ctx (cwd at launch); ask before outside-ctx access or destructive ops. Model output and all values are untrusted data.
- L1 path guard (fs_*). L2 shell classifier (soft): rm/dd/kill/sudo/overwrites/outside paths/git push ask; offline adds a network-command deny.
- L3 Landlock for every child (shell, tools, command-started MCP servers): RW ctx, /tmp, /var/tmp, /dev, ~/.cache, ~/.npm; RO+exec system dirs and $PATH dirs; rest of $HOME hidden. Fail closed. L4 rlimits. L5 `"internet": false` -> children in an empty netns, else seccomp.
- Plan mode: ctx read-only (kernel), fs_write/cron_set refused.
- Zero disk persistence by the runtime, except what the user opts into: secrets.vault (encrypted) or a .env file.

## 7. CODE MAP
botcore `tools/src/botcore/src` (~11K LOC): main.c (connect, system prompt, sub-agents, delegate, vault_seal), chat.c (usage, sampling, images), http.c (own HTTPS via vendored BearSSL; http_next_headers / http_last_header for protocols), remote.c (.env, MCP client, A2A client), front.c (TUI protocol; --acp, --a2a, --mcp, --run), botlua.c (Lua state, bot module, doc-comment parser; shared with packer), functions.c (functions as tools, bot.fs/http/shell glue), vault.c, keystore.c, guard.c, tool_fs.c (+ image_view), tool_shell.c (sandbox), tool_ext.c, tools.c, cron.c, vfs.c, term.c. lib/: cjson, bearssl, ca, lua.
`tools/src/packer/packer.c` (build | check | list; messages "packer: ..."), `tools/src/agent_tools` (agent_build, agent_inspect, tool_test: one ELF that includes the packer), `tui/`.
Build: `make` -> ./botter (TUI + botcore + Botter's pack); `make test | dist | clean`. Version from config.json. After botcore changes re-run `make`; a version change needs the sources touched (objects do not depend on it).
Sizes (KB of 1024): agent 532, botter 1043 (agent_tools grew to 350 KB: it carries Lua for the build checks). Start 0.4 ms, build ~2 ms, 0.4 MB.
Tests: `make test` = tests/run.sh, 213 local (needs python3; tmux for UI tests). Stand-ins: stub_llm.py (model: one tool call then "done", or a fixed text; reports usage, records sampling and image parts), remote_servers.py (MCP stateless + session, A2A peer), mcp_stdio_server.py, acp_client.py, http_servers.py. BOTTER_TEST_NET=1 adds real providers. Real-model checks are manual (owner's saved Gemini key).

## 8. ISSUES
- TLS 1.2 only (BearSSL); IP-literal HTTPS hosts fail the name check; DNS not interruptible; no custom CA.
- Shell classifier is bypassable (`python -c`); the hard layers are Landlock and the offline net cut.
- memfd exec may be blocked by confined SELinux / vm.memfd_noexec.
- Functions: the pure/uses label is a text scan of each function's own body (helpers it calls are not followed). Lua's bot.fs stays in the working directory without asking (functions are the author's code; the model only picks arguments).
- Botter's session is now model-driven (agent.md), not a flow: not yet tried with a real model end to end.
- Vault: strength = password; anyone with the password has the keys.
- Windows = WSL (untested). macOS: not planned now [D].

## 9. NEXT [D 2026-10-15]
Step 1 DONE (for HackerRank Orchestrate, Oct 24 2026 18:00 IST; past editions are CSV-in/CSV-out batch jobs scored on accuracy, code read by an AI judge, log.txt transcript, usage_report.md with tokens and cost): Lua runtime + functions as tools + bot.fs/json/csv/date + pure tests, --run, usage counting, temperature/seed, image_view.
Step 2 DONE: bot.http, bot.shell, bot.mcp, bot.agent, bot.secret; three-tier skills; Botter's FLOW.fwl ported to agent.md; FWL removed.
Step 3: practice on the September 2026 problem ("Buy or Wait?", github.com/interviewstreet/hackerrank-orchestrate-september26) with a real model.
Later: ACP/A2A full compatibility (drive ACP agents by command, session/load, A2A streaming and tasks, real peers), --mcp exposing Lua functions as tools, MCP OAuth, ASK picker in the TUI, parallel sub-agents.

## 10. AGENT RULES
- Update THIS file each session, keep terse. Never log keys.
- GIT: ~/Code/botter -> github.com/Mesum-Hussain/Botter, branch main. Commit/push when the owner asks. No secrets.
- Versions [D]: no new version number without asking; patch bumps; refresh the 0.2.0 release in place (move the tag, re-upload assets) when asked to publish.
- After code changes run `graphify update .` and `make test`.
- Ignored: /botter, /artifacts/, /tools/bin/, agent_tools binary, tui binaries, /graphify-out/, /pitch/, /dist/.

## 11. LICENSING
Own C + cJSON (MIT) + BearSSL (MIT) + Lua 5.4.7 (MIT, lib/lua/LICENSE) + Mozilla CA data (MPL-2.0, unmodified; notice in THIRD_PARTY.md). LICENSE MIT. Release static = musl.
