#!/usr/bin/env python3
"""Drive an agent over ACP like an editor does (stdlib only).

  acp_client.py BOT WORKDIR PERMISSION PROMPT...   PERMISSION = y | n (the answer to permission requests)
Every message from the agent is printed, one JSON object per line.
"""
import json, subprocess, sys

bot, cwd, perm, prompts = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
p = subprocess.Popen([bot, "--acp"], cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)


def send(o):
    p.stdin.write(json.dumps(dict(o, jsonrpc="2.0")) + "\n")
    p.stdin.flush()


def until(rid):
    for line in p.stdout:
        m = json.loads(line)
        print(json.dumps(m), flush=True)
        if m.get("method") == "session/request_permission":
            send({"id": m["id"], "result": {"outcome": {"outcome": "selected", "optionId": perm}}})
        if m.get("id") == rid and "method" not in m:
            return m
    sys.exit("the agent closed the connection")


send({"id": 0, "method": "initialize", "params": {"protocolVersion": 1, "clientCapabilities": {}}})
until(0)
send({"id": 1, "method": "session/new", "params": {"cwd": cwd, "mcpServers": []}})
sid = until(1)["result"]["sessionId"]
for i, text in enumerate(prompts):
    send({"id": 10 + i, "method": "session/prompt",
          "params": {"sessionId": sid, "prompt": [{"type": "text", "text": text}]}})
    until(10 + i)
p.stdin.close()
p.wait(timeout=10)
