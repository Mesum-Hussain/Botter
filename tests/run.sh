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
section "Packaging and FWL"

"$PACK" check "$ROOT" "$CORE" >"$T/self.log" 2>&1
check "botter's own project builds with 0 warnings" "$T/self.log" "check passed.* 0 warning"

# FWL (FLOW.fwl): a valid flow compiles; each kind of mistake is an error with its line and stops the build
mkagent "$T/flow_good"
mkdir -p "$T/flow_good/skills/pitching"
printf -- '---\nname: pitching\ndescription: p\n---\n' >"$T/flow_good/skills/pitching/SKILL.md"
cp "$TESTS/good.fwl" "$T/flow_good/FLOW.fwl"
"$PACK" check "$T/flow_good" "$CORE" >"$T/lint.log" 2>&1
check_not "FWL: a valid FLOW.fwl has no errors or warnings" "$T/lint.log" "FLOW.fwl"
# sq_bad NAME OLD NEW PATTERN: good.fwl with OLD replaced by NEW must report PATTERN
sq_bad() {
    rm -rf "$T/flow_bad" "$T/flow_bad.bot"
    cp -r "$T/flow_good" "$T/flow_bad"
    python3 -c 'import sys;p=sys.argv[1];s=open(p).read();assert sys.argv[2] in s;open(p,"w").write(s.replace(sys.argv[2],sys.argv[3],1))' \
        "$T/flow_bad/FLOW.fwl" "$2" "$3"
    "$PACK" build "$T/flow_bad" "$CORE" "$T/flow_bad.bot" >"$T/lint.log" 2>&1
    check "FWL: $1" "$T/lint.log" "$4"
}
sq_bad "capital pseudo-code is not FWL" 'Skip it.' 'SEARCH LEADS (with the scraper)' 'error: "SEARCH LEADS" is not FWL: only ASK, IF, ELSE'
sq_bad "a made-up capital keyword is not FWL" 'Skip it.' 'CALCULATE {t} = {f} * 2' 'error: "CALCULATE" is not FWL: only ASK, IF, ELSE'
sq_bad "BREAK is not FWL" 'Skip it.' 'BREAK' 'error: FWL has no BREAK'
sq_bad "keyword typo is an error with its line" 'RETRY 2 times:' 'RETRI 2 times:' 'FLOW.fwl:11: error: unknown keyword RETRI; did you mean RETRY'
if [ -e "$T/flow_bad.bot" ]; then fail "FWL: a flow with errors is not built"; else pass "FWL: a flow with errors is not built"; fi
sq_bad "a \$tool must exist" '$fs_read' '$fs_reed' 'FLOW.fwl:12: error: \$fs_reed: no such tool'
sq_bad "a #skill must exist" '#pitching' '#ghost' '#ghost: no such skill \(there is no skills/ghost/SKILL.md\)'
sq_bad "an @agent must exist" 'Skip it.' 'Ask @nobody about it.' '@nobody: no such agent'
sq_bad "marks inside quotes are only text" 'Skip it.' 'Tell the user "mail @nobody about #ghost for $5".' 'wrote .*flow_bad.bot'
sq_bad "the old statements say what to write instead" 'Skip it.' 'EXECUTE tool $fs_read' 'FWL has no EXECUTE: say it in a sentence that names the tool'
sq_bad "nothing is imported" 'STEP 1: GATHER' 'IMPORT:
STEP 1: GATHER' 'FWL has no IMPORT: nothing is imported any more'
sq_bad "ELIF is ELSE IF" '    ELSE IF {f} is empty:' '    ELIF {f} is empty:' 'FWL has no ELIF: write ELSE IF'
sq_bad "backtick names are gone" 'List {folder} with' 'List `folder` with' 'write \{folder\}, not `folder`'
sq_bad "a value read before anything fills it" 'IF {f.name} is "notes.txt":' 'IF {g.name} is "notes.txt":' 'FLOW.fwl:6: error: \{g\} has no value here'
sq_bad "loop value used after its loop" 'RETURN "Read' 'RETURN "{f.name} Read' 'only exists inside its FOR EACH'
sq_bad "a value filled and never used is a warning" 'Skip it.' 'Skip it and call the reason {reson}.' 'FLOW.fwl:9: warning: \{reson\} is filled here and never used again'
sq_bad "END lines are gone" 'RETURN "Read' 'END FOR
RETURN "Read' 'FLOW.fwl:13: error: FWL has no END line: a block ends where its indentation ends'
sq_bad "THEN is gone" '"notes.txt":' '"notes.txt" THEN' 'IF reads: IF <condition>:  \(no THEN'
sq_bad "IF without its colon" '"notes.txt":' '"notes.txt"' 'IF reads: IF <condition>:'
sq_bad "a block with nothing indented under it" '            Read {f.name}' 'Read {f.name}' 'FLOW.fwl:11: error: this line opens a block, but nothing is indented under it'
sq_bad "indentation that matches no block" '        Skip it.' '        Skip it.
      Skip it again.' 'FLOW.fwl:10: error: this line.s indentation matches no block'
