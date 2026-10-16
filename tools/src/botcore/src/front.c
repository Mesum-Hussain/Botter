#define _GNU_SOURCE
#include "front.h"
#include "term.h"
#include "cJSON.h"

#include "remote.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { F_OFF, F_UI, F_ACP, F_A2A, F_MCP, F_RUN };
static int g_on; /* F_* */

/* --acp / --a2a: the request being answered and the conversation it belongs to */
static int         g_out = STDOUT_FILENO; /* ACP: where JSON-RPC goes (stdout itself is silenced) */
static cJSON      *g_card, *g_rid;        /* who we are; id of the request in flight */
static char        g_sess[100];           /* session (ACP) / context (A2A) of the conversation in RAM */
static char       *g_queued;              /* a prompt held back behind our own "/new" */
static int         g_cmd, g_streamed, g_calls, g_seq;
static char        g_perm[700];           /* what the agent wants permission for */
static volatile sig_atomic_t g_busy, g_cancel;
static int         g_lsock = -1, g_conn = -1, g_v03; /* A2A: listener, caller, caller speaks 0.3 */
static const char *g_token;
static char       *g_acc;                 /* A2A: the answer so far */
static char        g_host[200];
static const char *g_task;                /* --run "task": the only one */
static char       *g_input, *g_err;       /* --run: the task in flight, and why it failed */
static const char *g_eval_dir;            /* --eval: where the cases are */

int front_remote(void) { return g_on >= F_ACP; }

int front_batch(void) { return g_on == F_RUN; }

const char *front_eval_dir(void) { return g_eval_dir; }

void front_result(const char *line)
{
    for (size_t off = 0, l = strlen(line); off < l;) {
        ssize_t w = write(g_out, line + off, l - off);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    if (write(g_out, "\n", 1) < 0) {
        return;
    }
}

int front_take_cmd(void)
{
    int c = g_cmd;
    g_cmd = 0;
    return c;
}

void front_set_card(void *card) { g_card = card; }

static void on_sigio(int sig)
{
    (void)sig;
    if (g_busy) { /* the editor wrote while a turn runs: session/cancel */
        g_cancel = 1;
        kill(getpid(), SIGINT);
    }
}

/* ACP: while a turn runs, input on stdin interrupts it (there is no one reading stdin then). */
static void busy(int on)
{
    if (g_on == F_ACP) {
        int fl = fcntl(STDIN_FILENO, F_GETFL);
        g_busy = on;
        fcntl(STDIN_FILENO, F_SETFL, on ? fl | O_ASYNC : fl & ~O_ASYNC);
    }
}

/* Listen for HTTP callers (A2A, or MCP over HTTP). token_name: the .env / vault line callers must match. */
static int a2a_listen(const char *addr, const char *flag, const char *token_name)
{
    const char *colon = strrchr(addr, ':');
    char host[64] = "127.0.0.1";
    if (colon) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - addr), addr);
    }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(colon ? colon + 1 : addr))};
    g_token = env_get(token_name);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 || !sa.sin_port) {
        fprintf(stderr, "%s wants [HOST:]PORT, e.g. %s 8080 or %s 0.0.0.0:8080\n", flag, flag, flag);
        return -1;
    }
    if (ntohl(sa.sin_addr.s_addr) >> 24 != 127 && !(g_token && *g_token)) {
        fprintf(stderr, "%s %s is reachable from other machines: put %s=<secret> in .env first "
                        "(callers send it as their bearer token)\n", flag, addr, token_name);
        return -1;
    }
    int one = 1;
    g_lsock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    setsockopt(g_lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (g_lsock < 0 || bind(g_lsock, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(g_lsock, 16) != 0) {
        fprintf(stderr, "%s %s: %s\n", flag, addr, strerror(errno));
        return -1;
    }
    fprintf(stderr, "%s: listening on http://%s:%d%s%s\n", flag + 2, host, ntohs(sa.sin_port),
            flag[2] == 'a' ? " (agent card: /.well-known/agent-card.json)" : " (MCP endpoint: /)",
            g_token && *g_token ? "" : ", no token: this machine only");
    return 0;
}

/* stdin is read through our own buffer (poll + read; no stdio buffering). */
static char  *g_rb;
static size_t g_rlen, g_rcap;

int front_init(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--acp")) {
        int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
        g_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        dup2(nul, STDOUT_FILENO); /* stdout carries the protocol only; notes go to stderr */
        fcntl(STDIN_FILENO, F_SETOWN, getpid());
        signal(SIGIO, on_sigio);
        g_on = F_ACP;
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--a2a")) {
        if (a2a_listen(argc > 2 ? argv[2] : "", "--a2a", "A2A_TOKEN")) {
            exit(2);
        }
        signal(SIGPIPE, SIG_IGN);
        g_on = F_A2A;
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--mcp")) { /* an MCP server: over stdio, or over HTTP when given a port */
        if (argc > 2) {
            if (a2a_listen(argv[2], "--mcp", "MCP_TOKEN")) {
                exit(2);
            }
            signal(SIGPIPE, SIG_IGN);
        } else {
            int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
            g_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
            dup2(nul, STDOUT_FILENO);
        }
        g_on = F_MCP;
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--eval")) { /* the agent's eval cases, against the real model */
        g_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        g_eval_dir = argc > 2 ? argv[2] : "evals";
        g_on = F_RUN;
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--run")) { /* batch: tasks in, results out, nobody to ask */
        g_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        dup2(STDERR_FILENO, STDOUT_FILENO); /* stdout carries the results only; notes go to stderr */
        g_task = argc > 2 ? argv[2] : NULL;
        g_on = F_RUN;
        return 1;
    }
    const char *v = getenv("BOTCORE_FRONTEND");
    if (!v || strcmp(v, "1") != 0) {
        return 0;
    }
    unsetenv("BOTCORE_FRONTEND");
    dup2(STDOUT_FILENO, STDERR_FILENO); /* one ordered stream for the UI */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_on = F_UI;
    return 1;
}

