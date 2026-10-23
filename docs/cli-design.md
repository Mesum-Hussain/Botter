# Botter as a CLI: design (experiment, 2026-10-22)

Status: **experiment, not decided.** Nothing is deleted until the acceptance test below passes.
Owner decisions 2026-10-22: no `.bot` by default (6); the TUI is dropped (removed 2026-10-23 with the builder agent);
no `botter ask`; no `botter add` (the Botter skill teaches the coding agent to add outside skills).

## 1. The idea in one paragraph

Botter stops being an agent that builds agents. It becomes a **format, a runtime and a CLI**, the way
Angular is a format plus `ng`. A person, or a coding agent such as Claude Code with the Botter skill,
writes the agent's source in the Botter format: Markdown and Lua first. `botter build` in the project
root checks it, runs its tests and writes one self-contained executable named after the agent.

```
$ cd lead-outreach
$ botter build
checked: AGENT.md, config.json, 4 functions, 2 skills, 0 warnings
tests:   12 passed
built:   ./lead-outreach (562 KB)
```

## 2. Why (evidence)

- Runs 8-18: Botter with gemini-3.1-flash-lite as the builder scored 0-1 of 25 whole rows on the
  September brief, at 104-243 calls and 0.7-2.3M input tokens per build. The model wrote stubs, tests that
  never called the code, and scorers it could game. The harness caught more each round; the model stayed
  the limit.
- 2026-10-22: a plain Python pipeline (`~/Code/botter_test/test_python`), written by a strong model
  iterating against the 25 examples, got 18 of 25 rows with every decision right. It answered 250 requests
  with 28 flash-lite calls: code decided everything, and the model only read messages and images into a
  fixed output format.
- So the engineering needs a strong model, and the weak model is good at narrow runtime reading. Strong
  coding agents already exist; Botter's own value is the format, the one-file runtime and the build check.
  Botter should stop competing with Claude Code and become the target it writes for.

## 3. What stays, what goes

| Stays | Goes (after the acceptance test) |
|---|---|
| botcore runtime: chat loop, Lua, sandbox, memory engine, MCP/A2A/ACP, `--run`, vault, keyring | Botter as an agent: its AGENT.md, the build pipeline in FLOW.lua (963 lines), functions/job.lua (619), functions/botter.lua (221) |
| The packer and its validator (layout, skills, Lua checks, tests at build) | `agent_tools` as a separate program (its code moves into the CLI) |
| The agent project format (§12.1 of progress.md) | `"builder": true` and the `vault_seal` model tool (sealing becomes `botter seal`) |
| `bot.*` Lua API, including `bot.worker`, `bot.llm`, `bot.test` | The builder skills as they are now (merged into one Botter skill) |

Kept rule [D]: the packer is never inside botcore. Built agents cannot build agents; only the `botter` CLI can.

## 4. The project format

Same as progress.md §12.1. Restated here because it becomes the public contract:

```
<agent>/
  AGENT.md                    persona, rules, how a session goes (system prompt)
  config.json                 name, display_name, version, description, internet, skills, requires,
                              temperature, seed, mcp, agents
  FLOW.lua                    OPTIONAL: flow(input) handles every job in code; absent = plain chat loop
  functions/*.lua             agent-wide Lua; documented functions are tools
  functions/tests/*.lua       test_* and score_*; run at build, never embedded
  skills/<n>/SKILL.md         playbook; references/, scripts/, assets/, functions/ as today
  agents/<n>/                 sub-agent, same layout again
  evals/                      examples to score against; never embedded
```

### Language order [D 2026-10-15, restated]
1. **Markdown** for instructions, **Lua** for logic. Runs inside the binary; needs nothing on the machine.
2. **Shell** scripts in `skills/<n>/scripts/`, when a program does the job (sqlite3, ffmpeg).
3. **Python, then Node**, only when nothing above can.

Rule: anything beyond Lua must be named in `config.json` `"requires"`. The build warns for an unlisted
interpreter; the agent checks `requires` at start and tells the user and the model what is missing. The
build prints a line such as `runs anywhere: no` when `requires` is not empty, so "one file" is never
claimed falsely.

## 5. The CLI

One static executable, `botter`: the CLI and packer code, with the botcore runtime and the Botter skill
embedded as payloads. No arguments prints help.

