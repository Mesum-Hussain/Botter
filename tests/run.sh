#!/bin/sh
# botter / botcore test suite.   make test      (or: sh tests/run.sh)
#
# Needs: a built tree (make), python3, musl-gcc. Uses a throwaway temp dir.
# Optional: BOTTER_TEST_NET=1 adds internet tests (LLM providers, badssl.com);
#           tmux enables the Ctrl-C test.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TESTS=$ROOT/tests
PACK=$ROOT/artifacts/packer
CORE=$ROOT/artifacts/botcore
BOTTER=$ROOT/botter
T=$(mktemp -d "${TMPDIR:-/tmp}/botter-test.XXXXXX")
PASS=0 FAIL=0 SKIP=0
STUB= REMOTE= A2A=
# Never touch the user's real key cache; the keyring tests use their own namespace.
export BOTCORE_NO_KEYRING=1
# own tmux server: a running one would hand its sessions its own environment (e.g. a real saved key)
tmux() { command tmux -L botter-test-$$ "$@"; }
KRNS=botter-test-$$

cleanup() {
    [ -n "$STUB" ] && kill "$STUB" 2>/dev/null
    [ -n "$REMOTE" ] && kill "$REMOTE" 2>/dev/null
    [ -n "$A2A" ] && kill "$A2A" 2>/dev/null
    tmux kill-server 2>/dev/null
    rm -f "$HOME/.botter_test_probe"
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

pass() { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ $# -gt 1 ] && printf '       %s\n' "$2"; }
skip() { SKIP=$((SKIP + 1)); printf '  \033[33mSKIP\033[0m %s (%s)\n' "$1" "$2"; }
# check NAME FILE PATTERN: pass if FILE matches the extended regex PATTERN
check() { if grep -Eq -- "$3" "$2" 2>/dev/null; then pass "$1"; else fail "$1" "expected /$3/ in: $(head -c 300 "$2" 2>/dev/null | tr '\n' ' ')"; fi; }
check_not() { if grep -Eq -- "$3" "$2" 2>/dev/null; then fail "$1" "unexpected /$3/"; else pass "$1"; fi; }
section() { printf '\n\033[1m%s\033[0m\n' "$1"; }

for f in "$PACK" "$CORE" "$BOTTER"; do
    [ -x "$f" ] || { echo "missing $f: run make first"; exit 2; }
done

# stub_start ARGS...: start the LLM stub, set PORT; records go to $T/rec
stub_start() {
    stub_stop
    rm -rf "$T/rec" "$T/port"
    python3 "$TESTS/stub_llm.py" --portfile "$T/port" --record "$T/rec" "$@" &
    STUB=$!
    i=0
    while [ ! -s "$T/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    PORT=$(cat "$T/port")
}
stub_stop() { [ -n "$STUB" ] && { kill "$STUB" 2>/dev/null; wait "$STUB" 2>/dev/null; }; STUB=; }
# tool_call TOOL JSON: stub that asks for one tool call
tool_call() { stub_start --tool "$1" --args "$2"; }
# shell_call COMMAND: stub that asks for shell_exec(COMMAND)
shell_call() { tool_call shell_exec "$(python3 -c 'import json,sys;print(json.dumps({"command":sys.argv[1]}))' "$1")"; }
# drive BOT WORKDIR [ENV...]: connect to the stub, send one message, quit. Output in $T/out.
drive() {
    bot=$1 wd=$2
    shift 2
    (cd "$wd" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\n/exit\n' "$PORT" |
        timeout 60 env "$@" "$bot" >"$T/out" 2>&1)
}
# mkagent DIR [config.json content]: a minimal agent project
mkagent() {
    mkdir -p "$1"
    echo "You are a test agent." >"$1/agent.md"
    [ $# -gt 1 ] && printf '%s\n' "$2" >"$1/config.json"
    return 0
}
build() { "$PACK" build "$1" "$CORE" "$2" >"$T/build.log" 2>&1; }

# ---------------------------------------------------------------------------
section "Packaging"

"$PACK" check "$ROOT" "$CORE" >"$T/self.log" 2>&1
check "botter's own project builds with 0 warnings" "$T/self.log" "check passed.* 0 warning"

# the flow languages are gone: an old flow file says what to do instead and stops the build
for old in FLOW.fwl FLOW.md SQNC.md; do
    mkagent "$T/flow_old"
    rm -f "$T/flow_old/"*.fwl "$T/flow_old/"FLOW.md "$T/flow_old/"SQNC.md "$T/flow_old.bot"
    echo 'RETURN "x"' >"$T/flow_old/$old"
    build "$T/flow_old" "$T/flow_old.bot"
    check "an old $old is reported" "$T/build.log" "$old is no longer used: write how the agent works in agent.md"
    [ -e "$T/flow_old.bot" ] && fail "an old $old stops the build"
done

# config.json: checked by the build, shown by list, --version, the start-up line and the system prompt
mkagent "$T/meta" '{"name": "meta-agent", "display_name": "Meta Agent", "version": "1.2.3", "description": "Tests metadata."}'
build "$T/meta" "$T/meta.bot"
check_not "config.json: a valid config builds without config.json warnings" "$T/build.log" "config.json"
"$PACK" list "$T/meta.bot" >"$T/list.log" 2>&1
check "config.json: list shows it" "$T/list.log" "^version +1\.2\.3"
"$T/meta.bot" --version >"$T/ver.log" 2>&1
check "config.json: --version prints the agent's name and version" "$T/ver.log" "^meta-agent 1\.2\.3"
# cfg_bad NAME JSON PATTERN: building with this config.json must report PATTERN
cfg_bad() {
    mkagent "$T/cfg_bad" "$2"
    rm -f "$T/cfg_bad.bot"
    "$PACK" build "$T/cfg_bad" "$CORE" "$T/cfg_bad.bot" >"$T/build.log" 2>&1
    check "config.json: $1" "$T/build.log" "$3"
}
cfg_bad "a version that is not MAJOR.MINOR.PATCH is an error" '{"name": "a", "version": "1.0"}' 'version "1\.0" is not MAJOR\.MINOR\.PATCH'
if [ -e "$T/cfg_bad.bot" ]; then fail "config.json: an invalid config is not built"; else pass "config.json: an invalid config is not built"; fi
cfg_bad "a bad name is an error" '{"name": "My Agent", "version": "1.0.0"}' 'name "My Agent" must be'
cfg_bad "invalid JSON is an error" '{"name": "a", "version": }' 'not a valid JSON object'
cfg_bad "a wrong type is an error" '{"name": "a", "version": "1.0.0", "internet": "yes"}' '"internet" must be true or false'
cfg_bad "an unknown key is a warning" '{"name": "a", "version": "1.0.0", "author": "x"}' 'warning: config.json: unknown key "author"'
cfg_bad "missing name, version, description are warnings" '{"internet": true}' 'warning: config.json: no "name"'
cfg_bad "requires takes command names" '{"name": "a", "version": "1.0.0", "requires": ["rm -rf"]}' '"requires" must be a list of names'
cfg_bad "a listed skill must exist" '{"name": "a", "version": "1.0.0", "skills": ["ghost"]}' 'skill "ghost" is listed but there is no skills/ghost/SKILL.md'
mkagent "$T/cfg_old"
echo '{}' >"$T/cfg_old/agent.json"
"$PACK" check "$T/cfg_old" "$CORE" >"$T/build.log" 2>&1
check "config.json: an old agent.json is reported" "$T/build.log" "agent.json is now config.json"
# skills / tools lists choose what is compiled in
mkagent "$T/pick" '{"name": "pick", "version": "1.0.0", "description": "d", "skills": ["keep"], "tools": ["tkeep"], "requires": ["python3"]}'
mkdir -p "$T/pick/skills/keep" "$T/pick/skills/drop" "$T/pick/tools/bin" "$T/pick/tools/doc"
for k in keep drop; do printf -- '---\nname: %s\ndescription: d\n---\n' $k >"$T/pick/skills/$k/SKILL.md"; done
for t in tkeep tdrop; do printf '#!/usr/bin/env python3\nimport sys; print(sys.stdin.read())\n' >"$T/pick/tools/bin/$t"; chmod +x "$T/pick/tools/bin/$t"; echo '{"description": "d", "parameters": {"type": "object", "properties": {}}}' >"$T/pick/tools/doc/$t.json"; done
build "$T/pick" "$T/pick.bot"
"$PACK" list "$T/pick.bot" >"$T/list.log" 2>&1
check "config.json skills: a listed skill is compiled in" "$T/list.log" "skills/keep/SKILL.md"
check_not "config.json skills: an unlisted skill is left out" "$T/list.log" "skills/drop/"
check "config.json tools: a listed tool is compiled in" "$T/list.log" "tools/bin/tkeep$"
check_not "config.json tools: an unlisted tool and its doc are left out" "$T/list.log" "tdrop"
check "config.json: leaving one out is a warning" "$T/build.log" 'skills "drop" is not listed in config.json "skills"'
mkagent "$T/needpy" '{"name": "needpy", "version": "1.0.0", "description": "d"}'
mkdir -p "$T/needpy/tools/bin" "$T/needpy/tools/doc"
printf '#!/usr/bin/env python3\nimport sys; print(sys.stdin.read())\n' >"$T/needpy/tools/bin/pyt"; chmod +x "$T/needpy/tools/bin/pyt"
echo '{"description": "d", "parameters": {"type": "object", "properties": {}}}' >"$T/needpy/tools/doc/pyt.json"
"$PACK" check "$T/needpy" "$CORE" >"$T/build.log" 2>&1
check "config.json: a script tool's interpreter missing from requires is a warning" "$T/build.log" 'runs with python3: add "python3" to "requires"'
# contracts the build checks: descriptors, skill frontmatter, tool scripts
mkagent "$T/ctr" '{"name": "ctr", "version": "1.0.0", "description": "d", "requires": ["python3"]}'
mkdir -p "$T/ctr/tools/bin" "$T/ctr/tools/doc" "$T/ctr/skills/bare"
echo "# Bare skill" >"$T/ctr/skills/bare/SKILL.md"
printf '#!/usr/bin/env python3\nimport sys\nprint("mock leads for " + sys.argv[1])\n' >"$T/ctr/tools/bin/argv"; chmod +x "$T/ctr/tools/bin/argv"
echo '{"name": "argv", "description": "d", "input": "keyword"}' >"$T/ctr/tools/doc/argv.json"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check "contracts: a descriptor without parameters is an error" "$T/build.log" 'error: tools/doc/argv.json: no "parameters"'
check "contracts: unknown descriptor keys are a warning" "$T/build.log" 'warning: tools/doc/argv.json: unknown key "input"'
check "contracts: a skill without frontmatter is an error" "$T/build.log" 'error: skills/bare/SKILL.md has no frontmatter'
check "contracts: a tool that reads argv is a warning" "$T/build.log" 'tools/bin/argv reads command-line arguments'
check "contracts: a tool that does not read stdin is a warning" "$T/build.log" 'tools/bin/argv does not seem to read stdin'
check "contracts: mock output in a tool is a warning" "$T/build.log" 'tools/bin/argv mentions "mock"'
printf '#!/bin/sh\n# adapted from artifacts/kit/scrape.py (MIT)\ncat >/dev/null\npython3 artifacts/kit/scrape.py\n' >"$T/ctr/tools/bin/uses_art"
echo '{"description": "d", "parameters": {"type": "object", "properties": {}}}' >"$T/ctr/tools/doc/uses_art.json"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check "contracts: a tool that runs code from artifacts/ is an error" "$T/build.log" 'error: tools/bin/uses_art uses files under artifacts/'
printf '#!/bin/sh\n# adapted from artifacts/kit/scrape.py (MIT)\ncat\n' >"$T/ctr/tools/bin/uses_art"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check_not "contracts: a comment crediting artifacts/ is fine" "$T/build.log" 'uses_art uses files under artifacts/'
printf '#!/usr/bin/env python3\nimport sys, time\nsys.stdin.read()\ntime.sleep(1)\n' >"$T/ctr/tools/bin/poller"
echo '{"description": "d", "parameters": {"type": "object", "properties": {}}}' >"$T/ctr/tools/doc/poller.json"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check "contracts: a tool that waits with the default timeout is a warning" "$T/build.log" 'tools/bin/poller waits \(sleep\)'

# ---------------------------------------------------------------------------
section "Runtime: network modes, sandbox, system prompt (stub LLM)"

mkagent "$T/on" && build "$T/on" "$T/on.bot"
mkagent "$T/off" '{"internet": false}' && build "$T/off" "$T/off.bot"
mkdir -p "$T/ws"

shell_call "echo hello-from-tool"
drive "$T/on.bot" "$T/ws" PATH=/usr/bin:/bin
check "tool loop: shell_exec runs and its output reaches the LLM" "$T/rec/tool.txt" "hello-from-tool"
shell_call "# it's only a comment
echo comment-ok"
drive "$T/on.bot" "$T/ws"
check "classifier: an apostrophe in a # comment does not make a command unsafe" "$T/rec/tool.txt" "comment-ok"
check "online agent: system prompt says internet access" "$T/rec/system.txt" "You have internet access"

shell_call "python3 -c \"import socket;socket.create_connection(('127.0.0.1',$PORT));print('NET-OK')\""
drive "$T/on.bot" "$T/ws"
check "online agent: shell commands reach the network" "$T/rec/tool.txt" "NET-OK"

shell_call "curl -s http://127.0.0.1:1/"
drive "$T/off.bot" "$T/ws"
check "offline agent: network commands are refused" "$T/rec/tool.txt" "blocked: the agent is offline-only"
check "offline agent: system prompt says offline" "$T/rec/system.txt" "You are OFFLINE"

shell_call "python3 -c \"import socket;socket.create_connection(('127.0.0.1',$PORT))\" 2>&1 | tail -1"
drive "$T/off.bot" "$T/ws"
check "offline agent: sockets fail in the kernel, the LLM still works" "$T/rec/tool.txt" "Network is unreachable|Permission denied"

shell_call "echo inside > inside.txt && cat inside.txt"
drive "$T/on.bot" "$T/ws"
if grep -q "Landlock unavailable" "$T/out"; then
    skip "Landlock tests" "kernel has no Landlock"
else
    check "Landlock: writing inside the working directory works" "$T/rec/tool.txt" "^inside"
    shell_call "python3 -c \"open('$HOME/.botter_test_probe','w')\" 2>&1 | tail -1"
    drive "$T/on.bot" "$T/ws"
    check "Landlock: writing to \$HOME is denied" "$T/rec/tool.txt" "Permission denied"
    shell_call "python3 -c \"import os;os.listdir(os.path.expanduser('~'))\" 2>&1 | tail -1"
    drive "$T/on.bot" "$T/ws"
    check "Landlock: listing \$HOME is denied" "$T/rec/tool.txt" "Permission denied"
    shell_call "python3 -c \"import tempfile;f=tempfile.NamedTemporaryFile();f.write(b'x');print('TMP-OK')\""
    drive "$T/on.bot" "$T/ws"
    check "Landlock: /tmp stays writable" "$T/rec/tool.txt" "TMP-OK"

    # Plan mode: drive with /plan typed before the message
    plan_drive() { (cd "$2" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\n/plan\ngo\n/exit\n' "$PORT" |
        timeout 60 "$1" >"$T/out" 2>&1); }
    shell_call "echo planned > plan.txt; ls plan.txt 2>&1 | tail -1"
    plan_drive "$T/on.bot" "$T/ws"
    check "Plan mode: shell commands cannot write in the working directory" "$T/rec/tool.txt" "Permission denied|Read-only"
    if [ -e "$T/ws/plan.txt" ]; then fail "Plan mode: no file was created"; else pass "Plan mode: no file was created"; fi
    check "Plan mode: the model is told the mode" "$T/rec/user.txt" "^\\[Mode: PLAN"
    tool_call fs_write '{"path":"plan2.txt","content":"x"}'
    plan_drive "$T/on.bot" "$T/ws"
    check "Plan mode: fs_write is refused" "$T/rec/tool.txt" "PLAN MODE"
    echo READ-OK >"$T/ws/readme.txt"
    shell_call "cat readme.txt"
    plan_drive "$T/on.bot" "$T/ws"
    check "Plan mode: read-only commands still run" "$T/rec/tool.txt" "READ-OK"
fi

# Streaming (Server-Sent Events) and what the user sees
stub_start --think
drive "$T/on.bot" "$T/ws"
check "streaming: the request asks for a stream" "$T/rec/stream.txt" "^stream"
check "streaming: the streamed answer is printed" "$T/out" "Hello there, streamed reply\\."
check_not "streaming: <think> reasoning is not printed as the answer" "$T/out" "pondering|<think>"
shell_call "echo split-args-ok"
drive "$T/on.bot" "$T/ws"
check "streaming: tool call arguments split over chunks are joined" "$T/rec/tool.txt" "split-args-ok"
check "streaming: provider extras in tool calls are kept (Gemini thought_signature)" "$T/rec/calls.txt" "sig-0"
check "user view: a tool shows what it does in plain words" "$T/out" "Running a command"
check_not "user view: tool names are not shown" "$T/out" "shell_exec"
stub_start --think --nostream
drive "$T/on.bot" "$T/ws"
check "streaming: an endpoint that refuses streams still works (plain fallback)" "$T/out" "Hello there, streamed reply\\."
check "streaming: the fallback request is plain" "$T/rec/stream.txt" "^plain"
check_not "streaming fallback: reasoning is not printed" "$T/out" "pondering|<think>"
stub_start --think --busy 2
drive "$T/on.bot" "$T/ws"
check "busy endpoint: a 503 (high demand) is retried and the answer arrives" "$T/out" "Hello there, streamed reply\\."
check_not "busy endpoint: no error is shown when a retry succeeds" "$T/out" "HTTP 503"

# Session key cache (kernel keyring, own namespace): 2nd run reconnects without asking
if [ -r /proc/keys ]; then
    shell_call "python3 -c \"import ctypes,os;l=ctypes.CDLL(None,use_errno=True);r=l.syscall(250,0,0,0,0,0);print('KEYCTL',r,os.strerror(ctypes.get_errno()))\""
    (cd "$T/ws" && printf '6\nhttp://127.0.0.1:%s/v1\nsk-cache-test\nstub\ngo\n/exit\n' "$PORT" |
        timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: tool children cannot use the keyring syscalls" "$T/rec/tool.txt" "KEYCTL -1 Function not implemented"
    (cd "$T/ws" && printf 'go\n/exit\n' | timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: the next run reconnects without asking" "$T/out" "saved for this session"
    check_not "key cache: no provider question on the next run" "$T/out" "Select an OpenAI-compatible provider"
    check "key cache: the saved key is sent" "$T/rec/auth.txt" "sk-cache-test"
    (cd "$T/ws" && printf '/forget\n/exit\n' | timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: /forget removes the saved entries" "$T/out" "Forgot 3 saved"
    (cd "$T/ws" && printf '' | timeout 60 env -u BOTCORE_NO_KEYRING BOTCORE_KEYRING_NS=$KRNS "$T/on.bot" >"$T/out" 2>&1)
    check "key cache: after /forget the provider is asked again" "$T/out" "Select an OpenAI-compatible provider"
else
    skip "key cache tests" "kernel has no keyring"
fi

mkagent "$T/skilled"
mkdir -p "$T/skilled/skills/pitching"
printf -- '---\nname: pitching\ndescription: Write one specific idea per lead. Read before pitching.\n---\n# Pitching\n' >"$T/skilled/skills/pitching/SKILL.md"
echo "# stale" >"$T/skilled/manifest.md"
build "$T/skilled" "$T/skilled.bot"
check "leftover manifest.md is reported and not embedded" "$T/build.log" "manifest.md is no longer used"
stub_start --text /dev/null
drive "$T/skilled.bot" "$T/ws"
check "system prompt lists skills with their frontmatter descriptions" "$T/rec/system.txt" "skills/pitching/SKILL.md: Write one specific idea per lead"

stub_start --text /dev/null
drive "$T/meta.bot" "$T/ws"
check "config.json: the agent shows its display name and version at start" "$T/out" "Meta Agent.*1\.2\.3.*Tests metadata"
check "config.json: the model knows its name and version" "$T/rec/system.txt" 'You are the agent "Meta Agent", version 1\.2\.3\. Tests metadata\.'
mkagent "$T/needcmd" '{"name": "needcmd", "version": "1.0.0", "description": "d", "requires": ["definitely-not-installed-cmd"]}'
build "$T/needcmd" "$T/needcmd.bot"
drive "$T/needcmd.bot" "$T/ws"
check "config.json requires: missing programs are reported at start" "$T/out" "not installed here: definitely-not-installed-cmd"

# Sub-agents: agents/<name>/agent.md, reached by the model's delegate tool or a function's bot.agent
mkagent "$T/team"
mkdir -p "$T/team/agents/helper" "$T/team/functions"
printf -- '---\ndescription: Answers pings.\n---\nYou are ZETA, the helper.\n' >"$T/team/agents/helper/agent.md"
printf -- '--- Ask the helper.\n---@param task string  what to ask\nfunction ask_helper(task)\n  return "helper said: " .. bot.agent("helper", task)\nend\n' >"$T/team/functions/team.lua"
build "$T/team" "$T/team.bot"
check "sub-agents: a function that delegates is reported as such" "$T/build.log" "ask_helper \(other agents\)"
tool_call delegate '{"agent": "helper", "task": "ping2"}'
drive "$T/team.bot" "$T/ws"
check "sub-agents: the model can delegate by itself" "$T/out" "Handing work to another agent"
check "sub-agents: the task reaches the sub-agent" "$T/rec/subuser.txt" "^ping2"
check "sub-agents: its result comes back as the tool result" "$T/rec/tool.txt" "^done"
check "sub-agents: the sub-agent runs on its own persona" "$T/rec/subsystem.txt" "You are ZETA, the helper"
check_not "sub-agents: the frontmatter is not part of the persona" "$T/rec/subsystem.txt" "description: Answers"
check_not "sub-agents: a sub-agent cannot delegate further" "$T/rec/subtools.txt" "delegate"
check "sub-agents: the main agent is told about them" "$T/rec/system.txt" "helper: Answers pings"
check "sub-agents: the main agent gets the delegate tool" "$T/rec/tools.txt" "delegate"
tool_call ask_helper '{"task": "ping3"}'
drive "$T/team.bot" "$T/ws"
check "sub-agents: bot.agent hands a function's task to a sub-agent" "$T/rec/subuser.txt" "^ping3"
check "sub-agents: and the function gets its answer" "$T/rec/tool.txt" "^helper said: done"
echo "You are a helper." >"$T/team/agents/helper/agent.md"
build "$T/team" "$T/team_bad.bot"
check "sub-agents: one without a description is an error" "$T/build.log" 'agents/helper/agent.md: start it with a frontmatter'

# MCP and A2A (calling out): servers and remote agents from config.json, one HTTPS request per call
python3 "$TESTS/remote_servers.py" "$T/rport" "$T/rrec" &
REMOTE=$!
i=0; while [ ! -s "$T/rport" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
RPORT=$(cat "$T/rport")
mkagent "$T/net" "{\"name\": \"net\", \"version\": \"1.0.0\", \"description\": \"Uses remote things.\",
  \"mcp\": {\"new\": {\"url\": \"http://127.0.0.1:$RPORT/mcp-new\", \"token\": \"MCP_TOKEN\"},
            \"old\": {\"url\": \"http://127.0.0.1:$RPORT/mcp-old\", \"token\": \"MCP_TOKEN\"}},
  \"agents\": {\"peer\": {\"url\": \"http://127.0.0.1:$RPORT/peer\", \"token\": \"PEER_TOKEN\", \"description\": \"Says things back.\"}}}"
mkdir -p "$T/net/functions"
printf -- '--- Use every remote thing once.\nfunction use_all()\n  return bot.mcp("new", "echo", { text = "hi" }) .. " | " .. bot.mcp("old", "echo", { text = "yo" }) .. " | " .. bot.agent("peer", "ping")\nend\n' >"$T/net/functions/net.lua"
build "$T/net" "$T/net.bot"
check "MCP/A2A: a function that uses MCP servers and a remote agent builds" "$T/build.log" "use_all \\(MCP servers, other agents\\)"
rm -rf "$T/netws"; mkdir -p "$T/netws"
printf 'MCP_TOKEN=tok-mcp\nexport PEER_TOKEN="tok-a2a"\n' >"$T/netws/.env"
tool_call use_all '{}'
drive "$T/net.bot" "$T/netws"
check "MCP: the servers' tools are listed at start" "$T/out" "MCP new: 1 tool"
check "MCP + A2A: bot.mcp on both kinds of server and bot.agent to the remote agent answer" "$T/rec/tool.txt" "echo:hi \\| echo:yo \\| remote says: ping"
check "MCP: a stateless server gets no handshake, the headers say what is asked" "$T/rrec/mcp-new.txt" "^tools/call version=2026-07-28 method=tools/call name=echo session=None meta=yes"
check_not "MCP: a stateless server is never sent initialize" "$T/rrec/mcp-new.txt" "^initialize"
check "MCP: an older server gets its session handshake" "$T/rrec/mcp-old.txt" "^notifications/initialized version=2025-06-18 .* session=sid-1"
check "MCP: and the session id on every call after" "$T/rrec/mcp-old.txt" "^tools/call version=2025-06-18 .* session=sid-1"
check "MCP: the model is offered the remote tools" "$T/rec/tools.txt" "new_echo.*old_echo"
check "A2A: SendMessage in the 1.0 wire format" "$T/rrec/a2a.txt" "^SendMessage version=1.0"
check "A2A: a working task is polled until it completes" "$T/rrec/a2a.txt" "^GetTask"
check "A2A: the main agent is told about remote agents" "$T/rec/system.txt" "peer: Says things back"
rm "$T/netws/.env"
drive "$T/net.bot" "$T/netws"
check "MCP: a server that refuses the token is reported, the agent still starts" "$T/out" "MCP new is not available: new refused the request \\(HTTP 401\\)"
sed -i 's|"token": "MCP_TOKEN"}|"token": "sk-live.secret"}|' "$T/net/config.json"
build "$T/net" "$T/net_bad.bot"
check "MCP: a secret in config.json is an error" "$T/build.log" '"token" is the NAME of a line in .env'
kill "$REMOTE" 2>/dev/null

# MCP servers given as a command: started by the agent, inside the tools' sandbox
cp "$TESTS/mcp_stdio_server.py" "$T/" # inside the sandbox the server sees what the agent's tools see: not \$HOME
mkagent "$T/local" "{\"name\": \"local\", \"version\": \"1.0.0\", \"description\": \"Starts an MCP server.\", \"requires\": [\"python3\"],
  \"mcp\": {\"loud\": {\"command\": [\"python3\", \"$T/mcp_stdio_server.py\"], \"env\": [\"LOUD_SUFFIX\"]},
            \"gone\": {\"command\": [\"definitely-not-an-mcp-server\"]}}}"
build "$T/local" "$T/local.bot"
check_not "MCP command: an agent with a started server builds" "$T/build.log" "error:"
check "MCP command: a program that is not in requires is a warning" "$T/build.log" 'mcp "gone" starts "definitely-not-an-mcp-server": list it in "requires"'
rm -rf "$T/localws"; mkdir -p "$T/localws"
echo 'LOUD_SUFFIX=!!' >"$T/localws/.env"
tool_call loud_shout '{"text": "hi"}'
drive "$T/local.bot" "$T/localws"
check "MCP command: the server is started and its tools listed" "$T/out" "MCP loud: 1 tool"
check "MCP command: its tool runs, with the secret it was given by name" "$T/rec/tool.txt" "HI!! home-"
check "MCP command: a server that is not installed is reported, the agent still starts" "$T/out" 'MCP gone is not available: "definitely-not-an-mcp-server" did not start'
if grep -q "Landlock unavailable" "$T/out"; then
    skip "MCP command: the server runs inside the sandbox (no Landlock here)"
else
    check "MCP command: the server runs inside the tools' sandbox" "$T/rec/tool.txt" "HI!! home-denied"
fi
sed -i 's|"command": \["python3", |"url": "https://x.invalid/mcp", "command": ["python3", |' "$T/local/config.json"
build "$T/local" "$T/local_bad.bot"
check "MCP command: a server is a url or a command, not both" "$T/build.log" '"command" is a list of texts, the program first'

# ACP and A2A (being called): the same agent behind a standard protocol, no terminal
mkagent "$T/served" '{"name": "served", "display_name": "Served Agent", "version": "3.1.0", "description": "Answers over protocols."}'
build "$T/served" "$T/served.bot"
echo "reply-from-the-model" >"$T/said"
stub_start --text "$T/said"
rm -rf "$T/sws"; mkdir -p "$T/sws"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\nA2A_TOKEN=open-sesame\n' "$PORT" >"$T/sws/.env"
timeout 60 python3 "$TESTS/acp_client.py" "$T/served.bot" "$T/sws" y "hello" "/exit" >"$T/acp.log" 2>"$T/acp.err"
check "ACP: initialize names the agent" "$T/acp.log" '"protocolVersion": 1.*"name": "Served Agent".*"version": "3.1.0"'
check "ACP: the answer arrives as message chunks" "$T/acp.log" '"sessionUpdate": "agent_message_chunk"'
check "ACP: a prompt ends with a stop reason" "$T/acp.log" '"id": 10, "result": \{"stopReason": "end_turn"\}'
check "ACP: /commands of a remote user are just text" "$T/acp.log" '"id": 11, "result": \{"stopReason": "end_turn"\}'
check_not "ACP: nothing but the protocol on stdout" "$T/acp.log" "Connected"
shell_call "rm -rf ./some-dir"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\nA2A_TOKEN=open-sesame\n' "$PORT" >"$T/sws/.env"
timeout 60 python3 "$TESTS/acp_client.py" "$T/served.bot" "$T/sws" n "clean up" >"$T/acp.log" 2>"$T/acp.err"
check "ACP: a tool call is shown to the editor" "$T/acp.log" '"sessionUpdate": "tool_call", "toolCallId": "call_1"|"toolCallId": "call_1".*"sessionUpdate": "tool_call"'
check "ACP: a risky command asks the editor's user" "$T/acp.log" '"method": "session/request_permission"'
check "ACP: the tool result follows" "$T/acp.log" '"sessionUpdate": "tool_call_update"|"tool_call_update"'
stub_start --slow
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/sws/.env"
timeout 30 python3 - "$T/served.bot" "$T/sws" >"$T/acp.log" 2>"$T/acp.err" <<'PY'
import json, subprocess, sys, time
p = subprocess.Popen([sys.argv[1], "--acp"], cwd=sys.argv[2], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
def send(o):
    p.stdin.write(json.dumps(dict(o, jsonrpc="2.0")) + "\n"); p.stdin.flush()
def until(rid):
    for line in p.stdout:
        m = json.loads(line)
        if m.get("id") == rid: return m
send({"id": 0, "method": "initialize", "params": {"protocolVersion": 1}}); until(0)
send({"id": 1, "method": "session/new", "params": {"cwd": sys.argv[2], "mcpServers": []}})
sid = until(1)["result"]["sessionId"]
send({"id": 2, "method": "session/prompt", "params": {"sessionId": sid, "prompt": [{"type": "text", "text": "think long"}]}})
time.sleep(1.5)
t = time.time()
send({"method": "session/cancel", "params": {"sessionId": sid}})
print(json.dumps(until(2)), "after %.1fs" % (time.time() - t))
PY
check "ACP: session/cancel stops a running turn" "$T/acp.log" '"stopReason": "cancelled".* after [0-2]\.'
stub_start --text "$T/said"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\nA2A_TOKEN=open-sesame\n' "$PORT" >"$T/sws/.env"
APORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
(cd "$T/sws" && exec "$T/served.bot" --a2a "$APORT" >"$T/a2a.out" 2>&1) &
A2A=$!
python3 - "$APORT" >"$T/a2a.log" 2>&1 <<'PY'
import json, sys, time, urllib.request, urllib.error
base = "http://127.0.0.1:%s" % sys.argv[1]
def call(path, body=None, token="open-sesame"):
    h = {"Content-Type": "application/json", "A2A-Version": "1.0"}
    if token: h["Authorization"] = "Bearer " + token
    try:
        r = urllib.request.urlopen(urllib.request.Request(base + path, json.dumps(body).encode() if body else None, h), timeout=30)
        return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, None
for _ in range(100):
    try: st, card = call("/.well-known/agent-card.json", token=None); break
    except OSError: time.sleep(0.1)
print("card", st, json.dumps(card))
msg = {"jsonrpc": "2.0", "id": 7, "method": "SendMessage",
       "params": {"message": {"messageId": "m1", "role": "ROLE_USER", "parts": [{"text": "hello"}]}}}
print("noauth", call("/", msg, token=None)[0])
print("v1", json.dumps(call("/", msg)[1]))
old = {"jsonrpc": "2.0", "id": 8, "method": "message/send",
       "params": {"message": {"kind": "message", "messageId": "m2", "role": "user", "parts": [{"kind": "text", "text": "hi"}]}}}
print("v03", json.dumps(call("/", old)[1]))
print("task", json.dumps(call("/", {"jsonrpc": "2.0", "id": 9, "method": "GetTask", "params": {"id": "x"}})[1]))
PY
check "A2A: the agent card names the agent and how to call it" "$T/a2a.log" '^card 200 .*"name": "Served Agent".*"protocolBinding": "JSONRPC", "protocolVersion": "1.0"'
check "A2A: the card says a bearer token is needed" "$T/a2a.log" '^card .*"httpAuthSecurityScheme"'
check "A2A: a caller without the token is refused" "$T/a2a.log" "^noauth 401"
check "A2A: SendMessage is answered with a message (1.0)" "$T/a2a.log" '^v1 .*"id": 7, "result": \{"message": \{.*"role": "ROLE_AGENT".*"text": "reply-from-the-model'
check "A2A: 0.3 callers get the 0.3 shape" "$T/a2a.log" '^v03 .*"result": \{.*"role": "agent".*"kind": "message"'
check "A2A: there are no tasks to get" "$T/a2a.log" '^task .*"code": -32001'
# one Botter agent calling another
mkagent "$T/caller" "{\"name\": \"caller\", \"version\": \"1.0.0\", \"description\": \"Calls the served agent.\",
  \"agents\": {\"served\": {\"url\": \"http://127.0.0.1:$APORT\", \"token\": \"SERVED_TOKEN\"}}}"
build "$T/caller" "$T/caller.bot"
rm -rf "$T/cws"; mkdir -p "$T/cws"
echo "SERVED_TOKEN=open-sesame" >"$T/cws/.env"
# the caller's own model is a second stub: it asks to delegate; the served agent answers from the first
python3 "$TESTS/stub_llm.py" --portfile "$T/port2" --record "$T/rec2" --tool delegate --args '{"agent": "served", "task": "are you there?"}' &
STUB2=$!
i=0; while [ ! -s "$T/port2" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
(cd "$T/cws" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\n/exit\n' "$(cat "$T/port2")" | timeout 60 "$T/caller.bot" >"$T/out" 2>&1)
kill "$STUB2" 2>/dev/null
check "A2A: one Botter agent delegates to another over the network" "$T/rec2/tool.txt" "reply-from-the-model"
kill "$A2A" 2>/dev/null
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/sws/.env"
(cd "$T/sws" && timeout 10 "$T/served.bot" --a2a "0.0.0.0:$APORT" >"$T/a2a.out" 2>&1)
check "A2A: listening for other machines without a token is refused" "$T/a2a.out" "put A2A_TOKEN=<secret> in .env first"

# MCP (being used): the agent as one tool for any MCP client, over stdio or HTTP
stub_start --text "$T/said"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\nMCP_TOKEN=mcp-secret\n' "$PORT" >"$T/sws/.env"
timeout 60 python3 - "$T/served.bot" "$T/sws" >"$T/mcp.log" 2>"$T/mcp.err" <<'PY'
import json, subprocess, sys
p = subprocess.Popen([sys.argv[1], "--mcp"], cwd=sys.argv[2], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
def call(i, method, params=None):
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params or {}}) + "\n"); p.stdin.flush()
    print(method, p.stdout.readline().strip(), flush=True)
call(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}})
p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n"); p.stdin.flush()
call(2, "tools/list")
call(3, "tools/call", {"name": "ask", "arguments": {"message": "hello"}})
call(4, "tools/call", {"name": "nope", "arguments": {}})
call(5, "resources/list")
PY
check "MCP server: initialize names the agent and offers tools" "$T/mcp.log" '^initialize .*"protocolVersion": ?"2025-06-18".*"tools": ?\{\}.*"name": ?"Served Agent"'
check "MCP server: the agent is one tool, ask" "$T/mcp.log" '^tools/list .*"name": ?"ask".*Answers over protocols'
check "MCP server: calling it returns the agent.s reply as the tool result" "$T/mcp.log" '^tools/call .*"id": ?3.*"text": ?"reply-from-the-model.*"isError": ?false'
check "MCP server: an unknown tool is an error" "$T/mcp.log" '^tools/call .*"id": ?4.*"code": ?-32602'
check "MCP server: what it does not offer is method not found" "$T/mcp.log" '^resources/list .*"code": ?-32601'
MPORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
(cd "$T/sws" && exec "$T/served.bot" --mcp "$MPORT" >"$T/mcp.out" 2>&1) &
A2A=$!
python3 - "$MPORT" >"$T/mcp.log" 2>&1 <<'PY'
import json, sys, time, urllib.request, urllib.error
url = "http://127.0.0.1:%s/" % sys.argv[1]
def call(body, token="mcp-secret"):
    h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2026-07-28"}
    if token: h["Authorization"] = "Bearer " + token
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, json.dumps(body).encode(), h), timeout=30)
        return r.status, (r.read() or b"").decode()
    except urllib.error.HTTPError as e:
        return e.code, ""
for _ in range(100):
    try: print("list", *call({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})); break
    except OSError: time.sleep(0.1)
print("noauth", call({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, token=None)[0])
print("note", call({"jsonrpc": "2.0", "method": "notifications/initialized"})[0])
print("call", *call({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "ask", "arguments": {"message": "hi"}}}))
PY
kill "$A2A" 2>/dev/null
check "MCP server over HTTP: tools/list without a handshake" "$T/mcp.log" '^list 200 .*"name": ?"ask"'
check "MCP server over HTTP: a caller without the token is refused" "$T/mcp.log" "^noauth 401"
check "MCP server over HTTP: a notification is accepted" "$T/mcp.log" "^note 202"
check "MCP server over HTTP: a tool call returns the agent.s reply" "$T/mcp.log" '^call 200 .*"text": ?"reply-from-the-model'

shell_call "echo no-curl-needed"
drive "$T/on.bot" "$T/ws" PATH=/nonexistent
check "runs with no curl (or anything) on PATH" "$T/rec/tool.txt" "no-curl-needed"

mkdir -p "$T/proj" && echo "You are built by the test." >"$T/proj/agent.md"
tool_call agent_build '{}'
drive "$BOTTER" "$T/proj" BOTTER_TUI=0
check "botter: agent_build builds a .bot from inside the sandbox" "$T/rec/tool.txt" "wrote .*proj\\.bot"
# tool_test: runs each tool with its descriptor's example, like botcore does
mkdir -p "$T/tt/tools/bin" "$T/tt/tools/doc"
printf '#!/usr/bin/env python3\nimport json, sys\na = json.load(sys.stdin)\nprint("echo:", a["q"])\n' >"$T/tt/tools/bin/good"
printf '#!/bin/sh\ncat >/dev/null\necho "Missing KEY in .env" >&2\nexit 3\n' >"$T/tt/tools/bin/bad"
printf '#!/bin/sh\ncat\n' >"$T/tt/tools/bin/noex"
echo '{"description": "d", "parameters": {"type": "object", "properties": {"q": {"type": "string"}}}, "example": {"q": "hi"}}' >"$T/tt/tools/doc/good.json"
echo '{"description": "d", "parameters": {"type": "object", "properties": {}}, "example": {}}' >"$T/tt/tools/doc/bad.json"
echo '{"description": "d", "parameters": {"type": "object", "properties": {}}}' >"$T/tt/tools/doc/noex.json"
tool_call tool_test '{"dir": "tt"}'
drive "$BOTTER" "$T" BOTTER_TUI=0
check "tool_test: a working tool runs with its example (stdin JSON, no exec bit)" "$T/rec/tool.txt" "good: ok"
check "tool_test: its real output is shown" "$T/rec/tool.txt" "stdout: echo: hi"
check "tool_test: a tool that only lacks .env settings needs setup, not a fix" "$T/rec/tool.txt" "bad: NEEDS SETUP"
check "tool_test: stderr reaches the model" "$T/rec/tool.txt" "Missing KEY in .env"
check "tool_test: a tool without example is reported" "$T/rec/tool.txt" 'noex: NOT TESTED: add .example.'
rm "$T/tt/tools/bin/noex" "$T/tt/tools/doc/noex.json"
tool_call tool_test '{"dir": "tt"}'
drive "$BOTTER" "$T" BOTTER_TUI=0
check "tool_test: all ok says so" "$T/rec/tool.txt" "all ok"
check_not "tool_test: all ok contains no FAILED (Botter loops while it does)" "$T/rec/tool.txt" "FAILED"
[ -x "$T/proj/proj.bot" ] && pass "botter: the built agent is executable" || fail "botter: the built agent is executable"

# Sealed vault (opt-in): Botter asks for the keys and a password itself; the built agent asks only for the password
mkdir -p "$T/sealproj" && echo "You are sealed." >"$T/sealproj/agent.md"
echo '{"name": "sealproj", "version": "1.0.0", "description": "Carries its keys.", "mcp": {"docs": {"url": "https://example.invalid/mcp", "token": "DOCS_TOKEN"}}}' >"$T/sealproj/config.json"
tool_call vault_seal '{}'
(cd "$T/sealproj" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\nn\n6\nhttp://127.0.0.1:%s/v1\nsk-sealed-123\nstub\ndocs-secret-9\n\nshort\ncorrect horse 1\nsomething else 2\ncorrect horse 1\ncorrect horse 1\n' "$PORT" "$PORT" |
    timeout 90 env BOTTER_TUI=0 "$BOTTER" >"$T/out" 2>&1)
check "vault: Botter says what the LLM key is for" "$T/out" "The model the agent thinks with"
check "vault: each MCP token is asked for by name, with its purpose" "$T/out" 'MCP server "docs".*example.invalid'
check "vault: the A2A serving token is explained, and that ACP needs none" "$T/out" "ACP, needs no key"
check "vault: a short password is refused" "$T/out" "Too short"
check "vault: the password is typed twice and must match" "$T/out" "The two do not match"
check "vault: the user is told the password is the whole login, offline" "$T/out" "That is the whole login"
check "vault: the model learns what was sealed, not the keys" "$T/rec/tool_all.txt" "Sealed into secrets.vault: the LLM key .*, the MCP token for docs"
check_not "vault: no key reaches the model" "$T/rec/tool_all.txt" "sk-sealed|docs-secret|correct horse"
check_not "vault: the file holds no key in the clear" "$T/sealproj/secrets.vault" "sk-sealed|docs-secret|127.0.0.1"
build "$T/sealproj" "$T/sealed.bot"
rm -rf "$T/sealws"; mkdir -p "$T/sealws"
(cd "$T/sealws" && printf 'not the password\ncorrect horse 1\ngo\n/exit\n' | timeout 60 "$T/sealed.bot" >"$T/out" 2>&1)
check "vault: a wrong password is refused" "$T/out" "Wrong password\\."
check "vault: the right password is the whole login" "$T/out" "Connected"
check "vault: an ordinary agent cannot seal anything" "$T/rec/tool.txt" "unknown tool 'vault_seal'"
check_not "vault: a sealed agent does not ask for a provider or key" "$T/out" "Select an OpenAI-compatible provider"
check "vault: the sealed LLM key is the one used" "$T/rec/auth.txt" "Bearer sk-sealed-123"
(cd "$T/sealws" && printf 'a\nb\nc\n' | timeout 60 "$T/sealed.bot" >"$T/out" 2>&1)
check "vault: three wrong passwords and it gives up" "$T/out" "Giving up"
(cd "$T/sealws" && timeout 20 "$T/sealed.bot" --a2a 18999 >"$T/out" 2>&1)
check "vault: served without a terminal, it says how to unlock first" "$T/out" "run it once in a terminal first"

# ---------------------------------------------------------------------------
section "Functions (Lua), batch mode, images, usage"

mkagent "$T/fn" '{"name": "fn", "version": "1.0.0", "description": "Lua test agent.", "temperature": 0, "seed": 7}'
mkdir -p "$T/fn/functions/tests"
cat >"$T/fn/functions/money.lua" <<'EOF'
--- Add up amounts.
---@param amounts number[]  the amounts
---@param label string?  what they are
---@return table  {total, count, label}
function total(amounts, label)
  local sum = 0
  for _, a in ipairs(amounts) do
    sum = sum + a
  end
  return { total = sum, count = #amounts, label = label or "amounts" }
end

--- Read a file in the working folder.
---@param path string  the file
function read_file(path)
  return bot.fs.read(path)
end

--- Write a CSV of totals.
---@param path string  where
function write_totals(path)
  bot.fs.write(path, bot.csv.encode({ { name = "a, b", total = 3 } }, { "name", "total" }))
  return "written"
end

--- Fails on purpose.
---@param why string  the reason
function fail_now(why)
  error("cannot: " .. why)
end

local function helper() return 1 end
EOF
cat >"$T/fn/functions/tests/money_test.lua" <<'EOF'
function test_total()
  assert(total({1, 2, 3.5}).total == 6.5)
end
function test_dates()
  assert(bot.date.add_days("2026-02-28", 1) == "2026-03-01")
  assert(bot.date.add_months("2026-01-31", 1) == "2026-02-28")
  assert(bot.date.diff_days("2026-09-07", "2026-12-06") == 90)
end
function test_csv_keeps_blank_fields()
  local rows = bot.csv.parse('id,amount\ne1,\ne2,"1,5"\n')
  assert(#rows == 2 and rows[1].amount == "" and rows[2].amount == "1,5")
end
EOF
"$PACK" build "$T/fn" "$CORE" "$T/fn.bot" >"$T/build.log" 2>&1
check "functions: packer lists each function, pure or what it reaches" "$T/build.log" "functions: total \(pure\), read_file \(reads files\), write_totals \(writes files\), fail_now \(pure\)"
check "functions: tests in functions/tests run at build time" "$T/build.log" "function tests: 3 passed, 0 failed"
"$PACK" list "$T/fn.bot" >"$T/list.log" 2>&1
check "functions: the .lua files are embedded" "$T/list.log" "functions/money.lua"
check_not "functions: their tests are not" "$T/list.log" "money_test"
# fn_bad NAME OLD NEW PATTERN: money.lua with OLD replaced by NEW must not build and must report PATTERN
fn_bad() {
    rm -rf "$T/fnb" "$T/fnb.bot"
    cp -r "$T/fn" "$T/fnb"
    python3 -c 'import sys;p=sys.argv[1];s=open(p).read();assert sys.argv[2] in s;open(p,"w").write(s.replace(sys.argv[2],sys.argv[3],1))' \
        "$T/fnb/${5:-functions/money.lua}" "$2" "$3"
    "$PACK" build "$T/fnb" "$CORE" "$T/fnb.bot" >"$T/lint.log" 2>&1
    if [ -e "$T/fnb.bot" ]; then fail "functions: $1 (it was built)"; else check "functions: $1" "$T/lint.log" "$4"; fi
}
fn_bad "a syntax error stops the build, with its line" 'sum = sum + a' 'sum = sum +' 'functions/money.lua:[0-9]+: error: .*unexpected symbol'
fn_bad "every parameter needs a @param" '---@param path string  where' '--- (where)' 'write_totals: parameter path has no ---@param line'
fn_bad "an unknown type is an error" '---@param why string' '---@param why text' '@param why: unknown type text'
fn_bad "a function cannot take a tool's name" 'function read_file(path)' 'function fs_read(path)' 'fs_read is already a tool.s name'
fn_bad "a failing test stops the build" '== 6.5' '== 7' 'test test_total FAILED' functions/tests/money_test.lua

# runtime: the model calls a function; arguments as the model sends them ("3.5" for a number)
mkdir -p "$T/fnws"
tool_call total '{"amounts":[1,2,"3.5"],"label":"fees"}'
drive "$T/fn.bot" "$T/fnws"
check "functions: the model's call runs the Lua function" "$T/rec/tool.txt" '"total":6.5'
check "functions: a documented function is offered as a tool" "$T/rec/tools.txt" "total read_file write_totals fail_now"
check_not "functions: a local helper is not a tool" "$T/rec/tools.txt" "helper"
check "config.json temperature and seed reach every request" "$T/rec/sampling.txt" "temperature=0 seed=7"
tool_call fail_now '{"why":"no data"}'
drive "$T/fn.bot" "$T/fnws"
check "functions: an error comes back with its file and line" "$T/rec/tool.txt" "ERROR: functions/money.lua:[0-9]+: cannot: no data"
echo "inside" >"$T/fnws/in.txt"
tool_call read_file '{"path":"in.txt"}'
drive "$T/fn.bot" "$T/fnws"
check "functions: bot.fs reads inside the working directory" "$T/rec/tool.txt" "^inside"
tool_call read_file '{"path":"/etc/hostname"}'
drive "$T/fn.bot" "$T/fnws"
check "functions: bot.fs refuses paths outside it" "$T/rec/tool.txt" "outside the working directory"
tool_call write_totals '{"path":"out.csv"}'
drive "$T/fn.bot" "$T/fnws"
check "functions: bot.fs.write and bot.csv.encode (quoted field)" "$T/fnws/out.csv" '^"a, b",3$'

# bot.http and bot.shell: the network and commands, under the same rules as the model's own tools
mkdir -p "$T/fnio/functions"
echo "You are a test agent." >"$T/fnio/agent.md"
cat >"$T/fnio/functions/outside.lua" <<'EOF'
--- Fetch the address in TEST_URL and say what came back.
function fetch()
  local r = bot.http.get(bot.secret("TEST_URL"), { ["X-Test"] = "yes" })
  return { status = r.status, model = r.json.data[1].id }
end

--- Fetch any address.
---@param url string  the address
function fetch_url(url)
  return bot.http.get(url).status
end

--- Run a command.
---@param cmd string  the command
function run(cmd)
  local out, code = bot.shell(cmd)
  return { out = out, code = code }
end
EOF
build "$T/fnio" "$T/fnio.bot"
check "functions: what reaches the network, commands and secrets is reported" "$T/build.log" "fetch \(the network, secrets\), fetch_url \(the network\), run \(commands\)"
mkdir -p "$T/fnio/off" && cp -r "$T/fnio/agent.md" "$T/fnio/functions" "$T/fnio/off/"
echo '{"internet": false}' >"$T/fnio/off/config.json"
build "$T/fnio/off" "$T/fnio_off.bot"
tool_call fetch '{}'
echo "TEST_URL=http://127.0.0.1:$PORT/v1/models" >"$T/fnws/.env"
drive "$T/fnio.bot" "$T/fnws"
check "bot.http: a GET with headers, its JSON decoded, a secret from .env" "$T/rec/tool.txt" '"status":200'
check "bot.http: the decoded JSON is a table" "$T/rec/tool.txt" '"model":"stub"'
drive "$T/fnio_off.bot" "$T/fnws"
check "bot.http: an offline agent's functions have no network" "$T/rec/tool.txt" "this agent is offline"
tool_call fetch_url '{"url": "http://example.com/"}'
drive "$T/fnio.bot" "$T/fnws"
check "bot.http: plain http only to this machine" "$T/rec/tool.txt" "must start with https://"
tool_call run '{"cmd": "echo from-shell; exit 3"}'
drive "$T/fnio.bot" "$T/fnws"
check "bot.shell: the output and the exit code" "$T/rec/tool.txt" '"out":"from-shell".*"code":3|"code":3.*"out":"from-shell"'
tool_call run '{"cmd": "rm -rf ./nothing-here"}'
drive "$T/fnio.bot" "$T/fnws"
check "bot.shell: a risky command needs a yes, which nobody gave" "$T/rec/tool.txt" "ERROR: bot.shell: .*denied"
rm -f "$T/fnws/.env"

# batch mode: one task (reply alone on stdout), or one task per line (JSON lines), each in a fresh conversation
stub_start
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/fnws/.env"
(cd "$T/fnws" && timeout 60 "$T/fn.bot" --run "say hi" >"$T/out" 2>"$T/err")
check "--run TASK: stdout is the reply only" "$T/out" "^done$"
check "--run TASK: the model is told nobody is there" "$T/rec/system.txt" "running unattended"
check "--run TASK: token usage on stderr" "$T/err" 'usage: \{"model":"stub","calls":1,"input_tokens":11,"output_tokens":7\}'
check "--run: streaming asks for usage" "$T/rec/sampling.txt" "usage=True"
(cd "$T/fnws" && printf 'first task\n\nsecond task\n' | timeout 60 "$T/fn.bot" --run >"$T/out" 2>"$T/err")
check "--run: one JSON line per task" "$T/out" '^\{"input":"second task","reply":"done"\}$'
if [ "$(wc -l <"$T/out")" -eq 2 ]; then pass "--run: blank lines are not tasks"; else fail "--run: blank lines are not tasks" "$(cat "$T/out")"; fi
check "--run: each task starts a fresh conversation" "$T/rec/user.txt" "^second task$"
check_not "--run: nothing else on stdout" "$T/out" "Connected|Using"
rm -f "$T/fnws/.env"
(cd "$T/fnws" && timeout 30 "$T/fn.bot" --run "say hi" >"$T/out" 2>"$T/err"; echo "exit $?" >>"$T/err")
check "--run without a model connection says how to give one" "$T/err" "No saved model connection, and nobody to ask here"
check "--run without a model connection fails (exit 1)" "$T/err" "^exit 1$"
check_not "--run without a model connection shows no provider menu" "$T/err" "Select an OpenAI-compatible provider"

# image_view: the picture goes to the model for the turn, then leaves the conversation
python3 -c 'import zlib,struct
def ch(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
open("'"$T/fnws/pic.png"'","wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",1,1,8,2,0,0,0))+ch(b"IDAT",zlib.compress(b"\0\0\0\0"))+ch(b"IEND",b""))'
tool_call image_view '{"path":"pic.png"}'
(cd "$T/fnws" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\nlook\nagain\n/exit\n' "$PORT" | timeout 60 "$T/fn.bot" >"$T/out" 2>&1)
check "image_view: the image reaches the model after the tool call" "$T/rec/parts_all.txt" "^text image_url$"
check "image_view: the next turn has a note instead of the image" "$T/rec/parts_all.txt" "^text text( |$)"
check "image_view: the tool says the image is attached" "$T/rec/tool_all.txt" "pic.png \(image/png, [0-9]+ bytes\) is attached"
echo "not an image" >"$T/fnws/fake.png"
tool_call image_view '{"path":"fake.png"}'
drive "$T/fn.bot" "$T/fnws"
check "image_view: a file that is not an image is refused" "$T/rec/tool.txt" "is not a PNG, JPEG, GIF or WEBP image"

# ---------------------------------------------------------------------------
section "Pack integrity"

shell_call "echo intact"
drive "$CORE" "$T/ws"
check_not "plain botcore (no pack) still starts" "$T/out" "truncated|damaged"
cp "$T/on.bot" "$T/trunc.bot" && truncate -s -100 "$T/trunc.bot"
drive "$T/trunc.bot" "$T/ws"
check "truncated .bot refuses to start with a clear message" "$T/out" "this agent file is truncated: it should be [0-9]+ bytes"
cp "$T/on.bot" "$T/grown.bot" && printf 'junk' >>"$T/grown.bot"
drive "$T/grown.bot" "$T/ws"
check ".bot with extra bytes appended is reported as damaged" "$T/out" "this agent file is damaged"
cp "$T/proj/proj.bot" "$T/trunc2.bot" && truncate -s -40 "$T/trunc2.bot"
drive "$T/trunc2.bot" "$T/ws"
check "agent built by botter (runtime taken from botter) detects truncation" "$T/out" "truncated"
drive "$T/proj/proj.bot" "$T/ws"
check "agent built by botter runs" "$T/out" "Connected"

# ---------------------------------------------------------------------------
section "HTTP client (local servers)"

if ! musl-gcc -Os -static -I"$ROOT/tools/src/botcore/src" -I"$ROOT/tools/src/botcore/lib/bearssl/inc" \
    -I"$ROOT/tools/src/botcore/lib/ca" -o "$T/http_client" "$TESTS/http_client.c" \
    "$ROOT/tools/src/botcore/src/http.c" "$ROOT/tools/src/botcore/obj-musl/libbearssl.a" 2>"$T/cc.log"; then
    fail "build the HTTP test client" "$(head -c 300 "$T/cc.log")"
else
    python3 "$TESTS/http_servers.py" "$T/hport" &
    HS=$!
    i=0
    while [ ! -s "$T/hport" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    OP=$(sed -n 1p "$T/hport")
    PP=$(sed -n 2p "$T/hport")
    H=$T/http_client
    $H "http://127.0.0.1:$OP/plain" >"$T/h"
    check "Content-Length body" "$T/h" '"plain":"ok"'
    $H "http://127.0.0.1:$OP/chunked" >"$T/h"
    check "chunked body is decoded" "$T/h" '\{"a":"chunked ok"\}'
    $H "http://127.0.0.1:$OP/post" sk-test '{"x":[1,"é"]}' >"$T/h"
    check "POST: status, Bearer header and UTF-8 body" "$T/h" 'status=201.*|"auth": "Bearer sk-test"'
    check "POST: body echoed intact" "$T/h" '"got": \{"x": \[1, "\\u00e9"\]\}'
    HTTPS_PROXY="http://u:wrong@127.0.0.1:$PP" $H https://example.invalid/ >"$T/h"
    check "proxy refusal is reported (407)" "$T/h" "refused the tunnel \\(HTTP 407\\)"
    $H "ftp://x/" >"$T/h"
    check "non-http URLs are rejected" "$T/h" "invalid URL"
    $H "http://127.0.0.1:1/" >"$T/h"
    check "connection refused is reported" "$T/h" "could not connect to 127.0.0.1 port 1"
    kill "$HS" 2>/dev/null

    if [ "${BOTTER_TEST_NET:-0}" = 1 ]; then
        section "HTTP client (internet: BOTTER_TEST_NET=1)"
        for u in https://generativelanguage.googleapis.com/v1beta/openai/models https://api.openai.com/v1/models \
            https://api.groq.com/openai/v1/models https://openrouter.ai/api/v1/models; do
            $H "$u" bogus >"$T/h"
            check "TLS + HTTP to $u" "$T/h" "rc=0 status=(200|400|401|403)"
        done
        $H https://self-signed.badssl.com/ >"$T/h"
        check "self-signed certificate refused" "$T/h" "not trusted"
        $H https://expired.badssl.com/ >"$T/h"
        check "expired certificate refused" "$T/h" "expired"
        $H https://wrong.host.badssl.com/ >"$T/h"
        check "wrong host name refused" "$T/h" "does not match the host name"
        python3 "$TESTS/http_servers.py" "$T/hport2" &
        HS=$!
        i=0
        while [ ! -s "$T/hport2" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
        PP=$(sed -n 2p "$T/hport2")
        HTTPS_PROXY="http://u:p%20w@127.0.0.1:$PP" $H https://api.groq.com/openai/v1/models bogus >"$T/h"
        check "HTTPS through an authenticated CONNECT proxy" "$T/h" "rc=0 status=401"
        kill "$HS" 2>/dev/null
    else
        skip "internet HTTPS tests" "set BOTTER_TEST_NET=1"
    fi
fi

# ---------------------------------------------------------------------------
section "Interactive (tmux)"
if command -v tmux >/dev/null 2>&1; then
    stub_start --slow
    tmux new-session -d -s botter-test -x 100 -y 20 -c "$T/ws" "$T/on.bot"
    sleep 0.5
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "hello" Enter
    sleep 1.5
    tmux send-keys -t botter-test C-c
    sleep 1
    tmux capture-pane -t botter-test -p >"$T/tmux.txt"
    tmux kill-server 2>/dev/null
    check "Ctrl-C cancels an in-flight LLM request within a second" "$T/tmux.txt" "\\(interrupted\\)"

    # Botter's TUI: messages are chat tagged [N]; the header goes with the first one
    stub_start
    tmux new-session -d -s botter-test -x 100 -y 30 -c "$T/ws" "env BOTTER_THEME=dark $BOTTER"
    sleep 0.8
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "first message" Enter
    sleep 1
    tmux send-keys -t botter-test "second message" Enter
    sleep 1.2
    tmux capture-pane -t botter-test -p >"$T/tui.txt"
    tmux kill-server 2>/dev/null
    check "TUI: messages reach the model tagged with their number" "$T/rec/user.txt" "^\\[2\\] second message"
    check "TUI: the model is told what the numbers mean" "$T/rec/system.txt" "numbers each user message and your reply"
    check_not "TUI: header is gone after the first message" "$T/tui.txt" "the agent that builds agents"
    check "TUI: the first message is number 1" "$T/rec/user_all.txt" "^\\[1\\] first message"

    # TUI after a tool call: streamed reply shown once, tool internals hidden
    stub_start --tool shell_exec --args '{"command":"echo hi"}'
    tmux new-session -d -s botter-test -x 100 -y 30 -c "$T/ws" "env BOTTER_THEME=dark $BOTTER"
    sleep 0.8
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "run it" Enter
    sleep 1.5
    tmux capture-pane -t botter-test -p >"$T/tui.txt"
    tmux kill-server 2>/dev/null
    n=$(grep -c "^ *done *$" "$T/tui.txt")
    if [ "$n" -eq 1 ]; then pass "TUI: a streamed reply is shown once"; else fail "TUI: a streamed reply is shown once" "seen $n times"; fi
    check_not "TUI: tool names are hidden unless ctrl+o" "$T/tui.txt" "shell_exec"
else
    skip "Ctrl-C test" "tmux not installed"
fi

stub_stop
printf '\n%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
