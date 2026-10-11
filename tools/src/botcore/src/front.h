#ifndef BC_FRONT_H
#define BC_FRONT_H

/*
 * Frontend protocol: lets a separate UI program drive botcore over pipes.
 * Enabled only when the process starts with BOTCORE_FRONTEND=1 (the variable is
 * removed so tools never see it). Without it nothing changes: plain REPL.
 *
 * botcore -> UI (stdout; stderr is merged into it so order is kept):
 *   ordinary output as before (may contain SGR colour codes), interleaved with
 *   records "\x1e" + one-line JSON object + "\n". Records:
 *     {"ev":"prompt","text":<prompt>,"secret":bool}   waiting for one input line
 *     {"ev":"busy","label":<text>} / {"ev":"idle"}    work in progress / done
 *     {"ev":"info","model":..,"cwd":..}                session details
 *     {"ev":"thinking","text":..}                      model reasoning
 *     {"ev":"text","text":..}                          assistant text sent alongside tool calls
 *     {"ev":"reply","text":..}                         final answer (markdown)
 *     {"ev":"tool","name":..,"args":..,"status":..} / {"ev":"tool_result","text":..}
 *     {"ev":"delta","kind":"text"|"thinking","text":..}  streamed piece of a reply
 *     {"ev":"mode","mode":"plan"|"build"}             Plan/Build mode
 *     {"ev":"flow"}                                   the agent's FLOW.md flow starts
 * UI -> botcore (stdin, one JSON object per line, answers a prompt):
 *     {"line":<text>} | {"eof":true} | {"intr":true}
 *   Interrupting work = SIGINT to the botcore process.
 */

/*
 * Two more front ends speak standard protocols instead (same hooks, so the agent itself does
 * not change; neither starts a process, and only --a2a listens, because being called needs it):
 *   --acp              Agent Client Protocol: an editor starts the agent and talks JSON-RPC over
 *                      stdin/stdout (initialize, session/new, session/prompt, session/update,
 *                      session/request_permission, session/cancel).
 *   --a2a [HOST:]PORT  Agent2Agent 1.0 (and 0.3) JSON-RPC over HTTP: the agent card at
 *                      /.well-known/agent-card.json, SendMessage answered with a Message.
 *                      Listens on 127.0.0.1 unless HOST says otherwise; A2A_TOKEN in .env is the
 *                      bearer token callers must send (required for any other HOST).
 * Remote users cannot type /commands, are never asked for keys (the model connection is the
 * saved one, or BOTTER_BASE_URL / BOTTER_API_KEY / BOTTER_MODEL in .env), and risky actions are
 * asked through the protocol (ACP) or refused (A2A).
 */
int  front_init(int argc, char **argv); /* 1 if a front end is on */
int  front_active(void);
int  front_remote(void);          /* --acp or --a2a */
int  front_take_cmd(void);        /* 1 if the line just read is the front end's own command (/new) */
void front_set_card(void *card);  /* cJSON: name, description, version, skills (takes it) */
/* Emit a record: event name, then key/value string pairs, NULL-terminated. */
void front_event(const char *ev, ...) __attribute__((sentinel));
/* term_readline() replacement used in frontend mode (same return codes). */
int  front_readline(const char *prompt, int secret, int (*idle)(void), char **out);

#endif