int front_active(void) { return g_on != F_OFF; }

/* ------------------------------------------------------------ ACP and A2A */

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* A JSON-RPC message to the editor (takes o). */
static void acp_send(cJSON *o)
{
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    char *s = cJSON_PrintUnformatted(o);
    size_t n = s ? strlen(s) : 0;
    if (s) {
        s[n] = '\n'; /* the terminating NUL is not sent */
        for (size_t off = 0; off <= n;) {
            ssize_t w = write(g_out, s + off, n + 1 - off);
            if (w <= 0 && errno != EINTR) {
                break;
            }
            off += w > 0 ? (size_t)w : 0;
        }
    }
    free(s);
    cJSON_Delete(o);
}

static void acp_result(const cJSON *id, cJSON *result, int code, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    if (err) {
        cJSON *e = cJSON_AddObjectToObject(o, "error");
        cJSON_AddNumberToObject(e, "code", code);
        cJSON_AddStringToObject(e, "message", err);
        cJSON_Delete(result);
    } else {
        cJSON_AddItemToObject(o, "result", result);
    }
    acp_send(o);
}

static cJSON *text_block(const char *t)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "text");
    cJSON_AddStringToObject(b, "text", t);
    return b;
}

/* session/update of the turn in flight (takes fields). */
static void acp_update(const char *kind, cJSON *fields)
{
    if (!g_rid) {
        cJSON_Delete(fields);
        return;
    }
    cJSON *o = cJSON_CreateObject(), *p = cJSON_AddObjectToObject(o, "params");
    cJSON_AddStringToObject(o, "method", "session/update");
    cJSON_AddStringToObject(p, "sessionId", g_sess);
    cJSON_AddStringToObject(fields, "sessionUpdate", kind);
    cJSON_AddItemToObject(p, "update", fields);
    acp_send(o);
}

static void acp_chunk(const char *kind, const char *text)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddItemToObject(f, "content", text_block(text));
    acp_update(kind, f);
}

