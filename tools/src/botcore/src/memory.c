#define _GNU_SOURCE
#include "memory.h"
#include "chat.h"
#include "guard.h"
#include "tools.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- small text helpers -------------------------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} out_t;

static void add(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(out_t *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
    }
}

static void human(char *out, size_t cap, long long n)
{
    if (n < 1024) {
        snprintf(out, cap, "%lld B", n);
    } else if (n < 1024 * 1024) {
        snprintf(out, cap, "%.1f KB", n / 1024.0);
    } else {
        snprintf(out, cap, "%.1f MB", n / (1024.0 * 1024));
    }
}

/* One line of at most max chars: control characters become spaces, "..." marks a cut. */
static void one_line(char *out, size_t cap, const char *s, size_t max)
{
    size_t n = 0;
    for (; *s && n + 4 < cap && n < max; s++) {
        unsigned char c = (unsigned char)*s;
        out[n++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80 && *s) { /* do not end inside a UTF-8 sequence */
        n--;
    }
    if (*s) {
        memcpy(out + n, "...", 3);
        n += 3;
    }
    out[n] = '\0';
}

/* The path the model knows: relative to the working directory when inside it. */
static const char *rel_path(const char *res)
{
    const char *ctx = guard_ctx();
    size_t n = strlen(ctx);
    return !strncmp(res, ctx, n) && res[n] == '/' ? res + n + 1 : res;
}

/* ---- history size + archiving ------------------------------------------ */

size_t mem_msg_bytes(const cJSON *m)
{
    size_t n = 0;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(m, "content"), *tc;
    if (cJSON_IsString(t)) {
        n += strlen(t->valuestring);
    }
    cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(m, "tool_calls"))
    {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(tc, "function"), "arguments");
        if (cJSON_IsString(a)) {
            n += strlen(a->valuestring);
        }
    }
    return n;
}

size_t mem_hist_bytes(const cJSON *hist)
{
    size_t n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, hist)
    {
        n += mem_msg_bytes(m);
    }
    return n;
}

static const char *role_of(const cJSON *m)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
    return cJSON_IsString(r) ? r->valuestring : "";
}

static int archived(const char *s)
{
    return !strncmp(s, "[archived #", 11);
}

/* Replace obj[key] with a new string (frees the old text, unlike cJSON_SetValuestring when shorter). */
static void set_text(cJSON *obj, const char *key, const char *text)
{
    cJSON_ReplaceItemInObjectCaseSensitive(obj, key, cJSON_CreateString(text));
}

static int keep_text(cJSON *archive, const char *text)
{
    cJSON_AddItemToArray(archive, cJSON_CreateString(text));
    return cJSON_GetArraySize(archive);
}

/* The arguments of the call a tool result answers, on one line (for the stub). */
static void call_args(const cJSON *hist, int upto, const char *id, char *out, size_t cap)
{
    out[0] = '\0';
    for (int i = upto - 1; i >= 0 && id && *id; i--) {
        const cJSON *tc;
        cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hist, i), "tool_calls"))
        {
            const cJSON *cid = cJSON_GetObjectItemCaseSensitive(tc, "id");
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(tc, "function"), "arguments");
            if (cJSON_IsString(cid) && !strcmp(cid->valuestring, id) && cJSON_IsString(a)) {
                one_line(out, cap, a->valuestring, 100);
                return;
            }
        }
    }
}

/* Long string values in a tool call's arguments -> stubs (the call stays valid JSON). */
static int fade_args(cJSON *call, cJSON *archive)
{
    cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
    cJSON *a = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
    if (!cJSON_IsString(a) || strlen(a->valuestring) < MEM_FADE_MIN) {
        return 0;
    }
    cJSON *args = cJSON_Parse(a->valuestring), *f;
    int n = 0;
    cJSON_ArrayForEach(f, (cJSON_IsObject(args) ? args : NULL))
    {
        if (!cJSON_IsString(f) || strlen(f->valuestring) < 200 || archived(f->valuestring)) {
            continue;
        }
        char size[32], stub[160];
        human(size, sizeof(size), (long long)strlen(f->valuestring));
        int id = keep_text(archive, f->valuestring);
        snprintf(stub, sizeof(stub), "[archived #%d: %s; recall(%d)]", id, size, id);
        cJSON_SetValuestring(f, stub);
        n++;
    }
    char *s = n ? cJSON_PrintUnformatted(args) : NULL;
    if (s) {
        set_text(fn, "arguments", s);
    }
    free(s);
    cJSON_Delete(args);
    return n;
}

