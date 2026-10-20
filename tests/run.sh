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
    echo "You are a test agent." >"$1/AGENT.md"
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
    check "an old $old is reported" "$T/build.log" "$old is no longer used: write how the agent works in AGENT.md"
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
cfg_bad "reasoning must be low, medium or high" '{"name": "a", "version": "1.0.0", "reasoning": "max"}' '"reasoning" must be "low", "medium" or "high"'
cfg_bad "an unknown key is a warning" '{"name": "a", "version": "1.0.0", "author": "x"}' 'warning: config.json: unknown key "author"'
cfg_bad "missing name, version, description are warnings" '{"internet": true}' 'warning: config.json: no "name"'
cfg_bad "requires takes command names" '{"name": "a", "version": "1.0.0", "requires": ["rm -rf"]}' '"requires" must be a list of names'
cfg_bad "a listed skill must exist" '{"name": "a", "version": "1.0.0", "skills": ["ghost"]}' 'skill "ghost" is listed but there is no skills/ghost/SKILL.md'
mkagent "$T/cfg_old"
echo '{}' >"$T/cfg_old/agent.json"
"$PACK" check "$T/cfg_old" "$CORE" >"$T/build.log" 2>&1
check "config.json: an old agent.json is reported" "$T/build.log" "agent.json is now config.json"
# the skills list chooses what is compiled in; a skill brings its scripts (runnable) and functions
mkagent "$T/pick" '{"name": "pick", "version": "1.0.0", "description": "d", "skills": ["keep"], "requires": ["python3"]}'
mkdir -p "$T/pick/skills/keep/scripts" "$T/pick/skills/keep/functions/tests" "$T/pick/skills/drop"
for k in keep drop; do printf -- '---\nname: %s\ndescription: d\n---\nRun scripts/run.py.\n' $k >"$T/pick/skills/$k/SKILL.md"; done
printf '#!/usr/bin/env python3\nprint("hi")\n' >"$T/pick/skills/keep/scripts/run.py"
printf -- '--- Say hi.\nfunction keep_hi()\n  return "hi"\nend\n' >"$T/pick/skills/keep/functions/hi.lua"
printf 'function test_hi()\n  assert(keep_hi() == "hi")\nend\n' >"$T/pick/skills/keep/functions/tests/hi.lua"
build "$T/pick" "$T/pick.bot"
"$PACK" list "$T/pick.bot" >"$T/list.log" 2>&1
check "config.json skills: a listed skill is compiled in" "$T/list.log" "skills/keep/SKILL.md"
check_not "config.json skills: an unlisted skill is left out" "$T/list.log" "skills/drop/"
check "skills: a script is embedded as a runnable file" "$T/list.log" "script +[0-9]+  skills/keep/scripts/run.py"
check "skills: a skill's functions are embedded" "$T/list.log" "skills/keep/functions/hi.lua"
check_not "skills: a skill's function tests are not embedded" "$T/list.log" "functions/tests"
check "skills: a skill's function tests run at build time" "$T/build.log" "function tests: 1 passed, 0 failed"
check "config.json: leaving a skill out is a warning" "$T/build.log" 'skill "drop" is not listed in config.json "skills"'
mkagent "$T/needpy" '{"name": "needpy", "version": "1.0.0", "description": "d"}'
mkdir -p "$T/needpy/skills/py/scripts"
printf -- '---\nname: py\ndescription: d\n---\nRun scripts/pyt.\n' >"$T/needpy/skills/py/SKILL.md"
printf '#!/usr/bin/env python3\nprint(1)\n' >"$T/needpy/skills/py/scripts/pyt"
"$PACK" check "$T/needpy" "$CORE" >"$T/build.log" 2>&1
check "config.json: a script's interpreter missing from requires is a warning" "$T/build.log" 'runs with python3: add "python3" to "requires"'
# the old layout is an error that says where things go
mkagent "$T/oldl" '{"name": "oldl", "version": "1.0.0", "description": "d", "tools": ["x"]}'
mv "$T/oldl/AGENT.md" "$T/oldl/agent.md"
mkdir -p "$T/oldl/tools/bin" "$T/oldl/tools/doc" "$T/oldl/agents/helper"
echo x >"$T/oldl/agents/helper/agent.md"
"$PACK" check "$T/oldl" "$CORE" >"$T/build.log" 2>&1
check "old layout: agent.md is now AGENT.md" "$T/build.log" "agent.md is now AGENT.md: rename it"
check "old layout: tools/bin says where programs go" "$T/build.log" "tools/bin is gone: put each program in the scripts/ folder"
check "old layout: tools/doc too" "$T/build.log" "tools/doc is gone"
check "old layout: config.json tools" "$T/build.log" '"tools" is gone'
check "old layout: a sub-agent's agent.md" "$T/build.log" "sub-agents use AGENT.md too"
# the skill validator
mkagent "$T/ctr" '{"name": "ctr", "version": "1.0.0", "description": "d", "requires": ["python3"]}'
mkdir -p "$T/ctr/skills/bare" "$T/ctr/skills/named" "$T/ctr/skills/good/references" "$T/ctr/skills/good/scripts"
echo "# Bare skill" >"$T/ctr/skills/bare/SKILL.md"
printf -- '---\nname: other\ndescription: %s\n---\nSee [the guide](references/missing.md).\nTODO: write the rest\n' "$(printf 'x%.0s' $(seq 1 1100))" >"$T/ctr/skills/named/SKILL.md"
printf -- '---\nname: good\ndescription: >\n  Writes reports. Use it when\n  the user asks for one.\n---\nRead [the style](references/style.md) first; run scripts/make.py; keep "TODO" items out.\n' >"$T/ctr/skills/good/SKILL.md"
echo "# Style" >"$T/ctr/skills/good/references/style.md"
echo "# Lost" >"$T/ctr/skills/good/references/lost.md"
printf 'import sys\nprint("mock leads for " + sys.argv[1])\n' >"$T/ctr/skills/good/scripts/make.py"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check "skills: a skill without frontmatter is an error" "$T/build.log" 'error: skills/bare/SKILL.md has no frontmatter'
check "skills: the name must be the folder's" "$T/build.log" 'error: skills/named/SKILL.md: "name: other" must be the folder.s name, named'
check "skills: a description over 1024 characters is an error" "$T/build.log" 'the description has 1100 characters'
check "skills: a link that does not resolve is an error" "$T/build.log" 'skills/named/SKILL.md links to references/missing.md, which is not in the skill'
check "skills: placeholder words are an error" "$T/build.log" 'skills/named/SKILL.md says "TODO"'
check_not "skills: a quoted placeholder word is a mention" "$T/build.log" 'skills/good/SKILL.md says'
check_not "skills: a folded description is read" "$T/build.log" 'skills/good/SKILL.md: frontmatter has no'
check "skills: a reference nothing points to is a warning" "$T/build.log" 'skills/good/references/lost.md: neither SKILL.md nor anything it points to'
check_not "skills: a linked reference is fine" "$T/build.log" 'references/style.md: neither'
check "skills: a script without #! is a warning" "$T/build.log" 'skills/good/scripts/make.py has no "#!" line'
printf '#!/usr/bin/env python3\nimport sys\nprint("mock leads for " + sys.argv[1])\n' >"$T/ctr/skills/good/scripts/make.py"
printf '#!/bin/sh\n# adapted from artifacts/kit/scrape.py (MIT)\npython3 artifacts/kit/scrape.py\n' >"$T/ctr/skills/good/scripts/uses_art"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check "skills: mock output in a script is a warning" "$T/build.log" 'skills/good/scripts/make.py mentions "mock"'
check "skills: a script that runs code from artifacts/ is an error" "$T/build.log" 'error: skills/good/scripts/uses_art uses files under artifacts/'
printf '#!/bin/sh\n# adapted from artifacts/kit/scrape.py (MIT)\necho ok\n' >"$T/ctr/skills/good/scripts/uses_art"
"$PACK" check "$T/ctr" "$CORE" >"$T/build.log" 2>&1
check_not "skills: a comment crediting artifacts/ is fine" "$T/build.log" 'uses_art uses files under artifacts/'

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
check "system prompt lists skills with their frontmatter descriptions" "$T/rec/system.txt" "^- pitching: Write one specific idea per lead"