/* What the agent does, as the protocol in use shows it (o: the event, as the UI gets it). */
static void remote_event(const cJSON *o)
{
    const char *ev = jstr(o, "ev"), *text = jstr(o, "text");
    if (!strcmp(ev, "permission")) {
        snprintf(g_perm, sizeof(g_perm), "%s", text);
    } else if (!strcmp(ev, "error")) {
        free(g_err);
        g_err = strdup(text);
    } else if (g_on == F_RUN) {
        if (!strcmp(ev, "reply")) { /* the result is the final answer, not the notes before tool calls */
            free(g_acc);
            g_acc = strdup(text);
        }
    } else if (g_on == F_A2A || g_on == F_MCP) {
        if ((!strcmp(ev, "reply") || !strcmp(ev, "text")) && *text) { /* the answer is the text, whole */
            char *n = NULL;
            if (asprintf(&n, "%s%s%s", g_acc ? g_acc : "", g_acc ? "\n\n" : "", text) >= 0) {
                free(g_acc);
                g_acc = n;
            }
        }
    } else if (!strcmp(ev, "delta")) {
        int th = !strcmp(jstr(o, "kind"), "thinking");
        g_streamed |= !th;
        acp_chunk(th ? "agent_thought_chunk" : "agent_message_chunk", text);
    } else if (!strcmp(ev, "thinking")) {
        acp_chunk("agent_thought_chunk", text);
    } else if (!strcmp(ev, "text") || !strcmp(ev, "reply")) {
        if (!g_streamed && *text) { /* not already sent piece by piece */
            acp_chunk("agent_message_chunk", text);
        }
        g_streamed = 0;
    } else if (!strcmp(ev, "tool") || !strcmp(ev, "tool_result")) {
        int call = !strcmp(ev, "tool");
        const char *nm = jstr(o, "name");
        char id[32];
        snprintf(id, sizeof(id), "call_%d", g_calls += call);
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "toolCallId", id);
        if (call) {
            cJSON *in = cJSON_Parse(jstr(o, "args"));
            cJSON_AddStringToObject(f, "title", *jstr(o, "status") ? jstr(o, "status") : nm);
            cJSON_AddStringToObject(f, "kind", !strcmp(nm, "fs_write") ? "edit" : !strcmp(nm, "shell_exec") ? "execute"
                                               : !strncmp(nm, "fs_", 3) || !strcmp(nm, "skill") ? "read" : "other");
            cJSON_AddStringToObject(f, "status", "in_progress");
            cJSON_AddItemToObject(f, "rawInput", in ? in : cJSON_CreateObject());
            g_streamed = 0;
        } else {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "status", strncmp(text, "ERROR", 5) ? "completed" : "failed");
            cJSON_AddStringToObject(c, "type", "content");
            cJSON_AddItemToObject(c, "content", text_block(text));
            cJSON_AddItemToArray(cJSON_AddArrayToObject(f, "content"), c);
        }
        acp_update(call ? "tool_call" : "tool_call_update", f);
    }
}

/* A2A: one HTTP response, then the connection is closed. */
static void a2a_respond(int c, int status, cJSON *body)
{
    char *s = body ? cJSON_PrintUnformatted(body) : NULL, head[200];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, status == 200 ? "OK" : status == 202 ? "Accepted" : status == 401 ? "Unauthorized" : "Error",
                     s ? strlen(s) : 0);
    if (send(c, head, (size_t)n, MSG_NOSIGNAL) == n && s) {
        for (size_t off = 0, l = strlen(s); off < l;) {
            ssize_t w = send(c, s + off, l - off, MSG_NOSIGNAL);
            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
    }
    free(s);
    cJSON_Delete(body);
    close(c);
}

static void a2a_error(int c, const cJSON *id, int code, const char *msg)
{
    cJSON *o = cJSON_CreateObject(), *e;
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    cJSON_AddItemToObject(o, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    a2a_respond(c, 200, o);
}

/* The request in flight gets its answer: the turn is over. */
/* --run: write all of s to the results stream */
static void out_all(const char *s)
{
    for (size_t off = 0, l = strlen(s); off < l;) {
        ssize_t w = write(g_out, s + off, l - off);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
}

static void finish_request(void)
{
    busy(0);
    if (g_on == F_RUN && g_input) {
        if (g_task) { /* one task: the answer as it is */
            out_all(g_acc ? g_acc : "");
            out_all("\n");
        } else { /* one JSON line per task */
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "input", g_input);
            cJSON_AddItemToObject(o, "reply", g_acc ? cJSON_CreateString(g_acc) : cJSON_CreateNull());
            if (!g_acc || g_err) {
                cJSON_AddStringToObject(o, "error", g_err ? g_err : "no answer");
            }
            char *line = cJSON_PrintUnformatted(o);
            out_all(line ? line : "{}");
            out_all("\n");
            free(line);
            cJSON_Delete(o);
        }
        free(g_input);
        g_input = NULL;
        free(g_err);
        g_err = NULL;
    } else if (g_on == F_ACP && g_rid) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "stopReason", g_cancel ? "cancelled" : "end_turn");
        acp_result(g_rid, r, 0, NULL);
    } else if (g_on == F_MCP && g_rid) { /* the tool call's result: what the agent said */
        cJSON *res = cJSON_CreateObject(), *o = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(res, "content"), text_block(g_acc ? g_acc : "(the agent had nothing to say)"));
        cJSON_AddBoolToObject(res, "isError", 0);
        cJSON_AddItemToObject(o, "id", cJSON_Duplicate(g_rid, 1));
        cJSON_AddItemToObject(o, "result", res);
        if (g_conn >= 0) {
            cJSON_AddStringToObject(o, "jsonrpc", "2.0");
            a2a_respond(g_conn, 200, o);
            g_conn = -1;
        } else {
            acp_send(o);
        }
    } else if (g_on == F_A2A && g_conn >= 0) {
        cJSON *o = cJSON_CreateObject(), *m = cJSON_CreateObject(), *part = cJSON_CreateObject();
        char mid[40];
        snprintf(mid, sizeof(mid), "msg-%d", ++g_seq);
        cJSON_AddStringToObject(o, "jsonrpc", "2.0");
        cJSON_AddItemToObject(o, "id", g_rid ? cJSON_Duplicate(g_rid, 1) : cJSON_CreateNull());
        cJSON_AddStringToObject(m, "messageId", mid);
        cJSON_AddStringToObject(m, "contextId", g_sess);
        cJSON_AddStringToObject(m, "role", g_v03 ? "agent" : "ROLE_AGENT");
        cJSON_AddStringToObject(part, "text", g_acc ? g_acc : "(the agent had nothing to say)");
        if (g_v03) { /* 0.3: the result is the message itself, with kinds */
            cJSON_AddStringToObject(m, "kind", "message");
            cJSON_AddStringToObject(part, "kind", "text");
            cJSON_AddItemToObject(o, "result", m);
        } else {
            cJSON_AddItemToObject(cJSON_AddObjectToObject(o, "result"), "message", m);
        }
        cJSON_AddItemToArray(cJSON_AddArrayToObject(m, "parts"), part);
        a2a_respond(g_conn, 200, o);
        g_conn = -1;
    }
    cJSON_Delete(g_rid);
    g_rid = NULL;
    g_cancel = 0;
    free(g_acc);
    g_acc = NULL;
}