sq_bad "indentation with no block above it" 'List {folder}' '    List {folder}' 'FLOW.fwl:3: error: this line is indented deeper than the lines around it'
sq_bad "ELSE lines up with its IF" '    ELSE:' '        ELSE:' 'ELSE lines up with its IF \(the IF on line 6\)'
sq_bad "tabs are not indentation" '        Skip it.' '	Skip it.' 'indent with spaces, not tabs'
sq_bad "CONNECT points to config.json" 'STEP 1: GATHER' 'CONNECT mcp://x AS y
STEP 1: GATHER' 'FWL has no CONNECT: list MCP servers in config.json'
sq_bad "no Markdown fence" 'STEP 1: GATHER' '```fwl
STEP 1: GATHER' 'FLOW.fwl is plain FWL: no ``` fence'
sq_bad "Markdown headings are rejected" 'STEP 1: GATHER' '## STEP 1: GATHER' 'Markdown .* is not part of FWL'
sq_bad "STEP labels are numbered in order" 'STEP 2: WORK' 'STEP 3: WORK' 'warning: STEP 3 follows STEP 1'
sq_bad "decisions written as prose are a warning" 'Skip it.' 'if it is empty, skip it' "FLOW.fwl:9: warning: write 'if' as FWL structure"
sq_bad "ASK choices come at least in twos" '"Which folder?" into' '"Which folder?" [one] into' 'ASK \[...\] needs at least two choices'
sq_bad "ASK USER is just ASK" 'ASK "Which folder?"' 'ASK USER "Which folder?"' 'ASK reads: ASK "question"'
sq_bad "a WHILE wants a bound" 'RETRY 2 times:' 'WHILE {f} is not empty:' 'warning: give the WHILE loop a bound'
mv "$T/flow_good/FLOW.fwl" "$T/flow_good/FLOW.md"
build "$T/flow_good" "$T/flow_bad.bot"
check "FWL: an old FLOW.md is reported" "$T/build.log" "FLOW.md: the flow is now FLOW.fwl"
mv "$T/flow_good/FLOW.md" "$T/flow_good/FLOW.fwl"

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
mkagent "$T/flow_old"
printf '```sqnc\nRETURN "x"\n```\n' >"$T/flow_old/SQNC.md"
"$PACK" check "$T/flow_old" "$CORE" >"$T/build.log" 2>&1
check "an old SQNC.md is reported" "$T/build.log" "SQNC.md: the flow is now FLOW.fwl"
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
check_not "no FLOW.fwl: no flow instruction in the system prompt" "$T/rec/system.txt" "driven by its FLOW.fwl"

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

mkagent "$T/flowagent"
cp -r "$T/flow_good/FLOW.fwl" "$T/flow_good/skills" "$T/flowagent/"
build "$T/flowagent" "$T/flow.bot"
stub_start --text /dev/null
(cd "$T/ws" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\ngo\nhello after the flow\n/exit\n' "$PORT" |
    timeout 60 "$T/flow.bot" >"$T/out" 2>&1)
check "FLOW.fwl present: the flow starts by itself" "$T/out" "Which folder\\?"
check "FLOW.fwl present: system prompt explains the [FWL] steps" "$T/rec/system.txt" "session is driven by its FLOW.fwl flow"

stub_start --text /dev/null
drive "$T/meta.bot" "$T/ws"
check "config.json: the agent shows its display name and version at start" "$T/out" "Meta Agent.*1\.2\.3.*Tests metadata"
check "config.json: the model knows its name and version" "$T/rec/system.txt" 'You are the agent "Meta Agent", version 1\.2\.3\. Tests metadata\.'
mkagent "$T/needcmd" '{"name": "needcmd", "version": "1.0.0", "description": "d", "requires": ["definitely-not-installed-cmd"]}'
build "$T/needcmd" "$T/needcmd.bot"
drive "$T/needcmd.bot" "$T/ws"
check "config.json requires: missing programs are reported at start" "$T/out" "not installed here: definitely-not-installed-cmd"

# FWL interpreter: botcore follows the structure; sentences and judgements go to the LLM
mkagent "$T/sqagent"
cp "$TESTS/run.fwl" "$T/sqagent/FLOW.fwl"
build "$T/sqagent" "$T/sq.bot"
check_not "FWL: the interpreter test flow builds" "$T/build.log" "error"
sq_drive() {
    rm -rf "$T/sqws"; mkdir -p "$T/sqws"
    (cd "$T/sqws" && printf '6\nhttp://127.0.0.1:%s/v1\n\nstub\nAlice\npurple\n2\n/exit\n' "$PORT" |
        timeout 60 "$T/sq.bot" >"$T/out" 2>&1)
}
stub_start --fwl yes
sq_drive
check "FWL: ASK shows the question" "$T/out" "What is your name\\?"
check "FWL: RETURN fills in values, a prose decision, .length, a choice and a tool's own output" "$T/out" "Hi Alice, mood happy, 3 items, green, tool-ran"
check "FWL: decisions get the history as text, no tool calls (Gemini answers those with a call)" "$T/rec/decide_hist.txt" "tool messages: 0, tools offered: 0"
check "FWL: STEP headings are shown" "$T/out" "Step 2 · WORK"
check "FWL: a sentence that names a tool runs it" "$T/out" "Running a command"
check "FWL: the model is told which tool \$name means" "$T/rec/user_all.txt" 'shell_exec means your tool shell_exec: use exactly that tool'
check "FWL: and which value the step produces" "$T/rec/user_all.txt" '\{out\} is what this step produces'
check "FWL: RETRY repeats a failing step" "$T/out" "Retrying \\(2 of 2\\)"
check "FWL: ASK with choices lists them" "$T/out" "1\\) red"
check "FWL: an answer that is no choice is asked again" "$T/out" "Please pick one of the choices"
check "FWL: FOR EACH + IF run the sentence for the matching item" "$T/rec/fwl.txt" "line 12\\] Mention the letter"
check_not "FWL: a comparison of values is decided without the LLM" "$T/rec/fwl.txt" 'Decide whether this condition is true right now: \{it\}'
check "FWL: a condition that needs judgement is decided by the LLM" "$T/rec/fwl.txt" 'Decide whether this condition is true right now: the user \{name\} seems happy'
check "FWL: a decision shows the values it names in full" "$T/rec/decide.txt" '^\{name\} = Alice'
stub_start --fwl no
sq_drive
check "FWL: the LLM's 'no' takes the ELSE branch" "$T/out" "Hi Alice, mood sad"

# Sub-agents: agents/<name>/agent.md, DELEGATE TO in FWL, the delegate tool for the model
mkagent "$T/team"
mkdir -p "$T/team/agents/helper"
printf -- '---\ndescription: Answers pings.\n---\nYou are ZETA, the helper.\n' >"$T/team/agents/helper/agent.md"
cp -r "$T/team" "$T/team2"
printf 'Ask @helper this: "ping". Keep its answer as {got}.\nRETURN "Got: {got}"\n' >"$T/team/FLOW.fwl"
build "$T/team" "$T/team.bot"
check_not "sub-agents: a flow that delegates builds" "$T/build.log" "error"
echo "pong-from-sub" >"$T/pong"
stub_start --fwl yes --text "$T/pong"
drive "$T/team.bot" "$T/ws"
check "sub-agents: a sentence that names @agent gets the sub-agent's own reply" "$T/out" "Got: pong-from-sub"
check "sub-agents: the sub-agent runs on its own persona" "$T/rec/subsystem.txt" "You are ZETA, the helper"
check_not "sub-agents: the frontmatter is not part of the persona" "$T/rec/subsystem.txt" "description: Answers"
check "sub-agents: it sees only the context it was given" "$T/rec/subuser.txt" "^ping"
check_not "sub-agents: a sub-agent cannot delegate further" "$T/rec/subtools.txt" "delegate"
check "sub-agents: the main agent is told about them" "$T/rec/system.txt" "helper: Answers pings"
check "sub-agents: the main agent gets the delegate tool" "$T/rec/tools.txt" "delegate"
build "$T/team2" "$T/team2.bot"
tool_call delegate '{"agent": "helper", "task": "ping2"}'
drive "$T/team2.bot" "$T/ws"
check "sub-agents: the model can delegate by itself" "$T/out" "Handing work to another agent"
check "sub-agents: the task reaches the sub-agent" "$T/rec/subuser.txt" "^ping2"
check "sub-agents: its result comes back as the tool result" "$T/rec/tool.txt" "^done"
sed -i 's/@helper/@nobody/' "$T/team/FLOW.fwl"
build "$T/team" "$T/team_bad.bot"
check "sub-agents: delegating to a missing one is an error" "$T/build.log" '@nobody: no such agent \(a sub-agent agents/nobody/agent.md'
echo "You are a helper." >"$T/team2/agents/helper/agent.md"
build "$T/team2" "$T/team_bad.bot"
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
printf 'Echo with $new_echo, passing {"text": "hi"}, and keep its answer as {a}.\nEcho with $old_echo, passing {"text": "yo"}, and keep its answer as {b}.\nAsk @peer this: "ping". Keep its answer as {c}.\nRETURN "{a} | {b} | {c}"\n' >"$T/net/FLOW.fwl"
build "$T/net" "$T/net.bot"
check_not "MCP/A2A: a flow that uses an MCP tool and a remote agent builds" "$T/build.log" "error"
rm -rf "$T/netws"; mkdir -p "$T/netws"
printf 'MCP_TOKEN=tok-mcp\nexport PEER_TOKEN="tok-a2a"\n' >"$T/netws/.env"
stub_start --fwl yes
drive "$T/net.bot" "$T/netws"
check "MCP: the servers' tools are listed at start" "$T/out" "MCP new: 1 tool"
check "MCP + A2A: tools on both kinds of server and the remote agent answer" "$T/out" "echo:hi \\| echo:yo \\| remote says: ping"
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
stub_start --fwl yes --text "$T/said"
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
printf 'Ask @served this: "are you there?". Keep its answer as {reply}.\nRETURN "Served said: {reply}"\n' >"$T/caller/FLOW.fwl"
build "$T/caller" "$T/caller.bot"
rm -rf "$T/cws"; mkdir -p "$T/cws"
echo "SERVED_TOKEN=open-sesame" >"$T/cws/.env"
drive "$T/caller.bot" "$T/cws"
check "A2A: one Botter agent delegates to another over the network" "$T/out" "Served said: reply-from-the-model"
kill "$A2A" 2>/dev/null
printf 'BOTTER_BASE_URL=http://127.0.0.1:%s/v1\nBOTTER_MODEL=stub\n' "$PORT" >"$T/sws/.env"
(cd "$T/sws" && timeout 10 "$T/served.bot" --a2a "0.0.0.0:$APORT" >"$T/a2a.out" 2>&1)
check "A2A: listening for other machines without a token is refused" "$T/a2a.out" "put A2A_TOKEN=<secret> in .env first"

# fwl_review: the LLM's logic check of a project's FLOW.fwl (builder agents only)
mkagent "$T/builder" '{"builder": true}'
build "$T/builder" "$T/builder.bot"
mkdir -p "$T/revws"
cp "$TESTS/run.fwl" "$T/revws/FLOW.fwl"
echo "An agent that greets people." >"$T/revws/agent.md"
tool_call fwl_review '{}'
drive "$T/builder.bot" "$T/revws"
check "fwl_review: the verdict reaches the agent" "$T/rec/tool.txt" '"makes_sense":false'
check "fwl_review: the report says why and what to do instead" "$T/rec/tool.txt" "why: reads a file that is never written"
check "fwl_review: the reviewer gets FLOW.fwl with line numbers and agent.md" "$T/rec/review.txt" "An agent that greets people"
tool_call fwl_review '{}'
drive "$T/on.bot" "$T/revws"
check "fwl_review: not available to ordinary agents" "$T/rec/tool.txt" "unknown tool 'fwl_review'"

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

    # Botter's TUI: its flow asks first; the first message answers it, later ones are chat tagged [N]
    stub_start --fwl yes
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
    if head -4 "$T/tui.txt" | grep -q "What agent should I build"; then pass "TUI: the flow's question stays at the top when the header goes"; else fail "TUI: the flow's question stays at the top when the header goes"; fi
    check "TUI: the first message answered the flow" "$T/rec/fwl.txt" "Answer the question"

    # TUI after a tool call: streamed reply shown once, tool internals hidden
    stub_start --fwl yes --tool shell_exec --args '{"command":"echo hi"}'
    tmux new-session -d -s botter-test -x 100 -y 30 -c "$T/ws" "env BOTTER_THEME=dark $BOTTER"
    sleep 0.8
    for k in 6 Enter "http://127.0.0.1:$PORT/v1" Enter Enter stub Enter; do
        tmux send-keys -t botter-test "$k"
        sleep 0.3
    done
    sleep 0.8
    tmux send-keys -t botter-test "hello" Enter
    sleep 1
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
