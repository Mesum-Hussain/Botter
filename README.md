<div align="center">

# Botter

### `.bot` is a file format for agents.<br>One agent, one file, nothing else to ship.

**532 KB** · **starts in 0.4 ms** · **nothing to install** · **runs on any x86-64 Linux**

</div>

---

Building an agent is a weekend. Shipping it is the rest of the month.

That's because until now, shipping an agent has meant shipping a runtime. The agent itself is small: a persona, some steps, a few tools, maybe a few hundred lines of real intent. But it can't go anywhere alone. It needs its framework, the framework needs its packages, the packages need the right Python or Node, and the whole stack ends up in a container that crosses a gigabyte. You aren't shipping an agent. You're shipping an environment with an agent somewhere inside it.

Everything else we make got a file format long ago. A document is a `.pdf`. A song is an `.mp3`. You send the file, and it works at the other end.

An agent is now a `.bot`:

```
lead-outreach.bot      one file, about 530 KB
```

That file is the whole agent: its persona, its skills, its functions, its sub-agents, its tools, and everything it needs to think, talk to a model and run them. No `requirements.txt`, no virtualenv, no Node version to match, no 1.2 GB container with LangChain buried somewhere inside. You copy it to a server and run it. It starts before you lift your finger off the Enter key.

And unlike a `.pdf`, a `.bot` needs no reader. Nothing has to be installed where it lands, because the file is also the program.

## Where a .bot comes from

You don't write a `.bot` by hand. You write an agent as a folder of plain text:

```
lead-outreach/
  agent.md       who it is and how it works
  skills/        playbooks for its tasks
  functions/     the exact part, in Lua: sums, dates, data files, checks
  tools/         programs it runs, when Lua is not enough
```

and Botter turns the folder into the file. Botter is the Markdown-first agent framework that writes `.bot` files: you describe an agent in Markdown and plain English, and it packs that into a single statically linked executable with the whole runtime inside.

## Markdown-first is not Markdown-only

Markdown is where you start, not where the agent's power stops. The same single file can hold all of this:

| You want | Botter gives you |
|---|---|
| Exact work that comes out the same every time | **Functions** in Lua: documented in plain English, tested at build time, run inside the agent |
| A team, not one agent | **Sub-agents**, each with its own persona, handed work by the main agent or by a function |
| Real capabilities | **Tools** in any language, plus file, shell, image and scheduling tools built in |
| One job over thousands of records | **Batch mode**: one task per line, a fresh conversation for each, JSON lines out, tokens counted |
| Other people's tools | **MCP** servers, unmodified: by URL, or started by command the way Claude Code and opencode run them |
| Other people's agents | **A2A**, both calling out and being called |
| Your agent as a tool for other agents | **MCP** again, the other way round: Claude Code, opencode or any MCP client can call it |
| The agent in your editor | **ACP**, for editors such as Zed |
| Safety on a real machine | A kernel **sandbox**, a network kill switch, and approval before anything destructive |
| Keys that travel with the agent | An optional password-**sealed vault** inside the file |

All of it fits in about 530 KB, in one process, with no framework to carry along.

## How small is small?

Same laptop, comparable agents:

| | Botter agent | OpenClaw agent | |
|---|---|---|---|
| On disk | **532 KB**, 1 file | 646 MB, 41,408 files | **~1,200× smaller** |
| Ready in | **0.4 ms** | 7.3 s | **~18,000× faster** |
| Idle memory | **0.4 MB** | 730 MB | **~1,800× lighter** |

<sub>OpenClaw 2026.9.9, default npm install (Node.js itself not counted), gateway start to accepting connections. Botter: a minimal built agent, start to first prompt. AMD Ryzen 5 7520U, Fedora 44.</sub>

A Botter agent boots more than thirty times faster than an *empty* Python process. `ldd` on it says `not a dynamic executable`. And if you still want a container, go ahead. It now holds your agent and whatever its tools need, and nothing else.

## One more thing: you can talk to the compiler

Every compiler you've ever used has the same personality. You hand it code, and it hands you back a list of everything you did wrong.

Botter talks back.

Run it in an empty folder and tell it what you want: *"an agent that finds local businesses without a website and pitches them my web design service."* It asks the two or three questions that matter. It proposes a plan. Then it writes the persona and the playbooks, writes the exact parts as Lua functions with their tests, writes any tools and tests them, checks its work against what you asked for, and builds the binary. If you want to grab the wheel at any point, every file is plain text sitting right there in your folder.

How does a compiler do all that? **Botter is itself an agent.** Its persona and its skills are plain-text files in this repo (`agent.md`, `skills/`), compiled by Botter and running on the same runtime as every agent it builds.

