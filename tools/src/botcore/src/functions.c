#define _GNU_SOURCE
#include "functions.h"
#include "botlua.h"
#include "chat.h"
#include "skills.h"
#include "guard.h"
#include "http.h"
#include "remote.h"
#include "term.h"
#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FN_READ_MAX   (64u * 1024 * 1024)
#define FN_RESULT_MAX (100u * 1024) /* what one call may put in the conversation */

static bl_state *g_bl;

/* A path the functions may use: inside the working directory. 0 ok (absolute path in out). */
static int inside_path(const char *path, char out[PATH_MAX], char *err, size_t el)
{
    int inside = 0;
    if (guard_resolve(path, out, &inside) != 0) {
        snprintf(err, el, "not a usable path");
        return -1;
    }
    if (!inside) {
        snprintf(err, el, "outside the working directory (%s)", guard_ctx());
        return -1;
    }
    return 0;
}

static char *io_read(void *ud, const char *path, size_t *len, char *err, size_t el)
{
    (void)ud;
    char p[PATH_MAX];
    if (inside_path(path, p, err, el) != 0) {
        return NULL;
    }
    FILE *f = fopen(p, "rb");
    if (!f) {
        snprintf(err, el, "%s", strerror(errno));
        return NULL;
    }
    size_t cap = 65536, n = 0, r;
    char *buf = malloc(cap + 1);
    while (buf && (r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            if (cap >= FN_READ_MAX) {
                snprintf(err, el, "larger than %u MB", FN_READ_MAX >> 20);
                free(buf);
                buf = NULL;
                break;
            }
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (!buf) {
        if (!err[0]) {
            snprintf(err, el, "out of memory");
        }
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int io_write(void *ud, const char *path, const char *data, size_t len, int append, char *err, size_t el)
{
    (void)ud;
    char p[PATH_MAX];
    if (guard_plan()) {
        snprintf(err, el, "Plan mode is read-only");
        return -1;
    }
    if (inside_path(path, p, err, el) != 0) {
        return -1;
    }
    if (!guard_write_ok(p)) {
        snprintf(err, el, "this task may write only %s", guard_writes_text());
        return -1;
    }
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) { /* parent folders, like fs_write */
        *s = '\0';
        mkdir(p, 0755);
        *s = '/';
    }
    FILE *f = fopen(p, append ? "ab" : "wb");
    if (!f) {
        snprintf(err, el, "%s", strerror(errno));
        return -1;
    }
    int ok = fwrite(data, 1, len, f) == len;
    ok &= fclose(f) == 0;
    if (!ok) {
        snprintf(err, el, "write failed: %s", strerror(errno));
    }
    return ok ? 0 : -1;
}

static int not_dots(const struct dirent *d) { return d->d_name[0] != '.'; }
static int by_name(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }

static char *io_list(void *ud, const char *path, char *err, size_t el)
{
    (void)ud;
    char p[PATH_MAX];
    if (inside_path(path, p, err, el) != 0) {
        return NULL;
    }
    struct dirent **names = NULL;
    int n = scandir(p, &names, not_dots, by_name);
    if (n < 0) {
        snprintf(err, el, "%s", strerror(errno));
        return NULL;
    }
    char *out = NULL;
    size_t ol = 0;
    FILE *m = open_memstream(&out, &ol);
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX + 256];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", p, names[i]->d_name);
        if (m) {
            fprintf(m, "%s%s\n", names[i]->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode) ? "/" : "");
        }
        free(names[i]);
    }
    free(names);
    if (m) {
        fclose(m);
    }
    return out ? out : strdup("");
}

static int io_exists(void *ud, const char *path)
{
    (void)ud;
    char p[PATH_MAX], err[8];
    return inside_path(path, p, err, sizeof(err)) == 0 && access(p, F_OK) == 0;
}

