# BOTCORE + BOTTER: SPEC + STATUS (AI-consumed; terse by design)

Updated: 2026-10-11 | Version 0.2.0 (released; binary refreshed in place) | Lang: C (gnu11) | Target: x86_64 Linux (dev: Fedora 44, kernel 7.2, SELinux enforcing, Landlock + userns on, musl-gcc)
State: FWL redesign, sub-agents, MCP (both directions, URL + command), A2A, ACP and the sealed vault are done and pushed. NEXT: full compatibility for ACP and A2A (§9).
Legend: [D] = owner decided

## 1. PRODUCT [D]
- Pitch: the Markdown-first agent framework; every agent ships as one portable executable. Not "no framework".
- botcore = single-binary agent runtime. Agent `<name>.bot` = static botcore ELF + appended read-only pack (text files + tool executables).
- Botter (binary `botter`) = the agent that builds agents: `mkdir x && cd x && botter` -> designs with the user, writes the files, builds `./<folder>.bot`. Botter is itself an agent project (repo root).
- packer = the separate program that appends a project to botcore. Only Botter carries it. [D] Do NOT merge it into botcore: ordinary agents must not be able to build agents.

## 2. AGENT PROJECT LAYOUT [D]
```
agent.md                 persona -> system prompt
FLOW.fwl                 OPTIONAL: what the agent does, step by step, in FWL (§3). Plain text, no Markdown fence.
config.json              name, display_name, version, description, internet, skills, tools, requires, builder, mcp, agents
skills/<n>/SKILL.md      playbooks (frontmatter name + description)
agents/<n>/agent.md      OPTIONAL sub-agents (frontmatter description, then persona)
tools/bin/<name>         ELF x86-64 or #! script; tools/doc/<name>.json REQUIRED {description, parameters, timeout_s<=600, network, example}
tools/doc/<name>.md      optional; tools/src/ = compiled-tool source, never embedded
secrets.vault            OPTIONAL: keys encrypted with a password, written only by vault_seal (§5)
artifacts/               build inputs, never embedded
```
Embedded: agent.md config.json FLOW.fwl secrets.vault skills/** agents/** tools/doc/** tools/bin/* (only listed skills/tools when config.json lists them). Output named after the folder.
config.json: `requires` = programs on PATH (build warns for unlisted #! interpreters and MCP commands; at start botcore tells the user AND the model which are missing). `builder: true` = Botter only (fwl_review, vault_seal). `mcp`, `agents`: §4.

## 3. FWL (Flow Writeup Language) [D 2026-10-11; was Owl, FLOW.md, ```owl]
- File `FLOW.fwl`. An old FLOW.md / SQNC.md in a project is a build error that names the new file. Extension settled after checking .fl/.flo/.flw/.fml: keep .fwl. Name FWL stays; a separate repo/spec only after a few more iterations [D].
- Goal [D]: certainty for plain-English instructions, NOT fewer model calls.
- Only these are CAPITALS: `ASK "q" [choice, choice] into {name}`, `IF c:` / `ELSE IF c:` / `ELSE:` (no ELIF), `FOR EACH {x} in {list}:`, `WHILE c, at most N times:`, `RETRY N times:`, `STEP n: NAME`, `RETURN ...`. A ':' line opens a block; indentation is the block (spaces only). Every other line is a sentence the model carries out. Any other all-caps first word is an error.
- Marks: `$tool`, `#skill`, `@agent` (sub-agent or config.json agent), `{value}` (`{a.b}`, `{a[0]}`, `{a.length}`). Nothing is imported. Marks inside "quotes" are text.
- Values: the first sentence that mentions a new {name} fills it (also one with no value at run time, or named "as {x}" / "call it {x}" / "into {x}"). One $tool (or one @agent) + one filled value -> the value is that tool's own result; else botcore asks the model ("[FWL] Work out the value of {x}"). "{x}" inside a quoted text is replaced by the value.
- Conditions: exact when both sides are values (`{x} is "yes"`, `is not`, `is empty`, `is not empty`, `contains`, `is greater/less than`, joined with and/or); anything else = one model call ("[FWL] Decide ...").
- Removed, each with an error that says what to write instead: EXECUTE, INVOKE, DELEGATE, SAVE, SET, IMPORT/LOAD, IN PARALLEL, END lines, THEN/DO, backtick names. No comments, no BREAK/CONTINUE.
- Code: fwl.c (parser + bind pass + sq_check; packer #includes it), fwl_run.c (interpreter), tool_review.c (fwl_review: model reviews the logic; builder only). Diagnostics `FLOW.fwl:LINE: error|warning: msg`; packer reports structure and name errors in one run.
- Verified with a real model (Gemini flash-lite): a test flow, and Botter building a working agent end to end.

## 4. PROTOCOLS [D 2026-10-11]
All in botcore, in C; no bridge. Tested against the peers in tests/ plus the real cases noted.
- MCP client (remote.c): config.json `"mcp": {"n": {"url", "token"}}` = Streamable HTTP, stateless 2026-07-28 first, session handshake (2025-06-18) when the server wants it; or `{"command": [...], "env": [NAMES]}` = server started by the agent over stdio, inside the tools' sandbox, killed with the agent. Tools appear as `<server>_<tool>`. Tools only (no resources/prompts; server-to-client requests declined). Bearer token only (no OAuth). Real test: npx @modelcontextprotocol/server-filesystem, 14 tools, used by the model.
- MCP server (front.c): `--mcp` (stdio; the client starts the agent) or `--mcp [HOST:]PORT` (HTTP, MCP_TOKEN). One tool, `ask` {message, new_conversation}. No cancel handling. Not yet tried from Claude Code / opencode.
- A2A client (remote.c): config.json `"agents": {"n": {"url", "token", "description"}}`; agent card, JSON-RPC 1.0 (SendMessage/GetTask) and 0.3; text only; polls a working task.
- A2A server (front.c): `--a2a [HOST:]PORT`; card at /.well-known/agent-card.json; SendMessage / message/send answered with a Message. No tasks, no streaming, plain HTTP, A2A_TOKEN bearer (required off-loopback).
- ACP agent (front.c): `--acp`; initialize, session/new, session/prompt, session/update (chunks, tool_call, tool_call_update), session/request_permission, session/cancel (SIGIO).
- Remote modes (--acp/--a2a/--mcp): no /commands from the caller; one conversation in RAM (a new session/context sends an internal /new); risky actions asked through ACP, refused in A2A/MCP; a flow starts with the first message; model connection = keyring, sealed vault, or .env BOTTER_BASE_URL / BOTTER_API_KEY / BOTTER_MODEL.
- Sub-agents: agents/<n>/agent.md; own chat in the same process, agent's tools and model, cannot ask the user or delegate. Reached by naming @n in a sentence, or by the model's `delegate` tool. Not parallel.

## 5. KEYS + UX [D]
- Default: provider -> key (no echo) -> model -> validate -> "Connected". Key in the kernel user keyring until reboot (keystore.c; BOTCORE_NO_KEYRING=1 off) and mlock'd in the process; never argv/env. /provider, /forget, /new, /plan, /build, /run, /details, /help.
- Sealed vault [D opt-in]: Botter's FLOW asks before building; `vault_seal` (in main.c, never through the model) asks for the LLM key (or reuses Botter's connection), each MCP/agent token by name with its purpose, the A2A serving token, then a password twice (>= 10 chars). vault.c: PBKDF2-HMAC-SHA256 600k + ChaCha20-Poly1305 -> secrets.vault. A sealed agent asks only for the password; headless modes use the key remembered in the keyring after one terminal login. env_get() reads the vault first, then ./.env. Tool credentials in a tool's own .env are NOT in the vault.
- Presets: Gemini (default gemini-3.1-flash-lite), OpenAI, OpenRouter, Groq, Ollama, Custom URL.
- Botter TUI only for botter; agents use the plain REPL. Users see status words, not tool names. ASK choices render as a numbered list everywhere (no picker in the TUI yet).

## 6. SAFETY MODEL
Free inside ctx (cwd at launch); ask before outside-ctx access or destructive ops. Model output and all values are untrusted data.
- L1 path guard (fs_*). L2 shell classifier (soft): rm/dd/kill/sudo/overwrites/outside paths/git push ask; offline adds a network-command deny.
- L3 Landlock for every child (shell, tools, command-started MCP servers): RW ctx, /tmp, /var/tmp, /dev, ~/.cache, ~/.npm; RO+exec system dirs and $PATH dirs; rest of $HOME hidden. Fail closed. L4 rlimits. L5 `"internet": false` -> children in an empty netns, else seccomp.
- Plan mode: ctx read-only (kernel), fs_write/cron_set refused.
- Zero disk persistence by the runtime, except what the user opts into: secrets.vault (encrypted) or a .env file.

## 7. CODE MAP
botcore `tools/src/botcore/src` (~11K LOC): main.c (connect, system prompt, flow glue, sub-agents, delegate, vault_seal), chat.c, http.c (own HTTPS via vendored BearSSL; http_next_headers / http_last_header for protocols), remote.c (.env, MCP client, A2A client), front.c (TUI protocol; --acp, --a2a, --mcp), fwl.c, fwl_run.c, tool_review.c, vault.c, keystore.c, guard.c, tool_fs.c, tool_shell.c (sandbox), tool_ext.c, tools.c, cron.c, vfs.c, term.c.
`tools/src/packer/packer.c` (build | check | list; messages "packer: ..."), `tools/src/agent_tools` (agent_build, agent_inspect, tool_test: one ELF that includes the packer), `tui/`.
Build: `make` -> ./botter (TUI + botcore + Botter's pack); `make test | dist | clean`. Version from config.json. After botcore changes re-run `make`; a version change needs the sources touched (objects do not depend on it).
Sizes (KB of 1024): agent 396, botter 715. Start 0.4 ms, build ~2 ms, 0.4 MB.
Tests: `make test` = tests/run.sh, 231 local (needs python3; tmux for UI tests). Stand-ins: stub_llm.py (model; acts on $tool/@agent in [FWL step] lines), remote_servers.py (MCP stateless + session, A2A peer), mcp_stdio_server.py, acp_client.py, http_servers.py. BOTTER_TEST_NET=1 adds real providers. Real-model checks are manual (owner's saved Gemini key).

## 8. ISSUES
- TLS 1.2 only (BearSSL); IP-literal HTTPS hosts fail the name check; DNS not interruptible; no custom CA.
- Shell classifier is bypassable (`python -c`); the hard layers are Landlock and the offline net cut.
- memfd exec may be blocked by confined SELinux / vm.memfd_noexec.
- FWL: a model can do more than its step asked (seen with flash-lite); steps are never skipped or reordered. A {typo} silently becomes a new value: only the "filled, never used" warning and fwl_review catch it.
- Vault: strength = password; anyone with the password has the keys.
- Windows = WSL (untested). macOS: not planned now [D].

## 9. NEXT [D 2026-10-11]: ACP and A2A to full compatibility
ACP
- Drive other ACP agents by command (`"agents": {"coder": {"command": ["opencode", "acp"]}}`), so a flow can hand work to Claude Code / opencode / any ACP agent. Reuse the command-started-server code in remote.c.
- Honour the MCP servers an editor passes in session/new (mcpServers).
- session/load, more than one session, images/audio prompts, modes.
- Test with a real editor (Zed).
A2A
- Streaming (SendStreamingMessage / message/stream), real tasks (GetTask, CancelTask) for long work, several contexts at once.
- File and data parts; HTTPS when serving (today: plain HTTP behind a proxy); REST and gRPC bindings only if a real peer needs them.
- Test with a real A2A agent / SDK.
Also open: try `--mcp` from Claude Code and opencode; MCP OAuth; an ASK picker in the TUI; parallel sub-agents; FWL comments.

## 10. AGENT RULES
- Update THIS file each session, keep terse. Never log keys.
- GIT: ~/Code/botter -> github.com/Mesum-Hussain/Botter, branch main. Commit/push when the owner asks. No secrets.
- Versions [D]: no new version number without asking; patch bumps; refresh the 0.2.0 release in place (move the tag, re-upload assets) when asked to publish.
- After code changes run `graphify update .` and `make test`.
- Ignored: /botter, /artifacts/, /tools/bin/, agent_tools binary, tui binaries, /graphify-out/, /pitch/, /dist/.

## 11. LICENSING
Own C + cJSON (MIT) + BearSSL (MIT) + Mozilla CA data (MPL-2.0, unmodified; notice in THIRD_PARTY.md). LICENSE MIT. Release static = musl.