int mem_compact(cJSON *hist, cJSON *archive)
{
    if (mem_hist_bytes(hist) < MEM_FADE_HIGH) {
        return 0;
    }
    int n = cJSON_GetArraySize(hist), first_user = -1;
    for (int i = 0; i < n && first_user < 0; i++) {
        if (!strcmp(role_of(cJSON_GetArrayItem(hist, i)), "user")) {
            first_user = i;
        }
    }
    /* The recent window stays raw: the last MEM_KEEP_TOOLS tool results, at most MEM_KEEP_BYTES. */
    int w = n, tools = 0;
    size_t bytes = 0;
    while (w > 0) {
        const cJSON *m = cJSON_GetArrayItem(hist, w - 1);
        size_t b = mem_msg_bytes(m);
        int is_tool = !strcmp(role_of(m), "tool");
        if ((is_tool && tools >= MEM_KEEP_TOOLS) || bytes + b > MEM_KEEP_BYTES || !strcmp(role_of(m), "system")) {
            break;
        }
        tools += is_tool;
        bytes += b;
        w--;
    }
    int done = 0;
    for (int i = 0; i < w; i++) {
        cJSON *m = cJSON_GetArrayItem(hist, i), *tc;
        const char *role = role_of(m);
        cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
        cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(m, "tool_calls"))
        {
            done += fade_args(tc, archive);
        }
        if (!cJSON_IsString(content) || strlen(content->valuestring) < MEM_FADE_MIN || archived(content->valuestring)) {
            continue;
        }
        char stub[400], size[32], what[240];
        human(size, sizeof(size), (long long)strlen(content->valuestring));
        if (!strcmp(role, "tool")) { /* the call says what it was */
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(m, "name");
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
            char args[110];
            call_args(hist, i, cJSON_IsString(id) ? id->valuestring : NULL, args, sizeof(args));
            snprintf(what, sizeof(what), "%s %s", cJSON_IsString(name) ? name->valuestring : "tool", args);
        } else if (!strcmp(role, "user") && i != first_user && strlen(content->valuestring) >= 4 * MEM_FADE_MIN) {
            char first[90]; /* a message says it itself */
            one_line(first, sizeof(first), content->valuestring, 60);
            snprintf(what, sizeof(what), "message \"%s\"", first);
        } else {
            continue; /* the task, short messages and the model's own words stay */
        }
        int id = keep_text(archive, content->valuestring);
        snprintf(stub, sizeof(stub), "[archived #%d: %s, %s; recall(%d)]", id, what, size, id);
        set_text(m, "content", stub);
        done++;
    }
    return done;
}

/* ---- ledger + profiles ---------------------------------------------------- */

typedef struct {
    char      path[256];
    long long bytes;
    long      lines;
    char      check[240];
} ledger_t;

typedef struct {
    char      path[256];
    long long bytes;
    long      rows;
    char      cols[400];
} profile_t;

static ledger_t  g_files[128];
static int       g_nfiles;
static profile_t g_profiles[32];
static int       g_nprofiles;

void mem_reset(void)
{
    g_nfiles = g_nprofiles = 0;
}

void mem_file_written(const char *res, const char *check)
{
    const char *p = rel_path(res);
    int i = 0;
    while (i < g_nfiles && strcmp(g_files[i].path, p) != 0) {
        i++;
    }
    int fresh = i == g_nfiles;
    if (fresh) {
        if (g_nfiles == (int)(sizeof(g_files) / sizeof(g_files[0]))) {
            return;
        }
        g_nfiles++;
    }
    ledger_t *l = &g_files[i];
    snprintf(l->path, sizeof(l->path), "%s", p);
    l->bytes = 0;
    l->lines = 0;
    FILE *f = fopen(res, "rb");
    int ch, last = '\n';
    while (f && (ch = getc(f)) != EOF) {
        l->bytes++;
        l->lines += ch == '\n';
        last = ch;
    }
    l->lines += last != '\n';
    if (f) {
        fclose(f);
    }
    if (check) {
        snprintf(l->check, sizeof(l->check), "%s", check);
    } else if (fresh) {
        l->check[0] = '\0';
    }
}

