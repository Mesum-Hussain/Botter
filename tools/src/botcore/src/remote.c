#define _GNU_SOURCE
#include "remote.h"
#include "http.h"
#include "term.h"
#include "vault.h"
#include "tools.h"
#include "vfs.h"

#include "guard.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define RPC_TIMEOUT_S 120
#define MCP_STATELESS "2026-07-28"
#define MCP_SESSION   "2025-06-18"

/* ------------------------------------------------------------------ .env */

const char *env_get(const char *name)
{
    static char *text;
    if (vault_get(name)) { /* a sealed agent carries its secrets */
        return vault_get(name);
    }
    if (!text) {
        FILE *f = fopen(".env", "r");
        size_t n = 0;
        text = calloc(1, 65537);
        if (f && text) {
            n = fread(text + 1, 1, 65535, f);
        }
        if (f) {
            fclose(f);
        }
        if (!text) {
            return NULL;
        }
        text[0] = '\n';
        text[n + 1] = '\0';
        for (char *p = text; *p; p++) { /* lines become strings */
            if (*p == '\n' || *p == '\r') {
                *p = '\0';
            }
        }
        text[65536] = '\1'; /* end mark */
    }
    size_t nl = strlen(name);
    for (const char *l = text + 1; l < text + 65536; l += strlen(l) + 1) {
        const char *s = l + strspn(l, " \t");
        s += strncmp(s, "export ", 7) ? 0 : 7;
        if (strncmp(s, name, nl) == 0 && s[nl] == '=') {
            char *v = (char *)s + nl + 1;
            size_t vl = strlen(v);
            if (vl >= 2 && (*v == '"' || *v == '\'') && v[vl - 1] == *v) { /* "quoted": drop the quotes for good */
                memmove(v, v + 1, vl - 2);
                v[vl - 2] = v[vl - 1] = '\0';
            }
            return v;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- shared */

static const cJSON *cfg(const char *key)
{
    static cJSON *c;
    static int tried;
    if (!tried) {
        tried = 1;
        const vfs_entry_t *e = vfs_find("config.json");
        c = e && e->kind == VFS_DATA ? cJSON_ParseWithLength(e->data, e->len) : NULL;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, key);
    return cJSON_IsObject(v) ? v : NULL;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* https anywhere; plain http only to this machine. */
static int url_allowed(const char *u)
{
    static const char *const local[] = {"http://127.0.0.1", "http://localhost", "http://[::1]"};
    if (!u || strpbrk(u, " \r\n\t")) {
        return 0;
    }
    if (!strncmp(u, "https://", 8)) {
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(local[i]);
        if (!strncmp(u, local[i], n) && (u[n] == ':' || u[n] == '/' || !u[n])) {
            return 1;
        }
    }
    return 0;
}

static const char *token_of(const cJSON *entry)
{
    const char *t = jstr(entry, "token");
    return t ? env_get(t) : NULL;
}

/*
 * One JSON-RPC exchange: POST body (or GET when body is NULL), answer as plain JSON or as
 * Server-Sent Events (the event that carries the result). NULL if there was no usable answer;
 * *status = HTTP status, 0 = no response, -2 = interrupted.
 */
static cJSON *rpc(const char *url, const char *token, const char *headers, const char *body, int *status)
{
    http_resp_t r;
    http_next_headers(headers);
    int rc = http_request(url, token, body, RPC_TIMEOUT_S, &r);
    *status = rc == HTTP_ERR_ABORT ? -2 : r.status;
    cJSON *j = rc == HTTP_OK ? cJSON_Parse(r.body) : NULL;
    if (rc == HTTP_OK && !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        j = NULL;
        for (const char *l = r.body; l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
            if (strncmp(l, "data:", 5) != 0) {
                continue;
            }
            cJSON *e = cJSON_Parse(l + 5);
            if (cJSON_GetObjectItem(e, "result") || cJSON_GetObjectItem(e, "error")) {
                cJSON_Delete(j);
                j = e;
            } else {
                cJSON_Delete(e);
            }
        }
    }
    http_resp_free(&r);
    return j;
}

/* "ERROR: <who>: <the JSON-RPC error message, or what went wrong with the request>" */
static char *rpc_error(const char *who, const cJSON *resp, int status)
{
    const char *m = jstr(cJSON_GetObjectItem(resp, "error"), "message");
    char *out = NULL;
    if (m) {
        return asprintf(&out, "ERROR: %s: %.2000s", who, m) < 0 ? NULL : out;
    }
    if (status == 401 || status == 403) {
        return asprintf(&out, "ERROR: %s refused the request (HTTP %d): check its \"token\" in config.json and .env", who,
                        status) < 0 ? NULL : out;
    }
    return asprintf(&out, status > 0 ? "ERROR: %s answered HTTP %d" : "ERROR: %s could not be reached", who, status) < 0
               ? NULL : out;
}

/* The "text" of every part in a content / parts array, one per line. */
static char *parts_text(const cJSON *parts)
{
    char *out = strdup("");
    const cJSON *p;
    cJSON_ArrayForEach(p, parts)
    {
        const char *t = jstr(p, "text"), *ty = jstr(p, "type");
        char *n = NULL;
        if (asprintf(&n, "%s%s%s%s%s", out, *out ? "\n" : "", t ? t : ty ? "[" : "", t ? "" : ty ? ty : "",
                     t || !ty ? "" : " content]") >= 0) {
            free(out);
            out = n;
        }
    }
    return out;
}

/* ------------------------------------------------------------------- MCP */

typedef struct {
    char        name[65], *url;
    const char *token;
    int         mode; /* 0 unknown, 1 stateless, 2 session handshake */
    char        sid[300], ver[16];
    pid_t       pid;      /* a server this agent started ("command"): JSON-RPC lines over its stdin/stdout */
    int         wfd, rfd;
    char       *rb;
    size_t      rlen, rcap;
} msrv_t;

typedef struct {
    char    fn[65]; /* <server>_<tool>: the name the model and FWL use */
    char   *tool;
    msrv_t *srv;
    cJSON  *def;
} mtool_t;

static msrv_t  *g_srv;
static mtool_t *g_mt;
static size_t   g_nsrv, g_nmt;
static int      g_id;

/*
 * A server given as a "command": started once, inside the same sandbox as the agent's tools
 * (working folder, no more network than the agent has), and gone when the agent is. Its
 * program is its own: an npx server needs Node on the machine, a native binary needs nothing.
 */
static int mcp_spawn(msrv_t *s, const cJSON *cmd, const cJSON *env)
{
    char *argv[32];
    int n = 0, in[2], out[2];
    const cJSON *a;
    cJSON_ArrayForEach(a, cmd)
    {
        if (cJSON_IsString(a) && n < 31) {
            argv[n++] = a->valuestring;
        }
    }
    argv[n] = NULL;
    if (!n || pipe2(in, O_CLOEXEC) != 0) {
        return -1;
    }
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    fflush(NULL);
    s->pid = fork();
    if (s->pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* no agent, no server */
        dup2(in[0], 0);
        dup2(out[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 2); /* servers log on stderr */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_IGN); /* Ctrl-C stops the agent's turn, not its servers */
        cJSON_ArrayForEach(a, env) /* the secrets it needs, by name, from the vault or .env */
        {
            if (cJSON_IsString(a) && env_get(a->valuestring)) {
                setenv(a->valuestring, env_get(a->valuestring), 1);
            }
        }
        if (chdir(guard_ctx()) != 0 || tool_sandbox_apply(24 * 3600, !guard_offline()) != 0) {
            _exit(126);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (s->pid < 0) {
        close(in[1]);
        close(out[0]);
        s->pid = 0;
        return -1;
    }
    s->wfd = in[1];
    s->rfd = out[0];
    return 0;
}

/* One exchange with a started server: write the message; unless it is a notification (id < 0),
 * read lines until the answer with that id (what the server asks of us meanwhile is declined). */
static cJSON *stdio_rpc(msrv_t *s, const char *body, int id, int *status)
{
    size_t bl = strlen(body);
    *status = 0;
    if (write(s->wfd, body, bl) != (ssize_t)bl || write(s->wfd, "\n", 1) != 1) {
        return NULL;
    }
    double waited = 0;
    while (id >= 0 && waited < RPC_TIMEOUT_S) {
        char *nl = s->rlen ? memchr(s->rb, '\n', s->rlen) : NULL;
        if (nl) {
            *nl = '\0';
            cJSON *j = cJSON_Parse(s->rb);
            size_t used = (size_t)(nl - s->rb) + 1;
            memmove(s->rb, nl + 1, s->rlen - used);
            s->rlen -= used;
            const cJSON *jid = cJSON_GetObjectItem(j, "id");
            if (cJSON_IsNumber(jid) && (int)jid->valuedouble == id && !cJSON_GetObjectItem(j, "method")) {
                *status = 200;
                return j;
            }
            if (jid && cJSON_GetObjectItem(j, "method")) { /* sampling, roots ...: not offered */
                char no[200], *ids = cJSON_PrintUnformatted(jid);
                snprintf(no, sizeof(no), "{\"jsonrpc\":\"2.0\",\"id\":%.60s,\"error\":{\"code\":-32601,\"message\":\"not supported\"}}\n",
                         ids ? ids : "null");
                free(ids);
                (void)!write(s->wfd, no, strlen(no));
            }
            cJSON_Delete(j);
            continue;
        }
        struct pollfd p = {s->rfd, POLLIN, 0};
        int r = poll(&p, 1, 500);
        waited += r == 0 ? 0.5 : 0;
        if (term_interrupted()) {
            *status = -2;
            return NULL;
        }
        if (r <= 0) {
            continue;
        }
        if (s->rcap - s->rlen < 65536) {
            char *nb = realloc(s->rb, s->rcap + 131072);
            if (!nb) {
                return NULL;
            }
            s->rb = nb;
            s->rcap += 131072;
        }
        ssize_t got = read(s->rfd, s->rb + s->rlen, s->rcap - s->rlen - 1);
        if (got <= 0) { /* the server is gone */
            return NULL;
        }
        s->rlen += (size_t)got;
    }
    return NULL;
}

static int mcp_handshake(msrv_t *s)
{
    int st;
    char body[400];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"" MCP_SESSION
             "\",\"capabilities\":{},\"clientInfo\":{\"name\":\"botter\",\"version\":\"1\"}}}", ++g_id);
    cJSON *j = s->pid ? stdio_rpc(s, body, g_id, &st)
                      : rpc(s->url, s->token, "Accept: application/json, text/event-stream\r\n", body, &st);
    const char *v = jstr(cJSON_GetObjectItem(j, "result"), "protocolVersion");
    if (!v) {
        cJSON_Delete(j);
        return -1;
    }
    snprintf(s->ver, sizeof(s->ver), "%s", v);
    snprintf(s->sid, sizeof(s->sid), "%s", http_last_header("Mcp-Session-Id"));
    cJSON_Delete(j);
    char h[500];
    snprintf(h, sizeof(h), "Accept: application/json, text/event-stream\r\nMCP-Protocol-Version: %s\r\n%s%s%s", s->ver,
             s->sid[0] ? "Mcp-Session-Id: " : "", s->sid, s->sid[0] ? "\r\n" : "");
    static const char READY[] = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cJSON_Delete(s->pid ? stdio_rpc(s, READY, -1, &st) : rpc(s->url, s->token, h, READY, &st));
    s->mode = 2;
    return 0;
}

/* One MCP request (takes params). The response object, or NULL; *status as rpc(). */
static cJSON *mcp_req(msrv_t *s, const char *method, const char *name, cJSON *params, int *status)
{
    cJSON *j = NULL;
    for (int attempt = 0; attempt < 2; attempt++) {
        cJSON *b = cJSON_CreateObject(), *p = cJSON_Duplicate(params, 1);
        char h[900];
        if (s->mode == 2) {
            snprintf(h, sizeof(h), "Accept: application/json, text/event-stream\r\nMCP-Protocol-Version: %s\r\n%s%s%s",
                     s->ver, s->sid[0] ? "Mcp-Session-Id: " : "", s->sid, s->sid[0] ? "\r\n" : "");
        } else { /* stateless: the request says who is asking and what for */
            cJSON *ci = cJSON_AddObjectToObject(cJSON_AddObjectToObject(p, "_meta"), "io.modelcontextprotocol/clientInfo");
            cJSON_AddStringToObject(ci, "name", "botter");
            cJSON_AddStringToObject(ci, "version", "1");
            snprintf(h, sizeof(h),
                     "Accept: application/json, text/event-stream\r\nMCP-Protocol-Version: " MCP_STATELESS
                     "\r\nMcp-Method: %s\r\n%s%s%s",
                     method, name ? "Mcp-Name: " : "", name ? name : "", name ? "\r\n" : "");
        }
        cJSON_AddStringToObject(b, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(b, "id", ++g_id);
        cJSON_AddStringToObject(b, "method", method);
        cJSON_AddItemToObject(b, "params", p);
        char *body = cJSON_PrintUnformatted(b);
        cJSON_Delete(b);
        cJSON_Delete(j);
        j = !body ? NULL : s->pid ? stdio_rpc(s, body, g_id, status) : rpc(s->url, s->token, h, body, status);
        free(body);
        if (cJSON_GetObjectItem(j, "result")) {
            s->mode = s->mode ? s->mode : 1;
            break;
        }
        /* An older server wants its session handshake first (and again when the session expired). */
        if (*status == -2 || attempt || !(s->mode == 0 || (s->mode == 2 && *status == 404)) || mcp_handshake(s) != 0) {
            break;
        }
    }
    cJSON_Delete(params);
    return j;
}

static void mcp_list(msrv_t *s)
{
    char cursor[300] = "", *why = NULL;
    int st = 0, n = 0;
    for (int page = 0; page < 20; page++) {
        cJSON *p = cJSON_CreateObject();
        if (cursor[0]) {
            cJSON_AddStringToObject(p, "cursor", cursor);
        }
        cJSON *j = mcp_req(s, "tools/list", NULL, p, &st);
        const cJSON *res = cJSON_GetObjectItem(j, "result"), *t;
        if (!res) {
            why = rpc_error(s->name, j, st);
            cJSON_Delete(j);
            break;
        }
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(res, "tools"))
        {
            const char *tn = jstr(t, "name"), *d = jstr(t, "description");
            mtool_t *g = tn && strlen(tn) == strspn(tn, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./")
                             ? realloc(g_mt, (g_nmt + 1) * sizeof(*g_mt)) : NULL;
            if (!g) {
                continue;
            }
            g_mt = g;
            mtool_t *m = &g_mt[g_nmt++];
            snprintf(m->fn, sizeof(m->fn), "%.24s_%.39s", s->name, tn);
            for (char *c = m->fn; *c; c++) {
                *c = isalnum((unsigned char)*c) || *c == '-' ? *c : '_';
            }
            m->tool = strdup(tn);
            m->srv = s;
            m->def = cJSON_CreateObject();
            cJSON *fn = cJSON_AddObjectToObject(m->def, "function"), *sch = cJSON_GetObjectItem(t, "inputSchema");
            cJSON_AddStringToObject(m->def, "type", "function");
            cJSON_AddStringToObject(fn, "name", m->fn);
            char desc[1100];
            snprintf(desc, sizeof(desc), "[%s, remote] %.1000s", s->name, d ? d : tn);
            cJSON_AddStringToObject(fn, "description", desc);
            cJSON_AddItemToObject(fn, "parameters", cJSON_IsObject(sch) ? cJSON_Duplicate(sch, 1)
                                                                        : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
            n++;
        }
        const char *nc = jstr(res, "nextCursor");
        snprintf(cursor, sizeof(cursor), "%s", nc ? nc : "");
        cJSON_Delete(j);
        if (!cursor[0]) {
            break;
        }
    }
    if (why) {
        fprintf(stderr, ANSI_DIM "(MCP %s is not available: %s)" ANSI_RESET "\n", s->name, why + 7);
    } else {
        printf(ANSI_DIM "MCP %s: %d tool%s" ANSI_RESET "\n", s->name, n, n == 1 ? "" : "s");
    }
    free(why);
}

void mcp_init(void)
{
    const cJSON *e;
    int n = cJSON_GetArraySize(cfg("mcp"));
    g_srv = n > 0 ? calloc((size_t)n, sizeof(*g_srv)) : NULL; /* never moved: tools point into it */
    const cJSON *servers = g_srv ? cfg("mcp") : NULL;
    cJSON_ArrayForEach(e, servers)
    {
        const char *u = jstr(e, "url");
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(e, "command");
        if (!cJSON_IsArray(cmd) && !url_allowed(u)) {
            fprintf(stderr, ANSI_DIM "(MCP %s: \"url\" must be https:// (http:// only for localhost))" ANSI_RESET "\n",
                    e->string);
            continue;
        }
        msrv_t *s = &g_srv[g_nsrv++];
        snprintf(s->name, sizeof(s->name), "%s", e->string);
        if (cJSON_IsArray(cmd)) { /* a server to start: it speaks the session protocol over its stdin/stdout */
            if (mcp_spawn(s, cmd, cJSON_GetObjectItemCaseSensitive(e, "env")) != 0 || mcp_handshake(s) != 0) {
                fprintf(stderr, ANSI_DIM "(MCP %s is not available: \"%s\" did not start as an MCP server; is it installed?)"
                                ANSI_RESET "\n", s->name, cJSON_IsString(cmd->child) ? cmd->child->valuestring : "?");
                if (s->pid > 0) {
                    kill(s->pid, SIGTERM);
                }
                g_nsrv--;
                continue;
            }
        } else {
            s->url = strdup(u);
            s->token = token_of(e);
        }
        mcp_list(s);
    }
}

void mcp_schema_append(cJSON *arr)
{
    for (size_t i = 0; i < g_nmt; i++) {
        cJSON_AddItemToArray(arr, cJSON_Duplicate(g_mt[i].def, 1));
    }
}

static mtool_t *mcp_find(const char *name)
{
    for (size_t i = 0; i < g_nmt; i++) {
        if (!strcmp(g_mt[i].fn, name)) {
            return &g_mt[i];
        }
    }
    return NULL;
}

const char *mcp_server(const char *name)
{
    mtool_t *m = mcp_find(name);
    return m ? m->srv->name : NULL;
}

char *mcp_call(const char *name, const char *args_json)
{
    mtool_t *m = mcp_find(name);
    if (!m) {
        return NULL;
    }
    cJSON *p = cJSON_CreateObject(), *a = cJSON_Parse(args_json && *args_json ? args_json : "{}");
    cJSON_AddStringToObject(p, "name", m->tool);
    cJSON_AddItemToObject(p, "arguments", cJSON_IsObject(a) ? a : cJSON_CreateObject());
    if (!cJSON_IsObject(a)) {
        cJSON_Delete(a);
    }
    int st = 0;
    cJSON *j = mcp_req(m->srv, "tools/call", m->tool, p, &st);
    const cJSON *res = cJSON_GetObjectItem(j, "result");
    char *out = NULL;
    if (!res) {
        out = st == -2 ? strdup("ERROR: interrupted") : rpc_error(m->srv->name, j, st);
    } else {
        char *t = parts_text(cJSON_GetObjectItem(res, "content"));
        const cJSON *sc = cJSON_GetObjectItem(res, "structuredContent");
        if (t && !*t && sc) { /* only structured output */
            free(t);
            t = cJSON_PrintUnformatted(sc);
        }
        if (asprintf(&out, "%s%.*s", cJSON_IsTrue(cJSON_GetObjectItem(res, "isError")) ? "ERROR: " : "",
                     TOOL_RESULT_MAX - 64, t ? t : "") < 0) {
            out = NULL;
        }
        free(t);
    }
    cJSON_Delete(j);
    if (out) {
        tools_sanitize_utf8(out);
    }
    return out ? out : strdup("ERROR: out of memory");
}

/* ------------------------------------------------------------------- A2A */

typedef struct {
    const cJSON *e;
    char        *endpoint; /* JSON-RPC URL from the agent card (found at first use) */
    int          v03;      /* the card offers only the 0.x wire format */
} peer_t;

static peer_t g_peer[16];
static int    g_npeer = -1;

int a2a_count(void)
{
    if (g_npeer < 0) {
        const cJSON *e;
        g_npeer = 0;
        const cJSON *agents = cfg("agents");
        cJSON_ArrayForEach(e, agents)
        {
            if (g_npeer < 16 && url_allowed(jstr(e, "url"))) {
                g_peer[g_npeer++].e = e;
            }
        }
    }
    return g_npeer;
}

const char *a2a_name(int i) { return g_peer[i].e->string; }

const char *a2a_desc(int i)
{
    const char *d = jstr(g_peer[i].e, "description");
    return d ? d : "a remote agent";
}

/* Where the agent takes JSON-RPC: from its card, else the configured URL itself. */
static void a2a_resolve(peer_t *p)
{
    const char *u = jstr(p->e, "url");
    size_t ul = strlen(u);
    char card[1100];
    int st;
    snprintf(card, sizeof(card), "%.*s%s", (int)(ul && u[ul - 1] == '/' ? ul - 1 : ul), u,
             ul > 5 && !strcmp(u + ul - 5, ".json") ? "" : "/.well-known/agent-card.json");
    cJSON *c = rpc(card, token_of(p->e), "A2A-Version: 1.0\r\n", NULL, &st);
    const cJSON *i;
    const char *ep = NULL;
    cJSON_ArrayForEach(i, cJSON_GetObjectItem(c, "supportedInterfaces"))
    {
        const char *b = jstr(i, "protocolBinding"), *v = jstr(i, "protocolVersion");
        if (!ep && b && !strcasecmp(b, "JSONRPC") && jstr(i, "url")) {
            ep = jstr(i, "url");
            p->v03 = v && v[0] == '0';
        }
    }
    if (!ep && jstr(c, "url")) { /* a 0.x card */
        ep = jstr(c, "url");
        p->v03 = 1;
    }
    p->endpoint = strdup(url_allowed(ep) ? ep : u);
    cJSON_Delete(c);
}

/* State of a task as one lowercase word: completed, working, failed, input ... */
static const char *a2a_state(const cJSON *task)
{
    static char s[40];
    const char *v = jstr(cJSON_GetObjectItem(task, "status"), "state");
    v = v ? v : "";
    v += strncmp(v, "TASK_STATE_", 11) ? 0 : 11;
    size_t i = 0;
    for (; v[i] && i < sizeof(s) - 1; i++) {
        s[i] = (char)tolower((unsigned char)v[i]);
    }
    s[i] = '\0';
    return s;
}

int a2a_call(const char *name, const char *task, char **reply)
{
    peer_t *p = NULL;
    for (int i = 0; i < a2a_count(); i++) {
        p = !strcmp(a2a_name(i), name) ? &g_peer[i] : p;
    }
    if (!p) {
        return -2;
    }
    if (!p->endpoint) {
        a2a_resolve(p);
    }
    char mid[48];
    snprintf(mid, sizeof(mid), "botter-%lx-%d", (long)time(NULL), ++g_id);
    cJSON *b = cJSON_CreateObject(), *msg = cJSON_CreateObject(), *part = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(b, "id", g_id);
    cJSON_AddStringToObject(b, "method", p->v03 ? "message/send" : "SendMessage");
    cJSON_AddItemToObject(cJSON_AddObjectToObject(b, "params"), "message", msg);
    cJSON_AddStringToObject(msg, "messageId", mid);
    cJSON_AddStringToObject(msg, "role", p->v03 ? "user" : "ROLE_USER");
    cJSON_AddStringToObject(part, "text", task);
    if (p->v03) {
        cJSON_AddStringToObject(msg, "kind", "message");
        cJSON_AddStringToObject(part, "kind", "text");
    }
    cJSON_AddItemToArray(cJSON_AddArrayToObject(msg, "parts"), part);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);

    int st = 0, rc = -1;
    cJSON *j = body ? rpc(p->endpoint, token_of(p->e), "A2A-Version: 1.0\r\n", body, &st) : NULL;
    free(body);
    *reply = NULL;
    for (int polls = 0;; polls++) {
        const cJSON *res = cJSON_GetObjectItem(j, "result");
        const cJSON *m = cJSON_GetObjectItem(res, "message"), *t = cJSON_GetObjectItem(res, "task");
        m = m ? m : cJSON_GetObjectItem(res, "parts") && !cJSON_GetObjectItem(res, "status") ? res : NULL;
        t = t ? t : cJSON_GetObjectItem(res, "status") ? res : NULL;
        if (st == -2) {
            rc = 1;
        } else if (m) {
            *reply = parts_text(cJSON_GetObjectItem(m, "parts"));
            rc = 0;
        } else if (!t) {
            *reply = rpc_error(name, j, st);
        } else {
            const char *state = a2a_state(t), *id = jstr(t, "id");
            char *said = parts_text(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(t, "status"), "message"), "parts"));
            if ((!strcmp(state, "working") || !strcmp(state, "submitted")) && id && polls < 150) {
                free(said);
                struct timespec ts = {2, 0}; /* the agent works on it: ask again */
                nanosleep(&ts, NULL);
                if (term_interrupted()) {
                    rc = 1;
                    break;
                }
                char q[400];
                snprintf(q, sizeof(q), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":{\"id\":\"%.200s\"}}", ++g_id,
                         p->v03 ? "tasks/get" : "GetTask", strpbrk(id, "\"\\") ? "" : id);
                cJSON_Delete(j);
                j = rpc(p->endpoint, token_of(p->e), "A2A-Version: 1.0\r\n", q, &st);
                continue;
            }
            if (!strcmp(state, "completed") || !strncmp(state, "input", 5)) {
                const cJSON *a;
                char *all = strdup("");
                cJSON_ArrayForEach(a, cJSON_GetObjectItem(t, "artifacts"))
                {
                    char *one = parts_text(cJSON_GetObjectItem(a, "parts")), *n = NULL;
                    if (asprintf(&n, "%s%s%s", all, *all && *one ? "\n" : "", one) >= 0) {
                        free(all);
                        all = n;
                    }
                    free(one);
                }
                *reply = *all ? all : said;
                free(*all ? said : all);
                rc = 0;
            } else {
                if (asprintf(reply, "ERROR: %s: the task ended as \"%s\"%s%.1000s", name, state, *said ? ": " : "", said) < 0) {
                    *reply = NULL;
                }
                free(said);
            }
        }
        break;
    }
    cJSON_Delete(j);
    if (rc == 0 && *reply) {
        tools_sanitize_utf8(*reply);
    }
    return rc;
}
