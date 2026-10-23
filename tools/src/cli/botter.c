/*
 * botter: the CLI that checks, runs, scores and builds Botter agents (docs/cli-design.md).
 *
 *   botter new <name> [--kind chat|batch|tool]   start a project from a template
 *   botter check [DIR]                           validate the project and run its tests
 *   botter run [agent args...]                   run the project from source (nothing written)
 *   botter eval <examples.csv> [options]         score the agent on solved examples, per column
 *   botter build [-o FILE] [DIR]                 check, test, then write one executable: ./<name>
 *   botter inspect FILE                          what a built agent holds
 *   botter skill [DIR]                           install the Botter skill (default .claude/skills)
 *   botter version | help
 *
 * One static executable. It carries the botcore runtime (the front of every agent) and the Botter
 * skill, both embedded at build time, plus the packer (../packer/packer.c), which built agents never
 * carry: only this CLI can make agents [D].
 */
#define _GNU_SOURCE
#define main pack_main
#include "../packer/packer.c"
#undef main

#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifndef BOTTER_VERSION
#define BOTTER_VERSION "dev"
#endif

/* ---------- embedded payloads (Makefile: -DBOTTER_RUNTIME_FILE, -DBOTTER_SKILL_FILE) ---------- */

__asm__(".section .rodata\n"
        ".balign 64\n.global bt_runtime\nbt_runtime:\n.incbin \"" BOTTER_RUNTIME_FILE "\"\n"
        ".global bt_runtime_end\nbt_runtime_end:\n"
        ".balign 64\n.global bt_skill\nbt_skill:\n.incbin \"" BOTTER_SKILL_FILE "\"\n"
        ".global bt_skill_end\nbt_skill_end:\n"
        ".previous\n");
extern const unsigned char bt_runtime[], bt_runtime_end[], bt_skill[], bt_skill_end[];

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("botter: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static int write_fd_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

/* The runtime as a file path the packer can read: an in-memory file. */
static const char *runtime_path(void)
{
    static char path[64];
    if (path[0]) {
        return path;
    }
    int fd = memfd_create("botcore-runtime", 0);
    if (fd < 0 || write_fd_all(fd, bt_runtime, (size_t)(bt_runtime_end - bt_runtime)) != 0) {
        fail("cannot prepare the runtime: %s", strerror(errno));
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    return path;
}

/* ---------- the embedded skill: "path\nsize\n" + bytes, repeated ---------- */

typedef struct {
    char                *path; /* "botter/SKILL.md" */
    const unsigned char *data;
    size_t               len;
} sfile_t;

static sfile_t *skill_files(size_t *n)
{
    static sfile_t *v;
    static size_t   nv;
    if (v) {
        *n = nv;
        return v;
    }
    const unsigned char *p = bt_skill, *end = bt_skill_end;
    size_t cap = 64;
    v = malloc(cap * sizeof(*v));
    while (v && p < end) {
        const unsigned char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            break;
        }
        char *path = strndup((const char *)p, (size_t)(nl - p));
        char *e;
        size_t len = strtoul((const char *)nl + 1, &e, 10);
        if (*e != '\n' || (const unsigned char *)e + 1 + len > end) {
            fail("the embedded skill is damaged");
        }
        if (nv == cap) {
            v = realloc(v, (cap *= 2) * sizeof(*v));
        }
        v[nv++] = (sfile_t){path, (const unsigned char *)e + 1, len};
        p = (const unsigned char *)e + 1 + len;
    }
    *n = nv;
    return v;
}

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *s = '/';
        }
    }
    return mkdir(tmp, mode) != 0 && errno != EEXIST ? -1 : 0;
}

static void write_file(const char *path, const void *data, size_t len, int exec)
{
    char *d = xstrdup(path);
    if (mkdirs(dirname(d), 0755) != 0) {
        fail("cannot create the folder for %s: %s", path, strerror(errno));
    }
    free(d);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, exec ? 0755 : 0644);
    if (fd < 0 || write_fd_all(fd, data, len) != 0 || close(fd) != 0) {
        fail("cannot write %s: %s", path, strerror(errno));
    }
}

/* ---------- botter skill ---------- */

static int cmd_skill(int argc, char **argv)
{
    const char *dir = argc > 0 ? argv[0] : ".claude/skills";
    size_t n;
    sfile_t *f = skill_files(&n);
    for (size_t i = 0; i < n; i++) {
        char *p = join(dir, f[i].path);
        write_file(p, f[i].data, f[i].len, strstr(f[i].path, "/scripts/") != NULL);
        free(p);
    }
    printf("installed the Botter skill (botter %s, %zu files) in %s/botter\n", BOTTER_VERSION, n, dir);
    printf("a coding agent that reads skills from %s will find it; others: point them at %s/botter/SKILL.md\n", dir, dir);
    return 0;
}

/* ---------- botter new ---------- */

static char *replace_all(const unsigned char *data, size_t len, const char *from, const char *to, size_t *out_len)
{
    size_t fl = strlen(from), tl = strlen(to), cap = len + 1, n = 0;
    char *o = malloc(cap);
    for (size_t i = 0; o && i < len;) {
        int hit = i + fl <= len && memcmp(data + i, from, fl) == 0;
        size_t add = hit ? tl : 1;
        if (n + add + 1 > cap) {
            o = realloc(o, cap = cap * 2 + add);
        }
        if (!o) {
            break;
        }
        if (hit) {
            memcpy(o + n, to, tl);
            i += fl;
        } else {
            o[n] = (char)data[i++];
        }
        n += add;
    }
    if (!o) {
        fail("out of memory");
    }
    o[n] = '\0';
    *out_len = n;
    return o;
}

