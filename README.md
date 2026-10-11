<div align="center">

# Botter

### Write your agent in Markdown. Ship it as one tiny file.

**356 KB** · **starts in 1.2 ms** · **no framework** · **runs on any x86-64 Linux**

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
$ ls -lh lead-outreach.bot
-rwxr-xr-x  1 you  you  356K  lead-outreach.bot
```

One file. No `requirements.txt`, no virtualenv, no Node version to match, no 1.2 GB container with LangChain buried somewhere inside. You copy it to a server and run it. It starts before you lift your finger off the Enter key.

That's Botter. You write agents in plain Markdown, and Botter compiles them into a single statically linked binary with the whole runtime baked in. Frameworks like LangChain and n8n are simply not needed. The agent carries everything it needs to think, talk to a model and run its tools.

## The part nobody talks about

Building an agent is a weekend. Shipping it is the rest of the month.

Every agent today drags a framework behind it, and every framework drags an ecosystem behind *it*. So you wrap the whole thing in Docker, watch the image cross a gigabyte, and hope the next `pip install` doesn't break it. The agent is maybe a few hundred lines of real intent. Everything else is packaging.

Botter deletes the packaging.

## How small is small?

Same laptop, comparable agents:

| | Botter agent | OpenClaw agent | |
|---|---|---|---|
| On disk | **356 KB**, 1 file | 646 MB, 41,408 files | **~1,800× smaller** |
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
make                 # needs gcc and musl-gcc (Fedora: dnf install musl-gcc musl-libc-static)
mkdir myagent && cd myagent
../botter            # pick a model, paste your key, describe your agent
./myagent.bot        # run it, or scp it to any x86-64 Linux box and run it there
```

Bring your own model: **Gemini, OpenAI, OpenRouter, Groq, Ollama** for local models, or any OpenAI-compatible endpoint. You type your key once. It lives in the kernel keyring, in RAM, until the machine reboots, which is handy for agents that live on servers.

---

## Anatomy of an agent

| File | What it is | Who can write it |
|---|---|---|
| `agent.md` | Persona and rules: who the agent is, what it must never do | Anyone |
| `FLOW.md` | The program, written in **Owl** | Anyone |
| `skills/<name>/SKILL.md` | Playbooks for specific tasks | Anyone |
| `tools/` | Executables plus a JSON description of each | Anyone who can code a little |
| `config.json` | Name, version, internet on/off, what gets compiled in | Botter, or you |

**Skills** follow the `SKILL.md` convention. The agent sees a one-line summary of each and opens the full playbook only when the task calls for it, so it can carry dozens without stuffing its context window.

**Tools** are any executable at all: Python, bash, Go, Rust, C. A tool gets one JSON object on stdin and prints its answer. That's the whole protocol. A tool brings its own needs with it, so a Python tool needs Python on the box and a static binary needs nothing. Users never see tool names or JSON. They see *"Searching for leads…"*.

## Owl: pseudocode that actually runs

Prompts are a terrible way to write a program. You say *"first do X, then Y, and if Z then stop"*, and the model nods and does something adjacent.

So Botter has a language. **Owl** (the Orchestration Writeup Language) is English for the steps and CAPITALS for the structure. A human can review it in a pull request. A model can read it without a manual.

```owl
STEP 1: GATHER
ASK USER "What do you sell, and to which kind of clients?"
SAVE answer INTO VARIABLE `target`
RETRY UP TO 2 TIMES DO
    EXECUTE tool `scrape_leads` with payload { "query": `target`, "limit": 20 }
END RETRY
SAVE result INTO VARIABLE `leads`
IF `leads` IS EMPTY THEN
    RETURN "No leads found. Try a broader client type or another city."
END IF

STEP 2: OUTREACH
FOR EACH `lead` IN `leads` DO
    INVOKE SKILL "pitching" USING context `lead`
    SAVE skill_output INTO VARIABLE `pitch`
    ASK USER "Send this message?\n\n" + `pitch`
    IF `answer` IS EQUAL TO "yes" THEN
        EXECUTE tool `send_email` with parameters:
           - to: `lead.email`
           - body: `pitch`
    ELSE IF the user asked for changes THEN
        Rewrite `pitch` with the user's changes and send it.
    END IF
END FOR
```

Look at that `ELSE IF the user asked for changes`. It's a condition written in English, sitting inside a real control structure, and that's the whole idea.

### It refuses to build nonsense

Owl comes with a linter, and Botter runs it before every build. Break the flow above and you get this:

