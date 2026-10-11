<div align="center">

# Botter

### The Markdown-first agent framework.<br>Every agent ships as one portable executable.

**396 KB** · **starts in 1.2 ms** · **nothing to install** · **runs on any x86-64 Linux**

</div>

---

Here's an agent:

```
lead-outreach/
  agent.md       who it is
  FLOW.md        what it does
  skills/        how it does it
  tools/         the jobs it can't do with words alone
```

And here's the same agent, ready to ship:

```
lead-outreach.bot      one file, under 400 KB
```

One file. No `requirements.txt`, no virtualenv, no Node version to match, no 1.2 GB container with LangChain buried somewhere inside. You copy it to a server and run it. It starts before you lift your finger off the Enter key.

That's Botter: a Markdown-first agent framework. You write an agent in plain Markdown, and Botter compiles it into a single statically linked binary with the whole runtime baked in. Nothing like LangChain or n8n has to be installed where the agent runs. It carries everything it needs to think, talk to a model and run its tools.

## Markdown-first is not Markdown-only

Markdown is where you start, not where the agent's power stops. The same single file can hold all of this:

| You want | Botter gives you |
|---|---|
| Multi-step work that runs the same way every time | **FWL** flows: loops, branches, retries and variables, run by an interpreter, with the model called only for judgement |
| A team, not one agent | **Sub-agents**, each with its own persona, handed work by the flow or by the main agent |
| Real capabilities | **Tools** in any language, plus file, shell and scheduling tools built in |
| Other people's tools | **MCP** servers, called over HTTPS with nothing started locally |
| Other people's agents | **A2A**, both calling out and being called |
| The agent in your editor | **ACP**, for editors such as Zed |
| Safety on a real machine | A kernel **sandbox**, a network kill switch, and approval before anything destructive |
| Keys that travel with the agent | An optional password-**sealed vault** inside the file |

All of it fits in under 400 KB, in one process, with no framework to carry along.

## The part nobody talks about

Building an agent is a weekend. Shipping it is the rest of the month.

Every agent today drags its framework along to wherever it runs, and every framework drags an ecosystem behind *it*. So you wrap the whole thing in Docker, watch the image cross a gigabyte, and hope the next `pip install` doesn't break it. The agent is maybe a few hundred lines of real intent. Everything else is packaging.

Botter deletes the packaging.

## How small is small?

Same laptop, comparable agents:

| | Botter agent | OpenClaw agent | |
|---|---|---|---|
| On disk | **396 KB**, 1 file | 646 MB, 41,408 files | **~1,600× smaller** |
| Ready in | **1.2 ms** | 7.3 s | **~6,000× faster** |
| Idle memory | **0.4 MB** | 730 MB | **~1,800× lighter** |

<sub>OpenClaw 2026.9.9, default npm install (Node.js itself not counted), gateway start to accepting connections. Botter: a minimal built agent, start to first prompt. AMD Ryzen 5 7520U, Fedora 44.</sub>

A Botter agent boots more than ten times faster than an *empty* Python process. `ldd` on it says `not a dynamic executable`. And if you still want a container, go ahead. It now holds your agent and whatever its tools need, and nothing else.

## One more thing: you can talk to the compiler

Every compiler you've ever used has the same personality. You hand it code, and it hands you back a list of everything you did wrong.

Botter talks back.

Run it in an empty folder and tell it what you want: *"an agent that finds local businesses without a website and pitches them my web design service."* It asks the two or three questions that matter. It proposes a plan. Then it writes the persona, the flow and the playbooks, writes the tools and tests them, lints its own work, has your model review the logic, and builds the binary. If you want to grab the wheel at any point, every file is plain text sitting right there in your folder.

How does a compiler do all that? **Botter is itself an agent.** Its persona, its flow and its skills are Markdown files in this repo, compiled by Botter and running on the same runtime as every agent it builds.

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
| `FLOW.md` | The program, written in **FWL** | Anyone |
| `skills/<name>/SKILL.md` | Playbooks for specific tasks | Anyone |
| `agents/<name>/agent.md` | Sub-agents: a persona of their own for one kind of job | Anyone |
| `tools/` | Executables plus a JSON description of each | Anyone who can code a little |
| `config.json` | Name, version, internet on/off, what gets compiled in, MCP servers and remote agents | Botter, or you |

**Skills** follow the `SKILL.md` convention. The agent sees a one-line summary of each and opens the full playbook only when the task calls for it, so it can carry dozens without stuffing its context window.

