#!/usr/bin/env python3
"""Minimal OpenAI-compatible LLM stub for botcore tests (stdlib only).

  stub_llm.py --portfile F --record DIR --tool NAME --args JSON   first reply: one tool call, then "done"
                                          [--repeat N]              N tool calls in a row, then "done"
                                          [--then NAME --then-args JSON]  then one call of another tool
  stub_llm.py --portfile F --record DIR --script FILE                one scripted step per request ("done" after);
                                                                     every request is logged to DIR/requests.jsonl
  stub_llm.py --portfile F --record DIR --text FILE               reply with FILE's content
  stub_llm.py --portfile F --record DIR --slow                    POST hangs 60 s (for Ctrl-C tests)

Binds 127.0.0.1 on a free port and writes it to --portfile when ready.
Records the last system prompt to DIR/system.txt, the last user message to DIR/user.txt and the
last tool result to DIR/tool.txt.
"""
import argparse, json, os, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--portfile", required=True)
ap.add_argument("--record", required=True)
ap.add_argument("--tool")
ap.add_argument("--args", default="{}")
ap.add_argument("--text")
ap.add_argument("--slow", action="store_true")
ap.add_argument("--nostream", action="store_true", help='answer 400 to "stream": true')
ap.add_argument("--busy", type=int, default=0, help="answer 503 (high demand) to the first N requests")
ap.add_argument("--repeat", type=int, default=1, help="call --tool this many times in one turn")
ap.add_argument("--then", help="after the --tool calls, one call of this tool")
ap.add_argument("--then-args", default="{}")
ap.add_argument("--script", help="JSON list, one step per request: {\"tool\": name, \"args\": {...}} or {\"text\": ...}")
ap.add_argument("--think", action="store_true", help="start the text answer with a <think> block")
a = ap.parse_args()
script = json.load(open(a.script)) if a.script else None
os.makedirs(a.record, exist_ok=True)


def save(name, text):
    with open(os.path.join(a.record, name), "w") as f:
        f.write(text)