```
FLOW.md:22: error: unknown keyword INVOCATE; did you mean INVOKE?
FLOW.md:26: error: EXECUTE tool `send_mail`: no such tool
FLOW.md:28: error: `pich` is used before it is set
FLOW.md:31: error: END FOR, but the IF opened on line 25 is still open
FLOW.md:35: error: `lead` only exists inside its FOR EACH loop
```

Line numbers, a typo suggestion, scope checking, and tools and skills checked against what's actually in the folder. These are compiler errors for a language that's mostly English.

### Then it gets a second opinion

Valid syntax doesn't mean the flow makes sense, so before building, your model reviews the logic. Can each step actually be done with the tool it names? Does the payload fit that tool's parameters? Does anything get sent, paid for or deleted without an `ASK USER` first? Can every loop end? You get the line, the reason and a suggested fix. Nothing changes until you say yes.

### The model only shows up when it's needed

At run time, an Owl interpreter inside the binary executes the flow one statement at a time. Tool calls, questions, variables, loops, retries and comparisons all run as plain code. No tokens, no guessing. The model is called only for the parts that need judgement: English instructions, skills, and conditions like *"the user asked for changes"*.

The result: a test flow with a question, a tool call, a loop, a branch, a retry and a file write made **two LLM calls, total**. It's cheaper and faster, and the model can't quietly skip step 3.

### Data stays data

Values live in `` `variables` ``, never mixed into instructions, and the model is told outright that whatever a variable holds is data. A scraped web page, an inbound email, anything. That makes prompt injection a lot harder to pull off.

Owl handles the flow of a single agent today. Teams of agents are next.

## What's inside 356 KB

Botter doesn't generate code, and no compiler or linker runs during a build. Your files are appended, read-only, to **botcore**, the runtime, and sealed with a checksum. A build takes **2.7 ms**. A truncated or tampered `.bot` refuses to start.

botcore is about 9,000 lines of C11, statically linked against musl, and it packs a surprising amount into a few hundred kilobytes:

- an Owl parser, linter and interpreter
- its own HTTPS stack, using vendored BearSSL with Mozilla's CA roots compiled in, so no curl, no OpenSSL and no system certificates
- an OpenAI-compatible client with streaming and tool calling
- file, shell and scheduling tools
- a sandbox enforced by the kernel itself

## Paranoid by default

An agent runs commands on your machine. Botter assumes that's dangerous and acts like it.

- **Landlock sandbox.** Every tool and shell command can write to the working folder and `/tmp`, and that's it. Your `~/.ssh`, your tokens and the rest of your home directory are invisible to it. `rm`, overwrites, `git push` and `sudo` all ask first.
- **Kill switch for the network.** Set `"internet": false` and the kernel cuts the agent's tools off the network. The model connection still works.
- **Plan mode.** Hit Tab. The kernel makes the folder read-only, and the agent can look and think but not touch.
- **No phoning home.** No telemetry, no accounts, no update checks. One connection: the model *you* picked.
- **Leaves no trace.** No logs, no history, no config files. The conversation lives in RAM. Your API key is never written to disk, never passed through environment variables or arguments, sits in locked memory and gets wiped on exit.

## The numbers

AMD Ryzen 5 7520U, Fedora 44, median of 15 to 30 runs.

| | Time / size | Peak memory |
|---|---|---|
| `botter` itself (UI + runtime + compiler) | **638 KB** | |
| A built agent | **356 KB** | |
| Build an agent | **2.7 ms** | 0.4 MB |
| Start an agent | **1.2 ms** | **0.4 MB** |
| Full session: connect, one turn with a tool call, exit | 134 ms | **3.8 MB** |
| *For scale:* `python3 -c pass` | 18.9 ms | 9.2 MB |
| *For scale:* `node -e 0` | 27.0 ms | 43.7 MB |

Those last two rows are interpreters doing *nothing*, before a single framework import.

## Where it's going

- **Multi-agent Owl.** Flows that coordinate a whole team of agents, not just one.
- **MCP.** Plug agents into Model Context Protocol servers. `CONNECT` is already a reserved word.
- **ACP.** Run Botter agents inside your code editor through the Agent Client Protocol.
- **A2A.** Let Botter agents talk to other agents, whatever they were built with.
- **Windows (WSL) and macOS.** Same single file, more places to run it.

## Hacking on Botter

```sh
make test                     # 114 tests: Owl, sandbox, streaming, HTTPS, key cache, TUI...
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