/* The line main gets: the prompt, or first our own "/new" when it belongs to another conversation. */
static int take_prompt(const char *sess, char *text, char **out)
{
    int other = g_sess[0] && strcmp(g_sess, sess) != 0;
    snprintf(g_sess, sizeof(g_sess), "%s", sess);
    busy(1);
    if (other) {
        g_queued = text;
        g_cmd = 1;
        text = strdup("/new");
    }
    *out = text;
    return *out ? TERM_LINE : TERM_EOF;
}

/* The text of an ACP prompt / A2A parts array: every text there is, one per line. */
static char *blocks_text(const cJSON *blocks)
{
    char *out = strdup("");
    const cJSON *b;
    cJSON_ArrayForEach(b, blocks)
    {
        const char *t = jstr(b, "text");
        const cJSON *res = cJSON_GetObjectItem(b, "resource");
        t = *t ? t : *jstr(res, "text") ? jstr(res, "text") : jstr(b, "uri");
        char *n = NULL;
        if (*t && asprintf(&n, "%s%s%s", out, *out ? "\n" : "", t) >= 0) {
            free(out);
            out = n;
        }
    }
    return out;
}

static int read_line(int timeout_ms, char **line);

/*
 * MCP server: one request. Everything but a tool call is answered here (*reply: the JSON-RPC
 * message to send, NULL for a notification); a call of the agent's one tool, "ask", becomes
 * the next prompt (returns its text; the result follows when the turn is over).
 */
static char *mcp_request(const cJSON *j, cJSON **reply, char sess[100])
{
    const char *m = jstr(j, "method");
    const cJSON *id = cJSON_GetObjectItem(j, "id"), *p = cJSON_GetObjectItem(j, "params");
    cJSON *res = NULL;
    const char *err = NULL;
    int code = -32601;
    *reply = NULL;
    if (!*m || !id) { /* a notification (initialized, cancelled) or a stray response */
        return NULL;
    }
    if (!strcmp(m, "initialize") || !strcmp(m, "server/discover")) {
        const char *v = jstr(p, "protocolVersion");
        cJSON *info;
        res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "protocolVersion", *v ? v : "2025-06-18");
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(res, "capabilities"), "tools");
        info = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(info, "name", jstr(g_card, "name"));
        cJSON_AddStringToObject(info, "version", jstr(g_card, "version"));
        cJSON_AddStringToObject(res, "instructions", jstr(g_card, "description"));
    } else if (!strcmp(m, "ping")) {
        res = cJSON_CreateObject();
    } else if (!strcmp(m, "tools/list")) {
        char d[900];
        cJSON *t = cJSON_CreateObject();
        snprintf(d, sizeof(d), "Ask the agent \"%.80s\": %.500s Send one message and get its reply. It remembers the "
                               "conversation between calls; pass new_conversation: true to start over.",
                 jstr(g_card, "name"), jstr(g_card, "description"));
        cJSON_AddStringToObject(t, "name", "ask");
        cJSON_AddStringToObject(t, "description", d);
        cJSON_AddItemToObject(t, "inputSchema",
                              cJSON_Parse("{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\",\"description\":"
                                          "\"What to say to the agent\"},\"new_conversation\":{\"type\":\"boolean\"}},"
                                          "\"required\":[\"message\"]}"));
        res = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(res, "tools"), t);
    } else if (!strcmp(m, "tools/call")) {
        const cJSON *a = cJSON_GetObjectItem(p, "arguments");
        const char *msg = jstr(a, "message");
        if (strcmp(jstr(p, "name"), "ask") != 0 || !*msg) {
            code = -32602;
            err = "this server has one tool, ask, and it needs a \"message\"";
        } else {
            g_seq += cJSON_IsTrue(cJSON_GetObjectItem(a, "new_conversation")) || !g_sess[0];
            snprintf(sess, 100, "mcp-%d", g_seq);
            g_rid = cJSON_Duplicate(id, 1);
            return strdup(msg);
        }
    } else {
        err = "method not found";
    }
    *reply = cJSON_CreateObject();
    cJSON_AddItemToObject(*reply, "id", cJSON_Duplicate(id, 1));
    if (err) {
        cJSON *e = cJSON_AddObjectToObject(*reply, "error");
        cJSON_AddNumberToObject(e, "code", code);
        cJSON_AddStringToObject(e, "message", err);
    } else {
        cJSON_AddItemToObject(*reply, "result", res);
    }
    return NULL;
}

