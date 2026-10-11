#!/usr/bin/env python3
"""A tiny MCP server over stdio, the way most MCP servers are run (stdlib only).

One tool, shout: the text in capitals, plus $LOUD_SUFFIX, plus whether this process may write
to the home directory (it runs inside the agent's sandbox, so it may not).
"""
import json, os, sys

for line in sys.stdin:
    q = json.loads(line)
    m, rid, p = q.get("method"), q.get("id"), q.get("params") or {}
    if rid is None:
        continue
    if m == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "loud", "version": "1"}}
    elif m == "tools/list":
        res = {"tools": [{"name": "shout", "description": "Say it loudly.",
                          "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]}
    elif m == "tools/call":
        try:
            open(os.path.expanduser("~/.botter_test_probe"), "w").close()
            home = "home-writable"
        except OSError:
            home = "home-denied"
        res = {"content": [{"type": "text", "text": p["arguments"]["text"].upper() + os.environ.get("LOUD_SUFFIX", "") + " " + home}]}
    else:
        print(json.dumps({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "no"}}), flush=True)
        continue
    print(json.dumps({"jsonrpc": "2.0", "id": rid, "result": res}), flush=True)
