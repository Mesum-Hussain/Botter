<div align="center">

# Botter

### The Markdown-first agent framework.<br>Every agent ships as one portable executable.

**396 KB** · **starts in 0.4 ms** · **nothing to install** · **runs on any x86-64 Linux**

</div>

---

Here's an agent:

```
lead-outreach/
  agent.md       who it is
  FLOW.fwl       what it does, step by step
  skills/        how it does it
  tools/         the jobs it can't do with words alone
```

And here's the same agent, ready to ship:

```
lead-outreach.bot      one file, about 400 KB
```

One file. No `requirements.txt`, no virtualenv, no Node version to match, no 1.2 GB container with LangChain buried somewhere inside. You copy it to a server and run it. It starts before you lift your finger off the Enter key.

That's Botter: a Markdown-first agent framework. You write an agent in Markdown and plain English, and Botter compiles it into a single statically linked binary with the whole runtime baked in. Nothing like LangChain or n8n has to be installed where the agent runs. It carries everything it needs to think, talk to a model and run its tools.

## Markdown-first is not Markdown-only

Markdown is where you start, not where the agent's power stops. The same single file can hold all of this:

| You want | Botter gives you |
|---|---|
| Multi-step work that happens in order, every time | **FWL** flows: plain sentences inside loops, branches and retries that an interpreter follows exactly |
| A team, not one agent | **Sub-agents**, each with its own persona, handed work by the flow or by the main agent |
| Real capabilities | **Tools** in any language, plus file, shell and scheduling tools built in |
| Other people's tools | **MCP** servers, unmodified: by URL, or started by command the way Claude Code and opencode run them |
| Other people's agents | **A2A**, both calling out and being called |
| Your agent as a tool for other agents | **MCP** again, the other way round: Claude Code, opencode or any MCP client can call it |
| The agent in your editor | **ACP**, for editors such as Zed |
| Safety on a real machine | A kernel **sandbox**, a network kill switch, and approval before anything destructive |
| Keys that travel with the agent | An optional password-**sealed vault** inside the file |

All of it fits in about 400 KB, in one process, with no framework to carry along.

## The part nobody talks about

Building an agent is a weekend. Shipping it is the rest of the month.

Every agent today drags its framework along to wherever it runs, and every framework drags an ecosystem behind *it*. So you wrap the whole thing in Docker, watch the image cross a gigabyte, and hope the next `pip install` doesn't break it. The agent is maybe a few hundred lines of real intent. Everything else is packaging.

Botter deletes the packaging.

## How small is small?

Same laptop, comparable agents:

| | Botter agent | OpenClaw agent | |
|---|---|---|---|
| On disk | **396 KB**, 1 file | 646 MB, 41,408 files | **~1,600× smaller** |
| Ready in | **0.4 ms** | 7.3 s | **~18,000× faster** |
| Idle memory | **0.4 MB** | 730 MB | **~1,800× lighter** |

<sub>OpenClaw 2026.9.9, default npm install (Node.js itself not counted), gateway start to accepting connections. Botter: a minimal built agent, start to first prompt. AMD Ryzen 5 7520U, Fedora 44.</sub>

A Botter agent boots more than thirty times faster than an *empty* Python process. `ldd` on it says `not a dynamic executable`. And if you still want a container, go ahead. It now holds your agent and whatever its tools need, and nothing else.

## One more thing: you can talk to the compiler

Every compiler you've ever used has the same personality. You hand it code, and it hands you back a list of everything you did wrong.

Botter talks back.

Run it in an empty folder and tell it what you want: *"an agent that finds local businesses without a website and pitches them my web design service."* It asks the two or three questions that matter. It proposes a plan. Then it writes the persona, the flow and the playbooks, writes the tools and tests them, lints its own work, has your model review the logic, and builds the binary. If you want to grab the wheel at any point, every file is plain text sitting right there in your folder.

How does a compiler do all that? **Botter is itself an agent.** Its persona, its flow and its skills are plain-text files in this repo (`agent.md`, `FLOW.fwl`, `skills/`), compiled by Botter and running on the same runtime as every agent it builds.

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