/* MCP over stdio: the client started the agent and writes one JSON-RPC message per line. */
static int mcp_next(int (*idle)(void), char **out)
{
    for (;;) {
        char *line = NULL, sess[100], *text;
        int r = read_line(idle ? 1000 : -1, &line);
        if (r == 1 && idle && idle()) {
            return TERM_TICK;
        }
        if (r == 1 || r == -2) {
            continue;
        }
        if (r < 0) {
            return TERM_EOF;
        }
        cJSON *j = cJSON_Parse(line), *reply = NULL;
        free(line);
        text = mcp_request(j, &reply, sess);
        cJSON_Delete(j);
        if (reply) {
            acp_send(reply);
        }
        if (text) {
            return take_prompt(sess, text, out);
        }
    }
}

static int acp_next(int (*idle)(void), char **out)
{
    for (;;) {
        char *line = NULL;
        int r = read_line(idle ? 1000 : -1, &line);
        if (r == 1 && idle && idle()) {
            return TERM_TICK;
        }
        if (r == 1 || r == -2) {
            continue;
        }
        if (r < 0) {
            return TERM_EOF;
        }
        cJSON *j = cJSON_Parse(line);
        free(line);
        const char *m = jstr(j, "method");
        const cJSON *id = cJSON_GetObjectItem(j, "id"), *p = cJSON_GetObjectItem(j, "params");
        if (!strcmp(m, "initialize")) {
            cJSON *res = cJSON_CreateObject(), *caps = cJSON_AddObjectToObject(res, "agentCapabilities"), *info;
            cJSON_AddNumberToObject(res, "protocolVersion", 1);
            cJSON_AddBoolToObject(caps, "loadSession", 0);
            cJSON_AddBoolToObject(cJSON_AddObjectToObject(caps, "promptCapabilities"), "embeddedContext", 1);
            info = cJSON_AddObjectToObject(res, "agentInfo");
            cJSON_AddStringToObject(info, "name", jstr(g_card, "name"));
            cJSON_AddStringToObject(info, "title", jstr(g_card, "name"));
            cJSON_AddStringToObject(info, "version", jstr(g_card, "version"));
            cJSON_AddArrayToObject(res, "authMethods");
            acp_result(id, res, 0, NULL);
        } else if (!strcmp(m, "session/new")) {
            char sid[40];
            cJSON *res = cJSON_CreateObject();
            snprintf(sid, sizeof(sid), "sess-%d", ++g_seq);
            cJSON_AddStringToObject(res, "sessionId", sid);
            acp_result(id, res, 0, NULL);
        } else if (!strcmp(m, "session/prompt")) {
            char *text = blocks_text(cJSON_GetObjectItem(p, "prompt")), sess[100];
            snprintf(sess, sizeof(sess), "%s", jstr(p, "sessionId"));
            g_rid = cJSON_Duplicate(id, 1);
            cJSON_Delete(j);
            return take_prompt(sess, text, out);
        } else if (*m && id) { /* session/load, authenticate ...: not offered */
            acp_result(id, NULL, -32601, "method not found");
        }
        cJSON_Delete(j);
    }
}