| Command | What it does |
|---|---|
| `botter new <name> [--kind chat\|batch\|tool]` | Make `<name>/` from a template: AGENT.md, config.json, and for `batch` a FLOW.lua + `solve_case` + one test |
| `botter check [DIR]` | Validate the project (layout, config, skills, Lua syntax and annotations) and run its tests, without building. Exit 1 on an error. (`botter test` was folded in: the packer's check already runs every test.) |
| `botter eval <examples.csv> [--answers a,b] [--inputs FILE] [--id col] [--tol 2%] [--case id] [--fn NAME] [--min X] [--json]` | Score the agent on solved examples: see 5.1 |
| `botter run [agent args...]` | Run the project from source without writing a file (pack into memory, exec). Every agent flag passes through: `botter run --run`, `botter run --mcp` |
| `botter build [-o FILE]` | check + test, then write `./<name>` (name from config.json). A failing test means no binary |
| `botter inspect FILE` | Version, checksum, embedded files, tools, `requires` of a built agent |
| `botter seal` | Ask (here, never through a model) for the agent's keys and a password; write `secrets.vault` |
| `botter skill [--install DIR]` | Write the Botter skill matching this version into DIR (default `.claude/skills/botter`), or print where it is |
| `botter version` | |

### 5.1 `botter eval`: the loop that made the Python baseline work
- Input: a CSV whose columns are the agent's input columns plus answer columns (e.g. `sample_requests.csv`).
- Code-driven agents (FLOW.lua with `solve_case(row) -> table`): call `solve_case` per row, no model calls
  except the agent's own `bot.llm` ones. Model-driven agents: send each row through `--run`.
- Score **per column**, numbers within `--tol` (default 2%), dates and words exact, lists (`a|b|c`)
  compared item by item. Free-text columns (longer than ~80 chars on average) are reported, not scored.
- Output: a per-column table, then whole rows, then failing rows grouped by column. `--case <id>` prints
  that row in full plus every `bot.say` line the case produced (the trace). `--json` for scripts.
- Exit 1 when a score is below a threshold given with `--min`.

This replaces Botter's examples scorer (FLOW.lua `botter_examples_scorer`), whose 0.01% tolerance on
numbers made the headline score 0/25 even for a good solver.

## 6. `.bot` [D 2026-10-22]

- **Default output: `./<name>`, no extension.** On Linux programs have none; `lead-outreach` reads as a
  command on PATH, `lead-outreach.bot` does not. `-o name.bot` still works.
- "This is an agent" already lives inside the file: the pack footer ends with the magic `BOTPACK1`.
  `botter inspect FILE` and a new `FILE --about` show name, version, Botter version and `requires`.
- The pitch moves from "`.bot` is a file format" to "an agent is one file". The extension can come back
  later if a desktop file association is ever wanted.

## 7. The Botter skill

One skill, embedded in the CLI and written out by `botter skill`, so it always matches the binary.
It is a guide to the format, not Botter's source code.

```
skills/botter/
  SKILL.md                 when to use it; workflow: new -> write -> test -> eval -> build; the rules that matter most
  references/layout.md     the project format, what is embedded, config.json fields
  references/lua-api.md    every bot.* function with a short example (json csv date fs http secret shell mcp
                           agent tool skill_dir worker ask say usage test llm today)
  references/flow.md       FLOW.lua patterns: chat agent (no FLOW), batch CSV agent (code decides, bot.llm
                           with a schema only for text/images), tool/MCP agent
  references/testing.md    test_* and score_*, botter eval, reading a trace
  references/scripts.md    the language order and "requires"
  references/outside-skills.md  adding someone else's skill: copy the folder into skills/<name>/, list it in
                           config.json "skills", add its interpreters to "requires", run `botter check`
  references/checklist.md  before `botter build`
  assets/templates/{chat,batch,tool}/   the files `botter new` writes
```

Sources to merge: project-layout, write-functions, write-agent-md-and-skills, write-scripts,
build-agent, review-agent, wrap-existing-project. They already hold most of the text.

## 8. Acceptance test (decides the experiment)

1. Build the CLI verbs: new, check, test, eval, run, build, inspect, skill (seal can follow).
2. Write the Botter skill.
3. In a fresh folder, Claude Code with only the Botter skill and the brief writes the September agent
   (batch kind: FLOW.lua, Lua forecast and plan code, `bot.llm` with a schema for messages and images),
   iterating with `botter eval dataset/sample_requests.csv`.
4. Pass if:
   - decision columns at least the Python baseline: method 25, status 22, plan 22, spending changes 22,
     earliest date 19 (of 25);
   - `botter build` gives one binary; `./september --run` on `dataset/requests.csv` writes `output.csv`
     for 250 requests with about 35 model calls or fewer;
   - nothing outside Markdown and Lua was needed (or the gap is written down).
5. Pass -> remove the builder pipeline, rewrite the README, ask the owner about a version number.
   Fail -> record what the format, Lua or runtime lacked, fix that first.

## 9. Decided questions (2026-10-22)

1. `.bot`: dropped as the default output name (6).
2. The TUI: dropped (2026-10-22). Built agents keep their own plain REPL.
3. `botter ask`: dropped. Its only purpose was letting people without a coding agent make an agent; the
   coding agent with the Botter skill is now the way in.
4. `botter add`: dropped. Adding an outside skill is a few file steps the coding agent does from the
   Botter skill (references/outside-skills.md).
5. Windows/macOS: out of scope now; the no-extension default must not block an `.exe` later.

## 10. Improving the Botter skill by training it (after the acceptance test)

Only the Botter skill is trained (the one that teaches coding agents to write Botter agents), never the skills
of each agent people build: one trained skill improves every agent made with it.

Use **SkillOpt** (Microsoft, MIT; paper arXiv 2605.23904) to train the skill against scored agent-building
tasks. Compared with Google's Harness² (Apache-2.0; arXiv 2610.14224):

| | SkillOpt | Harness² |
|---|---|---|
| What it improves | one skill document, trained across many tasks | the whole harness (8 parts), per task |
| Output | one `best_skill.md` that ships unchanged, zero extra calls at run time | an adopted harness per task + a lessons library |
| Feedback | scored tasks; an edit is kept only if a held-out validation score strictly improves | no ground truth: compares runs of candidate harnesses |
| Coding-agent targets | Claude Code, Codex, Copilot, Cursor | OpenCode, Codex |
| Setup | Python, `openai`-compatible backends | Python, Vertex AI / gcloud, benchmark installers |
| Extending it | a new benchmark is ~200 lines (loader, rollout, adapter, YAML) | adapters per benchmark plus a substrate |

SkillOpt matches the job: we want one skill that makes coding agents write better Botter agents, and we
can score every attempt with `botter eval`. Harness² improves a harness while it works on a task with no
answers to check against; some of its ideas are already in progress.md §13 item 2.

How it would run:
- **Task set first.** One problem is not a training set. Collect 10-20 agent-building tasks, each a folder
  with a brief, data and solved examples; split each task's examples into visible (for the builder) and
  hidden (for scoring). Split the tasks into train / validation / test. The September brief stays in
  **test**, so its score remains an honest measure.
- **A Botter environment for SkillOpt.** Rollout = copy a task folder and the current skill into a
  workspace (`.agents/skills/...`, as SkillOpt already does), run the coding agent (Claude Code exec
  backend), then `botter eval` on the hidden examples: hard = whole rows, soft = mean per-column score.
  Save the transcript for its reflection step.
- **What is trained:** SKILL.md (workflow, rules, pitfalls). references/ (layout, Lua API) are facts and
  stay hand-written; a trained rule that contradicts them is a bug.
- **Cost:** every rollout is a whole agent build by a coding agent. A small run (2 epochs, minibatch 4,
  validation 4) is ~50 builds. Train with a cheaper builder model first and check that the skill transfers
  to a stronger one (SkillOpt reports transfer across model sizes).
- SkillOpt is dev-time tooling: it never ships inside Botter or inside agents.

## 11. Implementation status (2026-10-23)

- Runtime: `--call NAME` (JSON lines into a global Lua function, one JSON line out; runs without a model
  connection, bot.llm then fails clearly), `--about`, `--seal` (keys into ./secrets.vault, asked at the
  terminal), `bot.llm{audio = {...}}` (MP3/WAV/OGG/FLAC/M4A as input_audio parts).
- CLI `tools/src/cli/botter.c` (one static file, ~970 KB with the runtime and the skill embedded via
  `.incbin`): data, new (`--here`), check, run, eval (`prepare(cases)` first when defined; words in any case;
  lists `|`/`;` in any order; Title Case columns match result keys; row-number ids), build, inspect, seal,
  skill, version, help.
- Skill `skill/botter/`: SKILL.md (the fast path) + references + templates (chat, batch with
  functions/read.lua, tool) + assets/lib/search.lua. Examples are neutral (no task-specific details).
- Removed 2026-10-23 (the CLI replaced them): the builder agent (repo-root AGENT.md, FLOW.lua, config.json,
  functions/, skills/), agent_tools, `make builder`, the TUI and its front end (BOTCORE_FRONTEND), config.json
  `"builder"` (now a build error that names `botter seal`), the vault_seal model tool, BOTCORE_RUNTIME_FD. The
  version lives in VERSION.
- Tests: 354 pass, 1 skipped.