static int cmd_new(int argc, char **argv)
{
    const char *name = NULL, *kind = "chat";
    int here = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--here")) {
            here = 1;
        } else if (!strcmp(argv[i], "--kind") && i + 1 < argc) {
            kind = argv[++i];
        } else if (argv[i][0] == '-') {
            fail("unknown option %s (botter new <name> [--kind chat|batch|tool] [--here])", argv[i]);
        } else {
            name = argv[i];
        }
    }
    if (!name) {
        fail("botter new <name> [--kind chat|batch|tool] [--here]");
    }
    if (!name_ok(name)) {
        fail("'%s' is not a valid agent name: lowercase letters, digits, - _ . (it becomes the file name)", name);
    }
    struct stat st;
    if (!here && stat(name, &st) == 0) {
        fail("%s already exists (to start the project in this folder: botter new %s --kind %s --here)", name, name, kind);
    }
    size_t kept = 0;
    char prefix[100];
    snprintf(prefix, sizeof(prefix), "botter/assets/templates/%s/", kind);
    size_t n, made = 0, pl = strlen(prefix);
    sfile_t *f = skill_files(&n);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(f[i].path, prefix, pl) != 0) {
            continue;
        }
        size_t len;
        char *body = replace_all(f[i].data, f[i].len, "{{name}}", name, &len);
        char *p = here ? xstrdup(f[i].path + pl) : join(name, f[i].path + pl);
        if (here && stat(p, &st) == 0) { /* never overwrite a file already in the folder */
            kept++;
        } else {
            write_file(p, body, len, 0);
        }
        free(p);
        free(body);
        made++;
    }
    if (!made) {
        rmdir(name);
        fail("no template '%s' (chat, batch or tool)", kind);
    }
    if (here) {
        printf("started %s here (%s agent): %zu file(s) written, %zu already there and kept\n", name, kind, made - kept, kept);
        printf("next: write the agent (the Botter skill explains the format: botter skill), then botter check\n");
        return 0;
    }
    printf("created %s/ (%s agent, %zu files)\n", name, kind, made);
    printf("next: cd %s, write the agent (the Botter skill explains the format: botter skill), then botter check\n", name);
    return 0;
}

/* ---------- building ---------- */

/* The project's name: config.json "name", else the folder's name. */
static const char *project_name(const char *dir)
{
    static char nm[80];
    const cJSON *v = g_cfg ? cJSON_GetObjectItemCaseSensitive(g_cfg, "name") : NULL;
    if (cJSON_IsString(v) && name_ok(v->valuestring)) {
        snprintf(nm, sizeof(nm), "%s", v->valuestring);
        return nm;
    }
    char real[PATH_MAX];
    const char *b = realpath(dir, real) ? strrchr(real, '/') + 1 : "agent";
    snprintf(nm, sizeof(nm), "%s", *b ? b : "agent");
    return nm;
}

/* Build quietly into memory: the packer's notes are shown only when the build fails. Returns an fd. */
static int build_to_memfd(const char *dir)
{
    char tdir[] = "/tmp/botter-run.XXXXXX";
    const char *rt = runtime_path();
    if (!mkdtemp(tdir)) {
        fail("cannot make a temporary folder: %s", strerror(errno));
    }
    char out[PATH_MAX], log[PATH_MAX];
    snprintf(out, sizeof(out), "%s/agent", tdir);
    snprintf(log, sizeof(log), "%s/build.log", tdir);
    int saved = dup(2), lf = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    fflush(stderr);
    dup2(lf, 2);
    int rc = cmd_build(dir, rt, out);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(lf);
    if (rc != 0) {
        size_t n = 0;
        char *t = slurp(log, &n);
        fprintf(stderr, "%s", t ? t : "");
        unlink(log);
        unlink(out);
        rmdir(tdir);
        fail("the project does not build (botter check shows the same)");
    }
    size_t n = 0;
    char *bin = slurp(out, &n);
    int mem = memfd_create("botter-agent", MFD_CLOEXEC);
    if (!bin || mem < 0 || write_fd_all(mem, bin, n) != 0) {
        fail("cannot load the built agent: %s", strerror(errno));
    }
    free(bin);
    unlink(log);
    unlink(out);
    rmdir(tdir);
    return mem;
}

static int cmd_check(int argc, char **argv)
{
    const char *dir = argc > 0 ? argv[0] : ".";
    int rc = cmd_build(dir, runtime_path(), NULL);
    if (rc == 0) {
        printf("check passed: %s, %d warning(s)\n", project_name(dir), warnings);
    }
    return rc;
}

static int cmd_buildc(int argc, char **argv)
{
    const char *dir = ".", *outarg = NULL;
    for (int i = 0; i < argc; i++) {
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) {
            outarg = argv[++i];
        } else if (argv[i][0] == '-') {
            fail("unknown option %s (botter build [-o FILE] [DIR])", argv[i]);
        } else {
            dir = argv[i];
        }
    }
    /* config.json first, for the default output name */
    char *cfgp = join(dir, "config.json");
    size_t cl = 0;
    char *cfgt = access(cfgp, F_OK) == 0 ? slurp(cfgp, &cl) : NULL;
    cJSON *cfg = cfgt ? cJSON_Parse(cfgt) : NULL;
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(cfg, "name");
    char out[PATH_MAX];
    if (outarg) {
        snprintf(out, sizeof(out), "%s", outarg);
    } else {
        char real[PATH_MAX];
        const char *b = cJSON_IsString(nm) && name_ok(nm->valuestring) ? nm->valuestring
                        : realpath(dir, real)                          ? strrchr(real, '/') + 1
                                                                       : "agent";
        snprintf(out, sizeof(out), "%s/%s", dir, *b ? b : "agent");
    }
    cJSON_Delete(cfg);
    free(cfgt);
    free(cfgp);
    struct stat st;
    if (stat(out, &st) == 0) { /* replace an earlier build, never anything else */
        unsigned char m[4] = {0};
        int fd = S_ISREG(st.st_mode) ? open(out, O_RDONLY | O_CLOEXEC) : -1;
        ssize_t r = fd >= 0 ? read(fd, m, 4) : -1;
        if (fd >= 0) {
            close(fd);
        }
        if (r < 4 || memcmp(m, "\x7f" "ELF", 4) != 0) {
            fail("refusing to overwrite %s: it exists and is not an agent built before", out);
        }
    }
    int rc = cmd_build(dir, runtime_path(), out);
    if (rc != 0) {
        return rc;
    }
    const cJSON *req = g_cfg ? cJSON_GetObjectItemCaseSensitive(g_cfg, "requires") : NULL;
    char needs[400] = "";
    for (const cJSON *r = cJSON_IsArray(req) ? req->child : NULL; r; r = r->next) {
        if (cJSON_IsString(r)) {
            size_t l = strlen(needs);
            snprintf(needs + l, sizeof(needs) - l, "%s%s", l ? ", " : "", r->valuestring);
        }
    }
    stat(out, &st);
    printf("built %s (%lld KB)\n", out, (long long)(st.st_size + 1023) / 1024);
    if (needs[0]) {
        printf("runs anywhere: no, it needs %s on the machine\n", needs);
    } else {
        printf("runs anywhere: yes (x86-64 Linux, nothing else needed)\n");
    }
    return 0;
}

static int cmd_run(int argc, char **argv)
{
    int mem = build_to_memfd(".");
    char **av = calloc((size_t)argc + 2, sizeof(*av));
    av[0] = (char *)project_name(".");
    for (int i = 0; i < argc; i++) {
        av[i + 1] = argv[i];
    }
    extern char **environ;
    fexecve(mem, av, environ);
    fail("cannot start the agent: %s", strerror(errno));
}