| File | What it is | Who can write it |
|---|---|---|
| `agent.md` | Persona and rules: who the agent is, what it must never do | Anyone |
| `FLOW.fwl` | What the agent does, step by step, in **FWL** | Anyone |
| `skills/<name>/SKILL.md` | Playbooks for specific tasks | Anyone |
| `agents/<name>/agent.md` | Sub-agents: a persona of their own for one kind of job | Anyone |
| `tools/` | Executables plus a JSON description of each | Anyone who can code a little |
| `config.json` | Name, version, internet on/off, what gets compiled in, MCP servers and remote agents | Botter, or you |

**Skills** follow the `SKILL.md` convention. The agent sees a one-line summary of each and opens the full playbook only when the task calls for it, so it can carry dozens without stuffing its context window.

**Sub-agents** live in the same file as the agent. Each gets its own persona and a fresh conversation, does one job with the agent's tools, and hands back only its result. A flow gives one work by naming it in a sentence, like `Have @researcher look into {lead}.`, and the agent can also delegate on its own. No second process, no second binary.

**What it needs.** If an agent relies on programs being installed, say `sqlite3` for a database or `python3` for a tool, they are listed under `requires` in `config.json`. The build warns when a tool needs something that isn't listed, and when the agent starts it checks the machine: the user is told what is missing, and so is the model, so it says "install sqlite3 first" instead of failing one command at a time.

**Tools** are any executable at all: Python, bash, Go, Rust, C. A tool gets one JSON object on stdin and prints its answer. That's the whole protocol. A tool brings its own needs with it, so a Python tool needs Python on the box and a static binary needs nothing. Users never see tool names or JSON. They see *"Searching for leads…"*.

## It speaks the standards

MCP, A2A and ACP are built into the runtime, in C. There is no bridge, no adapter and no second runtime to install: a Botter agent is still one file, and it starts nothing unless you tell it to.

- **MCP, by URL.** Name a server's URL in `config.json` and its tools join the agent's own. Each call is one HTTPS request from the agent's built-in client, using the stateless 2026 revision of the protocol, and falling back to the older session handshake for servers that still want it. Nothing is installed and nothing is started.
- **MCP, by command.** Most MCP servers are programs you start, like `npx -y @modelcontextprotocol/server-filesystem .`. Give the agent that same command and it starts the server itself, unmodified, inside the same sandbox as its tools, and stops it when it exits. The agent needs no Node or Python of its own; such a server needs whatever it was written in, so list that under `requires`.
- **MCP, being used.** `./myagent.bot --mcp` turns the agent into an MCP server with one tool, `ask`. Any MCP client that starts a command can use it: a coding agent such as Claude Code or opencode, an editor, another framework. The client starts the agent and talks to it over its input and output, so nothing listens. Give it a port (`--mcp 8080`) and it serves MCP over HTTP instead.
- **A2A, calling out.** List another agent's URL under `agents` and a flow hands it work by naming it `@billing` in a sentence, exactly like a sub-agent.
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

## FWL: plain English that runs the same way every time

Prompts are a terrible way to write a program. You say *"first do X, then Y, and if Z then stop"*, and the model nods and does something adjacent.

So Botter has a language. **FWL** (the Flow Writeup Language) is plain sentences inside a little structure. Only the structure is in CAPITALS; everything else is English.

```
STEP 1: Find leads
ASK "What do you sell, and to whom?" into {target}
RETRY 2 times:
    Find leads for {target} with $scrape_leads and keep them as {leads}.
IF {leads} is empty:
    RETURN "No leads found. Try a broader search."

STEP 2: Pitch each one
FOR EACH {lead} in {leads}:
    Write a pitch for {lead} as #pitching says and keep it as {pitch}.
    Have @reviewer check {pitch}, and fix what it finds.
    ASK "Send this to {lead.name}?\n\n{pitch}" [yes, no] into {send}
    IF {send} is "yes":
        Send {pitch} to {lead.email} with $send_email.
    ELSE:
        Skip this lead.

RETURN "Done."
```

That is the whole agent's behaviour, in a file called `FLOW.fwl`. Four marks say what a sentence is talking about:

| Mark | Means |
|---|---|
| `$send_email` | a tool |
| `#pitching` | a skill |
| `@reviewer` | an agent |
| `{lead}` | a remembered value |