/* Today's date; BOTTER_TODAY=YYYY-MM-DD pins it (a batch run that must give the same answers tomorrow). */
static int io_today(void *ud, char out[11])
{
    (void)ud;
    const char *pin = getenv("BOTTER_TODAY");
    if (pin && strlen(pin) == 10 && pin[4] == '-' && pin[7] == '-') {
        memcpy(out, pin, 11);
        return 0;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return strftime(out, 11, "%Y-%m-%d", &tm) == 10 ? 0 : -1;
}

static char *(*g_run_tool)(const char *, const char *);

void fn_set_tool_runner(char *(*run)(const char *name, const char *args_json)) { g_run_tool = run; }

static fn_host_t g_host;

void fn_set_host(const fn_host_t *h) { g_host = *h; }

static char *io_worker(void *ud, const char *spec, char *err, size_t el)
{
    (void)ud;
    if (!g_host.worker) {
        snprintf(err, el, "not available here");
        return NULL;
    }
    return g_host.worker(spec, err, el);
}

static char *io_llm(void *ud, const char *spec, char *err, size_t el)
{
    (void)ud;
    if (!g_host.llm) {
        snprintf(err, el, "not available here");
        return NULL;
    }
    return g_host.llm(spec, err, el);
}

static char *io_ask(void *ud, const char *q)
{
    (void)ud;
    return g_host.ask ? g_host.ask(q) : NULL;
}

static int io_usage(void *ud, long *calls, long long *in, long long *out)
{
    (void)ud;
    const chat_usage_t *u;
    for (int i = 0, n = chat_usage(&u); i < n; i++) {
        *calls += u[i].calls;
        *in += u[i].input_tokens;
        *out += u[i].output_tokens;
    }
    return 0;
}

static void io_say(void *ud, const char *t)
{
    (void)ud;
    if (g_host.say) {
        g_host.say(t);
    }
}

static char *io_tool(void *ud, const char *name, const char *args)
{
    (void)ud;
    return g_run_tool ? g_run_tool(name, args) : NULL;
}

/* https anywhere; plain http to this machine only; nothing when the agent is offline. */
static char *io_http(void *ud, const char *url, const char *body, const char *headers, int *status, char *err, size_t el)
{
    (void)ud;
    if (guard_offline()) {
        snprintf(err, el, "this agent is offline (config.json \"internet\": false)");
        return NULL;
    }
    static const char *const lo[] = {"http://localhost", "http://127.0.0.1", "http://[::1]"};
    int ok = !strncmp(url, "https://", 8);
    for (size_t i = 0; i < 3 && !ok; i++) {
        size_t n = strlen(lo[i]);
        ok = !strncmp(url, lo[i], n) && (url[n] == '\0' || url[n] == ':' || url[n] == '/');
    }
    if (!ok || strpbrk(url, " \r\n")) {
        snprintf(err, el, "the address must start with https:// (http:// only for this machine)");
        return NULL;
    }
    http_resp_t r;
    if (headers && *headers) {
        http_next_headers(headers);
    }
    int rc = http_request(url, NULL, body, 60, &r);
    char *out = NULL;
    if (rc != HTTP_OK) {
        snprintf(err, el, "%s", rc == HTTP_ERR_ABORT ? "interrupted" : r.err ? r.err : "no answer");
    } else {
        *status = r.status;
        out = strdup(r.body ? r.body : "");
    }
    http_resp_free(&r);
    return out;
}

static const char *io_secret(void *ud, const char *name)
{
    (void)ud;
    return env_get(name);
}

static const char *io_skill_dir(void *ud, const char *name)
{
    (void)ud;
    return sk_dir(name);
}

static int io_interrupted(void *ud)
{
    (void)ud;
    return term_interrupted();
}

static void diag(void *ud, const char *file, int line, int error, const char *msg)
{
    (void)ud;
    if (line) {
        fprintf(stderr, ANSI_BOLD_RED "%s:%d:" ANSI_RESET " %s%s\n", file, line, error ? "" : "warning: ", msg);
    } else {
        fprintf(stderr, ANSI_BOLD_RED "%s:" ANSI_RESET " %s%s\n", file, error ? "" : "warning: ", msg);
    }
}

/* functions/<n>.lua or skills/<skill>/functions/<n>.lua (tests/ below them are never embedded). */
static int fn_file(const char *path)
{
    const char *f = !strncmp(path, "functions/", 10) ? path + 10 : NULL;
    if (!f && !strncmp(path, "skills/", 7)) {
        const char *s = strchr(path + 7, '/');
        f = s && !strncmp(s, "/functions/", 11) ? s + 11 : NULL;
    }
    return f && *f && !strchr(f, '/');
}

void fn_init(void)
{
    size_t n = 0;
    const vfs_entry_t *t = vfs_table(&n);
    for (size_t i = 0; i < n; i++) {
        size_t pl = strlen(t[i].path);
        int flow = !strcmp(t[i].path, "FLOW.lua");
        if (t[i].kind != VFS_DATA || (!flow && (!fn_file(t[i].path) || pl < 5 || strcmp(t[i].path + pl - 4, ".lua") != 0))) {
            continue;
        }
        if (!g_bl) {
            static const bl_io io = {io_read,        io_write, io_list,   io_exists,    io_today,
                                     io_interrupted, io_tool,  io_http,   io_secret,    io_skill_dir,
                                     io_worker,      io_ask,   io_say,    io_llm,       io_usage, NULL};
            g_bl = bl_open(&io);
            if (!g_bl) {
                fprintf(stderr, ANSI_BOLD_RED "Error:" ANSI_RESET " out of memory starting Lua\n");
                return;
            }
        }
        /* the packer refused anything with errors, so this only fails on a hand-made pack */
        bl_load(g_bl, t[i].path, t[i].data, t[i].len, !flow, diag, NULL); /* FLOW.lua orchestrates: no tools */
    }
}

int fn_count(void) { return bl_count(g_bl); }

char *fn_turn_end(const char *reply) { return bl_call_hook(g_bl, "on_turn_end", reply); }

int fn_has_flow(void) { return vfs_find("FLOW.lua") && bl_has_global(g_bl, "flow"); }

char *fn_flow(const char *input, char **reply) { return bl_call_flow(g_bl, input, reply); }

char *fn_check(const char *name, const char *run_json)
{
    if (!g_bl) {
        return strdup("ERROR: this agent has no functions, so no check function");
    }
    return bl_call_hook(g_bl, name, run_json);
}

void fn_schema_append(cJSON *arr) { bl_schema_append(g_bl, arr, "functions/"); }

void fn_skill_schema_append(const char *skill, cJSON *arr)
{
    char prefix[200];
    snprintf(prefix, sizeof(prefix), "skills/%s/", skill);
    bl_schema_append(g_bl, arr, prefix);
}

int fn_is(const char *name) { return bl_find(g_bl, name) != NULL; }

char *fn_call(const char *name, const char *args_json)
{
    const bl_fn *f = bl_find(g_bl, name);
    if (!f) {
        return NULL;
    }
    if (!strncmp(f->file, "skills/", 7) && !chat_has_tool(chat_current(), name)) { /* a skill's function */
        char *m = NULL;
        return asprintf(&m, "ERROR: %s belongs to the skill %.*s: load it with the skill tool first", name,
                        (int)strcspn(f->file + 7, "/"), f->file + 7) < 0 ? NULL : m;
    }
    char *r = bl_call(g_bl, name, args_json);
    size_t n = r ? strlen(r) : 0;
    if (n > FN_RESULT_MAX) { /* keep the conversation small: say what was cut */
        char note[200];
        int w = snprintf(note, sizeof(note), "\n...(cut: %zu more bytes. Return less, or write it to a file with bot.fs.write.)",
                         n - FN_RESULT_MAX);
        char *nr = realloc(r, FN_RESULT_MAX + (size_t)w + 1);
        if (nr) {
            r = nr;
            memcpy(r + FN_RESULT_MAX, note, (size_t)w + 1);
        }
    }
    return r;
}