/* ---------- botter eval ---------- */

typedef struct {
    char **f;
    int    n;
} row_t;

/* RFC 4180-ish: quotes, doubled quotes, CRLF. */
static row_t *csv_parse(const char *t, int *nrows)
{
    row_t *rows = NULL;
    int nr = 0, cr = 0;
    const char *p = t;
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) { /* a UTF-8 byte order mark is not part of the first column's name */
        p += 3;
    }
    while (*p) {
        row_t r = {NULL, 0};
        int cf = 0;
        for (;;) {
            size_t cap = 64, n = 0;
            char *v = malloc(cap);
            if (*p == '"') {
                p++;
                for (; *p; p++) {
                    if (*p == '"' && p[1] == '"') {
                        p++;
                    } else if (*p == '"') {
                        p++;
                        break;
                    }
                    if (n + 2 > cap) {
                        v = realloc(v, cap *= 2);
                    }
                    v[n++] = *p;
                }
            }
            for (; *p && *p != ',' && *p != '\n' && *p != '\r'; p++) {
                if (n + 2 > cap) {
                    v = realloc(v, cap *= 2);
                }
                v[n++] = *p;
            }
            v[n] = '\0';
            if (r.n == cf) {
                r.f = realloc(r.f, (size_t)(cf = cf ? cf * 2 : 16) * sizeof(char *));
            }
            r.f[r.n++] = v;
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == '\r') {
                p++;
            }
            if (*p == '\n') {
                p++;
            }
            break;
        }
        if (r.n == 1 && !r.f[0][0]) { /* blank line */
            free(r.f[0]);
            free(r.f);
            continue;
        }
        if (nr == cr) {
            rows = realloc(rows, (size_t)(cr = cr ? cr * 2 : 64) * sizeof(row_t));
        }
        rows[nr++] = r;
    }
    *nrows = nr;
    return rows;
}

static int col_index(const row_t *h, const char *name)
{
    for (int i = 0; i < h->n; i++) {
        if (!strcmp(h->f[i], name)) {
            return i;
        }
    }
    return -1;
}

static char *strip_dup(const char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    return strndup(s, n);
}

static int is_number(const char *s, double *v)
{
    char *e;
    if (!*s) {
        return 0;
    }
    *v = strtod(s, &e);
    return *e == '\0' && isfinite(*v);
}

/* One token: numbers within tol (relative, at least a cent), else the same text. */
static int same_token(const char *g, const char *w, double tol)
{
    double x, y;
    if (is_number(g, &x) && is_number(w, &y)) {
        return fabs(x - y) <= fmax(0.0101, tol * fabs(y));
    }
    return strcasecmp(g, w) == 0; /* "Replied" = "replied" */
}

/* The parts of a list value (split at | or ;), each trimmed, sorted: a list matches in any order. */
static int split_items(char *s, char **items, int cap)
{
    int n = 0;
    for (char *sv = NULL, *t = strtok_r(s, "|;", &sv); t && n < cap; t = strtok_r(NULL, "|;", &sv)) {
        items[n++] = strip_dup(t);
    }
    for (int i = 1; i < n; i++) { /* insertion sort, case-insensitive */
        for (int j = i; j > 0 && strcasecmp(items[j - 1], items[j]) > 0; j--) {
            char *x = items[j];
            items[j] = items[j - 1];
            items[j - 1] = x;
        }
    }
    return n;
}

/* One list item: the same parts (split at :), numbers within tol. */
static int same_item(const char *got, const char *want, double tol)
{
    char *g = xstrdup(got), *w = xstrdup(want), *gs, *ws;
    char *ga = strtok_r(g, ":", &gs), *wa = strtok_r(w, ":", &ws);
    int ok = 1;
    while (ok && (ga || wa)) {
        char *gt = ga ? strip_dup(ga) : NULL, *wt = wa ? strip_dup(wa) : NULL;
        ok = gt && wt && same_token(gt, wt, tol);
        free(gt);
        free(wt);
        ga = strtok_r(NULL, ":", &gs);
        wa = strtok_r(NULL, ":", &ws);
    }
    free(g);
    free(w);
    return ok;
}

/* Same answer: equal text (any case), or the same list items (| or ;, any order), each item's parts (:)
   equal with numbers within tol. */
static int same_answer(const char *got, const char *want, double tol)
{
    char *g = strip_dup(got), *w = strip_dup(want);
    int ok = strcasecmp(g, w) == 0;
    if (!ok) {
        char *gi[256], *wi[256];
        int gn = split_items(g, gi, 256), wn = split_items(w, wi, 256);
        ok = gn == wn;
        for (int i = 0; i < gn && ok; i++) {
            ok = same_item(gi[i], wi[i], tol);
        }
        for (int i = 0; i < gn; i++) {
            free(gi[i]);
        }
        for (int i = 0; i < wn; i++) {
            free(wi[i]);
        }
    }
    free(g);
    free(w);
    return ok;
}

/* A column name as a key: lowercase, every run of other characters as one _ ("Product Area" -> product_area). */
static char *key_form(const char *s)
{
    char *k = malloc(strlen(s) + 1), *o = k;
    for (; *s; s++) {
        if (isalnum((unsigned char)*s)) {
            *o++ = (char)tolower((unsigned char)*s);
        } else if (o > k && o[-1] != '_') {
            *o++ = '_';
        }
    }
    while (o > k && o[-1] == '_') {
        o--;
    }
    *o = '\0';
    return k;
}

/* The result's value for an answer column: the exact name, else the same name in key form. */
static const cJSON *result_field(const cJSON *result, const char *col)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(result, col);
    if (v || !cJSON_IsObject(result)) {
        return v;
    }
    char *want = key_form(col);
    for (const cJSON *c = result->child; c && !v; c = c->next) {
        char *k = c->string ? key_form(c->string) : NULL;
        if (k && !strcmp(k, want)) {
            v = c;
        }
        free(k);
    }
    free(want);
    return v;
}

/* A result field as text: strings as they are, numbers without trailing zeros, null as "". */
static char *field_text(const cJSON *v)
{
    char b[64];
    if (!v || cJSON_IsNull(v)) {
        return xstrdup("");
    }
    if (cJSON_IsString(v)) {
        return xstrdup(v->valuestring);
    }
    if (cJSON_IsNumber(v)) {
        snprintf(b, sizeof(b), "%.10g", v->valuedouble);
        return xstrdup(b);
    }
    if (cJSON_IsBool(v)) {
        return xstrdup(cJSON_IsTrue(v) ? "true" : "false");
    }
    char *s = cJSON_PrintUnformatted(v);
    return s ? s : xstrdup("");
}