Nothing is imported or declared. The first sentence that mentions a new `{value}` fills it, and the model decides what goes in.

### What it buys you: certainty

Botter follows the structure itself. The loop runs once per lead, the `IF` branches the way it says, and the steps happen in order. Each sentence is handed to the model on its own, so none is skipped and none is reached before its turn.

Two things are never left to the model:

- **`ASK` is asked.** Botter shows the question and waits. With choices, the answer is one of them, so the `IF {send} is "yes":` under it is decided exactly and the sending step is only reached on a yes.
- **A tool's answer is the tool's answer.** When a sentence names one tool and keeps its result, the value is exactly what the tool returned, not the model's retelling of it.

A condition that compares values, like `{send} is "yes"` or `{leads} is empty`, is decided exactly. One that needs judgement, like `IF the customer is asking about their own account:`, is one short question to the model.

### It refuses to build nonsense

FWL comes with a linter, and Botter runs it before every build. Break the flow above and you get this:

```
FLOW.fwl:15: error: ELSE lines up with its IF (the IF on line 13), not with the lines inside it
FLOW.fwl:18: error: {lead} only exists inside its FOR EACH loop
FLOW.fwl:11: warning: {pich} is filled here and never used again: a typo, or drop the name
FLOW.fwl:10: error: #pitchng: no such skill (there is no skills/pitchng/SKILL.md)
FLOW.fwl:11: error: @reviewr: no such agent (a sub-agent agents/reviewr/agent.md, or one listed in config.json "agents")
FLOW.fwl:14: error: $send_mail: no such tool (built-in, tools/bin/send_mail, or <server>_<tool> of a config.json "mcp" server)
```

Line numbers, indentation and scope checking, and every tool, skill and agent checked against what the agent actually has. These are compiler errors for a language that is mostly English.

### Then it gets a second opinion

A flow that builds can still make no sense, so before building, your model reviews the logic. Can each step actually be done with the tool it names? Is anything sent, paid for or deleted without an `ASK` first? Can every loop end? Does a value look like a typo of another? You get the line, the reason and a suggested fix. Nothing changes until you say yes.

### Data stays data

Values live in `{braces}`, never mixed into instructions, and the model is told outright that whatever a value holds is data. A scraped web page, an inbound email, anything. That makes prompt injection a lot harder to pull off.

## What's inside 396 KB

Botter doesn't generate code, and no compiler or linker runs during a build. Your files are appended, read-only, to **botcore**, the runtime, and sealed with a checksum. A build takes about **2 ms**. A truncated or tampered `.bot` refuses to start.

botcore is about 11,000 lines of C, statically linked against musl, and it packs a surprising amount into a few hundred kilobytes:

- an FWL parser, linter and interpreter
- its own HTTPS stack, using vendored BearSSL with Mozilla's CA roots compiled in, so no curl, no OpenSSL and no system certificates
- an OpenAI-compatible client with streaming and tool calling
- clients for MCP and A2A, and MCP, A2A and ACP front ends
- file, shell and scheduling tools
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
| `botter` itself (UI + runtime + packer) | **715 KB** | |
| A built agent | **396 KB** | |
| Build an agent | **2 ms** | 0.4 MB |
| Start an agent | **0.4 ms** | **0.4 MB** |
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
make test                     # 231 tests: FWL, sandbox, sub-agents, MCP, A2A, ACP, vault, HTTPS, TUI... (needs python3; tmux for the UI tests)
BOTTER_TEST_NET=1 make test   # adds real providers and TLS failure cases
make dist                     # release tarball + sha256
```

`tools/src/botcore` is the runtime, `tools/src/packer` is the packer that appends an agent's files to it, `tools/src/agent_tools` holds Botter's build tools, and `tui/` is Botter's full-screen UI. `agent.md`, `FLOW.fwl`, `config.json` and `skills/` in the repo root are Botter itself.

## License

MIT. botcore includes cJSON (MIT), BearSSL (MIT) and Mozilla's CA data (MPL-2.0). See `THIRD_PARTY.md`, and ship those notices with agents you build.

---

<div align="center">

**Describe it. Build it. Ship one file.**

</div>