/*
 * packer: attach an agent project to a prebuilt botcore executable.
 *
 *   packer build <agent-dir> <botcore|file.bot> <out.bot>
 *   packer check <agent-dir> <botcore|file.bot>        (validate only)
 *   packer list  <file.bot>
 *
 * Embeds (read-only) from <agent-dir> (progress.md §12.1):
 *   AGENT.md, config.json, secrets.vault          persona; how to build and run the agent (load_config:
 *       name, display_name, version, description, internet, skills, requires, builder, mcp, agents,
 *       temperature, seed, reasoning); the sealed keys
 *   FLOW.lua                                      OPTIONAL orchestration: flow(input) gets every job
 *   functions/<name>.lua                          agent-wide Lua, always loaded (botcore/src/botlua.c checks
 *       it; functions/tests/<name>.lua run here and are never embedded; any error stops the build)
 *   skills/<name>/                                SKILL.md + anything the skill brings: references/,
 *       scripts/ (native ELF x86-64 or "#!" scripts: runnable at a real path at run time), assets/,
 *       functions/<n>.lua (tools only while the skill is loaded; functions/tests/ run here, never embedded)
 *   agents/<name>/AGENT.md                        sub-agents
 * Never embedded: evals/, artifacts/, hidden files, symlinks, anything else.
 * The old layout (agent.md, tools/bin, tools/doc) is an error that says where things go now.
 *
 * Output format: botcore ELF + zero padding to 4096 + BLOB + FOOTER, with the
 * ELF's 32-byte pack reference ("BOTPKREF", u64 blob_off, u64 file_size, u64 0)
 * patched so botcore can detect a truncated .bot. The format
 * is defined by botcore (src/blob.h); this file carries its own copy of it.
 * Plain C11/POSIX, no dependencies.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#define BLOB_MAGIC       "BOTBLOB1"
#define FOOTER_MAGIC     "BOTPACK1"
#define BLOB_HEADER_SIZE 16
#define BLOB_ENTRY_SIZE  32
#define FOOTER_SIZE      32
#define BLOB_ALIGN       4096
#define MAX_FILE         (256u * 1024 * 1024)

enum { K_DATA = 0, K_ELF = 1, K_SCRIPT = 2 };

typedef struct {
    char    *path;
    char    *full;
    char    *data;
    size_t   len;
    uint32_t kind;
} item_t;

static item_t *items;
static size_t  n_items, cap_items;
static int     errors, warnings;
static cJSON  *g_cfg; /* the project's config.json (NULL if absent or invalid) */

static void die(const char *msg)
{
    fprintf(stderr, "packer: %s\n", msg);
    exit(1);
}

static void err(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "packer: error: ");
    fprintf(stderr, fmt, a, b ? b : "");
    fputc('\n', stderr);
    errors++;
}

static void warn(const char *fmt, const char *a)
{
    fprintf(stderr, "packer: warning: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    warnings++;
}

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) {
        die("out of memory");
    }
    return d;
}

static char *join(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char  *r = malloc(n);
    if (!r) {
        die("out of memory");
    }
    snprintf(r, n, "%s/%s", a, b);
    return r;
}

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const unsigned char *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static uint32_t crc32_ieee(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        err("cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    size_t cap = 4096, n = 0;
    char  *buf = malloc(cap + 1);
    if (!buf) {
        die("out of memory");
    }
    size_t r;
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n > MAX_FILE) {
            err("%s is too large", path, NULL);
            free(buf);
            fclose(f);
            return NULL;
        }
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
            if (!buf) {
                die("out of memory");
            }
        }
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        err("read error on %s", path, NULL);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* ---- ELF inspection (x86-64 only) ---- */

/* returns 0 = not ELF64 LE, 1 = ELF; sets *x86 and *dynamic */
static int elf_info(const unsigned char *d, size_t n, int *x86, int *dynamic)
{
    *x86 = 0;
    *dynamic = 0;
    if (n < 64 || memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 2 || d[5] != 1) {
        return 0;
    }
    *x86 = (d[18] | (d[19] << 8)) == 62;
    uint64_t phoff = get64(d + 32);
    unsigned phentsize = d[54] | (d[55] << 8), phnum = d[56] | (d[57] << 8);
    for (unsigned i = 0; i < phnum; i++) {
        uint64_t o = phoff + (uint64_t)i * phentsize;
        if (phentsize < 4 || o + phentsize > n) {
            break;
        }
        if (get32(d + o) == 3) { /* PT_INTERP */
            *dynamic = 1;
        }
    }
    return 1;
}

/* ---- collecting files ---- */

static int bad_name(const char *name)
{
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '\\' || *p == '"') {
            return 1;
        }
    }
    return 0;
}

static void add_item(const char *rel, const char *full, uint32_t kind)
{
    if (n_items == cap_items) {
        cap_items = cap_items ? cap_items * 2 : 32;
        items = realloc(items, cap_items * sizeof(*items));
        if (!items) {
            die("out of memory");
        }
    }
    item_t *it = &items[n_items++];
    memset(it, 0, sizeof(*it));
    it->path = xstrdup(rel);
    it->full = xstrdup(full);
    it->kind = kind;
}