/* Input columns from another CSV in the same folder whose header is a strict subset (e.g. requests.csv). */
static row_t *guess_inputs(const char *examples, const row_t *eh, char *found, size_t fcap)
{
    char *d = xstrdup(examples);
    char *dir = dirname(d);
    DIR *dd = opendir(dir);
    struct dirent *e;
    row_t *best = NULL;
    while (dd && (e = readdir(dd))) {
        size_t l = strlen(e->d_name);
        char *full = join(dir, e->d_name), re[PATH_MAX], rx[PATH_MAX];
        if (l < 5 || strcmp(e->d_name + l - 4, ".csv") || !realpath(full, re) || !realpath(examples, rx) || !strcmp(re, rx)) {
            free(full);
            continue;
        }
        FILE *f = fopen(full, "r");
        char line[8192];
        if (f && fgets(line, sizeof(line), f)) {
            int nr;
            row_t *h = csv_parse(line, &nr);
            int sub = nr == 1 && h[0].n < eh->n && h[0].n > 0;
            for (int i = 0; sub && i < h[0].n; i++) {
                sub = col_index(eh, h[0].f[i]) >= 0;
            }
            if (sub && (!best || h[0].n > best->n)) {
                best = &h[0];
                snprintf(found, fcap, "%s", full);
            }
        }
        if (f) {
            fclose(f);
        }
        free(full);
    }
    if (dd) {
        closedir(dd);
    }
    free(d);
    return best;
}

/* Does the project define a global Lua function `name` (a line "function name(" in FLOW.lua or functions/)? */
static int defines_function(const char *name)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "function %s(", name);
    const char *files[64];
    int nf = 0;
    files[nf++] = xstrdup("FLOW.lua");
    DIR *d = opendir("functions");
    struct dirent *e;
    while (d && (e = readdir(d)) && nf < 63) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".lua")) {
            files[nf++] = join("functions", e->d_name);
        }
    }
    if (d) {
        closedir(d);
    }
    int found = 0;
    for (int i = 0; i < nf; i++) {
        size_t n = 0;
        char *s = slurp(files[i], &n);
        for (char *q = s; q && !found && (q = strstr(q, pat)); q++) {
            found = q == s || q[-1] == '\n';
        }
        free(s);
        free((char *)files[i]);
    }
    return found;
}