static void remember_profile(const char *path, long long bytes, long rows, const char *cols)
{
    int i = 0;
    while (i < g_nprofiles && strcmp(g_profiles[i].path, path) != 0) {
        i++;
    }
    if (i == g_nprofiles) {
        if (g_nprofiles == (int)(sizeof(g_profiles) / sizeof(g_profiles[0]))) {
            return;
        }
        g_nprofiles++;
    }
    profile_t *p = &g_profiles[i];
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->bytes = bytes;
    p->rows = rows;
    snprintf(p->cols, sizeof(p->cols), "%s", cols);
}

char *mem_state(const cJSON *archive)
{
    int na = cJSON_GetArraySize(archive);
    if (!na && !g_nfiles && !g_nprofiles) {
        return NULL;
    }
    out_t o = {malloc(16384), 0, 16384};
    if (!o.p) {
        return NULL;
    }
    o.p[0] = '\0';
    add(&o, "[Memory, kept for you by the harness (not part of the message above)]");
    if (g_nfiles) {
        add(&o, "\nFiles written this session:");
        for (int i = 0; i < g_nfiles; i++) {
            char size[32];
            human(size, sizeof(size), g_files[i].bytes);
            add(&o, "\n- %s: %s, %ld lines%s%s", g_files[i].path, size, g_files[i].lines, g_files[i].check[0] ? "; " : "",
                g_files[i].check);
        }
    }
    if (g_nprofiles) {
        add(&o, "\nData files (profiled):");
        for (int i = 0; i < g_nprofiles; i++) {
            char size[32];
            human(size, sizeof(size), g_profiles[i].bytes);
            add(&o, "\n- %s: %s, %ld rows; columns: %s", g_profiles[i].path, size, g_profiles[i].rows, g_profiles[i].cols);
        }
    }
    if (na) {
        add(&o, "\nOlder tool results and file contents are archived (%d so far): recall(id) shows one again.", na);
    }
    return o.p;
}

/* ---- CSV profile ----------------------------------------------------------- */

#define PROF_MAX_COLS 64
#define PROF_DISTINCT 12
#define PROF_SAMPLES  5

typedef struct {
    char   name[64];
    int    is_int, is_num, is_date;
    long   empty, filled;
    int    ndist, many;
    char  *dist[PROF_DISTINCT];
    double nmin, nmax;
    char   dmin[24], dmax[24];
} col_t;