static void walk(const char *rel, const char *full, uint32_t kind, int flat)
{
    DIR *d = opendir(full);
    if (!d) {
        err("cannot open %s: %s", full, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char *r = join(rel, e->d_name), *f = join(full, e->d_name);
        struct stat st;
        if (bad_name(e->d_name)) {
            err("unsupported file name under %s (control char, quote or backslash)", rel, NULL);
        } else if (lstat(f, &st) != 0) {
            err("cannot stat %s: %s", f, strerror(errno));
        } else if (S_ISDIR(st.st_mode)) {
            if (flat) {
                warn("ignoring sub-directory %s (tools/bin must be flat)", r);
            } else {
                walk(r, f, kind, flat);
            }
        } else if (S_ISREG(st.st_mode)) {
            add_item(r, f, kind);
        } else {
            warn("skipping non-regular file %s", r);
        }
        free(r);
        free(f);
    }
    closedir(d);
}

static void add_root_file(const char *dir, const char *rel)
{
    char *f = join(dir, rel);
    struct stat st;
    if (lstat(f, &st) == 0 && S_ISREG(st.st_mode)) {
        add_item(rel, f, K_DATA);
    }
    free(f);
}

static void add_tree(const char *dir, const char *rel, uint32_t kind, int flat)
{
    char *f = join(dir, rel);
    struct stat st;
    if (lstat(f, &st) == 0 && S_ISDIR(st.st_mode)) {
        walk(rel, f, kind, flat);
    }
    free(f);
}

static int cmp_item(const void *a, const void *b)
{
    return strcmp(((const item_t *)a)->path, ((const item_t *)b)->path);
}

static int has_item(const char *path)
{
    for (size_t i = 0; i < n_items; i++) {
        if (strcmp(items[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static const item_t *find_item(const char *path)
{
    for (size_t i = 0; i < n_items; i++) {
        if (!strcmp(items[i].path, path)) {
            return &items[i];
        }
    }
    return NULL;
}

#include "../botcore/src/botlua.c"

/* A name some tool of this agent already has: botcore's built-ins, tools/bin, MCP servers' tools. */
static int known_tool(const char *name)
{
    static const char *const builtin[] = {"fs_list",  "fs_read",  "fs_write",  "fs_edit",     "shell_exec", "skill",
                                          "recall",   "get_time", "image_view", NULL};
    for (int i = 0; builtin[i]; i++) {
        if (strcmp(builtin[i], name) == 0) {
            return 1;
        }
    }
    if (strcmp(name, "delegate") == 0) { /* sub-agents */
        return 1;
    }
    const cJSON *srv; /* a tool of an MCP server: <server>_<tool>; which tools it has is known only at run time */
    cJSON_ArrayForEach(srv, cJSON_GetObjectItemCaseSensitive(g_cfg, "mcp"))
    {
        size_t n = strlen(srv->string);
        if (!strncmp(name, srv->string, n) && name[n] == '_' && name[n + 1]) {
            return 1;
        }
    }
    return 0;
}

/* ---- functions/<name>.lua (botlua.c): checked, and functions/tests/<name>.lua run, at build time ---- */

static const char *g_dir; /* the project folder: what tests read files relative to */

/* functions/<name>.lua: embedded; functions/tests/ is not (it only runs here). */
static void add_functions(const char *dir)
{
    char *f = join(dir, "functions");
    DIR  *d = opendir(f);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        struct stat st;
        char *full = join(f, e->d_name), rel[300];
        size_t l = strlen(e->d_name);
        snprintf(rel, sizeof(rel), "functions/%s", e->d_name);
        if (e->d_name[0] != '.' && lstat(full, &st) == 0) {
            if (S_ISREG(st.st_mode) && l > 4 && !strcmp(e->d_name + l - 4, ".lua") && !bad_name(e->d_name)) {
                add_item(rel, full, K_DATA);
            } else if (!(S_ISDIR(st.st_mode) && !strcmp(e->d_name, "tests"))) {
                warn("%s is not embedded (functions/ holds .lua files, and tests/ for their tests)", rel);
            }
        }
        free(full);
    }
    if (d) {
        closedir(d);
    }
    free(f);
}

static char *t_read(void *ud, const char *path, size_t *len, char *e, size_t el)
{
    (void)ud;
    if (path[0] == '/' || strstr(path, "..")) {
        snprintf(e, el, "tests read files inside the project folder only");
        return NULL;
    }
    char *full = join(g_dir, path);
    FILE *f = fopen(full, "rb");
    free(full);
    if (!f) {
        snprintf(e, el, "%s", strerror(errno));
        return NULL;
    }
    size_t cap = 65536, n = 0, r;
    char *buf = malloc(cap + 1);
    while (buf && (r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            buf = realloc(buf, (cap *= 2) + 1);
        }
    }
    fclose(f);
    if (!buf) {
        die("out of memory");
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int t_exists(void *ud, const char *path)
{
    (void)ud;
    if (path[0] == '/' || strstr(path, "..")) {
        return 0;
    }
    char *full = join(g_dir, path);
    int ok = access(full, F_OK) == 0;
    free(full);
    return ok;
}

static int t_today(void *ud, char out[11])
{
    (void)ud;
    const char *pin = getenv("BOTTER_TODAY");
    time_t t = time(NULL);
    struct tm tm;
    if (pin && strlen(pin) == 10) {
        memcpy(out, pin, 11);
        return 0;
    }
    localtime_r(&t, &tm);
    return strftime(out, 11, "%Y-%m-%d", &tm) == 10 ? 0 : -1;
}

static void lua_diag(void *ud, const char *file, int line, int error, const char *msg)
{
    (void)ud;
    if (line) {
        fprintf(stderr, "%s:%d: %s: %s\n", file, line, error ? "error" : "warning", msg);
    } else {
        fprintf(stderr, "%s: %s: %s\n", file, error ? "error" : "warning", msg);
    }
    if (error) {
        errors++;
    } else {
        warnings++;
    }
}

/* functions/<n>.lua or skills/<skill>/functions/<n>.lua */
static int is_function_file(const char *path)
{
    const char *f = !strncmp(path, "functions/", 10) ? path + 10 : NULL;
    if (!f && !strncmp(path, "skills/", 7)) {
        const char *sl = strchr(path + 7, '/');
        f = sl && !strncmp(sl, "/functions/", 11) ? sl + 11 : NULL;
    }
    size_t l = f ? strlen(f) : 0;
    return l > 4 && !strchr(f, '/') && !strcmp(f + l - 4, ".lua");
}

static char g_skill_dir[4096];

/* bot.skill_dir in tests: the project's own skills/<name> folder. */
static const char *t_skill_dir(void *ud, const char *name)
{
    (void)ud;
    char p[300];
    snprintf(p, sizeof(p), "skills/%s/SKILL.md", name);
    if (strchr(name, '/') || !has_item(p)) {
        return NULL;
    }
    char *d = join(g_dir, "skills");
    snprintf(g_skill_dir, sizeof(g_skill_dir), "%s/%s", d, name);
    free(d);
    return g_skill_dir;
}

static bl_state *load_functions(int collect_fns)
{
    static const bl_io io = {.fs_read = t_read, .fs_exists = t_exists, .today = t_today, .skill_dir = t_skill_dir}; /* tests: no writes, no network */
    bl_state *s = bl_open(&io);
    if (!s) {
        die("out of memory starting Lua");
    }
    for (size_t i = 0; i < n_items; i++) {
        int flow = !strcmp(items[i].path, "FLOW.lua"); /* orchestration: loaded, never tools */
        if ((flow || is_function_file(items[i].path)) && items[i].data) {
            bl_load(s, items[i].path, items[i].data, items[i].len, collect_fns && !flow, collect_fns ? lua_diag : NULL, NULL);
        }
    }
    return s;
}

/* A score's details, at most 20 lines (a long list of differences hides the build's real errors). */
static void print_details(const char *details)
{
    int lines = 0, more = 0;
    for (const char *p = details; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (lines < 20) {
            fprintf(stderr, "%.*s\n", (int)n, p);
            lines++;
        } else {
            more++;
        }
        p = nl ? nl + 1 : p + n;
    }
    if (more) {
        fprintf(stderr, "... %d more line(s)\n", more);
    }
}

/* Run every test_ and score_ function of the test files in reldir (relative to the project). */
static void run_tests(const char *reldir, int *passed, int *failed)
{
    char *tdir = join(g_dir, reldir);
    DIR *d = opendir(tdir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcmp(e->d_name + l - 4, ".lua") != 0) {
            continue;
        }
        char rel[300], *full = join(tdir, e->d_name);
        size_t len = 0;
        snprintf(rel, sizeof(rel), "%s/%s", reldir, e->d_name);
        char *src = slurp(full, &len);
        free(full);
        bl_state *t = load_functions(0);
        if (src && bl_load(t, rel, src, len, 0, lua_diag, NULL) == 0) {
            int n = 0, ns = 0;
            char **scores = bl_globals(t, "score_", &ns); /* how close to the examples: reported, never a failure */
            for (int k = 0; k < ns; k++) {
                double got = 0, total = 0;
                char *details = NULL, *why = bl_call_score(t, scores[k], &got, &total, &details);
                if (why) {
                    fprintf(stderr, "%s: %s FAILED: %s\n", rel, scores[k], why);
                    errors++;
                } else {
                    fprintf(stderr, "packer: score %s: %g/%g (%.0f%%)\n", scores[k] + 6, got, total, 100.0 * got / total);
                    print_details(details);
                }
                free(why);
                free(details);
                free(scores[k]);
            }
            free(scores);
            char **names = bl_globals(t, "test_", &n);
            for (int k = 0; k < n; k++) {
                char *why = bl_call_plain(t, names[k]);
                if (why) {
                    fprintf(stderr, "%s: test %s FAILED: %s\n", rel, names[k], why);
                    (*failed)++;
                    errors++;
                } else {
                    (*passed)++;
                }
                free(why);
                free(names[k]);
            }
            free(names);
        }
        free(src);
        bl_close(t);
    }
    if (d) {
        closedir(d);
    }
    free(tdir);
}

/* Every function: a name no other tool has; then each test file, on its own copy of the functions. */
static void check_functions(void)
{
    int any = 0;
    const item_t *flow = find_item("FLOW.lua");
    for (size_t i = 0; i < n_items; i++) {
        any |= is_function_file(items[i].path);
    }
    if (!any && !flow) {
        return;
    }
    bl_state *s = load_functions(1);
    if (flow && !errors && !bl_has_global(s, "flow")) {
        fprintf(stderr, "FLOW.lua: error: it defines no global function flow(input): botcore calls flow with each job "
                        "(a user message, a --run line, a request) and shows what it returns\n");
        errors++;
    }
    if (flow && flow->data && (memmem(flow->data, flow->len, "\n---@param", 10) || !strncmp(flow->data, "---", 3))) {
        warn("%s", "FLOW.lua: its functions are not tools (FLOW.lua orchestrates); a function the model should call "
                   "belongs in functions/");
    }
    char line[1000] = "";
    for (int i = 0; i < bl_count(s); i++) {
        const bl_fn *f = bl_get(s, i);
        if (known_tool(f->name)) {
            fprintf(stderr, "%s:%d: error: %s is already a tool's name; call the function something else\n", f->file,
                    f->line, f->name);
            errors++;
        }
        size_t l = strlen(line);
        snprintf(line + l, sizeof(line) - l, "%s%s (%s)", l ? ", " : "", f->name, f->uses[0] ? f->uses : "pure");
    }
    if (line[0]) {
        fprintf(stderr, "packer: functions: %s%s\n", line, flow ? "; FLOW.lua: flow" : "");
    } else if (flow) {
        fprintf(stderr, "packer: functions: no tools; FLOW.lua: flow\n");
    } else {
        warn("%s", "functions/: no function has a --- doc comment directly above it, so the model can call none of "
                   "them (a batch agent calls them from FLOW.lua instead; see the Botter skill's "
                   "references/lua-api.md)");
    }
    bl_close(s);
    if (errors) {
        return;
    }
    int passed = 0, failed = 0;
    run_tests("functions/tests", &passed, &failed);
    char *sk = join(g_dir, "skills");
    DIR *sd = opendir(sk);
    struct dirent *se;
    while (sd && (se = readdir(sd))) {
        char rel[300];
        if (se->d_name[0] != '.' && snprintf(rel, sizeof(rel), "skills/%s/functions/tests", se->d_name) < (int)sizeof(rel)) {
            run_tests(rel, &passed, &failed);
        }
    }
    if (sd) {
        closedir(sd);
    }
    free(sk);
    if (passed || failed) {
        fprintf(stderr, "packer: function tests: %d passed, %d failed\n", passed, failed);
    }
}

static char g_core_version[32]; /* the runtime's BOTCORE_VERSION tag, "" if unknown */

static int name_ok(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 64 || !(islower((unsigned char)n[0]) || isdigit((unsigned char)n[0]))) {
        return 0;
    }
    for (size_t i = 0; i < l; i++) {
        if (!islower((unsigned char)n[i]) && !isdigit((unsigned char)n[i]) && !strchr("-_.", n[i])) {
            return 0;
        }
    }
    return 1;
}

/* MAJOR.MINOR.PATCH, numbers without leading zeros, optional -pre / +build. */
static int version_ok(const char *v)
{
    for (int part = 0; part < 3; part++) {
        if (!isdigit((unsigned char)*v) || (v[0] == '0' && isdigit((unsigned char)v[1]))) {
            return 0;
        }
        while (isdigit((unsigned char)*v)) {
            v++;
        }
        if (part < 2 && *v++ != '.') {
            return 0;
        }
    }
    if (*v == '-' || *v == '+') {
        for (v++; *v; v++) {
            if (!isalnum((unsigned char)*v) && !strchr(".-+", *v)) {
                return 0;
            }
        }
    }
    return *v == '\0';
}

static void cfg_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cfg_err(const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    err("config.json: %s%s", m, NULL);
}

static void cfg_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cfg_warn(const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    warn("config.json: %s", m);
}

/* A list of names ("skills", "tools", "requires"): 1 if valid (or absent). */
static int cfg_list_ok(const char *key, int (*valid)(const char *))
{
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(g_cfg, key), *e;
    if (!l) {
        return 1;
    }
    int ok = cJSON_IsArray(l);
    cJSON_ArrayForEach(e, (ok ? l : NULL))
    {
        ok &= cJSON_IsString(e) && valid(e->valuestring);
    }
    if (!ok) {
        cfg_err("\"%s\" must be a list of names", key);
    }
    return ok;
}

static int cmd_name_ok(const char *c)
{
    return *c && !strpbrk(c, " /;|&$`'\"\t");
}

static int tool_name_ok(const char *c)
{
    return *c && strlen(c) <= 64 && strspn(c, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(c);
}

static int in_cfg_list(const char *key, const char *name)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(g_cfg, key))
    {
        if (cJSON_IsString(e) && !strcmp(e->valuestring, name)) {
            return 1;
        }
    }
    return 0;
}

/*
 * config.json: how to build and run the agent.
 *   {"name": "lead-outreach", "display_name": "Lead Outreach", "version": "0.1.0",
 *    "description": "...", "internet": true, "skills": [...], "tools": [...],
 *    "requires": ["python3"]}
 * Read before the files are collected: "skills" / "tools" choose what is compiled in.
 */
static void load_config(const char *dir)
{
    char *old = join(dir, "agent.json");
    if (access(old, F_OK) == 0) {
        err("%s: agent.json is now config.json; rename it (its fields: the Botter skill's references/layout.md)%s", old, NULL);
    }
    free(old);
    char *f = join(dir, "config.json");
    size_t n = 0;
    char *d = access(f, F_OK) == 0 ? slurp(f, &n) : NULL;
    free(f);
    if (!d) {
        warn("no config.json: add one with the agent's %s (the Botter skill's references/layout.md)", "name, version and description");
        return;
    }
    g_cfg = cJSON_ParseWithLength(d, n);
    free(d);
    if (!cJSON_IsObject(g_cfg)) {
        err("config.json is not a valid JSON object%s%s", g_cfg ? "" : " (syntax error)", NULL);
        cJSON_Delete(g_cfg);
        g_cfg = NULL;
        return;
    }
    static const char *const strs[] = {"name", "display_name", "version", "description", NULL};
    static const char *const bools[] = {"internet", NULL};
    static const char *const lists[] = {"skills", "requires", NULL};
    if (cJSON_GetObjectItemCaseSensitive(g_cfg, "builder")) {
        cfg_err("\"builder\" is gone: remove it (keys are sealed with botter seal, for any agent)%s", "");
    }
    for (const cJSON *x = g_cfg->child; x; x = x->next) {
        int known = 0;
        if (!strcmp(x->string, "builder")) {
            continue;
        }
        for (int k = 0; strs[k]; k++) {
            if (!strcmp(x->string, strs[k])) {
                known = 1;
                if (!cJSON_IsString(x)) {
                    cfg_err("\"%s\" must be a string", x->string);
                }
            }
        }
        for (int k = 0; bools[k]; k++) {
            if (!strcmp(x->string, bools[k])) {
                known = 1;
                if (!cJSON_IsBool(x)) {
                    cfg_err("\"%s\" must be true or false", x->string);
                }
            }
        }
        for (int k = 0; lists[k]; k++) {
            known |= !strcmp(x->string, lists[k]);
        }
        if (!strcmp(x->string, "temperature")) { /* sent with every model request */
            known = 1;
            if (!cJSON_IsNumber(x) || x->valuedouble < 0 || x->valuedouble > 2) {
                cfg_err("\"temperature\" must be a number from 0 to 2 (0 = the most repeatable answers)%s", "");
            }
        }
        if (!strcmp(x->string, "tools")) { /* the old layout: programs are skill scripts now */
            known = 1;
            cfg_err("\"tools\" is gone: programs live in a skill's scripts/ folder (listed through \"skills\"), "
                    "and a Lua function in functions/ reaches one with bot.shell%s", "");
        }
        if (!strcmp(x->string, "reasoning")) { /* reasoning_effort on every model request */
            known = 1;
            if (!cJSON_IsString(x) || (strcmp(x->valuestring, "low") && strcmp(x->valuestring, "medium") &&
                                      strcmp(x->valuestring, "high"))) {
                cfg_err("\"reasoning\" must be \"low\", \"medium\" or \"high\" (how much the model thinks first)%s", "");
            }
        }
        if (!strcmp(x->string, "seed")) {
            known = 1;
            if (!cJSON_IsNumber(x) || x->valuedouble != (double)(long long)x->valuedouble) {
                cfg_err("\"seed\" must be a whole number%s", "");
            }
        }
        if (!strcmp(x->string, "mcp") || !strcmp(x->string, "agents")) { /* remote: {"name": {"url": ..., "token": ...}} */
            known = 1;
            const cJSON *e = cJSON_IsObject(x) ? x->child : NULL;
            if (!cJSON_IsObject(x)) {
                cfg_err("\"%s\" must be an object: {\"name\": {\"url\": \"https://...\"}}", x->string);
            }
            for (; e; e = e->next) {
                const cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "url"), *tk = cJSON_GetObjectItemCaseSensitive(e, "token");
                const char *us = cJSON_IsString(u) ? u->valuestring : "";
                char who[160];
                snprintf(who, sizeof(who), "%s \"%.60s\"", x->string, e->string);
                if (!name_ok(e->string) || strchr(e->string, '.')) {
                    cfg_err("%s: the name must be lowercase letters, digits, - or _", who);
                }
                const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(e, "command"), *a;
                if (cmd && !strcmp(x->string, "mcp")) { /* an MCP server this agent starts itself */
                    int ok = cJSON_IsArray(cmd) && cJSON_GetArraySize(cmd) > 0 && !u;
                    cJSON_ArrayForEach(a, cmd)
                    {
                        ok &= cJSON_IsString(a);
                    }
                    if (!ok) {
                        cfg_err("%s: \"command\" is a list of texts, the program first ([\"npx\", \"-y\", \"@scope/server\"]), "
                                "and replaces \"url\"", who);
                    } else if (!in_cfg_list("requires", cmd->child->valuestring)) {
                        cfg_warn("%s starts \"%s\": list it in \"requires\" so the agent can say when it is not installed",
                                 who, cmd->child->valuestring);
                    }
                    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(e, "env"))
                    {
                        if (!cJSON_IsString(a) || strpbrk(a->valuestring, " =")) {
                            cfg_err("%s: \"env\" lists NAMES of secrets (lines of .env or the vault), never the values", who);
                        }
                    }
                } else if (strncmp(us, "https://", 8) && strncmp(us, "http://127.0.0.1", 16) && strncmp(us, "http://localhost", 16)) {
                    cfg_err("%s needs \"url\": \"https://...\" (http:// only for localhost)%s", who,
                            strcmp(x->string, "mcp") ? "" : ", or \"command\": [...] for a server to start");
                }
                if (tk && (!cJSON_IsString(tk) || strpbrk(tk->valuestring, " =:/.") || strlen(tk->valuestring) > 64)) {
                    cfg_err("%s: \"token\" is the NAME of a line in .env (e.g. \"DOCS_TOKEN\"), never the secret itself: "
                            "config.json is embedded in the .bot for anyone to read", who);
                }
            }
        }
        if (!known) {
            cfg_warn("unknown key \"%.60s\" (known: name, display_name, version, description, internet, skills, tools, "
                     "requires, mcp, agents, temperature, seed)", x->string);
        }
    }
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(g_cfg, "name");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_cfg, "version");
    const cJSON *ds = cJSON_GetObjectItemCaseSensitive(g_cfg, "description");
    if (!nm) {
        cfg_warn("no \"name\" (%s)", "lowercase letters, digits, - _ .; usually the folder name");
    } else if (cJSON_IsString(nm) && !name_ok(nm->valuestring)) {
        cfg_err("name \"%s\" must be 1-64 lowercase letters, digits, - _ . (starting with a letter or digit); put the "
                "spelling people see in \"display_name\"", nm->valuestring);
    }
    if (!v) {
        cfg_warn("no \"version\" (%s)", "MAJOR.MINOR.PATCH, e.g. \"0.1.0\"");
    } else if (cJSON_IsString(v) && !version_ok(v->valuestring)) {
        cfg_err("version \"%s\" is not MAJOR.MINOR.PATCH (e.g. 1.0.0)", v->valuestring);
    }
    if (!ds) {
        cfg_warn("no \"description\" (%s)", "one sentence: what the agent does and for whom");
    } else if (cJSON_IsString(ds) && strlen(ds->valuestring) > 300) {
        cfg_warn("keep the description to one or two sentences (%s)", "300 characters");
    }
    cfg_list_ok("skills", tool_name_ok);
    cfg_list_ok("tools", tool_name_ok);
    cfg_list_ok("requires", cmd_name_ok);
}

/*
 * After the files are collected: "skills" / "tools" (when given) decide which are
 * compiled in; listed ones must exist, unlisted ones are left out with a warning.
 */
static void select_from_config(void)
{
    const cJSON *sk = cJSON_GetObjectItemCaseSensitive(g_cfg, "skills");
    char seen[1024] = "";
    size_t w = 0;
    for (size_t i = 0; i < n_items; i++) {
        const char *p = items[i].path;
        char nm[200] = "";
        if (cJSON_IsArray(sk) && !strncmp(p, "skills/", 7)) {
            snprintf(nm, sizeof(nm), "%.*s", (int)strcspn(p + 7, "/"), p + 7);
            if (!in_cfg_list("skills", nm)) {
                char tag[260];
                snprintf(tag, sizeof(tag), "|%s|", nm);
                if (!strstr(seen, tag) && strlen(seen) + strlen(tag) < sizeof(seen)) {
                    strcat(seen, tag);
                    char m[300];
                    snprintf(m, sizeof(m), "skill \"%s\" is not listed in config.json \"skills\"; it is left out", nm);
                    warn("%s", m);
                }
                free(items[i].path);
                free(items[i].full);
                continue;
            }
        }
        items[w++] = items[i];
    }
    n_items = w;
    const cJSON *e;
    cJSON_ArrayForEach(e, (cJSON_IsArray(sk) ? sk : NULL))
    {
        char p[300];
        snprintf(p, sizeof(p), "skills/%s/SKILL.md", cJSON_IsString(e) ? e->valuestring : "");
        if (!has_item(p)) {
            cfg_err("skill \"%s\" is listed but there is no %s", cJSON_IsString(e) ? e->valuestring : "?", p);
        }
    }
}

/* Skill scripts need their interpreter on the machine: it belongs in "requires". */
static void check_requires(void)
{
    for (size_t i = 0; i < n_items; i++) {
        if (items[i].kind != K_SCRIPT || !items[i].data) {
            continue;
        }
        const char *l = items[i].data + 2, *nl = strchr(l, '\n');
        char line[300];
        snprintf(line, sizeof(line), "%.*s", (int)(nl ? nl - l : (long)strlen(l)), l);
        char *tok = strtok(line, " \t\r");
        if (tok && !strcmp(tok, "/usr/bin/env")) {
            tok = strtok(NULL, " \t\r");
            if (tok && !strcmp(tok, "-S")) {
                tok = strtok(NULL, " \t\r");
            }
        } else if (tok && strrchr(tok, '/')) {
            tok = strrchr(tok, '/') + 1;
        }
        if (tok && strcmp(tok, "sh") != 0 && strcmp(tok, "bash") != 0 && !in_cfg_list("requires", tok)) {
            char m[300];
            snprintf(m, sizeof(m), "%s runs with %s: add \"%s\" to \"requires\"", items[i].path, tok, tok);
            cfg_warn("%s", m);
        }
    }
}

/* Load every item; a skill's programs (ELF, or a script with "#!") become runnable files. */
static void load_and_classify(void)
{
    for (size_t i = 0; i < n_items; i++) {
        item_t *it = &items[i];
        it->data = slurp(it->full, &it->len);
        if (!it->data || strncmp(it->path, "skills/", 7) != 0) {
            continue;
        }
        int x86, dyn;
        const char *sl = strchr(it->path + 7, '/');
        int in_scripts = sl && !strncmp(sl, "/scripts/", 9);
        if (elf_info((unsigned char *)it->data, it->len, &x86, &dyn)) {
            it->kind = K_ELF;
            if (!x86) {
                err("%s is an ELF program but not for x86-64%s", it->path, "");
            } else if (dyn) {
                warn("%s is dynamically linked; it only runs where its shared libraries exist "
                     "(link it statically for a portable agent)",
                     it->path);
            }
        } else if (it->len >= 3 && it->data[0] == '#' && it->data[1] == '!') {
            it->kind = K_SCRIPT;
        } else if (in_scripts && memchr(it->data, '\0', it->len < 4096 ? it->len : 4096) == NULL) {
            const char *dot = strrchr(it->path, '.');
            if (dot && (!strcmp(dot, ".py") || !strcmp(dot, ".sh") || !strcmp(dot, ".js") || !strcmp(dot, ".rb") ||
                        !strcmp(dot, ".pl"))) {
                warn("%s has no \"#!\" line, so it cannot run on its own: add one (#!/usr/bin/env python3, #!/bin/sh ...) "
                     "or run it through its interpreter",
                     it->path);
            }
        }
    }
}

/* ---- contracts the build can see: descriptors, skill frontmatter, tool scripts ---- */

static void diagf(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void diagf(int error, const char *fmt, ...)
{
    char    m[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if (error) {
        err("%s%s", m, NULL);
    } else {
        warn("%s", m);
    }
}

/* Frontmatter value of key (one line, or a folded/indented block after "key: >" or "key: |"), quotes removed. */
static int fm_value(const char *fm, size_t fl, const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\n%s:", key);
    const char *k = memmem(fm, fl, pat, strlen(pat)), *end = fm + fl;
    out[0] = '\0';
    if (!k) {
        return 0;
    }
    const char *v = k + strlen(pat);
    while (v < end && (*v == ' ' || *v == '\t')) {
        v++;
    }
    size_t n = 0;
    const char *nl = memchr(v, '\n', (size_t)(end - v));
    size_t first = nl ? (size_t)(nl - v) : (size_t)(end - v);
    int block = first == 0 || ((*v == '>' || *v == '|') && first <= 2);
    if (!block) {
        n = first < cap - 1 ? first : cap - 1;
        memcpy(out, v, n);
    } else {
        for (const char *l = nl ? nl + 1 : end; l < end && (*l == ' ' || *l == '\t');) { /* indented lines */
            const char *e = memchr(l, '\n', (size_t)(end - l));
            size_t ll = e ? (size_t)(e - l) : (size_t)(end - l);
            size_t sp = strspn(l, " \t");
            for (size_t i = sp; i < ll && n + 2 < cap; i++) {
                out[n++] = l[i];
            }
            if (n + 1 < cap) {
                out[n++] = ' ';
            }
            l = e ? e + 1 : end;
        }
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\r')) {
        n--;
    }
    out[n] = '\0';
    if (n >= 2 && (out[0] == '"' || out[0] == '\'') && out[n - 1] == out[0]) {
        memmove(out, out + 1, n - 2);
        out[n - 2] = '\0';
    }
    return 1;
}

/* Placeholder words that mean the text is unfinished (whole words, as written). */
static const char *placeholder(const char *t, size_t n)
{
    static const char *const words[] = {"TODO", "TBD", "FIXME", "XXX", NULL};
    for (int i = 0; words[i]; i++) {
        size_t wl = strlen(words[i]);
        for (const char *p = t; (p = memmem(p, (size_t)(t + n - p), words[i], wl)); p += wl) {
            /* a quoted word ("TODO", `TODO`) is a mention, not a placeholder */
            int before = p == t || (!isalnum((unsigned char)p[-1]) && !strchr("\"'`", p[-1]));
            int after = p + wl >= t + n || (!isalnum((unsigned char)p[wl]) && !strchr("\"'`", p[wl]));
            if (before && after) {
                return words[i];
            }
        }
    }
    return strcasestr(t, "lorem ipsum") ? "lorem ipsum" : NULL;
}


/* "a/b/../c" -> "a/c"; 0 when it climbs out. */
static int norm_path(char *p)
{
    char out[600] = "";
    for (char *save = NULL, *c = strtok_r(p, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        if (!strcmp(c, ".")) {
            continue;
        }
        if (!strcmp(c, "..")) {
            char *sl = strrchr(out, '/');
            if (!out[0]) {
                return 0;
            }
            if (sl) {
                *sl = '\0';
            } else {
                out[0] = '\0';
            }
            continue;
        }
        size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, "%s%s", l ? "/" : "", c);
    }
    strcpy(p, out);
    return 1;
}

/*
 * skills/<name>/: the agent picks a skill by its description and follows SKILL.md; everything else in the
 * folder must be reachable from it. Checks: frontmatter (name = folder, description <= 1024 chars), links
 * that resolve, references/ files that something points to, length, placeholder words.
 */
static void check_skill(const item_t *it)
{
    const char *d = it->data, *end = it->data + it->len;
    const char *close = it->len > 4 && strncmp(d, "---\n", 4) == 0 ? memmem(d + 4, end - d - 4, "\n---", 4) : NULL;
    char folder[200], v[2048];
    snprintf(folder, sizeof(folder), "%.*s", (int)strcspn(it->path + 7, "/"), it->path + 7);
    if (!close) {
        diagf(1, "%s has no frontmatter; start it with:\n---\nname: %s\ndescription: <what it does and when to use "
                 "it, one or two sentences>\n---",
              it->path, folder);
        return;
    }
    size_t fl = (size_t)(close - d) + 1;
    if (!fm_value(d, fl, "name", v, sizeof(v))) {
        diagf(1, "%s: frontmatter has no \"name:\" (it must be %s, the folder's name)", it->path, folder);
    } else if (strcmp(v, folder) != 0) {
        diagf(1, "%s: \"name: %s\" must be the folder's name, %s", it->path, v, folder);
    }
    if (!fm_value(d, fl, "description", v, sizeof(v)) || !v[0]) {
        diagf(1, "%s: frontmatter has no \"description:\" (the agent decides from it when to load the skill: say what "
                 "it does and when to use it)",
              it->path);
    } else if (strlen(v) > 1024) {
        diagf(1, "%s: the description has %zu characters; keep it under 1024 (what it does and when to use it; the "
                 "details belong in the body)",
              it->path, strlen(v));
    }
    long lines = 0;
    for (const char *p = d; p < end; p++) {
        lines += *p == '\n';
    }
    if (lines > 500) {
        diagf(0, "%s has %ld lines: keep SKILL.md under 500 and move details to references/ files it points to",
              it->path, lines);
    }
    char base[300];
    snprintf(base, sizeof(base), "skills/%s/", folder);
    size_t bl = strlen(base);
    /* every file of this skill: reachable from SKILL.md (by a link or by its path), and finished */
    size_t first = 0, count = 0;
    for (size_t i = 0; i < n_items; i++) {
        if (!strncmp(items[i].path, base, bl)) {
            if (!count++) {
                first = i;
            }
        }
    }
    char *reach = calloc(count + 1, 1);
    for (size_t k = 0; k < count; k++) {
        reach[k] = items[first + k].path == it->path || !strcmp(items[first + k].path, it->path);
    }
    for (int changed = 1; changed && reach;) {
        changed = 0;
        for (size_t k = 0; k < count; k++) {
            const item_t *m = &items[first + k];
            size_t ml = strlen(m->path);
            if (!reach[k] || m->kind != K_DATA || ml < 3 || strcmp(m->path + ml - 3, ".md") != 0) {
                continue;
            }
            for (size_t j = 0; j < count; j++) {
                const char *rel = items[first + j].path + bl, *bn = strrchr(rel, '/');
                if (!reach[j] && (memmem(m->data, m->len, rel, strlen(rel)) ||
                                  (bn && memmem(m->data, m->len, bn + 1, strlen(bn + 1))))) {
                    reach[j] = 1;
                    changed = 1;
                }
            }
        }
    }
    for (size_t k = 0; k < count && reach; k++) {
        const item_t *m = &items[first + k];
        const char *rel = m->path + bl;
        if (!reach[k] && !strncmp(rel, "references/", 11)) {
            diagf(0, "%s: neither SKILL.md nor anything it points to names this file, so the agent will not find it "
                     "(say in SKILL.md when to read it)",
                  m->path);
        }
        size_t ml = strlen(m->path);
        const char *ph = m->kind == K_DATA && ml > 3 && !strcmp(m->path + ml - 3, ".md") ? placeholder(m->data, m->len) : NULL;
        if (ph) {
            diagf(1, "%s says \"%s\": finish the text before building", m->path, ph);
        }
        if (m->kind != K_DATA || !(ml > 3 && !strcmp(m->path + ml - 3, ".md"))) {
            continue;
        }
        /* markdown links to files of the skill must resolve */
        for (const char *l = m->data; (l = memmem(l, (size_t)(m->data + m->len - l), "](", 2)); l += 2) {
            const char *t = l + 2, *e = t;
            while (e < m->data + m->len && *e != ')' && *e != ' ' && *e != '#' && *e != '\n') {
                e++;
            }
            char target[600], full[700];
            snprintf(target, sizeof(target), "%.*s", (int)(e - t), t);
            if (!target[0] || strstr(target, "://") || !strncmp(target, "mailto:", 7) || target[0] == '/') {
                continue;
            }
            const char *dir_end = strrchr(m->path, '/');
            snprintf(full, sizeof(full), "%.*s/%s", (int)(dir_end - m->path), m->path, target);
            int inside = norm_path(full) && !strncmp(full, base, bl - 1);
            size_t fl2 = strlen(full);
            int found = inside && find_item(full) != NULL;
            for (size_t j = 0; inside && !found && j < count; j++) { /* a link to a folder of the skill */
                found = !strncmp(items[first + j].path, full, fl2) && items[first + j].path[fl2] == '/';
            }
            if (!found) {
                diagf(1, "%s links to %s, which is not in the skill", m->path, target);
            }
        }
    }
    free(reach);
}

/* A skill's runnable files: real work only, and nothing from outside the agent. */
static void check_script(const item_t *it)
{
    char *low = malloc(it->len + 1);
    if (!low) {
        return;
    }
    for (size_t i = 0; i < it->len; i++) {
        low[i] = (char)tolower((unsigned char)it->data[i]);
    }
    low[it->len] = '\0';
    int uses_art = 0; /* outside comments: a credit line naming the source is fine */
    for (const char *l = it->data, *end = it->data + it->len; l < end && !uses_art;) {
        const char *nl = memchr(l, '\n', (size_t)(end - l));
        size_t      n = nl ? (size_t)(nl - l) : (size_t)(end - l);
        size_t      k = strspn(l, " \t");
        if (k < n && l[k] != '#' && strncmp(l + k, "//", 2) != 0) {
            uses_art = memmem(l, n, "artifacts/", 10) != NULL;
        }
        l += n + 1;
    }
    if (uses_art) {
        diagf(1, "%s uses files under artifacts/, which are not part of the .bot (the agent must run in any "
                 "folder): copy the code it needs into the skill itself, keeping the licence notice",
              it->path);
    }
    static const char *const fake[] = {"mock", "placeholder", "dummy", "lorem ipsum", "hard-coded", "hardcoded",
                                       "simulated", "fake ", NULL};
    for (int i = 0; fake[i]; i++) {
        if (strstr(low, fake[i])) {
            diagf(0, "%s mentions \"%s\": scripts must do the real job or fail with a clear error, never return "
                     "mock or placeholder output",
                  it->path, fake[i]);
            break;
        }
    }
    free(low);
}

static void check_contracts(void)
{
    for (size_t i = 0; i < n_items; i++) {
        const item_t *it = &items[i];
        size_t        l = strlen(it->path);
        if (!it->data) {
            continue;
        }
        if (strncmp(it->path, "skills/", 7) == 0 && l > 9 && strcmp(it->path + l - 9, "/SKILL.md") == 0 &&
            !strchr(it->path + 7, '/')[9]) {
            check_skill(it);
        } else if (strncmp(it->path, "agents/", 7) == 0 && l > 9 && strcmp(it->path + l - 9, "/AGENT.md") == 0) {
            const char *c = it->len > 4 && !strncmp(it->data, "---\n", 4) ? memmem(it->data + 4, it->len - 4, "\n---", 4) : NULL;
            if (!c || !memmem(it->data, (size_t)(c - it->data), "\ndescription:", 13)) {
                diagf(1, "%s: start it with a frontmatter that says what this sub-agent is for:\n---\ndescription: "
                         "<one sentence>\n---\nthen its persona (who it is, how it works, what it returns)",
                      it->path);
            }
        } else if (it->kind == K_SCRIPT) {
            check_script(it);
        }
    }
}

/* ---- output ---- */

static int write_all(FILE *f, const void *d, size_t n)
{
    return fwrite(d, 1, n, f) == n;
}

/* The layout before progress.md §12.1: each piece is an error that says where it goes now. */
static void old_layout(const char *dir)
{
    static const char *const moved[][2] = {
        {"agent.md", "agent.md is now AGENT.md: rename it"},
        {"tools/bin", "tools/bin is gone: put each program in the scripts/ folder of the skill that uses it; a Lua "
                      "function in functions/ reaches it with bot.shell(bot.skill_dir(\"<skill>\") .. \"/scripts/...\")"},
        {"tools/doc", "tools/doc is gone: a program's usage belongs in its skill's SKILL.md, or in the --- doc comment "
                      "of the Lua function that runs it"},
    };
    for (size_t i = 0; i < sizeof(moved) / sizeof(moved[0]); i++) {
        char *f = join(dir, moved[i][0]);
        struct stat st;
        /* agent.md: an exact name (a case-insensitive file system shows AGENT.md under both) */
        int there = lstat(f, &st) == 0;
        if (there && i == 0) {
            DIR *d = opendir(dir);
            struct dirent *e;
            there = 0;
            while (d && (e = readdir(d))) {
                there |= !strcmp(e->d_name, "agent.md");
            }
            if (d) {
                closedir(d);
            }
        }
        if (there) {
            err("%s: %s", f, moved[i][1]);
        }
        free(f);
    }
    char *ag = join(dir, "agents");
    DIR *d = opendir(ag);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char rel[300], *f;
        snprintf(rel, sizeof(rel), "agents/%s/agent.md", e->d_name);
        f = join(dir, rel);
        if (e->d_name[0] != '.' && access(f, F_OK) == 0) {
            char *up = join(ag, e->d_name), *n = join(up, "AGENT.md");
            if (access(n, F_OK) != 0) {
                err("%s: sub-agents use AGENT.md too: rename it%s", f, "");
            }
            free(up);
            free(n);
        }
        free(f);
    }
    if (d) {
        closedir(d);
    }
    free(ag);
}

/* skills/<name>/functions/tests/ run at build time and are never embedded. */
static void drop_skill_tests(void)
{
    size_t w = 0;
    for (size_t i = 0; i < n_items; i++) {
        const char *sl = !strncmp(items[i].path, "skills/", 7) ? strchr(items[i].path + 7, '/') : NULL;
        if (sl && !strncmp(sl, "/functions/tests/", 17)) {
            free(items[i].path);
            free(items[i].full);
            continue;
        }
        items[w++] = items[i];
    }
    n_items = w;
}

static int cmd_build(const char *dir, const char *botcore, const char *out)
{
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        err("%s is not a directory", dir, NULL);
        return 1;
    }
    size_t blen = 0;
    char  *core = slurp(botcore, &blen);
    if (core) { /* the runtime's version (main.c BC_VERSION_TAG) */
        const char *tag = memmem(core, blen, "BOTCORE_VERSION=", 16);
        if (tag) {
            snprintf(g_core_version, sizeof(g_core_version), "%.31s", tag + 16);
        }
    }
    if (!core) {
        return 1;
    }
    int x86, dyn;
    if (!elf_info((unsigned char *)core, blen, &x86, &dyn) || !x86) {
        err("%s is not an x86-64 ELF executable", botcore, NULL);
        return 1;
    }
    if (blen >= FOOTER_SIZE && memcmp(core + blen - 8, FOOTER_MAGIC, 8) == 0) {
        /* A finished .bot (e.g. the running agent itself) is accepted as the base:
         * the runtime is everything in front of its pack. */
        const unsigned char *ft = (const unsigned char *)core + blen - FOOTER_SIZE;
        uint64_t             boff = get64(ft), plen = get64(ft + 8);
        if (boff % BLOB_ALIGN != 0 || boff < 64 || boff + plen + FOOTER_SIZE != blen) {
            err("%s has a damaged agent pack footer", botcore, NULL);
            return 1;
        }
        blen = (size_t)boff;
    }
    if (dyn) {
        warn("%s is dynamically linked; the .bot will need the same libc on the target (use the static build)",
             botcore);
    }

    load_config(dir);
    old_layout(dir);
    add_root_file(dir, "AGENT.md");
    add_root_file(dir, "FLOW.lua"); /* optional orchestration (progress.md §12.3) */
    add_root_file(dir, "config.json");
    add_root_file(dir, "secrets.vault"); /* opt-in: the agent's keys, encrypted with a password (botter seal) */
    add_tree(dir, "skills", K_DATA, 0);
    add_tree(dir, "agents", K_DATA, 0);
    add_functions(dir);
    g_dir = dir;
    drop_skill_tests();
    select_from_config();
    if (errors) {
        return 1;
    }
    qsort(items, n_items, sizeof(*items), cmp_item);
    load_and_classify();
    check_contracts();
    if (errors) {
        return 1;
    }
    check_requires();
    if (!has_item("AGENT.md")) {
        warn("no AGENT.md (%s: default system prompt will be used)", "agent");
    }
    char *mf = join(dir, "manifest.md");
    if (access(mf, F_OK) == 0) {
        warn("%s is no longer used (botcore lists skills and tools itself); it is not embedded, delete it", mf);
    }
    free(mf);
    static const char *const flows[] = {"FLOW.fwl", "FLOW.md", "SQNC.md", NULL};
    for (int i = 0; flows[i]; i++) { /* the flow languages are gone: the model drives, functions do exact work */
        char *old = join(dir, flows[i]);
        if (access(old, F_OK) == 0) {
            err("%s is no longer used: write how the agent works in AGENT.md (or a skill it loads), and exact, "
                "repeatable steps as Lua in functions/ (see the Botter skill's references/lua-api.md)%s", old, "");
        }
        free(old);
    }
    if (errors) {
        return 1;
    }
    check_functions();
    if (errors) { /* functions that do not load, or whose tests fail, are not built */
        fprintf(stderr, "packer: functions have %d error(s); nothing was built\n", errors);
        return 1;
    }

    if (!out) { /* dry run: everything validated, write nothing */
        size_t nx = 0;
        for (size_t i = 0; i < n_items; i++) {
            nx += items[i].kind != K_DATA;
        }
        fprintf(stderr, "packer: check passed (runtime botcore %s): %zu files (%zu runnable), %d warning(s)\n",
                g_core_version[0] ? g_core_version : "?", n_items, nx, warnings);
        return 0;
    }

    /* layout */
    uint64_t pos = BLOB_HEADER_SIZE + (uint64_t)n_items * BLOB_ENTRY_SIZE;
    uint64_t *poff = calloc(n_items + 1, sizeof(*poff)), *doff = calloc(n_items + 1, sizeof(*doff));
    if (!poff || !doff) {
        die("out of memory");
    }
    size_t saved = 0;
    for (size_t i = 0; i < n_items; i++) {
        poff[i] = pos;
        pos += strlen(items[i].path) + 1;
        /* identical content (e.g. one multi-call tool binary under several names) is stored once */
        size_t j = 0;
        while (j < i && !(items[j].len == items[i].len && memcmp(items[j].data, items[i].data, items[i].len) == 0)) {
            j++;
        }
        if (j < i && items[i].len > 0) {
            doff[i] = doff[j];
            saved += items[i].len;
            continue;
        }
        doff[i] = pos;
        pos += items[i].len + 1;
    }
    if (pos > 0xFFFFFFFFull * 4) {
        die("agent pack too large");
    }
    unsigned char *blob = calloc(1, (size_t)pos);
    if (!blob) {
        die("out of memory");
    }
    memcpy(blob, BLOB_MAGIC, 8);
    put32(blob + 8, (uint32_t)n_items);
    for (size_t i = 0; i < n_items; i++) {
        unsigned char *e = blob + BLOB_HEADER_SIZE + i * BLOB_ENTRY_SIZE;
        size_t         pl = strlen(items[i].path);
        put32(e, (uint32_t)poff[i]);
        put32(e + 4, (uint32_t)pl);
        put64(e + 8, doff[i]);
        put64(e + 16, items[i].len);
        put32(e + 24, items[i].kind);
        memcpy(blob + poff[i], items[i].path, pl);
        memcpy(blob + doff[i], items[i].data, items[i].len);
    }
    uint64_t base = ((uint64_t)blen + BLOB_ALIGN - 1) / BLOB_ALIGN * BLOB_ALIGN;
    unsigned char ft[FOOTER_SIZE] = {0};
    put64(ft, base);
    put64(ft + 8, pos);
    put32(ft + 16, crc32_ieee(blob, (size_t)pos));
    memcpy(ft + 24, FOOTER_MAGIC, 8);

    /* pack reference: exactly one "BOTPKREF" in the runtime */
    unsigned char *ref = NULL;
    int nref = 0;
    for (size_t i = 0; i + 32 <= blen; i++) {
        if (core[i] == 'B' && memcmp(core + i, "BOTPKREF", 8) == 0) {
            ref = (unsigned char *)core + i;
            nref++;
        }
    }
    if (nref == 1) {
        put64(ref + 8, base);
        put64(ref + 16, base + pos + FOOTER_SIZE);
        put64(ref + 24, 0);
    } else {
        warn("%s has no unique pack reference (older botcore?): a truncated .bot will not be detected", botcore);
    }

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", out, (int)getpid());
    FILE *o = fopen(tmp, "wb");
    if (!o) {
        err("cannot write %s: %s", tmp, strerror(errno));
        return 1;
    }
    static const unsigned char zeros[BLOB_ALIGN];
    int ok = write_all(o, core, blen) && write_all(o, zeros, (size_t)(base - blen)) && write_all(o, blob, (size_t)pos) &&
             write_all(o, ft, sizeof(ft));
    ok = (fclose(o) == 0) && ok;
    if (!ok || chmod(tmp, 0755) != 0 || rename(tmp, out) != 0) {
        remove(tmp);
        err("failed to write %s", out, NULL);
        return 1;
    }
    size_t nexec = 0;
    for (size_t i = 0; i < n_items; i++) {
        nexec += items[i].kind != K_DATA;
    }
    fprintf(stderr, "packer: wrote %s (runtime botcore %s): %zu files (%zu runnable), pack %llu bytes, total %llu bytes", out,
            g_core_version[0] ? g_core_version : "?", n_items,
            nexec, (unsigned long long)pos, (unsigned long long)(base + pos + FOOTER_SIZE));
    if (saved) {
        fprintf(stderr, " (%zu duplicate bytes stored once)", saved);
    }
    fputc('\n', stderr);
    return 0;
}

static int cmd_list(const char *file)
{
    size_t         n = 0;
    unsigned char *d = (unsigned char *)slurp(file, &n);
    if (!d) {
        return 1;
    }
    if (n < FOOTER_SIZE || memcmp(d + n - 8, FOOTER_MAGIC, 8) != 0) {
        printf("%s: no agent pack\n", file);
        return 1;
    }
    const unsigned char *ft = d + n - FOOTER_SIZE;
    uint64_t             boff = get64(ft), blen = get64(ft + 8);
    if (boff + blen + FOOTER_SIZE != n || blen < BLOB_HEADER_SIZE) {
        printf("%s: damaged footer\n", file);
        return 1;
    }
    if (crc32_ieee(d + boff, (size_t)blen) != get32(ft + 16)) {
        printf("%s: checksum mismatch\n", file);
        return 1;
    }
    const unsigned char *b = d + boff;
    uint32_t             cnt = get32(b + 8);
    static const char   *kn[] = {"data", "elf", "script"};
    for (uint32_t i = 0; i < cnt; i++) { /* config.json first */
        const unsigned char *e = b + BLOB_HEADER_SIZE + (size_t)i * BLOB_ENTRY_SIZE;
        if (strcmp((const char *)b + get32(e), "config.json") != 0) {
            continue;
        }
        cJSON *j = cJSON_ParseWithLength((const char *)b + get64(e + 8), (size_t)get64(e + 16));
        for (const cJSON *f = cJSON_IsObject(j) ? j->child : NULL; f; f = f->next) {
            char *t = cJSON_IsString(f) ? strdup(f->valuestring) : cJSON_PrintUnformatted(f);
            if (t) {
                printf("%-13s %s\n", f->string, t);
            }
            free(t);
        }
        cJSON_Delete(j);
    }
    printf("%s: %u entries, pack at offset %llu (%llu bytes)\n", file, cnt, (unsigned long long)boff,
           (unsigned long long)blen);
    for (uint32_t i = 0; i < cnt; i++) {
        const unsigned char *e = b + BLOB_HEADER_SIZE + (size_t)i * BLOB_ENTRY_SIZE;
        uint32_t             k = get32(e + 24);
        printf("  %-7s %9llu  %s\n", k < 3 ? kn[k] : "?", (unsigned long long)get64(e + 16), (const char *)b + get32(e));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "build") == 0) {
        return cmd_build(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && strcmp(argv[1], "check") == 0) {
        return cmd_build(argv[2], argv[3], NULL);
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return cmd_list(argv[2]);
    }
    fprintf(stderr,
            "usage:\n  %s build <agent-dir> <botcore|file.bot> <out.bot>\n  %s check <agent-dir> <botcore|file.bot>\n"
            "  %s list <file.bot>\n",
            argv[0], argv[0], argv[0]);
    return 2;
}