stub_start --text /dev/null
drive "$T/meta.bot" "$T/ws"
check "config.json: the agent shows its display name and version at start" "$T/out" "Meta Agent.*1\.2\.3.*Tests metadata"
check "config.json: the model knows its name and version" "$T/rec/system.txt" 'You are the agent "Meta Agent", version 1\.2\.3\. Tests metadata\.'
mkagent "$T/needcmd" '{"name": "needcmd", "version": "1.0.0", "description": "d", "requires": ["definitely-not-installed-cmd"]}'
build "$T/needcmd" "$T/needcmd.bot"
drive "$T/needcmd.bot" "$T/ws"
check "config.json requires: missing programs are reported at start" "$T/out" "not installed here: definitely-not-installed-cmd"

# Sub-agents: agents/<name>/AGENT.md, reached by the model's delegate tool or a function's bot.agent
mkagent "$T/team"
mkdir -p "$T/team/agents/helper" "$T/team/functions"
printf -- '---\ndescription: Answers pings.\n---\nYou are ZETA, the helper.\n' >"$T/team/agents/helper/AGENT.md"
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
echo "You are a helper." >"$T/team/agents/helper/AGENT.md"
build "$T/team" "$T/team_bad.bot"
check "sub-agents: one without a description is an error" "$T/build.log" 'agents/helper/AGENT.md: start it with a frontmatter'

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

mkdir -p "$T/proj" && echo "You are built by the test." >"$T/proj/AGENT.md"
tool_call agent_build '{}'
drive "$BOTTER" "$T/proj" BOTTER_TUI=0
check "botter: agent_build builds a .bot from inside the sandbox" "$T/rec/tool.txt" "wrote .*proj\\.bot"
mkdir -p "$T/proj2" && echo "You are built by the plain runtime." >"$T/proj2/AGENT.md"
drive "$ROOT/artifacts/botter.bot" "$T/proj2"
check "botter: agent_build also works run as a plain .bot from a folder the sandbox hides" "$T/rec/tool.txt" "wrote .*proj2\\.bot"
[ -x "$T/proj/proj.bot" ] && pass "botter: the built agent is executable" || fail "botter: the built agent is executable"

# Sealed vault (opt-in): Botter asks for the keys and a password itself; the built agent asks only for the password
mkdir -p "$T/sealproj" && echo "You are sealed." >"$T/sealproj/AGENT.md"
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

mkagent "$T/fn" '{"name": "fn", "version": "1.0.0", "description": "Lua test agent.", "temperature": 0, "seed": 7, "reasoning": "low"}'
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
check "config.json temperature, seed and reasoning reach every request" "$T/rec/sampling.txt" "temperature=0 seed=7 reasoning=low"
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
echo "You are a test agent." >"$T/fnio/AGENT.md"
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
mkdir -p "$T/fnio/off" && cp -r "$T/fnio/AGENT.md" "$T/fnio/functions" "$T/fnio/off/"
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