We use Botter to build Botter.

## Try it

```sh
curl -LO https://github.com/Mesum-Hussain/Botter/releases/latest/download/botter
chmod +x botter && sudo mv botter /usr/local/bin/

mkdir myagent && cd myagent
botter               # pick a model, paste your key, describe your agent
./myagent.bot        # run it, or scp it to any x86-64 Linux box and run it there
```

Bring your own model: **Gemini, OpenAI, OpenRouter, Groq, Ollama** for local models, or any OpenAI-compatible endpoint. You type your key once. It lives in the kernel keyring, in RAM, until the machine reboots, which is handy for agents that live on servers.

---

## Anatomy of an agent

| File | What it is |
|---|---|
| `agent.md` | Persona, rules and how a session goes: who the agent is, what it does, what it must never do |
| `functions/<name>.lua` | Exact work in **Lua**, each documented function a tool; `functions/tests/` runs at build time |
| `skills/<name>/SKILL.md` | Playbooks for specific tasks |
| `agents/<name>/agent.md` | Sub-agents: a persona of their own for one kind of job |
| `tools/` | Executables plus a JSON description of each |
| `config.json` | Name, version, internet on/off, what gets compiled in, MCP servers and remote agents |

**Skills** follow the `SKILL.md` convention. The agent sees a one-line summary of each and opens the full playbook only when the task calls for it, so it can carry dozens without stuffing its context window.

**Sub-agents** live in the same file as the agent. Each gets its own persona and a fresh conversation, does one job with the agent's tools, and hands back only its result. The agent delegates on its own, and a function can hand one a task with `bot.agent("researcher", task)`. No second process, no second binary.

**What it needs.** If an agent relies on programs being installed, say `sqlite3` for a database or `python3` for a tool, they are listed under `requires` in `config.json`. The build warns when a tool needs something that isn't listed, and when the agent starts it checks the machine: the user is told what is missing, and so is the model, so it says "install sqlite3 first" instead of failing one command at a time.

**Tools** are any executable at all: bash, Python, Go, Rust, C. A tool gets one JSON object on stdin and prints its answer. That's the whole protocol. A tool brings its own needs with it, so a Python tool needs Python on the box and a static binary needs nothing. Users never see tool names or JSON. They see *"Searching for leads…"*.

Botter picks in this order: a **Lua function** first (it needs nothing on the machine), a **shell script** when the job needs a program such as `sqlite3` or `ffmpeg`, and **Python or Node** only for a library there is no other way to get.

## It speaks the standards

MCP, A2A and ACP are built into the runtime, in C. There is no bridge, no adapter and no second runtime to install: a Botter agent is still one file, and it starts nothing unless you tell it to.

- **MCP, by URL.** Name a server's URL in `config.json` and its tools join the agent's own. Each call is one HTTPS request from the agent's built-in client, using the stateless 2026 revision of the protocol, and falling back to the older session handshake for servers that still want it. Nothing is installed and nothing is started.
- **MCP, by command.** Most MCP servers are programs you start, like `npx -y @modelcontextprotocol/server-filesystem .`. Give the agent that same command and it starts the server itself, unmodified, inside the same sandbox as its tools, and stops it when it exits. The agent needs no Node or Python of its own; such a server needs whatever it was written in, so list that under `requires`.
- **MCP, being used.** `./myagent.bot --mcp` turns the agent into an MCP server with one tool, `ask`. Any MCP client that starts a command can use it: a coding agent such as Claude Code or opencode, an editor, another framework. The client starts the agent and talks to it over its input and output, so nothing listens. Give it a port (`--mcp 8080`) and it serves MCP over HTTP instead.
- **A2A, calling out.** List another agent's URL under `agents` and the agent hands it work exactly like a sub-agent; a function can too, with `bot.agent("billing", task)`.
- **A2A, being called.** `./myagent.bot --a2a 8080` publishes an agent card and answers other agents. Like `--mcp 8080`, it listens only because you asked it to.
- **ACP.** `./myagent.bot --acp` is what an editor such as Zed runs to put the agent in its sidebar, with tool calls and permission prompts shown in the editor.

```json
{
  "mcp":    { "docs":    { "url": "https://example.com/mcp", "token": "DOCS_TOKEN" },
              "files":   { "command": ["npx", "-y", "@modelcontextprotocol/server-filesystem", "."] } },
  "agents": { "billing": { "url": "https://billing.example.com", "token": "BILLING_TOKEN" } }
}
```

