#!/usr/bin/env python3
"""Fake peers for the MCP / A2A client tests (stdlib only).

  remote_servers.py PORTFILE RECORD_DIR
    /mcp-new   a stateless MCP server (2026-07-28): no handshake, headers say what is asked
    /mcp-old   a session MCP server: initialize first, Mcp-Session-Id after, answers as SSE
    /peer      an A2A 1.0 agent: card, SendMessage -> a working task, GetTask -> completed
  Every request needs "Authorization: Bearer tok-<mcp|a2a>".
"""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

rec = sys.argv[2]
os.makedirs(rec, exist_ok=True)


def save(name, text):
    with open(os.path.join(rec, name), "a") as f:
        f.write(text + "\n")


TOOLS = [{"name": "echo", "description": "Echo the text back.",
          "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def out(self, status, obj=None, sse=False, headers=()):
        b = b"" if obj is None else json.dumps(obj).encode()
        if sse:
            b = b"event: message\ndata: " + json.dumps({"jsonrpc": "2.0", "method": "notifications/progress"}).encode() + \
                b"\n\nevent: message\ndata: " + b + b"\n\n"
        self.send_response(status)
        self.send_header("Content-Type", "text/event-stream" if sse else "application/json")
        self.send_header("Content-Length", str(len(b)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/peer/.well-known/agent-card.json":
            port = self.server.server_address[1]
            self.out(200, {"name": "Peer", "description": "A test peer.", "version": "1", "capabilities": {},
                           "defaultInputModes": ["text/plain"], "defaultOutputModes": ["text/plain"], "skills": [],
                           "supportedInterfaces": [{"url": "http://127.0.0.1:%d/peer/rpc" % port,
                                                    "protocolBinding": "JSONRPC", "protocolVersion": "1.0"}]})
        else:
            self.out(404)

    def do_POST(self):
        q = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        m, rid, p = q.get("method"), q.get("id"), q.get("params") or {}
        ok = lambda res, **kw: self.out(200, {"jsonrpc": "2.0", "id": rid, "result": res}, **kw)
        err = lambda code, msg, st=200: self.out(st, {"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": msg}})
        kind = "a2a" if self.path.startswith("/peer") else "mcp"
        if self.headers.get("Authorization") != "Bearer tok-" + kind:
            return self.out(401, {"error": "no token"})
        if self.path == "/peer/rpc":
            save("a2a.txt", "%s version=%s" % (m, self.headers.get("A2A-Version")))
            if m == "SendMessage":
                self.server.task_text = p["message"]["parts"][0]["text"]
                save("a2a.txt", "role=%s" % p["message"]["role"])
                ok({"task": {"id": "t1", "contextId": "c1", "status": {"state": "TASK_STATE_WORKING"}}})
            elif m == "GetTask" and p.get("id") == "t1":
                ok({"task": {"id": "t1", "contextId": "c1", "status": {"state": "TASK_STATE_COMPLETED"},
                             "artifacts": [{"artifactId": "a1", "parts": [{"text": "remote says: " + self.server.task_text}]}]}})
            else:
                err(-32601, "method not found")
            return
        old = self.path == "/mcp-old"
        save(kind + ("-old" if old else "-new") + ".txt", "%s version=%s method=%s name=%s session=%s meta=%s" % (
            m, self.headers.get("MCP-Protocol-Version"), self.headers.get("Mcp-Method"), self.headers.get("Mcp-Name"),
            self.headers.get("Mcp-Session-Id"), "yes" if "_meta" in p else "no"))
        if old:
            if m == "initialize":
                return ok({"protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
                           "serverInfo": {"name": "old", "version": "1"}}, headers=[("Mcp-Session-Id", "sid-1")])
            if self.headers.get("Mcp-Session-Id") != "sid-1":
                return err(-32000, "Bad Request: no valid session ID", 400)
            if m == "notifications/initialized":
                return self.out(202)
        elif m == "initialize" or self.headers.get("MCP-Protocol-Version") != "2026-07-28":
            return err(-32601, "this server is stateless: no handshake", 400)
        if m == "tools/list":
            ok({"tools": TOOLS}, sse=old)
        elif m == "tools/call" and p.get("name") == "echo":
            ok({"content": [{"type": "text", "text": "echo:" + p["arguments"].get("text", "")}], "isError": False})
        else:
            err(-32602, "unknown tool")


srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
with open(sys.argv[1] + ".tmp", "w") as f:
    f.write(str(srv.server_address[1]))
os.rename(sys.argv[1] + ".tmp", sys.argv[1])
srv.serve_forever()