# what a model gets back while it writes functions: the build's own check, at once
mkdir -p "$T/lw"
tool_call fs_write '{"path": "functions/good.lua", "content": "--- Add two numbers.\n---@param a number  first\n---@param b number  second\nfunction add(a, b)\n  return a + b\nend\n"}'
drive "$T/on.bot" "$T/lw"
check "fs_write: a functions file is checked when written, its tools named" "$T/rec/tool.txt" "Lua check of functions/good.lua: ok. Tools the model can call: add"
tool_call fs_write '{"path": "functions/bad.lua", "content": "function add(a, b)\n  return a +\nend\n"}'
drive "$T/on.bot" "$T/lw"
check "fs_write: a syntax error comes back with its line" "$T/rec/tool.txt" "functions/bad.lua:3: error: unexpected symbol"
tool_call fs_write '{"path": "functions/mod.lua", "content": "local M = {}\nfunction M.add(a, b) return a + b end\nreturn M\n"}'
(cd "$T/lw" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\nwrite it\nwrite it again\n/exit\n' "$PORT" | timeout 60 "$T/on.bot" >"$T/out" 2>&1)
check "fs_write: the agent's own file is replaced without asking" "$T/rec/tool_all.txt" "replaced existing file"
check "fs_write: a file with no doc comment is reported as callable by nobody" "$T/rec/tool.txt" "no function here has a --- doc comment"
echo "user data" >"$T/lw/notes.txt"
tool_call fs_write '{"path": "notes.txt", "content": "gone"}'
drive "$T/on.bot" "$T/lw"
check "fs_write: a file that was there before still needs a yes" "$T/rec/tool.txt" "denied by user: not overwriting"
tool_call fs_edit '{"path": "functions/good.lua", "old_text": "return a + b", "new_text": "return a + b + 0"}'
drive "$T/on.bot" "$T/lw"
check "fs_edit: replaces an exact piece, without asking" "$T/rec/tool.txt" "replaced 1 place in .*good.lua"
check "fs_edit: and checks the file again" "$T/rec/tool.txt" "Lua check of functions/good.lua: ok"
grep -q "return a + b + 0" "$T/lw/functions/good.lua" && pass "fs_edit: the file has the change" || fail "fs_edit: the file has the change"
tool_call fs_edit '{"path": "functions/good.lua", "old_text": "not in the file", "new_text": "x"}'
drive "$T/on.bot" "$T/lw"
check "fs_edit: text that is not there says how to fix the call" "$T/rec/tool.txt" "old_text is not in"
mkdir -p "$T/teach/functions"; echo "x" >"$T/teach/AGENT.md"
printf -- '--- Uses io.\nfunction read_it()\n  return io.open("x")\nend\n--- Uses require.\nfunction need()\n  return require("functions.date")\nend\n' >"$T/teach/functions/t.lua"
build "$T/teach" "$T/teach.bot"
tool_call read_it '{}'
drive "$T/teach.bot" "$T/fnws"
check "Lua: io names what to use instead" "$T/rec/tool.txt" "io.open is not available in functions: read and write files with bot.fs.read"
tool_call need '{}'
drive "$T/teach.bot" "$T/fnws"
check "Lua: require says every file is already loaded" "$T/rec/tool.txt" "require is not available in functions: every file in functions/ is already loaded"
printf 'local function helper() return 1 end\n' >"$T/teach/functions/t.lua"
build "$T/teach" "$T/teach.bot"
check "functions: no documented function at all is a build warning" "$T/build.log" "no function has a --- doc comment directly above it"

# code that only looks done is a warning: a placeholder note, or a loop with nothing in it
mkdir -p "$T/stub/functions"; echo "x" >"$T/stub/AGENT.md"
printf -- '--- Forecast.\n---@param n number  days\nfunction forecast(n)\n  -- Simplified logic for demo\n  for i = 1, n do\n    -- add the events here\n  end\n  return 0\nend\n' >"$T/stub/functions/f.lua"
"$PACK" check "$T/stub" "$CORE" >"$T/build.log" 2>&1
check "functions: a placeholder note is a warning" "$T/build.log" 'forecast looks unfinished \("simplified"\)'
check "functions: a loop that does nothing is a warning" "$T/build.log" "forecast has a loop that does nothing"

# scores: a test file's score_ functions measure the agent against examples; the build reports them
mkdir -p "$T/fn/functions/tests"
printf 'function score_examples()\n  return 3, 4, "example 2: got 5, expected 6"\nend\nfunction score_broken()\n  return "no"\nend\n' >"$T/fn/functions/tests/score.lua"
"$PACK" check "$T/fn" "$CORE" >"$T/build.log" 2>&1
check "scores: the build reports passed/total" "$T/build.log" "packer: score examples: 3/4 \(75%\)"
check "scores: with what differs" "$T/build.log" "^example 2: got 5, expected 6$"
check "scores: one that returns no numbers is an error" "$T/build.log" "score_broken FAILED: a score_ function returns passed, total"
rm "$T/fn/functions/tests/score.lua"
# bot.tool: a function uses one of the agent's tools the way the model would
printf -- '--- List the folder with the fs_list tool.\nfunction listing()\n  return bot.tool("fs_list", { path = "." })\nend\n' >"$T/fn/functions/viatool.lua"
build "$T/fn" "$T/fn.bot"
check "bot.tool: reported as reaching tools" "$T/build.log" "listing \(tools\)"
tool_call listing '{}'
echo "x" >"$T/fnws/seen-by-tool.txt"
drive "$T/fn.bot" "$T/fnws"
check "bot.tool: the tool's answer comes back to the function" "$T/rec/tool.txt" "seen-by-tool.txt"
rm "$T/fn/functions/viatool.lua"; build "$T/fn" "$T/fn.bot"

# --eval: cases in evals/*.json against the model, graded by expectations or the agent's own Lua check
mkdir -p "$T/ev/functions" "$T/ev/evals"; echo "x" >"$T/ev/AGENT.md"
cat >"$T/ev/functions/judge.lua" <<'EOF'
function says_done(run)
  local r = bot.json.decode(run)
  if r.reply ~= "done" then
    return "expected done, got " .. r.reply .. " (case data: " .. tostring(r.data and r.data.note) .. ")"
  end
end
function says_more(run)
  return "the reply is too short"
end
EOF
build "$T/ev" "$T/ev.bot"
cat >"$T/ev/evals/cases.json" <<'EOF'
[
  {"name": "answers", "input": "list it", "expect": {"contains": ["done"], "tools_used": ["fs_list"], "regex": "^d.ne$"}},
  {"name": "wrong text", "input": "list it", "expect": {"not_contains": ["done"], "equals": "finished"}},
  {"name": "too many calls", "input": "list it", "expect": {"max_tool_calls": 0, "tools_not_used": ["fs_list"]}},
  {"name": "lua judge", "input": "list it", "check": "says_done", "data": {"note": "n1"}},
  {"name": "lua judge fails", "input": "list it", "check": "says_more"},
  {"name": "cannot fail", "input": "list it"}
]
EOF
tool_call fs_list '{}'
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/ev/.env"
(cd "$T/ev" && timeout 60 "$T/ev.bot" --eval >"$T/out" 2>"$T/err"; echo "exit $?" >>"$T/err")
check "--eval: one JSON line per case, a passing one" "$T/out" '"name":"answers","passed":true'
check "--eval: contains/equals failures say what" "$T/out" 'the reply contains \\"done\\".*the reply is not exactly \\"finished\\"'
check "--eval: tool-call limits are graded" "$T/out" 'fs_list was called.*1 tool calls, at most 0 expected'
check "--eval: the agent's Lua check passes" "$T/out" '"name":"lua judge","passed":true'
check "--eval: a check's reason comes back" "$T/out" 'says_more: the reply is too short'
check "--eval: a case that could never fail is refused" "$T/out" 'could never fail'
check "--eval: the summary and the usage on stderr" "$T/err" '^eval: 2/6 passed$'
check "--eval: a failing case fails the run" "$T/err" '^exit 1$'
rm -f "$T/ev/.env"

# on_turn_end: an agent's hook sends the model back to work, until it says the turn may end
mkdir -p "$T/hook/functions" "$T/hookws"; echo "x" >"$T/hook/AGENT.md"
cat >"$T/hook/functions/hook.lua" <<'EOF'
function on_turn_end(reply)
  if bot.fs.exists("nudged.txt") then
    return nil
  end
  bot.fs.write("nudged.txt", reply)
  return "One item is still open: finish it."
end
EOF
build "$T/hook" "$T/hook.bot"
check "on_turn_end: the build names the hook" "$T/build.log" "hook: on_turn_end"
stub_start
drive "$T/hook.bot" "$T/hookws"
check "on_turn_end: its text goes back to the model as the next message" "$T/rec/user_all.txt" "^One item is still open: finish it.$"
check "on_turn_end: the user sees that it continues" "$T/out" "\(continuing: One item is still open"
check "on_turn_end: the hook got the model's reply" "$T/hookws/nudged.txt" "^done$"
n=$(grep -c "One item is still open" "$T/rec/user_all.txt")
[ "$n" -eq 1 ] && pass "on_turn_end: nil ends the turn" || fail "on_turn_end: nil ends the turn" "sent $n times"

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
section "Context + memory engine (stub LLM)"
mkdir -p "$T/memws"
python3 -c 'import random
random.seed(1)
rows=["id,user,kind,amount,day"]+["e%d,u%d,%s,%.2f,2026-0%d-1%d"%(i,i%7,random.choice(["income","expense"]),random.random()*900,1+i%9,i%10) for i in range(2000)]
open("'"$T/memws/events.csv"'","w").write("\n".join(rows)+"\n")
open("'"$T/memws/big.txt"'","w").write(("x"*99+"\n")*300)'
tool_call fs_read '{"path":"events.csv"}'
drive "$CORE" "$T/memws"
check "fs_read of a big CSV returns a profile" "$T/rec/tool.txt" "^events.csv: [0-9.]+ KB, 2000 data rows, 5 columns. This is a profile"
check "profile: column types and ranges" "$T/rec/tool.txt" "amount: number [0-9.]+ \.\. [0-9.]+"
check "profile: dates" "$T/rec/tool.txt" "day: date 2026-01-10 \.\. 2026-09-1"
check "profile: a column with few values lists them" "$T/rec/tool.txt" 'kind: text, one of: "(income|expense)", "(income|expense)"'
check "profile: the first rows" "$T/rec/tool.txt" "^e0,u0,"
check "profiled files are kept in memory" "$T/rec/tool.txt" "^- events.csv: [0-9.]+ KB, 2000 rows; columns: id, user, kind, amount, day"
tool_call fs_read '{"path":"events.csv","offset":0}'
drive "$CORE" "$T/memws"
check "fs_read with an offset gives the raw text" "$T/rec/tool.txt" "^id,user,kind,amount,day$"
stub_start --tool fs_read --args '{"path":"big.txt"}' --repeat 12 --then recall --then-args '{"id": 1}'
drive "$CORE" "$T/memws"
if [ "$(sort -n "$T/rec/sizes.txt" | tail -1)" -lt 100000 ]; then pass "old tool results are archived (requests stay small)"; else fail "old tool results are archived (requests stay small)" "$(tr '\n' ' ' <"$T/rec/sizes.txt")"; fi
check "archived results leave a stub naming the call" "$T/rec/request.json" '\[archived #1: fs_read \{\\"path\\":\\"big.txt\\"\}, 29.3 KB; recall\(1\)\]'
check "recall(id) brings an archived result back" "$T/rec/tool.txt" "^x{99}$"
check "the memory block says results are archived" "$T/rec/tool.txt" "are archived \([0-9]+ so far\): recall\(id\)"
stub_start --tool fs_write --args '{"path":"long.txt","content":"'"$(printf 'y%.0s' $(seq 1 10000))"'"}' --repeat 9
drive "$CORE" "$T/memws"
check "long tool-call arguments are archived too" "$T/rec/request.json" 'content\\":\\"\[archived #[0-9]+: 9.8 KB; recall\([0-9]+\)\]'
tool_call note '{"text": "amounts are in INR"}'
drive "$CORE" "$T/memws"
check "note: kept" "$T/rec/tool.txt" "^ok: 1 note in memory"
check "note: shown in the memory block" "$T/rec/tool.txt" "^1\. amounts are in INR$"
tool_call note '{"forget": 3}'
drive "$CORE" "$T/memws"
check "note: forgetting a note that does not exist says so" "$T/rec/tool.txt" "there is no note 3"
mkdir -p "$T/memws/functions"
tool_call fs_write '{"path":"functions/add.lua","content":"--- Add.\n---@param a number\n---@param b number\nfunction add(a, b)\n  return a + b\nend\n"}'
drive "$CORE" "$T/memws"
check "file ledger: written files with their Lua check" "$T/rec/tool.txt" "^- functions/add.lua: 85 B, 6 lines; Lua ok; tools: add$"
tool_call fs_list '{}'
drive "$CORE" "$T/memws"
check_not "nothing in memory: no memory block" "$T/rec/tool.txt" "Memory, kept for you"

# ---------------------------------------------------------------------------
section "Skills at run time (stub LLM)"
mkagent "$T/sk" '{"name": "sk", "version": "1.0.0", "description": "d"}'
mkdir -p "$T/sk/skills/report/references" "$T/sk/skills/report/scripts" "$T/sk/skills/report/functions" "$T/sk/functions" "$T/skws" "$T/skrt"
printf -- '---\nname: report\ndescription: Writes reports. Use it for any report.\n---\nRead references/guide.md, then run scripts/hello.sh NAME.\n' >"$T/sk/skills/report/SKILL.md"
echo "The guide says hi." >"$T/sk/skills/report/references/guide.md"
printf '#!/bin/sh\necho "hello $1"\n' >"$T/sk/skills/report/scripts/hello.sh"
printf -- '--- Count the reports.\nfunction report_count()\n  return 3\nend\n' >"$T/sk/skills/report/functions/count.lua"
cat >"$T/sk/functions/main.lua" <<'EOF'
--- Run the report script.
function run_hello()
  local out = bot.shell("'" .. bot.skill_dir("report") .. "/scripts/hello.sh' world")
  return out
end

--- Read the guide through fs_read.
function read_guide()
  return bot.tool("fs_read", { path = bot.skill_dir("report") .. "/references/guide.md" })
end

--- Try to change the guide.
function write_guide()
  return bot.tool("fs_write", { path = bot.skill_dir("report") .. "/references/guide.md", content = "x" })
end
EOF
build "$T/sk" "$T/sk.bot"
check "skills: the agent with a skill builds" "$T/build.log" "wrote .*sk.bot"
tool_call skill '{"name": "report"}'
drive "$T/sk.bot" "$T/skws" XDG_RUNTIME_DIR="$T/skrt"
check "skill tool: loads the skill and says where its files are" "$T/rec/tool.txt" "^Skill report is loaded. Its files are at $T/skrt/botcore-skills-[0-9]+-[A-Za-z0-9]+/report/"
check "skill tool: lists the skill's files" "$T/rec/tool.txt" "Files: references/guide.md \([0-9]+ bytes\), scripts/hello.sh \([0-9]+ bytes, runnable\)"
check "skill tool: returns SKILL.md" "$T/rec/tool.txt" "^Read references/guide.md, then run"
check "skill tool: the skill's functions become tools" "$T/rec/tool.txt" "New tools from this skill: report_count"
check "skill tool: offered from the next request on" "$T/rec/tools.txt" "report_count"
if ls "$T/skrt" | grep -q botcore-skills; then fail "skills: the files are removed when the agent exits" "$(ls "$T/skrt")"; else pass "skills: the files are removed when the agent exits"; fi
tool_call fs_list '{}'
drive "$T/sk.bot" "$T/skws" XDG_RUNTIME_DIR="$T/skrt"
check_not "skills: a skill's functions are not tools before it is loaded" "$T/rec/tools.txt" "report_count"
check "skills: the skill tool is offered" "$T/rec/tools.txt" "skill"
tool_call report_count '{}'
drive "$T/sk.bot" "$T/skws" XDG_RUNTIME_DIR="$T/skrt"
check "skills: calling a skill's function before loading says to load it" "$T/rec/tool.txt" "report_count belongs to the skill report: load it with the skill tool first"
tool_call run_hello '{}'
drive "$T/sk.bot" "$T/skws" XDG_RUNTIME_DIR="$T/skrt"
check "skills: a function runs a skill script at its real path, without a question" "$T/rec/tool.txt" "^hello world"
check_not "skills: nothing was asked" "$T/out" "Permission needed"
tool_call read_guide '{}'
drive "$T/sk.bot" "$T/skws" XDG_RUNTIME_DIR="$T/skrt"
check "skills: fs_read reads a skill file without a question" "$T/rec/tool.txt" "The guide says hi"
tool_call write_guide '{}'
drive "$T/sk.bot" "$T/skws" XDG_RUNTIME_DIR="$T/skrt"
check "skills: skill files are read-only" "$T/rec/tool.txt" "one of your skill.s files, which are read-only"

# ---------------------------------------------------------------------------
section "FLOW.lua: orchestration on botcore primitives (stub LLM)"
mkflow() { # mkflow NAME FLOW_LUA_TEXT: an agent whose FLOW.lua is that text; built into $T/NAME.bot
    mkagent "$T/$1" "{\"name\": \"$1\", \"version\": \"1.0.0\", \"description\": \"d\"}"
    printf '%s\n' "$2" >"$T/$1/FLOW.lua"
    build "$T/$1" "$T/$1.bot"
}
flowrun() { # flowrun NAME TASK: one --run task in $T/flws
    printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/flws/.env"
    (cd "$T/flws" && timeout 60 "$T/$1.bot" --run "$2" >"$T/out" 2>"$T/err")
}
mkdir -p "$T/flws"
mkflow flw 'function flow(input)
  bot.say("starting " .. input)
  local r = bot.worker{task = "do: " .. input, context = "THE-SPEC-123", tools = {"fs_write", "fs_read"}, writes = {"out/"}}
  return "flow got: " .. r
end'
check "FLOW.lua: the build reports the flow" "$T/build.log" "FLOW.lua: flow"
tool_call fs_write '{"path": "out/a.txt", "content": "made by a worker"}'
flowrun flw hello
check "FLOW.lua: flow(input) answers the job" "$T/out" "^flow got: done$"
check "FLOW.lua: bot.say shows progress (stderr in batch mode)" "$T/err" "^starting hello$"
check "bot.worker: the task is the worker's message" "$T/rec/user.txt" "^do: hello$"
check "bot.worker: the worker is told it is one" "$T/rec/system.txt" "You are a worker: you do one task"
check "bot.worker: the pinned context reaches it" "$T/rec/system.txt" "THE-SPEC-123"
check "bot.worker: it is told what it may write" "$T/rec/system.txt" "You may write only: out/"
check "bot.worker: it gets only the tools it was given" "$T/rec/tools.txt" "^fs_read fs_write$"
check "bot.worker: it writes inside its scope" "$T/flws/out/a.txt" "made by a worker"
tool_call fs_write '{"path": "elsewhere.txt", "content": "x"}'
flowrun flw hello
check "bot.worker: a write outside its scope is refused" "$T/rec/tool.txt" "this task may write only out/; elsewhere.txt is not one of them"
[ ! -e "$T/flws/elsewhere.txt" ] && pass "bot.worker: nothing was written there" || fail "bot.worker: nothing was written there"
tool_call shell_exec '{"command": "echo hi"}'
flowrun flw hello
check "bot.worker: a tool it was not given is refused" "$T/rec/tool.txt" "shell_exec is not one of this task.s tools"
mkflow flask 'function flow(input)
  return "answer: " .. tostring(bot.ask("Which one?"))
end'
stub_start --text /dev/null
flowrun flask go
check "bot.ask: nobody to ask in batch mode gives nil" "$T/out" "^answer: nil$"
(cd "$T/flws" && rm -f .env && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\nthe-blue-one\n/exit\n' "$PORT" | timeout 60 "$T/flask.bot" >"$T/out" 2>&1)
check "bot.ask: in a terminal the person is asked" "$T/out" "Which one\\?"
check "bot.ask: and the answer comes back to the flow" "$T/out" "answer: the-blue-one"
mkflow flerr 'function flow(input)
  error("boom in the flow")
end'
flowrun flerr go
check "FLOW.lua: an error in the flow is reported" "$T/err" "boom in the flow"
mkflow flnone 'function helper() end'
check "FLOW.lua: one without flow() is a build error" "$T/build.log" "FLOW.lua: error: it defines no global function flow"
mkflow fldoc '--- Not a tool.
---@param x string  x
function flow(x) return x end'
check "FLOW.lua: documented functions there are a warning (not tools)" "$T/build.log" "FLOW.lua: its functions are not tools"
# bot.llm: the agent's own model from code (judgement inside a code-driven procedure)
mkflow fllm 'function flow(input)
  if input == "plain" then return "said: " .. bot.llm("Say something.") end
  if input == "image" then return bot.llm{prompt = "What is in it?", images = {"pic.png"}} end
  local t = bot.llm{prompt = "Read: " .. input, schema = {amount = "number", essential = "boolean"}}
  return "amount=" .. t.amount .. " essential=" .. tostring(t.essential)
end'
printf '[{"text": "Here it is:\\n```json\\n{\\"amount\\": 42, \\"essential\\": true}\\n```"}]' >"$T/llm1.json"
stub_start --script "$T/llm1.json"
flowrun fllm "rent 42"
check "bot.llm: a schema gives a table (JSON found inside a fenced reply)" "$T/out" "^amount=42 essential=true$"
check "bot.llm: the schema is asked for" "$T/rec/requests.jsonl" 'Reply with only one JSON object \(no other text\) with these fields: \{\\"amount\\":\\"number\\",\\"essential\\":\\"boolean\\"\}'
check "bot.llm: its calls are counted in the usage" "$T/err" '"calls":1,'
printf '[{"text": "{\\"amount\\": 7}"}, {"text": "{\\"amount\\": 7, \\"essential\\": false}"}]' >"$T/llm2.json"
stub_start --script "$T/llm2.json"
flowrun fllm "rent 7"
check "bot.llm: a reply without the JSON is asked again, saying what is missing" "$T/rec/requests.jsonl" 'missing: essential'
check "bot.llm: and the corrected answer comes back" "$T/out" "^amount=7 essential=false$"
printf '[{"text": "nope"}, {"text": "still nope"}]' >"$T/llm3.json"
stub_start --script "$T/llm3.json"
flowrun fllm "rent 9"
check "bot.llm: no JSON after the retry is an error the flow sees" "$T/err" "bot.llm: the model did not reply with the JSON asked for"
printf '[{"text": "hello there"}]' >"$T/llm4.json"
stub_start --script "$T/llm4.json"
flowrun fllm plain
check "bot.llm: a plain prompt gives text" "$T/out" "^said: hello there$"
cp "$T/fnws/pic.png" "$T/flws/pic.png"
flowrun fllm image
check "bot.llm: images go with the prompt" "$T/rec/parts.txt" "^text text image_url$"
# bot.test: the project in the working folder, loaded fresh, its tests run
mkflow fltest 'function flow(input)
  local r = bot.test(input ~= "all" and input or nil)
  return r.passed .. "/" .. r.failed .. "\n" .. r.report
end'
mkdir -p "$T/flws/functions/tests"
printf -- '--- Add.\n---@param a number  a\n---@param b number  b\nfunction add(a, b)\n  return a + b\nend\n' >"$T/flws/functions/add.lua"
printf 'function test_good()\n  assert(add(1, 2) == 3)\nend\nfunction test_bad()\n  assert(add(1, 2) == 4, "one and two")\nend\nfunction score_sums()\n  return 1, 2, "one differs"\nend\n' >"$T/flws/functions/tests/add.lua"
flowrun fltest all
check "bot.test: counts passed and failed tests" "$T/out" '^1/1$'
flowrun fltest "functions/tests/add.lua"
check "bot.test: says which test failed and why" "$T/out" "test test_bad FAILED: .*one and two"
check "bot.test: reports scores" "$T/out" "score sums: 1/2"
printf 'function add(a, b\n' >"$T/flws/functions/add.lua"
flowrun fltest "functions/tests/add.lua"
check "bot.test: a project that does not load fails, with the reason" "$T/out" "functions/add.lua:[0-9]+: error"
printf -- '--- Save.\n---@param t string  text\nfunction save(t)\n  bot.fs.write("out/saved.csv", t)\n  bot.fs.append("out/saved.csv", "!")\nend\n' >"$T/flws/functions/add.lua"
printf 'function test_save()\n  save("hi")\n  assert(bot.fs.exists("out/saved.csv"))\n  assert(bot.fs.read("./out/saved.csv") == "hi!", bot.fs.read("out/saved.csv"))\nend\n' >"$T/flws/functions/tests/add.lua"
flowrun fltest "functions/tests/add.lua"
check "bot.test: a test's writes stay in memory and read back" "$T/out" '^1/0$'
[ ! -e "$T/flws/out/saved.csv" ] && pass "bot.test: a test never writes the folder" || fail "bot.test: a test never writes the folder"

# ---------------------------------------------------------------------------
section "Botter's build pipeline (FLOW.lua, scripted stub LLM)"
rm -rf "$T/pipe" && mkdir -p "$T/pipe"
printf '# Adder\nWrite an agent that adds a and b of each row of data.csv.\n## Sums\nsum = a + b\n## Submission\nSend it in.\n' >"$T/pipe/brief.md"
printf 'a,b\n1,2\n3,4\n' >"$T/pipe/data.csv"
printf 'x\n1\n' >"$T/pipe/extra.csv"
python3 - "$T/pipe.json" <<'PY'
import json, sys
test = 'function test_add()\n  assert(add_row(1, 2) == 3, "1 + 2")\nend\n'
idle = 'function test_add()\n  local result = 3\n  assert(result == 3, "made up")\nend\n'
invented = 'function test_add()\n  local row = "row7"\n  assert(add_row(1, 2) == 3, row)\nend\n'
wrong = '--- Add a and b.\n---@param a number  a\n---@param b number  b\n---@return number  the sum\nfunction add_row(a, b)\n  return a - b\nend\n'
right = wrong.replace("a - b", "a + b")
steps = [
  {"tool": "start_build", "args": {"request": "build the adder from brief.md"}},
  {"text": "Starting."},
  {"tool": "job_goal", "args": {"text": "Add a and b of each row."}},
  {"tool": "job_input", "args": {"path": "data.csv", "description": "columns a, b"}},
  {"tool": "job_input", "args": {"path": "nope.csv", "description": "a file that is not there"}},
  {"tool": "job_output", "args": {"path": "out.csv", "columns": "sum", "description": "one sum per row"}},
  {"tool": "job_rule", "args": {"text": "sum = a + b", "source": "brief.md", "kind": "exact"}},
  {"tool": "job_rule", "args": {"text": "Sum = a + b.", "source": "brief.md", "kind": "exact"}},
  {"tool": "job_success", "args": {"text": "every sum is right"}},
  {"text": "1 rule recorded"},
  {"tool": "job_rule", "args": {"text": "sum = a + b", "source": "brief.md: Sums", "kind": "exact"}},
  {"tool": "job_skip_section", "args": {"section": "Submission", "why": "how to hand it in"}},
  {"tool": "job_skip_file", "args": {"path": "extra.csv", "why": "not used by the adder"}},
  {"text": "sections done"},
  {"tool": "design_kind", "args": {"kind": "batch", "why": "one row per line"}},
  {"tool": "design_function", "args": {"name": "add_row", "params": "a number, b number", "returns": "number",
                                        "purpose": "adds a and b; the caller writes out.csv", "rules": ""}},
  {"tool": "design_model_step", "args": {"name": "adds", "purpose": "the model adds the numbers", "rules": "R1"}},
  {"text": "planned"},
  {"tool": "design_function", "args": {"name": "add_row", "params": "a number, b number", "returns": "number: the sum",
                                        "purpose": "adds a and b; the caller writes out.csv", "rules": "R1"}},
  {"text": "planned again"},
  {"tool": "fs_write", "args": {"path": "functions/tests/add_row.lua", "content": idle}},
  {"text": "tests written"},
  {"tool": "fs_write", "args": {"path": "functions/tests/add_row.lua", "content": invented}},
  {"text": "tests written"},
  {"tool": "fs_write", "args": {"path": "functions/tests/add_row.lua", "content": test}},
  {"text": "tests written"},
  {"tool": "fs_write", "args": {"path": "functions/add_row.lua", "content": wrong}},
  {"text": "written"},
  {"tool": "run_tests", "args": {"path": "functions/tests/add_row.lua"}},
  {"tool": "fs_write", "args": {"path": "functions/add_row.lua", "content": right}},
  {"text": "fixed"},
  {"tool": "fs_write", "args": {"path": "AGENT.md", "content": "You add numbers. For each line, call add_row with a and b and reply with the sum.\n"}},
  {"text": "AGENT.md written"},
]
json.dump(steps, open(sys.argv[1], "w"))
PY
stub_start --script "$T/pipe.json"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/pipe/.env"
(cd "$T/pipe" && timeout 120 "$ROOT/artifacts/botter.bot" --run "Build the agent described in brief.md" >"$T/out" 2>"$T/err")
check "pipeline: the lead hands the build over" "$T/err" "^Starting\.$"
check "pipeline: the job is recorded and readable" "$T/pipe/.botter/job.md" "^R1\. \[exact\] sum = a \+ b \(brief.md; brief.md: Sums\)$"
check "pipeline: the analyst must account for every section of the brief" "$T/rec/requests.jsonl" "sections of the brief no rule comes from: brief.md: Sums; brief.md: Submission"
check "pipeline: a section without rules is recorded as such" "$T/pipe/.botter/job.md" "^- Submission$"
check "pipeline: a rule recorded twice is not added again" "$T/rec/requests.jsonl" "already recorded as R1"
check_not "pipeline: the job holds no duplicate rule" "$T/pipe/.botter/job.md" "^R2\."
check "pipeline: an exact rule left to the model is sent back" "$T/rec/requests.jsonl" "exact or hybrid but only a model step or skill covers: R1"
check "pipeline: a recording tool checks on the spot" "$T/rec/requests.jsonl" "nope.csv does not exist"
check "pipeline: the design is recorded" "$T/pipe/.botter/design.md" "^- add_row\(a number, b number\) -> number"
check "pipeline: the analyst gets its role and only its tools" "$T/rec/requests.jsonl" "\"system\": \"You are Botter.s analyst.*\"tools\": \[[^]]*\"job_rule\""
check "pipeline: the designer works from the pinned job" "$T/rec/requests.jsonl" "You are Botter.s designer.*What you must follow \(the job\):.*R1\. \[exact\] sum = a"
check "pipeline: tests are written before the code, by another worker" "$T/pipe/functions/tests/add_row.lua" "add_row\(1, 2\) == 3"
check "pipeline: a failed check sends the task back with the failure" "$T/rec/requests.jsonl" "did not pass the check.*test_add FAILED"
check "pipeline: the code passes its tests in the end" "$T/pipe/functions/add_row.lua" "return a \+ b"
check "pipeline: the design check wants every output column made" "$T/rec/requests.jsonl" "out.csv: no function returns \(and no model step writes\) sum"
check "pipeline: the job lists the output's columns" "$T/pipe/.botter/job.md" "^- out.csv: columns sum\. "
check "pipeline: tests on invented ids are sent back" "$T/rec/requests.jsonl" "uses ids that no data file holds: row7"
check "pipeline: a test that never calls its function is sent back" "$T/rec/requests.jsonl" "test_add never calls add_row: every test_ function calls add_row"
check "pipeline: every data file must be accounted for" "$T/rec/requests.jsonl" "data files the job does not mention: extra.csv"
check "pipeline: a data file the agent does not use is recorded as such" "$T/pipe/.botter/job.md" "^- extra.csv: not used by the adder$"
check "pipeline: workers run the tests with run_tests" "$T/rec/requests.jsonl" "0 passed, 1 failed.*test_add FAILED"
check "pipeline: the cost of each phase is logged" "$T/pipe/.botter/log.md" "^- cost of add_row: [0-9]+ calls, [0-9]+ input tokens"
check "pipeline: the report says what the build cost" "$T/out" "^Model use for this build: [0-9]+ calls"
check "pipeline: config.json is written by code" "$T/pipe/config.json" '"temperature": ?0'
check "pipeline: the agent is built" "$T/out" "^Built .*pipe\.bot"
check "pipeline: the report quotes what was measured" "$T/out" "^Functions passing their tests: 1/1\.$"
check "pipeline: a log of every step" "$T/pipe/.botter/log.md" "add_row: done \(attempt 2\)"
[ -x "$T/pipe/pipe.bot" ] && pass "pipeline: the .bot is executable" || fail "pipeline: the .bot is executable"
printf '[{"tool": "start_build", "args": {"request": "continue", "resume": true}}, {"text": "Continuing."}, {"tool": "fs_write", "args": {"path": "AGENT.md", "content": "You add numbers with add_row.\\n"}}, {"text": "done"}]' >"$T/resume.json"
stub_start --script "$T/resume.json"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/pipe/.env"
(cd "$T/pipe" && timeout 120 "$ROOT/artifacts/botter.bot" --run "continue the build" >"$T/out" 2>"$T/err")
check "pipeline: resume continues the build in .botter/" "$T/pipe/.botter/log.md" "^- resumed: continue the build"
check "pipeline: finished tasks are not redone" "$T/out" "^Functions passing their tests: 1/1\.$"
check_not "pipeline: the job is not studied again" "$T/rec/requests.jsonl" "You are Botter.s analyst"
# a fix that breaks passing tests is undone; tests that stay red are set aside and the rest still builds
for d in ex stuck noagent; do rm -rf "$T/$d" && mkdir -p "$T/$d" && printf 'a,b\n1,2\n3,4\n' >"$T/$d/data.csv"; done
printf 'a,b,sum\n2,2,5\n' >"$T/ex/sample_expected.csv"
python3 - "$T" <<'PY'
import json, sys
t = sys.argv[1]
test = 'function test_add()\n  assert(add_row(1, 2) == 3, "1 + 2")\nend\n'
code = '--- Add a and b.\n---@param a number  a\n---@param b number  b\n---@return number  the sum\nfunction add_row(a, b)\n  return a + b\nend\n'
broken = code.replace("a + b", "a + b + 1")
wrong = code.replace("a + b", "a * b")
solver = ('--- Decide one case.\n---@param case table  the case\n---@return table  the answers\n'
          'function solve_case(case)\n  return { sum = add_row(tonumber(case.a), tonumber(case.b)) }\nend\n')
def job(examples):
    j = [{"tool": "start_build", "args": {"request": "the adder"}}, {"text": "Starting."},
         {"tool": "job_goal", "args": {"text": "Add a and b of each row."}},
         {"tool": "job_input", "args": {"path": "data.csv", "description": "columns a, b"}},
         {"tool": "job_output", "args": {"path": "out.csv", "columns": "sum", "description": "one sum per row"}},
         {"tool": "job_rule", "args": {"text": "sum = a + b", "source": "brief", "kind": "exact"}},
         {"tool": "job_success", "args": {"text": "every sum is right"}}]
    if examples:
        j.append({"tool": "job_examples", "args": {"path": "sample_expected.csv", "description": "a, b and the expected sum"}})
    design = [{"text": "1 rule"},
         {"tool": "design_kind", "args": {"kind": "batch", "why": "one row per line"}},
         {"tool": "design_function", "args": {"name": "add_row", "params": "a number, b number", "returns": "number: the sum",
                                               "purpose": "adds a and b; the caller writes out.csv", "rules": "R1"}},
         {"text": "planned"}]
    if examples:  # the examples hold answers: the design check asks for solve_case
        design += [{"tool": "design_function", "args": {"name": "solve_case", "params": "case table", "returns": "table {sum}",
                                                         "purpose": "decides one case with add_row", "rules": "", "uses": "add_row"}},
                   {"text": "planned"}]
    return j + design + [
         {"tool": "fs_write", "args": {"path": "functions/tests/add_row.lua", "content": test}}, {"text": "tests written"}]
def write(path, content, said="written"):
    return [{"tool": "fs_write", "args": {"path": path, "content": content}}, {"text": said}]
agent = write("AGENT.md", "You add numbers with add_row.\n")
ex = job(True) + write("functions/add_row.lua", code) + write("functions/solve_case.lua", solver)
for _ in range(4):
    ex += write("functions/add_row.lua", broken, "fixed")
json.dump(ex + agent, open(t + "/ex.json", "w"))
stuck = job(False)
for _ in range(3):
    stuck += write("functions/add_row.lua", wrong)
stuck += [{"text": "test right"}]
for _ in range(2):
    stuck += write("functions/add_row.lua", wrong)
json.dump(stuck + agent, open(t + "/stuck.json", "w"))
json.dump(job(False) + write("functions/add_row.lua", code) + [{"text": "I wrote nothing"}] * 3, open(t + "/noagent.json", "w"))
PY
for d in ex stuck noagent; do
    stub_start --script "$T/$d.json"
    printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/$d/.env"
    (cd "$T/$d" && timeout 120 "$ROOT/artifacts/botter.bot" --run "Build the adder" >"$T/$d.out" 2>"$T/$d.err")
    cp "$T/rec/requests.jsonl" "$T/$d.requests"
done
check "pipeline: a fix that breaks a passing test is sent back" "$T/ex.requests" "your change broke tests that passed before:.*add_row.lua: test_add"
check "pipeline: a fix that still breaks tests is undone" "$T/ex/.botter/log.md" "^- fix examples 1: undone \(it broke passing tests\)"
check "pipeline: the undone fix leaves the code as it was" "$T/ex/functions/add_row.lua" "^  return a \+ b$"
check "pipeline: the report quotes the examples score" "$T/ex.out" "^Score on the brief's examples: 0/1\.$"
check "pipeline: examples with answers need solve_case in the design" "$T/ex.requests" "sample_expected.csv holds 1 worked examples with the expected sum: plan solve_case\(case table\)"
check "pipeline: Botter writes the examples scorer itself" "$T/ex/functions/tests/examples.lua" "^-- Botter wrote this file from sample_expected.csv"
check "pipeline: the scorer compares the answer columns" "$T/ex/functions/tests/examples.lua" '^local SCORED = \{ "sum" \}'
check "pipeline: solve_case is written against Botter's scorer" "$T/ex.requests" "You are Botter.s coder. You write solve_case\(case\)"
check "pipeline: the report gives the right answers per column" "$T/ex.out" "^Right per answer \(Botter.s own comparison with the expected answers\): sum 0/1\.$"
check "pipeline: the agent with a low score still builds" "$T/ex.out" "^Built .*ex\.bot"
check "pipeline: tests that stay red are set aside" "$T/stuck/.botter/failing-tests/add_row.lua" "test_add"
check "pipeline: the report lists the set-aside tests" "$T/stuck.out" "functions/tests/add_row.lua failed .*moved to .botter/failing-tests/"
check "pipeline: the rest of the agent still builds" "$T/stuck.out" "^Built .*stuck\.bot"
check "pipeline: the report counts the failing function" "$T/stuck.out" "^Functions passing their tests: 0/1\.$"
check "pipeline: no AGENT.md, no agent" "$T/noagent.out" "AGENT.md was not written.*nothing was built"
[ ! -e "$T/noagent/noagent.bot" ] && pass "pipeline: nothing is built without AGENT.md" || fail "pipeline: nothing is built without AGENT.md"
rm -rf "$T/haiku" && mkdir -p "$T/haiku"
python3 - "$T/haiku.json" <<'PY'
import json, sys
skill = "---\nname: haiku\ndescription: Writes a haiku. Use it when the user asks for a poem.\n---\n1. Read references/form.md.\n2. Write three lines in that form.\n"
steps = [
  {"tool": "start_build", "args": {"request": "a haiku agent"}}, {"text": "Starting."},
  {"tool": "job_goal", "args": {"text": "Write haiku on request."}},
  {"tool": "job_input", "args": {"path": "", "description": "a topic the user types"}},
  {"tool": "job_output", "args": {"path": "", "columns": "", "description": "a haiku in the reply"}},
  {"tool": "job_rule", "args": {"text": "three lines of 5, 7 and 5 syllables", "source": "request", "kind": "judgement"}},
  {"tool": "job_success", "args": {"text": "the form is right"}}, {"text": "1 rule"},
  {"tool": "design_kind", "args": {"kind": "chat", "why": "talks with a person"}},
  {"tool": "design_skill", "args": {"name": "haiku", "description": "Writes a haiku. Use it when the user asks for a poem.",
                                     "files": "references/form.md: the 5-7-5 form", "rules": "R1"}},
  {"tool": "design_check", "args": {}}, {"text": "planned"},
  {"tool": "fs_write", "args": {"path": "skills/haiku/SKILL.md", "content": skill}}, {"text": "SKILL.md written"},
  {"tool": "fs_write", "args": {"path": "skills/haiku/references/form.md", "content": "# Form\nThree lines: 5, 7, 5 syllables.\n"}},
  {"text": "reference written"},
  {"tool": "fs_write", "args": {"path": "AGENT.md", "content": "You write haiku. Load the haiku skill for every poem.\n"}},
  {"text": "AGENT.md written"},
]
json.dump(steps, open(sys.argv[1], "w"))
PY
stub_start --script "$T/haiku.json"
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/haiku/.env"
(cd "$T/haiku" && timeout 120 "$ROOT/artifacts/botter.bot" --run "Build an agent that writes haiku" >"$T/out" 2>"$T/err")
check "pipeline: a skill is planned with its files" "$T/haiku/.botter/design.md" "^  - references/form.md: the 5-7-5 form$"
check "pipeline: design_check says the plan is complete" "$T/rec/requests.jsonl" '"last": "complete"'
check "pipeline: the skill's files are written, each by its own task" "$T/rec/requests.jsonl" "Write skills/haiku/references/form.md \(the 5-7-5 form\)"
check "pipeline: config.json lists the skill" "$T/haiku/config.json" '"skills": ?\["haiku"\]'
check "pipeline: the report says how the skill came out" "$T/out" "^Skills: haiku \(done\)\.$"
"$PACK" list "$T/haiku/haiku.bot" >"$T/list.log" 2>&1
check "pipeline: the built agent carries the skill" "$T/list.log" "skills/haiku/references/form.md"

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
