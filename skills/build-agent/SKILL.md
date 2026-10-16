---
name: build-agent
description: How to validate, build and verify the .bot file with agent_build and agent_inspect, and how to read their messages. Load it before building.
---

# Building

Steps
0. Run every skill script once by hand with shell_exec on a real input, and read its output.
1. agent_build {"dry_run": true}  validate only. Fix every "error:" and "warning:" line, then repeat until it says "check passed" with 0 warnings.
2. agent_build {}                 build ./<project folder name>.bot in the working directory (or pass "output"). Build once, at the end, after 0 and 1 pass: every build costs time.
3. agent_inspect {"file": "<name>.bot"}   list the embedded files and runnable programs; confirm everything expected is there.
4. Tell the user to run it: ./<name>.bot

Arguments: "dir" is the project folder relative to the working directory (default ".", the working directory itself). All paths must stay inside the working directory.

Sealing keys (optional, before the build): vault_seal {} asks the user, directly and not through you, for the agent's keys (the LLM key; the token of each MCP server and remote agent in config.json; the token callers must send if the agent is served over A2A) and for a password, typed twice. It writes secrets.vault into the project and the build embeds it. A sealed agent asks only for that password at start and connects with the sealed keys; an agent without a vault asks whoever runs it for their own LLM key. Never ask for a key or a password in the conversation, never write one into a file, and never read or edit secrets.vault: only vault_seal handles them. Offer sealing for an agent the user runs themselves; advise against it for an agent they will hand to others (whoever knows the password has the keys). To change a key or the password, run vault_seal again and rebuild.

What the build does: copies the botcore runtime (the same engine botter runs on) and appends the project files to it. No compiler or linker runs. The result is one static x86-64 executable. Rebuilding replaces the old .bot.

## Functions
- "packer: functions: name (pure), other (reads files)": what each function's own code reaches. Check that it matches what you meant.
- "functions/x.lua:N: error: ...": a Lua syntax error, a parameter without ---@param, an unknown type, or a name another tool already has. The build stops; read write-functions.
- "functions/tests/x.lua: test test_y FAILED: ...": a test failed, so the function does not do what its test says. Fix the function (or the test, if the test was wrong) until "function tests: N passed, 0 failed".
- "FLOW.fwl is no longer used": move how the agent works into AGENT.md or a skill, and exact steps into functions/, then delete FLOW.fwl.

## Errors you may see
- "agent.md is now AGENT.md", "tools/bin is gone", "tools/doc is gone", "\"tools\" is gone": the old layout. Rename agent.md to AGENT.md; move each program into the scripts/ folder of the skill that uses it (see write-scripts) and remove "tools" from config.json.
- "skills/x/SKILL.md: \"name: y\" must be the folder's name": make name and folder the same.
- "the description has N characters": keep it under 1024: what the skill does and when to use it.
- "SKILL.md links to X, which is not in the skill": fix the link or add the file.
- "says \"TODO\"" (or "TBD", "FIXME"): finish that text.
- "ELF program but not for x86-64": wrong architecture; rebuild the program for x86-64.
- "refusing to overwrite ... not an executable": the output path points at an existing non-.bot file; choose another output name.
- "outside the working directory": use a path under the working directory.

## Warnings (build still succeeds)
- "dynamically linked": that program only runs where its shared libraries exist. Relink statically for a portable agent.
- "no AGENT.md": the agent will use a generic prompt. Write AGENT.md.
- "manifest.md is no longer used": delete it; the runtime lists skills itself.
- "neither SKILL.md nor anything it points to names this file": say in SKILL.md when to read that references/ file.
- "has no \"#!\" line": add one to the script, or tell the agent to run it through its interpreter.
- "SKILL.md has N lines": move details into references/ files.

## After building
- Running the .bot asks for a provider, API key and model each time and saves nothing.
- Scripts need their interpreter on the machine that runs the agent ("requires"); native static programs need nothing.
- The .bot can be copied to any x86-64 Linux machine.
