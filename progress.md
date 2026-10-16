# BOTCORE + BOTTER: SPEC + STATUS (AI-consumed; terse by design)

Updated: 2026-10-17 | Version 0.2.0 (released; binary refreshed in place) | Lang: C (gnu11) | Target: x86_64 Linux (dev: Fedora 44, kernel 7.2, SELinux enforcing, Landlock + userns on, musl-gcc)
State: sub-agents, MCP (both directions, URL + command), A2A, ACP and the sealed vault are done and pushed. DONE 2026-10-15 (not yet committed): Lua functions replaced FWL (§3), batch mode --run, image_view, token usage, temperature/seed. NEXT: the architecture rebuild in §12 (supersedes §3c and §9 step 3; the hackathon is a test of it, not the goal).
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
config.json: `requires` = programs on PATH (build warns for unlisted #! interpreters and MCP commands; at start botcore tells the user AND the model which are missing). `builder: true` = Botter only (vault_seal). `temperature`, `seed`: sent with every model request. A FLOW.fwl / FLOW.md / SQNC.md in a project is a build error that says where its content goes (FLOW.lua is valid again: §12.3). `mcp`, `agents`: §4.

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

## 3b. BUILDER HARNESS [D 2026-10-15: build from a brief, no questions]
Goal: Botter builds an agent from a problem statement + data on its own, with a weak model (gemini-3.1-flash-lite).
- agent.md: autonomous (no plan approval, no questions a default answers, no seal question), steps explore (head every data file) -> PLAN.md checklist -> functions+tests -> agent.md -> config.json -> dry run until 0 warnings -> compare with the brief's examples -> build -> report. The Lua rules and the bot module are inline (models skip skills).
- on_turn_end(reply) hook [framework feature]: a global Lua function in an agent's functions/; botcore calls it after each user turn; a returned text goes back to the model (max 8 in a row). Botter's (functions/botter.lua) sends it back while PLAN.md has "- [ ]" items or functions/*.lua comments say the code is unfinished.
- fs_write / fs_edit on functions/*.lua return the build's Lua check at once (diagnostics + which functions are tools). fs_edit (exact text replace) never asks inside the working dir; fs_write replaces files written this session without asking, earlier files still ask.
- Lua teaches: require/dofile/load/io.*/os.*/package/math.random raise "X is not available in functions: use ...".
- Lint (warning): placeholder words in a documented function's comments, loops with only comments; a functions/ folder with no tool and no hook.
- CHAT_MAX_TOOL_ROUNDS 25 -> 150. BOTTER_TRACE=<file> dumps the conversation after each turn (harness debugging).
- Fixed: agent_build failed when Botter ran as a plain .bot from a folder Landlock hides (reopened /proc/self/fd/3 by path); now copies the runtime into a memfd.
- Scores: a test file's score_<name>() returns passed, total[, details]; the build prints "packer: score <name>: p/t (x%)". Botter's hook demands a score when the folder has example files (name contains sample/example/expected/golden/ground_truth), requires it to cover every example (rows counted by the hook), refuses agent code that names an example file (answer-key leakage), and sends the model back with the report while the score improves (max 6 rounds, stops after 2 without progress).
- bot.tool(name, args): any of the agent's tools from Lua, same rules as the model.
- --eval [dir]: evals/*.json cases (one or a list per file): input + expect {contains, not_contains, equals, regex, tools_used, tools_not_used, max_tool_calls} and/or check = a Lua function(json of {input, reply, tools, data}) returning nil or why. Fresh conversation per case, one JSON line per case, summary + usage on stderr, exit 1 on a failure; a case that cannot fail is refused.
- Run 8 (all checks on): every shortcut caught (open plan, no score, answer-key read, mocked test); honest final score 0/25 after 88 calls / 3M input tokens. The harness is honest now; the gap left is the model.
- Measured on September 2026 (5 runs, flash-lite): no questions, explores data, uses the Lua check, builds a .bot (16-35 calls). But the model takes shortcuts: stubs with fixed results, ticks plan items it did not do, once copied sample_requests.csv into output.csv. The harness catches marked stubs, not unmarked ones or copied answers. Open: a stronger signal (score against the brief's examples inside the hook), and trying a stronger model for building.

## 3c. (SUPERSEDED by §12) NEXT SESSION [D 2026-10-15]: harness architecture (performance + results), not new integrations
Owner's focus: improve the harness itself for better results and lower cost; widening integrations waits.
Baseline to measure every change against: run 8 on the September 2026 brief (check/by-botter9, trace9.json): flash-lite, 88 calls, 3.03M input / 17.6k output tokens, honest score 0/25. Final context ~220k chars: tool results 135.7k (62%; fs_read 94k = raw 32 KB slices of the 2.8 MB events CSV), tool-call args 60k (27%; whole files via fs_write), system 10.7k, model text 6.7k. Every call resends the whole history, so cost ~ calls x context.
Plan, in order of payoff:
1. Fade old tool traffic: past a context threshold, replace large old tool results and already-written file contents with one-line stubs ("[fs_read dataset/x.csv: 32 KB, removed; read it again if needed]"). Batch it (only at the threshold) so provider prompt caching keeps working between batches. Expect 3-5x fewer input tokens and a cleaner context.
2. Pin the task: history trimming (CHAT_HIST_MAX_BYTES, drops whole oldest turns) can drop the original brief. Keep the first user message plus a short summary of what was dropped.
3. Data reads: fs_read on a large file returns size, line count and the first lines with a hint (head/grep/a Lua function) instead of 32 KB of raw rows.
4. config.json "reasoning": "low|medium|high" -> reasoning_effort on every request; Botter builds with it.
5. Focused feedback: on a low score the hook sends one failing example in full detail at a time, not a 6000-char report.
Also open: the model's final report can contradict its score (run 8 said "fully functional" at 0/25): the report should quote the measured score, not judge it. Try a stronger builder model on the same brief to separate harness from model.
Ideas noted for later (integrations, on hold): OpenAI-compatible --serve (/v1/chat/completions); one listener for A2A + MCP + chat; each remote A2A skill as its own tool; recover tool calls written as text (<tool_call>/JSON) for local models; large tool output to a temp file instead of cutting at 32 KB; richer ACP (commands, model switch, sub-agent progress); ACP agents by command as sub-agents.
Local research material lives in inspiration/ (gitignored); never name it in commits or docs.

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
Tests: `make test` = tests/run.sh, 246 local (needs python3; tmux for UI tests). Stand-ins: stub_llm.py (model: one tool call then "done", or a fixed text; reports usage, records sampling and image parts), remote_servers.py (MCP stateless + session, A2A peer), mcp_stdio_server.py, acp_client.py, http_servers.py. BOTTER_TEST_NET=1 adds real providers. Real-model checks are manual (owner's saved Gemini key).

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

## 12. ARCHITECTURE REBUILD [D 2026-10-16] (task graph)
Goal [D]: deep architecture, not quick fixes. Botter understands the job, makes a robust plan, breaks it into small checkable tasks, then designs an agent that performs it, including complex multi-folder skills (frontend-slides class). The hackathon (Oct 24) is a test of this, not the target. No backward compatibility in the packer [D]: nobody uses the old layout; old files are build errors that say where things go now.
Why: run 8 = one growing conversation where a weak model explores, plans, codes, tests and judges itself; every guard added caught one shortcut and the next appeared. Fix the structure: the harness owns the plan, the context and the verdict.

### 12.1 Agent layout [D 2026-10-16, revised same day]
```
AGENT.md              persona, rules, how a session goes (was agent.md)
FLOW.lua              OPTIONAL orchestration (12.3): plans, runs workers, runs checks, replans. Absent = plain chat loop
config.json           kept [D] (cJSON is 9.2 KB and needed anyway: every API/MCP/A2A/ACP message is JSON)
functions/*.lua       agent-wide Lua, always loaded: core work, infrastructure (DB access, hooks such as on_turn_end), helpers
functions/tests/*.lua build-time tests and score_*(); never embedded
skills/<n>/SKILL.md   frontmatter name, description; body = when + procedure
skills/<n>/references/  read on demand
skills/<n>/functions/*.lua  skill-scoped Lua tools, visible only while the skill is loaded
skills/<n>/scripts/   executables (sh/py/...), runnable at a real path at runtime
skills/<n>/assets/    templates/files copied into output
agents/<n>/           sub-agent = same layout again
evals/                cases the harness scores against; never embedded
```
- Rule [D]: skills are on-demand capabilities (a playbook plus what it needs). What the whole agent depends on (FLOW.lua, DB connection, hooks, core functions) is not a playbook: it lives at the top level, always loaded. Keep top-level documented functions few: each is a tool schema resent every request (~100-300 tokens) and weak models pick worse from long lists.
- Gone: top-level tools/ (executables are skill scripts/; a function reaches one with bot.shell). No backward compatibility: old layout files are build errors that say where things go.
- All Lua (FLOW.lua, functions/, skills/*/functions/) shares one state: everything can call everything. Tool visibility: functions/ always, a skill's functions only while it is loaded (or when the flow gives a worker that skill).
- Standard Agent Skills run unmodified [D]: at runtime a skill's files appear at a real read-only path (private dir under the sandbox's tmp, or memfd-backed), wiped on exit; the system prompt tells the model the path; scripts run through shell_exec there. requires covers their interpreters.
- Packer skill validator: frontmatter (name = folder, description present, <= 1024 chars), links in SKILL.md resolve, every references/ file reachable from SKILL.md, SKILL.md > 500 lines = warning, placeholders/TODO = error, scripts executable or #!, Lua checks as today.