static int cmd_eval(int argc, char **argv)
{
    const char *examples = NULL, *answers = NULL, *inputs = NULL, *idcol = NULL, *one = NULL, *fn = "solve_case";
    double tol = 0.02, min = -1;
    int json = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--answers") && v) {
            answers = v, i++;
        } else if (!strcmp(a, "--inputs") && v) {
            inputs = v, i++;
        } else if (!strcmp(a, "--id") && v) {
            idcol = v, i++;
        } else if (!strcmp(a, "--case") && v) {
            one = v, i++;
        } else if (!strcmp(a, "--fn") && v) {
            fn = v, i++;
        } else if (!strcmp(a, "--tol") && v) {
            tol = atof(v) / (strchr(v, '%') ? 100.0 : 1.0), i++;
        } else if (!strcmp(a, "--min") && v) {
            min = atof(v), i++;
        } else if (!strcmp(a, "--json")) {
            json = 1;
        } else if (a[0] == '-') {
            fail("unknown option %s (see botter help)", a);
        } else {
            examples = a;
        }
    }
    if (!examples) {
        fail("botter eval <examples.csv> [--answers a,b] [--inputs FILE] [--id COL] [--tol 2%%] [--case ID] [--fn NAME] [--min 0.8] [--json]");
    }
    size_t tl = 0;
    char *text = slurp(examples, &tl);
    if (!text) {
        fail("cannot read %s", examples);
    }
    int nr;
    row_t *rows = csv_parse(text, &nr);
    if (nr < 2) {
        fail("%s has no example rows", examples);
    }
    row_t *h = &rows[0];
    int *is_answer = calloc((size_t)h->n, sizeof(int)), *is_text = calloc((size_t)h->n, sizeof(int));
    char from[PATH_MAX] = "";
    if (answers) {
        char *a = xstrdup(answers), *s, *t;
        for (t = strtok_r(a, ",", &s); t; t = strtok_r(NULL, ",", &s)) {
            int c = col_index(h, t);
            if (c < 0) {
                fail("--answers: %s has no column %s", examples, t);
            }
            is_answer[c] = 1;
        }
    } else {
        row_t *ih = NULL;
        if (inputs) {
            size_t il = 0;
            char *it = slurp(inputs, &il);
            int inr;
            row_t *irows = it ? csv_parse(it, &inr) : NULL;
            ih = irows && inr ? &irows[0] : NULL;
            snprintf(from, sizeof(from), "%s", inputs);
        } else {
            ih = guess_inputs(examples, h, from, sizeof(from));
        }
        if (!ih) {
            fail("which columns are the answers? Give --answers col1,col2 or --inputs FILE (a CSV with the input "
                 "columns only)");
        }
        for (int c = 0; c < h->n; c++) {
            is_answer[c] = col_index(ih, h->f[c]) < 0;
        }
    }
    int id = idcol ? col_index(h, idcol) : 0;
    if (id < 0) {
        fail("--id: no column %s", idcol);
    }
    /* Without --id, the first column names the cases when it is a short unique id; else the row number does
       (row 1 = the first example). */
    char **ids = calloc((size_t)nr, sizeof(char *));
    int by_row = 0;
    for (int r = 1; !idcol && r < nr && !by_row; r++) {
        const char *v = id < rows[r].n ? rows[r].f[id] : "";
        by_row = !*v || strlen(v) > 40 || strchr(v, ' ') || strchr(v, '\n');
        for (int q = 1; q < r && !by_row; q++) {
            by_row = q < nr && id < rows[q].n && !strcmp(rows[q].f[id], v);
        }
    }
    for (int r = 1; r < nr; r++) {
        char b[32];
        snprintf(b, sizeof(b), "row %d", r);
        ids[r] = by_row ? xstrdup(b) : xstrdup(id < rows[r].n ? rows[r].f[id] : "");
    }
    /* free-text answers (long, with spaces: explanations) are shown, not scored */
    int nans = 0;
    for (int c = 0; c < h->n; c++) {
        if (!is_answer[c]) {
            continue;
        }
        size_t total = 0, spaces = 0;
        for (int r = 1; r < nr; r++) {
            const char *v = c < rows[r].n ? rows[r].f[c] : "";
            total += strlen(v);
            for (; *v; v++) {
                spaces += *v == ' ';
            }
        }
        is_text[c] = total / (size_t)(nr - 1) > 60 && spaces > (size_t)(nr - 1) * 5;
        nans += !is_text[c];
    }
    if (!nans) {
        fail("no answer columns to score in %s", examples);
    }

    /* the cases: each example row without its answers, as a JSON object of strings */
    char tin[] = "/tmp/botter-eval.XXXXXX";
    int infd = mkstemp(tin);
    cJSON *all_cases = cJSON_CreateArray();
    int ncase = 0, *case_row = calloc((size_t)nr, sizeof(int));
    for (int r = 1; r < nr; r++) {
        if (one && strcmp(ids[r], one) != 0 && !(by_row && !strcmp(ids[r] + 4, one))) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        for (int c = 0; c < h->n; c++) {
            if (!is_answer[c]) {
                cJSON_AddStringToObject(o, h->f[c], c < rows[r].n ? rows[r].f[c] : "");
            }
        }
        char *line = cJSON_PrintUnformatted(o);
        write_fd_all(infd, line, strlen(line));
        write_fd_all(infd, "\n", 1);
        free(line);
        cJSON_AddItemToArray(all_cases, o);
        case_row[ncase++] = r;
    }
    if (!ncase) {
        fail("no example %s", one);
    }
    lseek(infd, 0, SEEK_SET);
    unlink(tin);

    int mem = build_to_memfd(".");
    char terr[] = "/tmp/botter-eval-err.XXXXXX";
    int errfd = one ? -1 : mkstemp(terr);
    if (errfd >= 0) {
        unlink(terr);
    }
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) {
        fail("pipe: %s", strerror(errno));
    }
    if (!json) {
        printf("eval: %d example(s) from %s through %s()%s%s\n", ncase, examples, fn, from[0] ? "; inputs = the columns of " : "",
               from);
        fflush(stdout);
    }
    if (defines_function("prepare")) {
        /* prepare(cases): every case at once, so the agent can read them in batches (into its cache) before
           solve_case sees them one by one */
        char tp[] = "/tmp/botter-prep.XXXXXX";
        int pin = mkstemp(tp), pout[2];
        unlink(tp);
        char *arr = cJSON_PrintUnformatted(all_cases);
        write_fd_all(pin, arr, strlen(arr));
        write_fd_all(pin, "\n", 1);
        free(arr);
        lseek(pin, 0, SEEK_SET);
        if (pipe2(pout, O_CLOEXEC) != 0) {
            fail("pipe: %s", strerror(errno));
        }
        pid_t pp = fork();
        if (pp == 0) {
            dup2(pin, 0);
            dup2(pout[1], 1);
            if (errfd >= 0) {
                dup2(errfd, 2);
            }
            char *av[] = {(char *)project_name("."), "--call", "prepare", NULL};
            extern char **environ;
            fexecve(mem, av, environ);
            _exit(127);
        }
        close(pout[1]);
        close(pin);
        FILE *po = fdopen(pout[0], "r");
        char *pl = NULL;
        size_t pc = 0;
        cJSON *pr = po && getline(&pl, &pc, po) > 0 ? cJSON_Parse(pl) : NULL;
        const cJSON *perr = cJSON_GetObjectItemCaseSensitive(pr, "error");
        if (!json) {
            printf("prepare(): %s\n", !pr ? "the agent stopped" : cJSON_IsString(perr) ? perr->valuestring : "done");
            fflush(stdout);
        }
        cJSON_Delete(pr);
        free(pl);
        if (po) {
            fclose(po);
        }
        waitpid(pp, NULL, 0);
    }
    cJSON_Delete(all_cases);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(infd, 0);
        dup2(pfd[1], 1);
        if (errfd >= 0) {
            dup2(errfd, 2);
        }
        char *av[] = {(char *)project_name("."), "--call", (char *)fn, NULL};
        extern char **environ;
        fexecve(mem, av, environ);
        _exit(127);
    }
    close(pfd[1]);
    FILE *res = fdopen(pfd[0], "r");
    int *right = calloc((size_t)h->n, sizeof(int)), rows_right = 0, errs = 0;
    cJSON *jcases = cJSON_CreateArray();
    char *wrong_txt = NULL;
    size_t wrong_len = 0;
    FILE *wrong = open_memstream(&wrong_txt, &wrong_len);
    char *line = NULL;
    size_t cap = 0;
    for (int k = 0; k < ncase; k++) {
        const row_t *r = &rows[case_row[k]];
        const char *cid = ids[case_row[k]];
        cJSON *o = getline(&line, &cap, res) > 0 ? cJSON_Parse(line) : NULL;
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(o, "result");
        const cJSON *er = cJSON_GetObjectItemCaseSensitive(o, "error");
        cJSON *jc = cJSON_CreateObject();
        cJSON_AddStringToObject(jc, "id", cid);
        cJSON *jw = cJSON_AddObjectToObject(jc, "wrong");
        int all = 1;
        if (!cJSON_IsObject(result)) {
            const char *why = cJSON_IsString(er) ? er->valuestring : o ? "the function returned no table" : "the agent stopped";
            fprintf(wrong, "  %s: ERROR %s\n", cid, why);
            cJSON_AddStringToObject(jc, "error", why);
            errs++;
            all = 0;
        }
        for (int c = 0; c < h->n; c++) {
            if (!is_answer[c]) {
                continue;
            }
            char *got = field_text(result_field(result, h->f[c]));
            const char *want = c < r->n ? r->f[c] : "";
            int ok = cJSON_IsObject(result) && same_answer(got, want, tol);
            if (one && !json) {
                printf("  %s %-32s got %s\n  %*s want %s\n", is_text[c] ? "·" : ok ? "✓" : "✗", h->f[c], got[0] ? got : "(empty)",
                       36, "", want[0] ? want : "(empty)");
            }
            if (!is_text[c]) {
                right[c] += ok;
                all &= ok;
                if (!ok && cJSON_IsObject(result)) {
                    fprintf(wrong, "  %s: %s: got %s, want %s\n", cid, h->f[c], got[0] ? got : "(empty)",
                            want[0] ? want : "(empty)");
                    cJSON *pair = cJSON_AddArrayToObject(jw, h->f[c]);
                    cJSON_AddItemToArray(pair, cJSON_CreateString(got));
                    cJSON_AddItemToArray(pair, cJSON_CreateString(want));
                }
            }
            free(got);
        }
        rows_right += all;
        cJSON_AddItemToArray(jcases, jc);
        cJSON_Delete(o);
    }
    fclose(wrong);
    int status = 0;
    waitpid(pid, &status, 0);
    /* the agent's notes: its model use, and the end of its log when something broke */
    char *usage = NULL, *tail = NULL;
    if (errfd >= 0) {
        lseek(errfd, 0, SEEK_SET);
        FILE *ef = fdopen(errfd, "r");
        char *l = NULL;
        size_t lc = 0;
        size_t tcap = 0;
        FILE *tf = open_memstream(&tail, &tcap);
        char *last[12] = {0};
        int nl = 0;
        while (ef && getline(&l, &lc, ef) > 0) {
            if (!strncmp(l, "usage: ", 7)) { /* one line per agent process (prepare, solve_case): add them up */
                cJSON *now = cJSON_Parse(l + 7), *before = usage ? cJSON_Parse(usage) : NULL;
                static const char *const k[] = {"calls", "input_tokens", "output_tokens"};
                for (int i = 0; now && before && i < 3; i++) {
                    cJSON *a = cJSON_GetObjectItemCaseSensitive(now, k[i]), *b = cJSON_GetObjectItemCaseSensitive(before, k[i]);
                    if (cJSON_IsNumber(a) && cJSON_IsNumber(b)) {
                        cJSON_SetNumberValue(a, a->valuedouble + b->valuedouble);
                    }
                }
                free(usage);
                usage = now ? cJSON_PrintUnformatted(now) : xstrdup(l + 7);
                if (now) { /* keep the line shape: JSON then a newline */
                    char *u2 = NULL;
                    if (asprintf(&u2, "%s\n", usage) >= 0) {
                        free(usage);
                        usage = u2;
                    }
                }
                cJSON_Delete(now);
                cJSON_Delete(before);
            } else {
                free(last[nl % 12]);
                last[nl % 12] = xstrdup(l);
                nl++;
            }
        }
        for (int i = nl > 12 ? nl - 12 : 0; i < nl; i++) {
            fputs(last[i % 12], tf);
        }
        fclose(tf);
    }

    double mean = 0;
    int ncols = 0;
    for (int c = 0; c < h->n; c++) {
        if (is_answer[c] && !is_text[c]) {
            mean += (double)right[c] / ncase;
            ncols++;
        }
    }
    mean /= ncols;
    if (json) {
        cJSON *o = cJSON_CreateObject(), *cols = cJSON_AddObjectToObject(o, "columns");
        cJSON_AddStringToObject(o, "examples", examples);
        cJSON_AddNumberToObject(o, "cases", ncase);
        for (int c = 0; c < h->n; c++) {
            if (is_answer[c] && !is_text[c]) {
                cJSON_AddNumberToObject(cols, h->f[c], right[c]);
            }
        }
        cJSON_AddNumberToObject(o, "rows_right", rows_right);
        cJSON_AddNumberToObject(o, "mean_column_score", mean);
        cJSON_AddNumberToObject(o, "errors", errs);
        cJSON_AddNumberToObject(o, "tolerance", tol);
        if (usage) {
            cJSON_AddItemToObject(o, "usage", cJSON_Parse(usage));
        }
        cJSON_AddItemToObject(o, "cases_detail", jcases);
        char *s = cJSON_Print(o);
        puts(s);
        free(s);
        cJSON_Delete(o);
    } else if (!one) {
        printf("\n%-34s right\n", "column");
        for (int c = 0; c < h->n; c++) {
            if (is_answer[c] && !is_text[c]) {
                printf("%-34s %3d/%d  %3.0f%%\n", h->f[c], right[c], ncase, 100.0 * right[c] / ncase);
            }
        }
        printf("%-34s %3d/%d\n", "whole rows", rows_right, ncase);
        printf("%-34s %.2f\n", "mean column score", mean);
        for (int c = 0, first = 1; c < h->n; c++) {
            if (is_text[c]) {
                printf("%s%s", first ? "not scored (free text): " : ", ", h->f[c]);
                first = 0;
            }
        }
        printf("\nnumbers within %g%%; words in any case; lists (| or ;) item by item in any order\n", tol * 100);
        if (wrong_len) {
            printf("\nwrong or failed:\n%s", wrong_txt);
            printf("one case in full, with the agent's notes: botter eval %s --case <id>\n", examples);
        }
        if (errs && tail && *tail) {
            printf("\nthe agent's last notes:\n%s", tail);
        }
    } else {
        printf("whole row %s\n", rows_right ? "right" : "wrong");
        if (errs) {
            printf("%s", wrong_txt);
        }
    }
    if (usage && !json) {
        printf("model use: %s", usage);
    }
    if (errs) {
        return 1;
    }
    return min >= 0 && mean < min ? 1 : 0;
}