**Sub-agents** live in the same file as the agent. Each gets its own persona and a fresh conversation, does one job with the agent's tools, and hands back only its result. A flow gives one work with `DELEGATE TO @researcher USING context $lead`, and the agent can also delegate on its own. No second process, no second binary.

**Tools** are any executable at all: Python, bash, Go, Rust, C. A tool gets one JSON object on stdin and prints its answer. That's the whole protocol. A tool brings its own needs with it, so a Python tool needs Python on the box and a static binary needs nothing. Users never see tool names or JSON. They see *"Searching for leads…"*.

## It speaks the standards, and starts nothing

Most agent runtimes reach the outside world by launching things: a tool server here, a bridge there. A Botter agent is still one process.

- **MCP.** Name a server's URL in `config.json` and its tools join the agent's own. Each call is one HTTPS request from the agent's built-in client, using the stateless 2026 revision of the protocol, and falling back to the older session handshake for servers that still want it. Nothing is installed and nothing is started.
- **A2A, calling out.** List another agent's URL under `agents` and a flow hands it work with `DELEGATE TO @billing`, exactly like a sub-agent.
- **A2A, being called.** `./myagent.bot --a2a 8080` publishes an agent card and answers other agents. This is the only mode that listens, and only when you ask for it.
- **ACP.** `./myagent.bot --acp` is what an editor such as Zed runs to put the agent in its sidebar, with tool calls and permission prompts shown in the editor.

```json
{
  "mcp":    { "docs":    { "url": "https://example.com/mcp", "token": "DOCS_TOKEN" } },
  "agents": { "billing": { "url": "https://billing.example.com", "token": "BILLING_TOKEN" } }
}
```

`token` is only a name, never the secret itself. The value comes from the agent's sealed vault, or from a line of that name in a `.env` file next to where the agent runs. All four protocols together add about 16 KB to the binary.

## FWL: pseudocode that actually runs

Prompts are a terrible way to write a program. You say *"first do X, then Y, and if Z then stop"*, and the model nods and does something adjacent.

So Botter has a language. **FWL** (the Flow Writeup Language) is English for the steps, CAPITALS for the keywords, `$names` for the data and indentation for the blocks. A human can review it in a pull request. A model can read it without a manual.

```fwl
IMPORT:
    $pitching

STEP 1: GATHER
ASK USER "What do you sell, and to which kind of clients?"
SAVE answer INTO VARIABLE $target
RETRY UP TO 2 TIMES:
    EXECUTE tool $scrape_leads with payload { "query": $target, "limit": 20 }
SAVE result INTO VARIABLE $leads
IF $leads IS EMPTY:
    RETURN "No leads found. Try a broader client type or another city."

STEP 2: OUTREACH
FOR EACH $lead IN $leads:
    INVOKE SKILL $pitching USING context $lead
    SAVE skill_output INTO VARIABLE $pitch
    ASK USER "Send this message?\n\n" + $pitch
    IF $answer IS EQUAL TO "yes":
        EXECUTE tool $send_email with parameters:
           - to: $lead.email
           - body: $pitch
    ELSE IF the user asked for changes:
        Rewrite $pitch with the user's changes and send it.
```

Look at that `ELSE IF the user asked for changes:`. It's a condition written in English, sitting inside a real control structure, and that's the whole idea.

### It refuses to build nonsense

FWL comes with a linter, and Botter runs it before every build. Break the flow above and you get this:

```
FLOW.md:15: error: this line opens a block, but nothing is indented under it (indent the lines that belong to it)
FLOW.md:20: error: unknown keyword INVOCATE; did you mean INVOKE?
FLOW.md:27: error: ELSE lines up with its IF (the IF on line 23), not with the lines inside it
```

Fix the shape, and it checks the names:

```
FLOW.md:24: error: EXECUTE tool $send_mail: no such tool (built-in, or tools/bin/send_mail with tools/doc/send_mail.json)
FLOW.md:26: error: $pich is used before it is set (SAVE ... INTO VARIABLE $pich, SET $pich TO ..., or FOR EACH $pich IN ...)
FLOW.md:29: error: $lead only exists inside its FOR EACH loop
```

Line numbers, a typo suggestion, indentation and scope checking, and tools and skills checked against what's actually in the folder. These are compiler errors for a language that's mostly English.

### Then it gets a second opinion