### 12.2 Context + memory engine (botcore, every agent) [D: RAM; Botter also .botter/]
The prompt is assembled each request, not one ever-growing transcript:
| Part | Content | Writer |
| pinned | system, job.md (spec), current task | harness |
| working memory | facts, decisions, open questions; `note` tool, capped, shown each turn | model |
| file ledger | path, purpose line, Lua check result, functions defined | harness, auto on fs_write/fs_edit |
| data profiles | per data file: size, rows, columns, types, a few sample rows | harness, once per file (fs_read of a big file returns this) |
| recent window | last N tool exchanges raw | - |
| archive | older tool results/arguments as one-line stubs with an id; `recall(id)` restores | harness |
- Compaction at thresholds, in batches, so provider prompt caching holds between batches. Pinned task never dropped (fixes trim_history dropping the brief).
- Built agents: all in RAM. Botter: .botter/ in the project folder (job.md, tasks, memory, ledger, eval results) so a build resumes and the owner can inspect it [D].

### 12.3 Task harness: FLOW.lua on botcore primitives [D]
FLOW.lua [D, name restored]: what FLOW was meant to be from the start (a communication gap made it look like a conversation script). It orchestrates tasks; it never scripts what the model says. Entry: `function flow(input)`, called once per incoming job (a user message, a --run line, an A2A/MCP request); uses bot.worker / bot.check / bot.tasks (and bot.llm, 12.5) plus any function. No FLOW.lua = the plain chat loop. The packer checks it like functions/ (syntax, names, tests in functions/tests/).
Phases: understand -> job.md -> CHECKPOINT (owner confirms/corrects in one message; flag to skip for unattended runs) [D] -> plan -> execute tasks -> verify -> integrate -> eval -> report (quotes measured score).
- job.md: fixed sections (inputs, outputs, rules, edge cases, success criteria, examples). Brief examples become evals/ here, before code; then frozen (model cannot edit evals/).
- Task graph held by the harness, not a markdown checklist. Task = goal, skills to load, files it may write, depends_on, acceptance check the harness runs (lua test | dry run | eval subset | validator | Lua check fn). No runnable check = task rejected.
- One fresh worker context per task: pinned spec + task + ledger lines for its files + memory. Done = harness ran the check and it passed; a fail returns one focused failure.
- Open design questions: 12.7 (brainstorm in progress).