/* ACP: ask the editor's user. "y", "a" (always) or "n". */
static const char *acp_permission(int always)
{
    static const char *const opt[][3] = {{"y", "Allow", "allow_once"}, {"a", "Always allow", "allow_always"},
                                         {"n", "Reject", "reject_once"}};
    if (!g_rid) {
        return "n";
    }
    char id[32], call[32];
    snprintf(id, sizeof(id), "perm-%d", ++g_seq);
    snprintf(call, sizeof(call), "call_%d", g_calls);
    cJSON *o = cJSON_CreateObject(), *p = cJSON_AddObjectToObject(o, "params"), *tc, *opts;
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "method", "session/request_permission");
    cJSON_AddStringToObject(p, "sessionId", g_sess);
    tc = cJSON_AddObjectToObject(p, "toolCall");
    cJSON_AddStringToObject(tc, "toolCallId", call);
    cJSON_AddStringToObject(tc, "title", g_perm);
    opts = cJSON_AddArrayToObject(p, "options");
    for (int i = 0; i < 3; i++) {
        if (i != 1 || always) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "optionId", opt[i][0]);
            cJSON_AddStringToObject(e, "name", opt[i][1]);
            cJSON_AddStringToObject(e, "kind", opt[i][2]);
            cJSON_AddItemToArray(opts, e);
        }
    }
    busy(0); /* we read stdin ourselves until the answer is in */
    acp_send(o);
    const char *ans = "n";
    for (;;) {
        char *line = NULL;
        int r = read_line(-1, &line);
        if (r == -2) {
            continue;
        }
        if (r < 0) {
            break;
        }
        cJSON *j = cJSON_Parse(line);
        free(line);
        int mine = !strcmp(jstr(j, "id"), id) && !*jstr(j, "method");
        const cJSON *oc = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "result"), "outcome");
        if (mine && !strcmp(jstr(oc, "outcome"), "selected")) {
            ans = !strcmp(jstr(oc, "optionId"), "y") ? "y" : !strcmp(jstr(oc, "optionId"), "a") ? "a" : "n";
        }
        if (!strcmp(jstr(j, "method"), "session/cancel")) {
            g_cancel = 1; /* the editor answers the request as cancelled next */
        } else if (!mine && *jstr(j, "method") && cJSON_GetObjectItem(j, "id")) {
            acp_result(cJSON_GetObjectItem(j, "id"), NULL, -32000, "busy: the agent is waiting for a permission answer");
        }
        cJSON_Delete(j);
        if (mine) {
            break;
        }
    }
    busy(1);
    return ans;
}