/* botter seal: the agent's keys (model connection, MCP and A2A tokens) into ./secrets.vault behind a password,
   asked at this terminal; the next botter build embeds the vault. */
static int cmd_seal(void)
{
    char *a[] = {"--seal"};
    return cmd_run(1, a);
}

/* ---------- botter data: what the data files hold, in one screen per file ---------- */

typedef struct {
    char  *path;
    row_t *rows;
    int    nr;
    char **keys;  /* per column: sorted values when the column is a unique key, else NULL */
    int   *nkeys;
} csvf_t;

typedef struct {
    char  *group; /* folder (two levels at most) + extension */
    int    count;
    off_t  bytes;
} tally_t;

static int cmp_strp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int looks_date(const char *s)
{
    return strlen(s) >= 10 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[3]) && s[4] == '-' &&
           isdigit((unsigned char)s[5]) && s[7] == '-' && isdigit((unsigned char)s[9]) &&
           (!s[10] || s[10] == ' ' || s[10] == 'T');
}

static const char *bool_word(const char *s)
{
    static const char *const w[] = {"true", "false", "yes", "no", "0", "1", NULL};
    for (int i = 0; w[i]; i++) {
        if (!strcasecmp(s, w[i])) {
            return w[i];
        }
    }
    return NULL;
}

static void human_size(off_t b, char *out, size_t cap)
{
    if (b >= 1 << 20) {
        snprintf(out, cap, "%.1f MB", (double)b / (1 << 20));
    } else if (b >= 1 << 10) {
        snprintf(out, cap, "%.0f KB", (double)b / (1 << 10));
    } else {
        snprintf(out, cap, "%lld B", (long long)b);
    }
}