### 12.4 Botter on top
New AGENT.md; skills: understand-job, plan-tasks, design-agent, write-skill (own, informed by public skill-creators, Apache-2.0: progressive disclosure, description = trigger, bundle repeated logic as scripts, test by running with/without the skill, validator), write-lua, write-scripts, evals. Measured on the September brief vs run 8 (score, input tokens, calls).

### 12.5 Code-driven agents (option 3, where it fits)
bot.llm(prompt, schema?) -> text/table (the agent's own model, counted in usage); a Lua entry for --run (one record -> code runs the procedure, model only for judgement). Lifts the agent.md rule "never write a function that calls an LLM" for this one sanctioned path.

### 12.6 Order (measure each vs run 8 before the next)
1 context+memory engine -> 2 skills as packages + layout + validator + runtime skill paths -> 3 task harness -> 4 Botter rewritten -> 5 code-driven agents.

### 12.7 Task graph design (brainstorm 2026-10-16)
- Orchestration = FLOW.lua on C primitives [D]: botcore gives bot.worker{task, skills, writes, context} (fresh context-managed chat -> result), bot.check{...} (runs an acceptance check), bot.tasks (store). Phases/retries/replanning = Botter's FLOW.lua: Botter is the first code-driven agent; policy tested with stub_llm; built agents can reuse the primitives.
- Models [D]: one model everywhere by default (proves the harness carries a weak model); config may name a stronger model for plan/replan/review roles; measure both on September.
- Parallel [D]: sequential v1; the graph validator already enforces disjoint write scopes so parallel (fork per worker) is a switch later.
- Write scopes (proposed): each task lists files it may write; enforced in fs_write/fs_edit and by a narrower Landlock ruleset for the worker's shell. Implementation workers cannot write tests or evals/.
- Tests before code, other worker (proposed): job.md -> evals from the brief (frozen) -> test task (sees spec, not code) -> implementation task whose check is those tests.
- design.md (proposed) between job.md and tasks: target agent's skills, which are always, code vs model split, sub-agents. Pipeline: understand -> job.md -> CHECKPOINT -> design.md -> task graph -> execute.
- Graph validator (proposed): rejects tasks without a runnable check, > ~3 written files, inputs nobody produces, overlapping write scopes between tasks that can run together, cycles; rejection goes back to the planner.
- Non-executable quality (proposed): packer validator + fresh reviewer worker with a rubric (sees spec + artifact only) + the built agent's eval score; skills evaluated with vs without the skill.
- Stuck task [D all four, in order]: 1 split or re-approach (blocker report as input) -> 2 amend a check the planner wrote (never one from frozen evals), with a reason logged in the report -> 3 record a known limitation, build the rest, report it with the measured score -> 4 ask the owner (interactive only). Budget per task.
- FLOW.lua = the original Lua-flow idea of 2026-10-15, as orchestration: the model proposes the plan and does tasks, code enforces order and verdicts.

### 12.8 Always-on agents and state [D 2026-10-16]
- Agents are stateless [D]: a .bot holds no durable state (the binary is read-only and checksummed). Deployed on a server, it runs next to a store the deployer provides (e.g. a container with SQLite); state lives there, reached through functions/ (top level: infrastructure, always loaded).
- Harness state (task graph, context/memory engine) = RAM per run/session. Botter's .botter/ = plain files in the project folder (build resume, inspection).
- SQLite: candidate to embed like Lua (bot.sql), or used via the sqlite3 program from a skill script. Size cost to measure before deciding (roughly several hundred KB vs the 532 KB agent); later.
- Triggers (webhooks, chat channels, folder watch): all planned, after the hackathon [D]. Focus now: agent architecture.
- Ideas kept from a local daemon that supervises coding-agent sessions (inspiration/): LLM plans, deterministic supervisor owns lifecycle; one task + one isolated workspace + own feedback loop per worker; facts recorded, status derived; feedback routed to the owning task; a failed probe is a fact, not death.

### 12.9 design.md: how Botter designs the agent [D 2026-10-17]
Pipeline: understand -> job.md -> design.md -> CHECKPOINT (moved here [D]: owner sees job.md + a one-screen design summary: kind, code/model split, functions, skills; confirms or corrects once; flag skips it for unattended runs) -> task graph -> workers -> checks -> evals -> report.
- Form [D]: job.md and design.md are built by structured tool calls (design_kind, design_unit, design_function, design_skill, design_trace, ...), each validated on the spot; the harness renders the readable .md files (a view, not the source). Stored in .botter/.
- design.md answers, in order:
  1 kind: chat assistant | batch (--run) | service (--a2a/--mcp) | scheduled (cron) | producer (output files); sets run mode and whether FLOW.lua is needed.
  2 per unit of work: EXACT (rules, maths, dates, joins, formats) -> Lua function | JUDGEMENT (reading text/images, tone, explaining) -> model | HYBRID -> model extracts facts against a schema, code decides, model explains from code's facts only.
  3 who drives: fixed checkable procedure -> FLOW.lua (model called for narrow steps); open conversation -> chat loop + skills; mixed -> FLOW.lua handing the open part to a chat worker.
  4 data model: entities, files, joins, ids, units/currencies, store if any.
  5 components: top-level functions (signatures; few, capped), skills (name, description = when to use, modes, references/scripts/assets each with an outline), sub-agents (only for a separate persona or isolated context, e.g. a critic), external (MCP, HTTP APIs, programs in requires).
  6 failure behaviour: missing data, ambiguous input, API down, malformed record (decided at design time so stubs do not appear at coding time).
  7 traceability: every job.md requirement -> component -> check.
- Design validator (Lua in Botter's FLOW.lua): every requirement traced, every component has a check, EXACT units are functions, top-level function cap, skill descriptions say when.
- Reviewer worker (fresh context; sees job.md + design.md only): gaps, misread rules, over-engineering.
- Task graph derived mostly mechanically from design.md (function -> test task then implementation task; each skill file -> task; integration and evals last); the planner model only orders and fills gaps.
- Rich skills: in the design a skill is a small tree (router SKILL.md + references/scripts/assets with outlines); each file is its own task and check (script -> test; reference -> reachable from SKILL.md; whole skill -> validator + evals with vs without it).
- Domain knowledge [D]: the brief, the owner's documents/folders, the data, plus web research by Botter with the owner in the loop: Botter shows what it wants to look up and the sources it found, the owner approves before they shape a skill; references cite their sources. Unattended runs: no web research unless allowed up front. Web research is Botter's ability only; built agents are online or offline per the owner's config.
- Worked example (September "Buy or Wait?"): kind batch, FLOW.lua flow(request_id): gather (EXACT load_request) -> read messages/images (HYBRID, schema {event_id, change, amount, date, essential}) -> forecast (EXACT) -> evaluate options (EXACT) -> choose (EXACT) -> explanation (JUDGEMENT from chosen facts) -> append_row/finish (EXACT). No skills; ~6 top-level functions; evals = sample_requests.csv, frozen.