/* A2A: wait for the next caller; answer everything but a message on the spot. */
static int a2a_next(int (*idle)(void), char **out)
{
    for (;;) {
        struct pollfd pf = {g_lsock, POLLIN, 0};
        int r = poll(&pf, 1, idle ? 1000 : -1);
        if (r == 0 && idle && idle()) {
            return TERM_TICK;
        }
        if (r <= 0) {
            continue;
        }
        int c = accept4(g_lsock, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            continue;
        }
        struct timeval tv = {15, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        size_t cap = 1 << 20, len = 0, need = 0;
        char *rq = malloc(cap + 1), *body = NULL;
        while (rq && len < cap) { /* the headers, then Content-Length bytes */
            ssize_t n = recv(c, rq + len, cap - len, 0);
            if (n <= 0) {
                break;
            }
            len += (size_t)n;
            rq[len] = '\0';
            if (!body && (body = strstr(rq, "\r\n\r\n"))) {
                const char *cl = strcasestr(rq, "\ncontent-length:");
                body += 4;
                need = (size_t)(body - rq) + (cl && cl < body ? strtoul(cl + 16, NULL, 10) : 0);
            }
            if (body && len >= need) {
                break;
            }
        }
        if (!rq || !body || len < need) {
            free(rq);
            close(c);
            continue;
        }
        const char *hh = strcasestr(rq, "\nhost:"), *au = strcasestr(rq, "\nauthorization: bearer ");
        snprintf(g_host, sizeof(g_host), "%.*s", hh && hh < body ? (int)strcspn(hh + 6 + strspn(hh + 6, " "), "\r\n") : 0,
                 hh ? hh + 6 + strspn(hh + 6, " ") : "");
        if (!strncmp(rq, "GET ", 4)) {
            if (g_on == F_MCP) { /* no stream to open: every answer comes with its request */
                a2a_respond(c, 405, NULL);
            } else if (!strstr(rq, "/.well-known/agent") || strstr(rq, "/.well-known/agent") > strstr(rq, "\r\n")) {
                a2a_respond(c, 404, NULL);
            } else { /* the agent card: who this is and how to call it */
                cJSON *card = cJSON_Duplicate(g_card, 1), *i = cJSON_CreateObject(), *caps;
                char url[260];
                snprintf(url, sizeof(url), "http://%s/", g_host[0] ? g_host : "127.0.0.1");
                cJSON_AddStringToObject(i, "url", url);
                cJSON_AddStringToObject(i, "protocolBinding", "JSONRPC");
                cJSON_AddStringToObject(i, "protocolVersion", "1.0");
                cJSON_AddItemToArray(cJSON_AddArrayToObject(card, "supportedInterfaces"), i);
                cJSON_AddStringToObject(card, "url", url); /* for 0.3 callers */
                cJSON_AddStringToObject(card, "protocolVersion", "0.3.0");
                cJSON_AddStringToObject(card, "preferredTransport", "JSONRPC");
                caps = cJSON_AddObjectToObject(card, "capabilities");
                cJSON_AddBoolToObject(caps, "streaming", 0);
                cJSON_AddBoolToObject(caps, "pushNotifications", 0);
                cJSON_AddItemToObject(card, "defaultInputModes", cJSON_Parse("[\"text/plain\"]"));
                cJSON_AddItemToObject(card, "defaultOutputModes", cJSON_Parse("[\"text/plain\"]"));
                if (g_token && *g_token) {
                    cJSON_AddItemToObject(card, "securitySchemes",
                                          cJSON_Parse("{\"bearer\":{\"httpAuthSecurityScheme\":{\"scheme\":\"Bearer\"}}}"));
                    cJSON_AddItemToObject(card, "securityRequirements", cJSON_Parse("[{\"schemes\":{\"bearer\":{\"list\":[]}}}]"));
                }
                a2a_respond(c, 200, card);
            }
            free(rq);
            continue;
        }
        size_t tl = g_token ? strlen(g_token) : 0;
        if (tl && !(au && au < body && !strncmp(au + 23, g_token, tl) && (au[23 + tl] == '\r' || au[23 + tl] == ' '))) {
            a2a_respond(c, 401, cJSON_Parse("{\"error\":\"send the agent's token as a Bearer token\"}"));
            free(rq);
            continue;
        }
        cJSON *j = cJSON_Parse(body);
        free(rq);
        if (g_on == F_MCP) {
            cJSON *reply = NULL;
            char sess[100], *text = mcp_request(j, &reply, sess);
            cJSON_Delete(j);
            if (text) {
                g_conn = c;
                fprintf(stderr, "MCP: ask in %s\n", sess);
                return take_prompt(sess, text, out);
            }
            if (reply) {
                cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
            }
            a2a_respond(c, reply ? 200 : 202, reply);
            continue;
        }
        const char *m = jstr(j, "method");
        const cJSON *id = cJSON_GetObjectItem(j, "id"), *msg = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "params"), "message");
        int v03 = !strcmp(m, "message/send");
        if (v03 || !strcmp(m, "SendMessage")) {
            char *text = blocks_text(cJSON_GetObjectItem(msg, "parts")), ctx[100];
            if (!text || !*text) {
                free(text);
                a2a_error(c, id, -32602, "the message has no text part");
            } else { /* a message without contextId starts a new conversation */
                snprintf(ctx, sizeof(ctx), "%s", jstr(msg, "contextId"));
                if (!ctx[0]) {
                    snprintf(ctx, sizeof(ctx), "ctx-%d", ++g_seq);
                }
                g_rid = cJSON_Duplicate(id, 1);
                g_conn = c;
                g_v03 = v03;
                fprintf(stderr, "A2A: message in %s\n", ctx);
                cJSON_Delete(j);
                return take_prompt(ctx, text, out);
            }
        } else if (strstr(m, "Task") || !strncmp(m, "tasks/", 6)) {
            a2a_error(c, id, -32001, "task not found: this agent answers every message directly");
        } else if (!strcmp(m, "SendStreamingMessage") || !strcmp(m, "message/stream")) {
            a2a_error(c, id, -32004, "streaming is not supported: use SendMessage");
        } else {
            a2a_error(c, id, -32601, "method not found");
        }
        cJSON_Delete(j);
    }
}

/* --run: the next task, each in a conversation of its own. */
static int run_next(char **out)
{
    static int done;
    char sess[32];
    if (g_task) {
        if (done++) {
            return TERM_EOF;
        }
        g_input = strdup(g_task);
        snprintf(sess, sizeof(sess), "run-1");
        return take_prompt(sess, strdup(g_task), out);
    }
    for (;;) {
        char *line = NULL;
        int r = read_line(-1, &line);
        if (r == -2 || r == 1) {
            continue;
        }
        if (r < 0) {
            return TERM_EOF;
        }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }
        if (!n) {
            free(line);
            continue;
        }
        g_input = strdup(line);
        snprintf(sess, sizeof(sess), "run-%d", ++g_seq);
        return take_prompt(sess, line, out);
    }
}