Valid syntax doesn't mean the flow makes sense, so before building, your model reviews the logic. Can each step actually be done with the tool it names? Does the payload fit that tool's parameters? Does anything get sent, paid for or deleted without an `ASK USER` first? Can every loop end? You get the line, the reason and a suggested fix. Nothing changes until you say yes.

### The model only shows up when it's needed

At run time, an FWL interpreter inside the binary executes the flow one statement at a time. Tool calls, questions, variables, loops, retries and comparisons all run as plain code. No tokens, no guessing. The model is called only for the parts that need judgement: English instructions, skills, and conditions like *"the user asked for changes"*.

The result: a test flow with a question, a tool call, a loop, a branch, a retry and a file write made **two LLM calls, total**. It's cheaper and faster, and the model can't quietly skip step 3.

### Data stays data

Values live in `$variables`, never mixed into instructions, and the model is told outright that whatever a variable holds is data. A scraped web page, an inbound email, anything. That makes prompt injection a lot harder to pull off.

A flow can hand work to sub-agents with `DELEGATE TO`. Running them side by side is next.

## What's inside 396 KB

Botter doesn't generate code, and no compiler or linker runs during a build. Your files are appended, read-only, to **botcore**, the runtime, and sealed with a checksum. A build takes **2.7 ms**. A truncated or tampered `.bot` refuses to start.

botcore is about 12,000 lines of C11, statically linked against musl, and it packs a surprising amount into a few hundred kilobytes:

- an FWL parser, linter and interpreter
- its own HTTPS stack, using vendored BearSSL with Mozilla's CA roots compiled in, so no curl, no OpenSSL and no system certificates
- an OpenAI-compatible client with streaming and tool calling
- clients for MCP and A2A, and ACP and A2A front ends
- file, shell and scheduling tools
- a sandbox enforced by the kernel itself

## Paranoid by default

An agent runs commands on your machine. Botter assumes that's dangerous and acts like it.

- **Landlock sandbox.** Every tool and shell command can write to the working folder and `/tmp`, and that's it. Your `~/.ssh`, your tokens and the rest of your home directory are invisible to it. `rm`, overwrites, `git push` and `sudo` all ask first.
- **Kill switch for the network.** Set `"internet": false` and the kernel cuts the agent's tools off the network. The model connection still works.
- **Plan mode.** Hit Tab. The kernel makes the folder read-only, and the agent can look and think but not touch.
- **No phoning home.** No telemetry, no accounts, no update checks. One connection: the model *you* picked.
- **Leaves no trace.** No logs, no history, no config files. The conversation lives in RAM. Your API key is never written to disk, never passed through environment variables or arguments, sits in locked memory and gets wiped on exit.

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

AMD Ryzen 5 7520U, Fedora 44, median of 15 to 30 runs.

| | Time / size | Peak memory |
|---|---|---|
| `botter` itself (UI + runtime + compiler) | **726 KB** | |
| A built agent | **396 KB** | |
| Build an agent | **2.7 ms** | 0.4 MB |
| Start an agent | **1.2 ms** | **0.4 MB** |
| Full session: connect, one turn with a tool call, exit | 134 ms | **3.8 MB** |
| *For scale:* `python3 -c pass` | 18.9 ms | 9.2 MB |
| *For scale:* `node -e 0` | 27.0 ms | 43.7 MB |

Those last two rows are interpreters doing *nothing*, before a single framework import.

## Where it's going

- **Parallel sub-agents.** Sub-agents work one at a time today; `IN PARALLEL` will run them side by side.
- **Streaming over A2A.** An agent called over A2A answers each message whole today.
- **Windows (WSL) and macOS.** Same single file, more places to run it.

## Hacking on Botter

```sh
make                          # builds ./botter; needs gcc and musl-gcc (Fedora: dnf install musl-gcc musl-libc-static)
make test                     # 209 tests: FWL, sandbox, sub-agents, MCP, A2A, ACP, vault, HTTPS, TUI...
BOTTER_TEST_NET=1 make test   # adds real providers and TLS failure cases
make dist                     # release tarball + sha256
```

`tools/src/botcore` is the runtime, `tools/src/pack` is the packer, and `tui/` is Botter's full-screen UI. `agent.md`, `FLOW.md` and `skills/` are Botter itself.

## License

MIT. botcore includes cJSON (MIT), BearSSL (MIT) and Mozilla's CA data (MPL-2.0). See `THIRD_PARTY.md`, and ship those notices with agents you build.

---

<div align="center">

**Describe it. Build it. Ship one file.**

</div>