static void data_walk(const char *root, const char *rel, int depth, csvf_t **csvs, int *nc, tally_t **tl, int *nt)
{
    char *dir = rel[0] ? join(root, rel) : xstrdup(root);
    struct dirent **names = NULL;
    int n = scandir(dir, &names, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        const char *nm = names[i]->d_name;
        if (nm[0] == '.' || !strcmp(nm, "node_modules") || !strcmp(nm, "__pycache__") || !strcmp(nm, "cache") ||
            (!rel[0] && (!strcmp(nm, "functions") || !strcmp(nm, "skills") || !strcmp(nm, "agents")))) {
            free(names[i]);
            continue;
        }
        char *r = rel[0] ? join(rel, nm) : xstrdup(nm), *full = join(root, r);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (depth < 8) {
                data_walk(root, r, depth + 1, csvs, nc, tl, nt);
            }
        } else if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t l = strlen(nm);
            const char *dot = strrchr(nm, '.');
            if (l > 4 && !strcasecmp(nm + l - 4, ".csv")) {
                *csvs = realloc(*csvs, (size_t)(*nc + 1) * sizeof(csvf_t));
                memset(&(*csvs)[*nc], 0, sizeof(csvf_t));
                (*csvs)[(*nc)++].path = xstrdup(r);
            } else if (!(!rel[0] && (!strcmp(nm, "FLOW.lua") || !strcmp(nm, "AGENT.md") || !strcmp(nm, "config.json")))) {
                /* group: the first two folders of the path, and the extension */
                char g[PATH_MAX];
                const char *s1 = strchr(r, '/'), *s2 = s1 ? strchr(s1 + 1, '/') : NULL;
                size_t gl = !s1 ? 0 : (size_t)((s2 ? s2 : s1) - r);
                snprintf(g, sizeof(g), "%.*s%s*%s", (int)gl, r, gl ? "/" : "", dot && dot != nm ? dot : "");
                int k = 0;
                while (k < *nt && strcmp((*tl)[k].group, g)) {
                    k++;
                }
                if (k == *nt) {
                    *tl = realloc(*tl, (size_t)(*nt + 1) * sizeof(tally_t));
                    (*tl)[k] = (tally_t){xstrdup(g), 0, 0};
                    (*nt)++;
                }
                (*tl)[k].count++;
                (*tl)[k].bytes += st.st_size;
            }
        }
        free(r);
        free(full);
        free(names[i]);
    }
    free(names);
    free(dir);
}

/* Every column of one CSV: its kind, range or values, blanks, and links to key columns of other files. */
static void data_profile(csvf_t *all, int nc, int me)
{
    csvf_t *f = &all[me];
    row_t *h = &f->rows[0];
    int n = f->nr - 1;
    printf("\n%s: %d rows, %d columns\n", f->path, n, h->n);
    for (int c = 0; c < h->n; c++) {
        char **v = malloc((size_t)(n ? n : 1) * sizeof(char *));
        int nb = 0, num = 1, date = 1, boo = 1, listy = 0, spaces = 0;
        size_t total = 0;
        double lo = INFINITY, hi = -INFINITY;
        for (int r = 1; r <= n; r++) {
            const char *x = c < f->rows[r].n ? f->rows[r].f[c] : "";
            if (!*x) {
                continue;
            }
            double d;
            v[nb++] = (char *)x;
            total += strlen(x);
            spaces += strchr(x, ' ') != NULL;
            listy += (strchr(x, ';') || strchr(x, '|')) && !strchr(x, ' ');
            if (is_number(x, &d)) {
                lo = fmin(lo, d), hi = fmax(hi, d);
            } else {
                num = 0;
            }
            date &= looks_date(x);
            boo &= bool_word(x) != NULL;
        }
        qsort(v, (size_t)nb, sizeof(char *), cmp_strp);
        int distinct = 0;
        for (int i = 0; i < nb; i++) {
            distinct += !i || strcmp(v[i], v[i - 1]);
        }
        double avg = nb ? (double)total / nb : 0;
        char desc[2048] = "";
        size_t dl = 0;
#define ADD(...) (dl += (size_t)snprintf(desc + dl, dl < sizeof(desc) ? sizeof(desc) - dl : 0, __VA_ARGS__))
        int is_key = nb == n && distinct == n && n > 1 && !num && !date && !spaces && !listy && avg <= 60;
        if (!nb) {
            ADD("empty");
        } else if (boo && distinct <= 2) {
            ADD("yes/no:");
            for (int i = 0, k = 0; i < nb; i = k) {
                for (k = i; k < nb && !strcmp(v[k], v[i]); k++) {
                }
                ADD(" %s %d", v[i], k - i);
            }
        } else if (is_key) {
            ADD("unique id, %s .. %s", v[0], v[nb - 1]);
        } else if (num) {
            ADD("number %g .. %g, %d distinct", lo, hi, distinct);
        } else if (date) {
            ADD("date %s .. %s%s", v[0], v[nb - 1], distinct == nb ? ", all different" : "");
        } else if (avg > 40 && spaces * 2 > nb) {
            const char *e = v[nb / 2];
            ADD("free text, %.0f chars on average, e.g. \"%.70s%s\"", avg, e, strlen(e) > 70 ? "..." : "");
        } else if (listy * 5 >= nb && listy) {
            /* lists: the items people put in them */
            char **it = NULL;
            int ni = 0;
            for (int i = 0; i < nb; i++) {
                char *cp = xstrdup(v[i]);
                for (char *sv = NULL, *tk = strtok_r(cp, ";|", &sv); tk; tk = strtok_r(NULL, ";|", &sv)) {
                    it = realloc(it, (size_t)(ni + 1) * sizeof(char *));
                    it[ni++] = xstrdup(tk);
                }
                free(cp);
            }
            qsort(it, (size_t)ni, sizeof(char *), cmp_strp);
            int di = 0;
            for (int i = 0; i < ni; i++) {
                di += !i || strcmp(it[i], it[i - 1]);
            }
            ADD("list (; or |), %d different items", di);
            if (di <= 25) {
                ADD(":");
                for (int i = 0, k = 0; i < ni; i = k) {
                    for (k = i; k < ni && !strcmp(it[k], it[i]); k++) {
                    }
                    ADD(" %s %d", it[i], k - i);
                }
            }
            for (int i = 0; i < ni; i++) {
                free(it[i]);
            }
            free(it);
        } else if (distinct <= 15 && nb >= 2 * distinct) {
            ADD("one of %d:", distinct);
            for (int i = 0, k = 0; i < nb; i = k) {
                for (k = i; k < nb && !strcmp(v[k], v[i]); k++) {
                }
                ADD(" %s %d", v[i], k - i);
            }
        } else {
            ADD("text, %d distinct%s, e.g. %.40s | %.40s", distinct, distinct == n ? " (all different)" : "", v[0], v[nb - 1]);
        }
        if (nb < n) {
            ADD("; blank %d", n - nb);
        }
        /* links: every value of this column is a key of another file */
        for (int o = 0; o < nc && nb && !num; o++) {
            for (int oc = 0; o != me && oc < all[o].rows[0].n; oc++) {
                if (!all[o].keys || !all[o].keys[oc]) {
                    continue;
                }
                int found = 1;
                for (int i = 0; i < nb && found; i++) {
                    found = bsearch(&v[i], all[o].keys[oc], (size_t)all[o].nkeys[oc], sizeof(char *), cmp_strp) != NULL;
                }
                if (found) {
                    ADD("; -> %s:%s", all[o].path, all[o].rows[0].f[oc]);
                }
            }
        }
#undef ADD
        for (char *q = desc; *q; q++) { /* values with line breaks stay on one line */
            if (*q == '\n' || *q == '\r' || *q == '\t') {
                *q = ' ';
            }
        }
        printf("  %-28s %s\n", h->f[c], desc);
        free(v);
    }
}

