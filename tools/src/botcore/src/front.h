#ifndef BC_FRONT_H
#define BC_FRONT_H

/*
 * Front ends: standard protocols and batch mode on the same hooks, so the agent itself does not change
 * (none starts a process; only --a2a and --mcp PORT listen, because being called needs it):
 *   --acp              Agent Client Protocol: an editor starts the agent and talks JSON-RPC over
 *                      stdin/stdout (initialize, session/new, session/prompt, session/update,
 *                      session/request_permission, session/cancel).
 *   --a2a [HOST:]PORT  Agent2Agent 1.0 (and 0.3) JSON-RPC over HTTP: the agent card at
 *                      /.well-known/agent-card.json, SendMessage answered with a Message.
 *                      Listens on 127.0.0.1 unless HOST says otherwise; A2A_TOKEN in .env is the
 *                      bearer token callers must send (required for any other HOST).
 *   --mcp [[HOST:]PORT]  Model Context Protocol server: the agent is one tool, "ask", for any MCP
 *                      client (a coding agent, an editor). Without a port the client starts the
 *                      agent and talks over stdin/stdout; with one it listens for Streamable HTTP
 *                      like --a2a does (MCP_TOKEN is the bearer token).
 *   --run ["task"]     Batch: one task per stdin line (or the one given), each in a fresh conversation;
 *                      stdout gets one JSON line per task {"input","reply","tools"[,"error"]} (tools = the
 *                      tools it called; the reply alone for a single task); everything else goes to stderr,
 *                      with token usage at the end. Checking the results is the caller's job (Botter: Lua).
 * Remote users cannot type /commands, are never asked for keys (the model connection is the
 * saved one, or BOTTER_BASE_URL / BOTTER_API_KEY / BOTTER_MODEL in .env), and risky actions are
 * asked through the protocol (ACP) or refused (A2A, MCP).
 */
int  front_init(int argc, char **argv); /* 1 if a front end is on */
int  front_active(void);
int  front_remote(void);          /* --acp, --a2a, --mcp or --run */
int  front_batch(void);           /* --run */
const char *front_call(void);     /* --call NAME: the Lua function every input line goes to, else NULL */
void front_result(const char *line); /* --run: one line on the results stream (stdout) */
int  front_take_cmd(void);        /* 1 if the line just read is the front end's own command (/new) */
void front_set_card(void *card);  /* cJSON: name, description, version, skills (takes it) */
/* Emit a record: event name, then key/value string pairs, NULL-terminated. */
void front_event(const char *ev, ...) __attribute__((sentinel));
/* term_readline() in a front end (same return codes): the next request, or a protocol answer. */
int  front_readline(const char *prompt, int secret, char **out);

#endif