class H(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        self.send({"data": [{"id": "stub"}]})

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if a.slow:
            time.sleep(60)
            return
        if a.busy > 0:
            a.busy -= 1
            save("busy.txt", "503 sent")
            self.send({"error": {"code": 503, "message": "This model is currently experiencing high demand."}}, 503)
            return
        msgs = req["messages"]
        with open(os.path.join(a.record, "sizes.txt"), "a") as f:  # request size, for the memory engine tests
            f.write("%d\n" % sum(len(m.get("content") or "") if isinstance(m.get("content"), str) else 0 for m in msgs))
        save("request.json", json.dumps(msgs))
        save("auth.txt", self.headers.get("Authorization", ""))
        if msgs and msgs[0]["role"] == "system":
            save("system.txt", msgs[0]["content"])
        for m in msgs:  # image_view sends a list of parts: the text of it is what the tests read
            if isinstance(m.get("content"), list):
                m["parts"] = m["content"]
                m["content"] = " ".join(p.get("text", "") for p in m["content"] if p.get("type") == "text")
        users = [m for m in msgs if m["role"] == "user"]
        if users:
            save("user.txt", users[-1]["content"])
            with open(os.path.join(a.record, "user_all.txt"), "a") as f:
                f.write(users[-1]["content"] + "\n")
        names = " ".join(t["function"]["name"] for t in req.get("tools") or [])
        if msgs and msgs[0]["role"] == "system" and "You are the sub-agent" in msgs[0]["content"]:
            save("subsystem.txt", msgs[0]["content"])
            save("subuser.txt", users[0]["content"] if users else "")
            save("subtools.txt", names)
        else:
            save("tools.txt", names)

        # the tool result being answered: the last message, or the one before image_view's pictures
        after_tool = msgs[-1] if msgs[-1]["role"] == "tool" else \
            msgs[-2] if msgs[-1].get("parts") and len(msgs) > 1 and msgs[-2]["role"] == "tool" else None
        last_user = max([i for i, m in enumerate(msgs) if m["role"] == "user" and not m.get("parts")] or [0])
        turn_tools = sum(1 for m in msgs[last_user:] if m["role"] == "tool")
        stream = bool(req.get("stream"))
        save("stream.txt", "stream" if stream else "plain")
        save("sampling.txt", "temperature=%s seed=%s reasoning=%s usage=%s" % (req.get("temperature"), req.get("seed"), req.get("reasoning_effort"),
                                                                 (req.get("stream_options") or {}).get("include_usage")))
        parts = [p for m in msgs for p in m.get("parts", [])]
        if parts:
            save("parts.txt", " ".join(p.get("type", "?") for p in parts))
            fmts = [p.get("input_audio", {}).get("format", "?") for p in parts if p.get("type") == "input_audio"]
            if fmts:
                save("audio.txt", " ".join(fmts))
            with open(os.path.join(a.record, "parts_all.txt"), "a") as f:
                f.write(" ".join(p.get("type", "?") for p in parts) + "\n")
        if stream and a.nostream:
            self.send({"error": {"message": "stream not supported"}}, 400)
            return
        for m in msgs:
            if m.get("tool_calls"):
                save("calls.txt", json.dumps(m["tool_calls"]))

        def sse(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
            time.sleep(0.02)

        usage = {"prompt_tokens": 11, "completion_tokens": 7}

        def reply(msg):
            if not stream:
                self.send({"choices": [{"message": dict(role="assistant", **msg), "finish_reason": "stop"}],
                           "usage": usage})
                return
            # Server-Sent Events: text in 3-character pieces, tool call arguments split in two
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(b": keep-alive comment\n\n")
            text = msg.get("content") or ""
            for i in range(0, len(text), 3):
                sse({"choices": [{"index": 0, "delta": {"content": text[i:i + 3]}}]})
            for k, c in enumerate(msg.get("tool_calls") or []):
                args = c["function"]["arguments"]
                h = len(args) // 2
                first = {"index": k, "id": c["id"], "type": "function",
                         "function": {"name": c["function"]["name"], "arguments": args[:h]},
                         "extra_content": {"google": {"thought_signature": "sig-%d" % k}}}
                sse({"choices": [{"index": 0, "delta": {"tool_calls": [first]}}]})
                sse({"choices": [{"index": 0, "delta": {"tool_calls": [{"index": k, "function": {"arguments": args[h:]}}]}}]})
            sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
            if (req.get("stream_options") or {}).get("include_usage"):
                sse({"choices": [], "usage": usage})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()

        if script is not None:
            with open(os.path.join(a.record, "requests.jsonl"), "a") as f:
                f.write(json.dumps({"system": msgs[0]["content"] if msgs and msgs[0]["role"] == "system" else "",
                                    "user": users[0]["content"] if users else "", "tools": names.split(),
                                    "last": msgs[-1].get("content") if msgs else ""}) + "\n")
            step = script.pop(0) if script else {"text": "done"}
            if "tool" in step:
                call = {"id": "s%d" % len(msgs), "type": "function",
                        "function": {"name": step["tool"], "arguments": json.dumps(step.get("args", {}))}}
                reply({"content": None, "tool_calls": [call]})
            else:
                reply({"content": step.get("text", "done")})
        elif a.text:
            reply({"content": open(a.text).read()})
        elif a.tool and (not after_tool or turn_tools < a.repeat):
            call = {"id": "c%d" % (turn_tools + 1), "type": "function", "function": {"name": a.tool, "arguments": a.args}}
            reply({"content": None, "tool_calls": [call]})
        elif a.then and turn_tools == a.repeat:
            call = {"id": "t1", "type": "function", "function": {"name": a.then, "arguments": a.then_args}}
            reply({"content": None, "tool_calls": [call]})
        else:
            if after_tool:
                save("tool.txt", after_tool["content"])
                with open(os.path.join(a.record, "tool_all.txt"), "a") as f:
                    f.write(after_tool["content"] + "\n")
            reply({"content": "<think>pondering the task</think>Hello there, streamed reply." if a.think else "done"})


srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
tmp = a.portfile + ".tmp"
with open(tmp, "w") as f:
    f.write(str(srv.server_address[1]))
os.rename(tmp, a.portfile)
srv.serve_forever()