static int cmd_data(int argc, char **argv)
{
    const char *root = argc > 0 ? argv[0] : ".";
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fail("botter data [DIR]: %s is not a folder", root);
    }
    csvf_t *csvs = NULL;
    tally_t *tl = NULL;
    int nc = 0, nt = 0;
    data_walk(root, "", 0, &csvs, &nc, &tl, &nt);
    for (int i = 0; i < nc; i++) { /* parse, and keep the sorted values of unique key columns */
        char *full = join(root, csvs[i].path);
        size_t len = 0;
        char *text = slurp(full, &len);
        free(full);
        if (text && len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) {
            memmove(text, text + 3, len - 2);
        }
        csvs[i].rows = text ? csv_parse(text, &csvs[i].nr) : NULL;
        if (!csvs[i].rows || csvs[i].nr < 1) {
            csvs[i].nr = 0;
            continue;
        }
        int ncol = csvs[i].rows[0].n, n = csvs[i].nr - 1;
        csvs[i].keys = calloc((size_t)ncol, sizeof(char *));
        csvs[i].nkeys = calloc((size_t)ncol, sizeof(int));
        for (int c = 0; c < ncol && n > 1; c++) {
            char **v = malloc((size_t)n * sizeof(char *));
            int ok = 1, k = 0;
            double d;
            for (int r = 1; r <= n && ok; r++) {
                const char *x = c < csvs[i].rows[r].n ? csvs[i].rows[r].f[c] : "";
                ok = *x && strlen(x) <= 60 && !is_number(x, &d) && !strpbrk(x, " ;|");
                v[k++] = (char *)x;
            }
            if (ok) {
                qsort(v, (size_t)k, sizeof(char *), cmp_strp);
                for (int j = 1; j < k && ok; j++) {
                    ok = strcmp(v[j], v[j - 1]) != 0;
                }
            }
            if (ok) {
                csvs[i].keys[c] = (char *)v;
                csvs[i].nkeys[c] = k;
            } else {
                free(v);
            }
        }
    }
    printf("data under %s: %d CSV file(s)%s\n", root, nc, nt ? ", and other files below" : "");
    for (int i = 0; i < nc; i++) {
        if (csvs[i].nr < 1) {
            printf("\n%s: empty or unreadable\n", csvs[i].path);
        } else if (csvs[i].nr == 1) {
            printf("\n%s: header only: ", csvs[i].path);
            for (int c = 0; c < csvs[i].rows[0].n; c++) {
                printf("%s%s", c ? ", " : "", csvs[i].rows[0].f[c]);
            }
            printf("\n");
        } else {
            data_profile(csvs, nc, i);
        }
    }
    if (nt) {
        printf("\nother files (folder, type: count, size)\n");
        for (int i = 0; i < nt; i++) {
            char hs[32];
            human_size(tl[i].bytes, hs, sizeof(hs));
            printf("  %-44s %5d  %s\n", tl[i].group, tl[i].count, hs);
        }
    }
    printf("\nkinds: unique id | number | date | yes/no | one of N (counts) | list | free text (the model reads it) | text\n"
           "-> file:column = every value is an id in that file (a join)\n");
    return 0;
}

/* ---------- help, dispatch ---------- */

static void help(void)
{
    printf("botter %s: make agents that are one file.\n\n"
           "  botter new <name> [--kind chat|batch|tool] [--here]   start a project from a template\n"
           "  botter data [DIR]                            what the data files hold: columns, kinds, joins, other files\n"
           "  botter check [DIR]                           validate the project and run its tests\n"
           "  botter run [agent args...]                   run the project from source (e.g. botter run --run \"task\")\n"
           "  botter eval <examples.csv> [options]         score the agent on solved examples, column by column\n"
           "      --answers a,b  --inputs FILE  --id COL  --tol 2%%  --case ID  --fn solve_case  --min 0.8  --json\n"
           "  botter build [-o FILE] [DIR]                 check, test, then write the agent: ./<name>\n"
           "  botter inspect FILE                          what a built agent holds\n"
           "  botter seal                                  seal the agent's keys into it behind a password (asked here)\n"
           "  botter skill [DIR]                           install the Botter skill (default .claude/skills)\n"
           "  botter version\n\n"
           "A project: AGENT.md, config.json, optional FLOW.lua, functions/*.lua (+ tests/), skills/<name>/.\n"
           "The Botter skill explains the format; a coding agent can write the project from it.\n",
           BOTTER_VERSION);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *cmd = argc > 1 ? argv[1] : "help";
    int n = argc > 2 ? argc - 2 : 0;
    char **rest = argv + 2;
    if (!strcmp(cmd, "new")) {
        return cmd_new(n, rest);
    }
    if (!strcmp(cmd, "seal")) {
        return cmd_seal();
    }
    if (!strcmp(cmd, "data")) {
        return cmd_data(n, rest);
    }
    if (!strcmp(cmd, "check")) {
        return cmd_check(n, rest);
    }
    if (!strcmp(cmd, "run")) {
        return cmd_run(n, rest);
    }
    if (!strcmp(cmd, "eval")) {
        return cmd_eval(n, rest);
    }
    if (!strcmp(cmd, "build")) {
        return cmd_buildc(n, rest);
    }
    if (!strcmp(cmd, "inspect")) {
        if (!n) {
            fail("botter inspect FILE");
        }
        return cmd_list(rest[0]);
    }
    if (!strcmp(cmd, "skill")) {
        return cmd_skill(n, rest);
    }
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        const char *tag = memmem(bt_runtime, (size_t)(bt_runtime_end - bt_runtime), "BOTCORE_VERSION=", 16);
        printf("botter %s (botcore %.20s)\n", BOTTER_VERSION, tag ? tag + 16 : "?");
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        help();
        return 0;
    }
    fprintf(stderr, "botter: unknown command %s\n\n", cmd);
    help();
    return 2;
}