`token` is only a name, never the secret itself. The value comes from the agent's sealed vault, or from a line of that name in a `.env` file next to where the agent runs.

For example, to give Claude Code your agent as a tool:

```sh
claude mcp add lead-outreach -- /path/to/lead-outreach.bot --mcp
```

With `--acp`, `--a2a` and `--mcp` there is nobody at a keyboard, so the agent never asks for a key there. It uses the model connection you saved by running it once in a terminal, its sealed vault, or `BOTTER_BASE_URL`, `BOTTER_API_KEY` and `BOTTER_MODEL` in that `.env` file. `--a2a` and `--mcp` with a port listen on this machine only unless you give them an address (`--a2a 0.0.0.0:8080`), and then they insist on a token that callers must send (`A2A_TOKEN` or `MCP_TOKEN`). Risky actions, which a person at the terminal would be asked about, are refused in these modes.

## Functions: the part that has to be exact

A model is good at reading a messy email and bad at adding up thirty numbers. Ask it for both and you get a confident answer that is off by a few hundred.

So a Botter agent splits the work. The **model** runs the conversation, from `agent.md` and the skills: it reads, decides, writes and talks. **Functions** do the exact part, in plain Lua, the same way every time:

```lua
--- What the balance will be on each of the next days, from today's
--- balance and the known payments. Call it before saying anything is affordable.
---@param today string  today's date, YYYY-MM-DD
---@param balance number  today's balance
---@param payments table[]  {date = "YYYY-MM-DD", amount = number}, negative = money out
---@param days integer  how many days ahead
---@return table  the lowest balance and the day it happens
function lowest_balance(today, balance, payments, days)
  local lowest, lowest_day = balance, today
  for day = 1, days do
    local date = bot.date.add_days(today, day)
    for _, p in ipairs(payments) do
      if p.date == date then
        balance = balance + p.amount
      end
    end
    if balance < lowest then
      lowest, lowest_day = balance, date
    end
  end
  return { lowest = lowest, on = lowest_day }
end
```

The `---` comment is how the model learns the function exists: it becomes a tool, with that description and those parameters. The model decides **when** to call it; the code decides **what comes out**.

- **No new language.** It is Lua 5.4, the language games and editors already embed, built into the runtime. A function reaches the outside world only through a small `bot` module: JSON and CSV, dates, files in the working folder, HTTPS, the shell, MCP servers and other agents, each under the same rules and permission prompts as the model's own tools.
- **Tested before it ships.** Put tests in `functions/tests/` and the build runs them. A failing test means no `.bot`.
- **Checked like code.** A syntax error, a parameter without a type, or a name another tool already has stops the build with the file and line. The build also says what each function reaches: `lowest_balance (pure)`, `save_report (writes files)`, `fetch_rates (the network, secrets)`.
- **Nothing to install.** Lua runs inside the agent. The runtime grew by about 140 KB to carry it, and an agent without functions never starts it.

## Batch mode

Some jobs are not conversations: answer 250 support tickets, score 10,000 rows. `--run` does one task per line, each in a fresh conversation, and prints one JSON line per task:

```sh
cut -d, -f1 requests.csv | ./triage.bot --run > answers.jsonl
./triage.bot --run "Summarise yesterday's report"     # one task: just the answer
```

Nobody is there to approve anything, so risky actions are refused. Set `"temperature": 0` (and a `"seed"`) in `config.json` for answers that repeat, and the run ends with the token count per model on stderr, ready for a cost report.

## Inside a .bot file

A `.bot` is three things, end to end:

```
lead-outreach.bot
┌───────────────┐
│ botcore       │  the runtime: a static x86-64 Linux executable, the same in every agent
├───────────────┤
│ your agent    │  agent.md, skills, functions, sub-agents, tools, config.json, read-only
├───────────────┤
│ footer        │  32 bytes: where the agent's files begin, how long they are, their checksum
└───────────────┘
```

So building one is not compiling. Botter doesn't generate code, and no compiler or linker runs during a build. Your files are appended to **botcore** and sealed with a checksum, which takes about **2 ms**. A truncated or tampered `.bot` refuses to start.

Because the front of the file is an ordinary executable, the operating system already knows what to do with it. There is no `.bot` player to install and no file association to set up. You run it.

botcore is about 11,000 lines of C, statically linked against musl, and it packs a surprising amount into half a megabyte:

- Lua 5.4 for functions (only its safe core: no `io`, no `os`, no loading code from disk), with the `bot` module on top
- its own HTTPS stack, using vendored BearSSL with Mozilla's CA roots compiled in, so no curl, no OpenSSL and no system certificates
- an OpenAI-compatible client with streaming and tool calling
- clients for MCP and A2A, and MCP, A2A and ACP front ends
- file, shell, image and scheduling tools
- a sandbox enforced by the kernel itself