/* One field from p (quotes handled): up to cap-1 bytes into out; *eol = the row ended. Returns the next p. */
static const char *csv_field(const char *p, const char *e, char delim, char *out, size_t cap, int *eol)
{
    size_t n = 0;
    *eol = 0;
    if (p < e && *p == '"') {
        for (p++; p < e; p++) {
            if (*p == '"') {
                if (p + 1 < e && p[1] == '"') {
                    p++;
                } else {
                    p++;
                    break;
                }
            }
            if (n + 1 < cap) {
                out[n++] = *p;
            }
        }
    }
    for (; p < e && *p != delim && *p != '\n'; p++) {
        if (*p != '\r' && n + 1 < cap) {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    if (p >= e || *p == '\n') {
        *eol = 1;
    }
    return p < e ? p + 1 : p;
}

static int looks_int(const char *s)
{
    if (*s == '-' || *s == '+') {
        s++;
    }
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

static int looks_num(const char *s, double *v)
{
    char *end;
    if (!*s || !(isdigit((unsigned char)*s) || ((*s == '-' || *s == '+' || *s == '.') && s[1]))) {
        return 0;
    }
    *v = strtod(s, &end);
    return *end == '\0';
}

static int looks_date(const char *s)
{
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7 ? s[i] != '-' : !isdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return s[10] == '\0' || s[10] == 'T' || s[10] == ' ';
}

static void col_see(col_t *c, const char *v)
{
    while (*v == ' ') {
        v++;
    }
    if (!*v) {
        c->empty++;
        return;
    }
    double d;
    int first = c->filled++ == 0;
    c->is_int = (first || c->is_int) && looks_int(v);
    int num = looks_num(v, &d);
    if ((first || c->is_num) && num) {
        c->nmin = first || d < c->nmin ? d : c->nmin;
        c->nmax = first || d > c->nmax ? d : c->nmax;
    }
    c->is_num = (first || c->is_num) && num;
    int date = looks_date(v);
    if ((first || c->is_date) && date) {
        if (first || strncmp(v, c->dmin, sizeof(c->dmin) - 1) < 0) {
            snprintf(c->dmin, sizeof(c->dmin), "%.23s", v);
        }
        if (first || strncmp(v, c->dmax, sizeof(c->dmax) - 1) > 0) {
            snprintf(c->dmax, sizeof(c->dmax), "%.23s", v);
        }
    }
    c->is_date = (first || c->is_date) && date;
    if (c->many) {
        return;
    }
    for (int i = 0; i < c->ndist; i++) {
        if (!strcmp(c->dist[i], v)) {
            return;
        }
    }
    if (c->ndist == PROF_DISTINCT) {
        c->many = 1;
    } else {
        c->dist[c->ndist++] = strndup(v, 60);
    }
}

static void num_text(char *out, size_t cap, double v, int is_int)
{
    if (is_int) {
        snprintf(out, cap, "%.0f", v);
    } else {
        snprintf(out, cap, "%.10g", v);
    }
}

int mem_profile(const char *res, const char *path, long long size, char *result, size_t rl)
{
    const char *dot = strrchr(path, '.');
    char delim = dot && !strcasecmp(dot, ".csv") ? ',' : dot && !strcasecmp(dot, ".tsv") ? '\t' : 0;
    if (!delim || size < MEM_PROFILE_MIN) {
        return 0;
    }
    long long scan = size > 64LL * 1024 * 1024 ? 64LL * 1024 * 1024 : size;
    int fd = open(res, O_RDONLY | O_CLOEXEC);
    const char *d = fd >= 0 ? mmap(NULL, (size_t)scan, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    if (fd >= 0) {
        close(fd);
    }
    if (d == MAP_FAILED) {
        return 0;
    }
    if (memchr(d, '\0', (size_t)(scan < 4096 ? scan : 4096))) { /* binary after all */
        munmap((void *)d, (size_t)scan);
        return 0;
    }
    const char *p = d, *e = d + scan;
    col_t *cols = calloc(PROF_MAX_COLS, sizeof(col_t));
    int ncols = 0, extra = 0, eol = 0;
    char field[256];
    while (p < e && !eol && cols) { /* header */
        p = csv_field(p, e, delim, field, sizeof(field), &eol);
        if (ncols < PROF_MAX_COLS) {
            snprintf(cols[ncols++].name, sizeof(cols[0].name), "%.63s", field);
        } else {
            extra++;
        }
    }
    const char *sample[PROF_SAMPLES];
    size_t sample_len[PROF_SAMPLES];
    long rows = 0;
    while (p < e && cols) {
        const char *row = p;
        int c = 0;
        eol = 0;
        while (p < e && !eol) {
            p = csv_field(p, e, delim, field, sizeof(field), &eol);
            if (c < ncols) {
                col_see(&cols[c], field);
            }
            c++;
        }
        if (c == 1 && !field[0]) {
            continue; /* blank line */
        }
        if (rows < PROF_SAMPLES) {
            sample[rows] = row;
            sample_len[rows] = (size_t)(p - row);
        }
        rows++;
    }
    out_t o = {result, 0, rl};
    char sz[32], names[400] = "";
    human(sz, sizeof(sz), size);
    add(&o, "%s: %s, %ld data rows%s, %d columns. This is a profile of the file, not its text.\nColumns:", path, sz, rows,
        scan < size ? " in the first 64 MB" : "", ncols + extra);
    for (int i = 0; i < ncols && cols; i++) {
        col_t *c = &cols[i];
        char a[40], b[40];
        size_t nl = strlen(names);
        snprintf(names + nl, sizeof(names) - nl, "%s%s", nl ? ", " : "", c->name);
        add(&o, "\n  %s: ", c->name);
        if (!c->filled) {
            add(&o, "always empty");
        } else if (c->is_num) {
            num_text(a, sizeof(a), c->nmin, c->is_int);
            num_text(b, sizeof(b), c->nmax, c->is_int);
            add(&o, "%s %s .. %s", c->is_int ? "integer" : "number", a, b);
        } else if (c->is_date) {
            add(&o, "date %s .. %s", c->dmin, c->dmax);
        } else if (c->many) {
            char ex[80];
            one_line(ex, sizeof(ex), c->dist[0], 40);
            add(&o, "text, many values, e.g. \"%s\"", ex);
        } else {
            add(&o, "text, one of:");
            for (int k = 0; k < c->ndist; k++) {
                char ex[80];
                one_line(ex, sizeof(ex), c->dist[k], 40);
                add(&o, "%s \"%s\"", k ? "," : "", ex);
            }
        }
        if (c->filled && (c->is_num || c->is_date) && !c->many && c->ndist <= 6) {
            add(&o, " (values:");
            for (int k = 0; k < c->ndist; k++) {
                add(&o, "%s %s", k ? "," : "", c->dist[k]);
            }
            add(&o, ")");
        }
        if (c->empty) {
            add(&o, "; %ld empty", c->empty);
        }
    }
    if (extra) {
        add(&o, "\n  (%d more columns not profiled)", extra);
    }
    add(&o, "\nFirst rows:\n");
    for (long i = 0; i < rows && i < PROF_SAMPLES; i++) {
        char line[420];
        size_t n = sample_len[i] < sizeof(line) - 4 ? sample_len[i] : sizeof(line) - 4;
        memcpy(line, sample[i], n);
        line[n] = '\0';
        line[strcspn(line, "\r\n")] = '\0';
        add(&o, "%s%s\n", line, sample_len[i] > n ? "..." : "");
    }
    add(&o, "To use it: process it with code (a Lua function: bot.csv.parse(bot.fs.read(\"%s\"))), look up rows with "
            "shell_exec (grep/awk), or fs_read with offset (0 = start) for the raw text.",
        path);
    remember_profile(path, size, rows, names);
    for (int i = 0; i < ncols && cols; i++) {
        for (int k = 0; k < cols[i].ndist; k++) {
            free(cols[i].dist[k]);
        }
    }
    free(cols);
    munmap((void *)d, (size_t)scan);
    return 1;
}

/* ---- tool: recall ----------------------------------------------------------- */


static bool recall_tool(const cJSON *in, char *result, size_t rl)
{
    chat_t *c = chat_current();
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(in, "id");
    int n = c ? cJSON_GetArraySize(c->archive) : 0;
    if (!cJSON_IsNumber(id) || id->valueint < 1 || id->valueint > n) {
        snprintf(result, rl, "no archived item %d (ids 1..%d)", cJSON_IsNumber(id) ? id->valueint : 0, n);
        return false;
    }
    const cJSON *t = cJSON_GetArrayItem(c->archive, id->valueint - 1);
    const char *s = cJSON_IsString(t) ? t->valuestring : "";
    size_t len = strlen(s);
    if (len < rl) {
        memcpy(result, s, len + 1);
    } else {
        size_t k = rl - 120;
        while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) {
            k--;
        }
        memcpy(result, s, k);
        snprintf(result + k, rl - k, "\n[cut at %zu of %zu bytes]", k, len);
    }
    return true;
}

const tool_t TOOLS_MEM[] = {
    {"recall",
     "Show an archived tool result or file content again, by the id in its [archived #id ...] stub.",
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}", recall_tool},
};
const size_t TOOLS_MEM_N = sizeof(TOOLS_MEM) / sizeof(TOOLS_MEM[0]);