static int remote_readline(const char *prompt, int (*idle)(void), char **out)
{
    if (!strstr(prompt, ">>")) { /* not the chat prompt */
        if (strstr(prompt, "Allow?")) { /* a risky action: the editor's user decides; a remote caller cannot */
            *out = strdup(g_on == F_ACP ? acp_permission(strstr(prompt, "always") != NULL) : "n");
            return *out ? TERM_LINE : TERM_EOF;
        }
        fprintf(stderr, "No saved model connection, and nobody to ask here: run the agent once in a terminal, or put "
                        "BOTTER_BASE_URL, BOTTER_API_KEY and BOTTER_MODEL in .env\n");
        return TERM_EOF;
    }
    if (g_queued) { /* our "/new" is done: now the prompt it was for */
        *out = g_queued;
        g_queued = NULL;
        return TERM_LINE;
    }
    finish_request();
    if (g_on == F_RUN) {
        return run_next(out);
    }
    return g_on == F_ACP ? acp_next(idle, out) : g_on == F_MCP && g_lsock < 0 ? mcp_next(idle, out) : a2a_next(idle, out);
}

static void emit(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        fputc('\x1e', stdout);
        fputs(s, stdout);
        fputc('\n', stdout);
        free(s);
    }
}

void front_event(const char *ev, ...)
{
    if (!g_on) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ev", ev);
    va_list ap;
    va_start(ap, ev);
    for (;;) {
        const char *k = va_arg(ap, const char *);
        if (!k) {
            break;
        }
        const char *v = va_arg(ap, const char *);
        cJSON_AddStringToObject(o, k, v ? v : "");
    }
    va_end(ap);
    if (g_on == F_UI) {
        emit(o);
    } else {
        remote_event(o);
    }
    cJSON_Delete(o);
}

/* Next '\n'-terminated line from stdin -> malloc'd (no newline). 0 ok, 1 timeout, -1 EOF, -2 EINTR. */
static int read_line(int timeout_ms, char **line)
{
    for (;;) {
        char *nl = g_rlen ? memchr(g_rb, '\n', g_rlen) : NULL;
        if (nl) {
            size_t n = (size_t)(nl - g_rb);
            *line = strndup(g_rb, n);
            explicit_bzero(g_rb, n + 1); /* may hold a secret */
            memmove(g_rb, nl + 1, g_rlen - n - 1);
            g_rlen -= n + 1;
            return *line ? 0 : -1;
        }
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int r = poll(&p, 1, timeout_ms);
        if (r < 0) {
            return errno == EINTR ? -2 : -1;
        }
        if (r == 0) {
            return 1;
        }
        if (g_rcap - g_rlen < 4096) {
            size_t nc = g_rcap ? g_rcap * 2 : 8192;
            char *nb = malloc(nc);
            if (!nb) {
                return -1;
            }
            if (g_rb) {
                memcpy(nb, g_rb, g_rlen);
                explicit_bzero(g_rb, g_rcap);
                free(g_rb);
            }
            g_rb = nb;
            g_rcap = nc;
        }
        ssize_t n = read(STDIN_FILENO, g_rb + g_rlen, g_rcap - g_rlen);
        if (n < 0 && errno == EINTR) {
            return -2;
        }
        if (n <= 0) {
            return -1;
        }
        g_rlen += (size_t)n;
    }
}

int front_readline(const char *prompt, int secret, int (*idle)(void), char **out)
{
    *out = NULL;
    if (g_on >= F_ACP) {
        return remote_readline(prompt, idle, out);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ev", "prompt");
    cJSON_AddStringToObject(o, "text", prompt);
    cJSON_AddBoolToObject(o, "secret", secret);
    emit(o);
    cJSON_Delete(o);

    for (;;) {
        char *line = NULL;
        int r = read_line(idle ? 1000 : -1, &line);
        if (r == 1) {
            if (idle && idle()) {
                return TERM_TICK;
            }
            continue;
        }
        if (r == -2) {
            return TERM_INTR;
        }
        if (r < 0) {
            return TERM_EOF;
        }
        cJSON *j = cJSON_Parse(line);
        explicit_bzero(line, strlen(line));
        free(line);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "line");
        int rc = TERM_EOF;
        if (cJSON_IsString(v)) {
            *out = strdup(v->valuestring);
            explicit_bzero(v->valuestring, strlen(v->valuestring));
            rc = *out ? TERM_LINE : TERM_EOF;
        } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "intr"))) {
            rc = TERM_INTR;
        } else if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "eof"))) {
            cJSON_Delete(j);
            continue; /* unknown message: ignore */
        }
        cJSON_Delete(j);
        return rc;
    }
}