## Paranoid by default

An agent runs commands on your machine. Botter assumes that's dangerous and acts like it.

- **Landlock sandbox.** Every tool and shell command can write to the working folder, the temporary directories and the `~/.cache` and `~/.npm` caches, and that's it. Your `~/.ssh`, your tokens and the rest of your home directory are invisible to it. `rm`, overwrites, `git push` and `sudo` all ask first.
- **Kill switch for the network.** Set `"internet": false` and the kernel cuts the agent's tools off the network. The model connection still works.
- **Plan mode.** Hit Tab in Botter, or type `/plan` in any agent. The kernel makes the folder read-only, and the agent can look and think but not touch.
- **No phoning home.** No telemetry, no accounts, no update checks. The only connections are the ones you set up: the model you picked, and any MCP servers or agents you listed in `config.json`.
- **Leaves no trace.** No logs, no history, no settings files. The conversation lives in RAM. The key you type is never written to disk and never passed through environment variables or arguments: the running agent holds it in locked memory and wipes it on exit, and the kernel keyring remembers it, in RAM, until the machine restarts. A key only ever reaches the disk if you choose that yourself, encrypted in a sealed vault or as a line in a `.env` file.

## Keys: yours to type, or sealed inside

By default a Botter agent carries no keys. Whoever runs it picks a provider and types their own key once; it lives in the kernel keyring and in locked memory, and is never written to disk. That is the right default for an agent you share.

For an agent you run yourself there is a second way. When Botter builds the agent it offers to **seal** its keys into the file: the LLM key, and the tokens for any MCP servers and remote agents. Botter asks for each one by name and says what it is for, then asks for a password, twice. The keys are typed straight into Botter on your machine; the model never sees them.

```
$ ./lead-outreach.bot
This agent is sealed: its keys are inside it, locked with a password.
Password:
```

That is the whole login. The password is checked on the machine itself, with no account and no server, and nothing is stored outside the file. The keys are encrypted with ChaCha20-Poly1305 under a key stretched from the password with PBKDF2 (600,000 rounds), so the file is only as safe as the password: Botter refuses anything under ten characters. Anyone who knows the password has the keys, so a sealed agent is not one to hand around.

## The numbers

AMD Ryzen 5 7520U, Fedora 44, median of 15 to 30 runs. Sizes are in KB of 1,024 bytes. "Start" is from launch to the first prompt. The full session talks to a model stand-in on the same machine, so it measures Botter, not a provider.

| | Time / size | Peak memory |
|---|---|---|
| `botter` itself (UI + runtime + packer) | **1,043 KB** | |
| A built agent | **532 KB** | |
| Build an agent | **2 ms** | 0.4 MB |
| Start an agent | **0.4 ms** | **0.4 MB** |
| Start an agent with Lua functions | 0.9 ms | 0.4 MB |
| Full session: connect, one turn with a tool call, exit | 131 ms | **3.9 MB** |
| *For scale:* `python3 -c pass` | 15 ms | 9.2 MB |
| *For scale:* `node -e 0` | 29 ms | 40.6 MB |

Those last two rows are interpreters doing *nothing*, before a single framework import.

## Where it's going

- **Parallel sub-agents.** Sub-agents work one at a time today; running several side by side is next.
- **Streaming over A2A.** An agent called over A2A answers each message whole today.
- **Windows (WSL) and macOS.** Same single file, more places to run it.

## Hacking on Botter

```sh
make                          # builds ./botter; needs gcc and musl-gcc (Fedora: dnf install musl-gcc musl-libc-static)
make test                     # 213 tests: functions, batch mode, sandbox, sub-agents, MCP, A2A, ACP, vault, HTTPS, TUI... (needs python3; tmux for the UI tests)
BOTTER_TEST_NET=1 make test   # adds real providers and TLS failure cases
make dist                     # release tarball + sha256
```

`tools/src/botcore` is the runtime, `tools/src/packer` is the packer that appends an agent's files to it, `tools/src/agent_tools` holds Botter's build tools, and `tui/` is Botter's full-screen UI. `agent.md`, `config.json` and `skills/` in the repo root are Botter itself.

## License

MIT. botcore includes cJSON (MIT), BearSSL (MIT), Lua (MIT) and Mozilla's CA data (MPL-2.0). See `THIRD_PARTY.md`, and ship those notices with agents you build.

---

<div align="center">

**Describe it. Build it. Ship one file.**

</div>