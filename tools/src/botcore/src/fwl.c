/*
 * FWL front end: parser (syntax tree + syntax errors) and build-time checks.
 * Shared by botcore (fwl_run.c interprets the tree) and botter_pack (which
 * #includes this file and runs sq_check). See fwl.h and skills/write-flow.
 */
#define _GNU_SOURCE
#include "fwl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SQ_MAXD 64

static const char *const SQ_BLOCK[] = {"", "IF", "FOR", "WHILE", "RETRY", "PARALLEL", "IMPORT"};
enum { B_IF = 1, B_FOR, B_WHILE, B_RETRY, B_PAR, B_IMPORT };

/* ---------------------------------------------------------------- helpers */

static void sq_diag(sq_prog *p, int line, int error, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void sq_diag(sq_prog *p, int line, int error, const char *fmt, ...)
{
    char m[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if (error) {
        p->errors++;
    } else {
        p->warnings++;
    }
    if (p->diag) {
        p->diag(p->ud, line, error, m);
    }
}

static char *sq_dup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) {
        abort();
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void *sq_alloc(size_t n)
{
    void *v = calloc(1, n);
    if (!v) {
        abort();
    }
    return v;
}

static void sq_push_node(sq_list *l, sq_node *x)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, (size_t)l->cap * sizeof(*l->v));
        if (!l->v) {
            abort();
        }
    }
    l->v[l->n++] = x;
}

static const char *sq_skip(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Case-insensitive word at s; its length or 0. */
static size_t sq_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncasecmp(s, w, n) == 0 && !isalnum((unsigned char)s[n]) && s[n] != '_' ? n : 0;
}

/* Text ends with the UPPERCASE keyword w (as a separate word)? */
static int sq_ends(const char *s, const char *w)
{
    size_t n = strlen(s), k = strlen(w);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    return n >= k && strncmp(s + n - k, w, k) == 0 && (n == k || s[n - k - 1] == ' ' || s[n - k - 1] == '\t');
}

/* Copy of s without a trailing keyword w (and the spaces before it). */
static char *sq_strip_end(const char *s, const char *w)
{
    size_t n = strlen(s), k = strlen(w);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    if (n >= k && strncmp(s + n - k, w, k) == 0) {
        n -= k;
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    return sq_dup(s, n);
}

static int sq_ident(const char *s, size_t n)
{
    if (n == 0 || n > 64 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return 0;
        }
    }
    return 1;
}

size_t sq_dollar(const char *c)
{
    if (c[0] != '$' || !(isalpha((unsigned char)c[1]) || c[1] == '_')) {
        return 0;
    }
    size_t i = 2;
    for (;;) {
        while (isalnum((unsigned char)c[i]) || c[i] == '_') {
            i++;
        }
        if (c[i] == '.' && (isalpha((unsigned char)c[i + 1]) || c[i + 1] == '_')) {
            i += 2; /* a '.' that ends a sentence is not part of the name */
        } else if (c[i] == '[' && isdigit((unsigned char)c[i + 1])) {
            size_t j = i + 1;
            while (isdigit((unsigned char)c[j])) {
                j++;
            }
            if (c[j] != ']') {
                return i;
            }
            i = j + 1;
            if (c[i] != '.' && c[i] != '[') {
                return i;
            }
        } else {
            return i;
        }
    }
}

int sq_ref(const char *s, size_t n, char root[65])
{
    size_t i = 0;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
        i++;
    }
    if (!sq_ident(s, i)) {
        return 0;
    }
    snprintf(root, 65, "%.*s", (int)i, s);
    while (i < n) {
        if (s[i] == '.') {
            size_t j = ++i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
                i++;
            }
            if (i == j) {
                return 0;
            }
        } else if (s[i] == '[') {
            size_t j = ++i;
            while (i < n && isdigit((unsigned char)s[i])) {
                i++;
            }
            if (i == j || i == n || s[i] != ']') {
                return 0;
            }
            i++;
        } else {
            return 0;
        }
    }
    return 1;
}

/* "name" at s -> out; position after it, or NULL. */
static const char *sq_quoted(const char *s, char *out, size_t cap)
{
    s = sq_skip(s);
    if (*s != '"') {
        return NULL;
    }
    const char *e = strchr(s + 1, '"');
    if (!e) {
        return NULL;
    }
    snprintf(out, cap, "%.*s", (int)(e - s - 1), s + 1);
    return e + 1;
}

/* $name (one identifier) at s -> out; position after it, or NULL. */
static const char *sq_var(const char *s, char out[65])
{
    s = sq_skip(s);
    size_t n = 1;
    while (*s == '$' && (isalnum((unsigned char)s[n]) || s[n] == '_')) {
        n++;
    }
    if (*s != '$' || !sq_ident(s + 1, n - 1)) {
        return NULL;
    }
    snprintf(out, 65, "%.*s", (int)(n - 1), s + 1);
    return s + n;
}

/* $skill-name or @agent-name (sigil) at s -> out; position after it, or NULL. */
static const char *sq_named(const char *s, char sigil, char *out, size_t cap)
{
    s = sq_skip(s);
    size_t n = 1;
    while (*s == sigil && (isalnum((unsigned char)s[n]) || s[n] == '_' || s[n] == '-')) {
        n++;
    }
    if (*s != sigil || n == 1 || n > cap) {
        return NULL;
    }
    snprintf(out, cap, "%.*s", (int)(n - 1), s + 1);
    return s + n;
}

/*
 * Syntax of an expression or prose text: strings closed, $refs closed and
 * well-formed, and (braces) balanced { } [ ] ( ). Returns 0 if fine.
 */
static int sq_syntax(sq_prog *p, int line, const char *s, int braces)
{
    int bal[3] = {0, 0, 0};
    for (const char *c = s; *c; c++) {
        if (*c == '"') {
            const char *q = c + 1;
            while (*q && *q != '"') {
                q += *q == '\\' && q[1] ? 2 : 1;
            }
            if (!*q) {
                sq_diag(p, line, 1, "a string is not closed: add the missing \"");
                return -1;
            }
            c = q;
        } else if (*c == '$' && sq_dollar(c)) {
            c += sq_dollar(c) - 1;
        } else if (*c == '`') {
            const char *q = strchr(c + 1, '`');
            char root[65];
            if (q && sq_ref(c + 1, (size_t)(q - c - 1), root)) {
                sq_diag(p, line, 1, "write $%.*s, not `%.*s`: FWL names start with $", (int)(q - c - 1), c + 1,
                        (int)(q - c - 1), c + 1);
                return -1;
            }
        } else if (braces) {
            const char *o = "{[(", *cl = "}])", *k;
            if ((k = strchr(o, *c))) {
                bal[k - o]++;
            } else if ((k = strchr(cl, *c))) {
                if (--bal[k - cl] < 0) {
                    sq_diag(p, line, 1, "unbalanced '%c'", *c);
                    return -1;
                }
            }
        }
    }
    if (braces && (bal[0] || bal[1] || bal[2])) {
        sq_diag(p, line, 1,
                "a payload's { } [ ] ( ) are not balanced; keep a payload on one line, or write 'with parameters:' "
                "followed by '- name: value' lines");
        return -1;
    }
    return 0;
}

int sq_is_literal(const char *s)
{
    int any = 0;
    for (const char *c = s; *c;) {
        if (*c == '"') {
            const char *q = c + 1;
            while (*q && *q != '"') {
                q += *q == '\\' && q[1] ? 2 : 1;
            }
            c = *q ? q + 1 : q;
            any = 1;
        } else if (*c == '$' && sq_dollar(c)) {
            c += sq_dollar(c);
            any = 1;
        } else if (strchr(" \t+,:{}[]-.0123456789", *c)) {
            any |= isdigit((unsigned char)*c) || *c == '{' || *c == '[';
            c++;
        } else if (isalpha((unsigned char)*c) || *c == '_') {
            const char *e = c;
            while (isalnum((unsigned char)*e) || *e == '_') {
                e++;
            }
            size_t n = (size_t)(e - c);
            int kw = (n == 4 && !strncmp(c, "true", 4)) || (n == 5 && !strncmp(c, "false", 5)) ||
                     (n == 4 && !strncmp(c, "null", 4));
            const char *a = sq_skip(e);
            if (!kw && *a != ':') { /* a bare word that is not a JSON key: prose */
                return 0;
            }
            any = 1;
            c = e;
        } else {
            return 0;
        }
    }
    return any;
}

/* ----------------------------------------------------------------- parser */

typedef struct {
    int      kind;
    int      line;
    int      has_else;
    int      count; /* statements in the current body / branch */
    int      col;   /* column of the line that opened the block */
    int      body;  /* column of the lines inside it (-1 until the first one) */
    int      alt;   /* text column of the last "1. " line inside it (-1 = none) */
    int      bline; /* line of the current branch's header */
    sq_node *node;
    sq_list *cur;
} sq_frame;

typedef struct {
    sq_prog *p;
    int      line;
    sq_frame st[SQ_MAXD];
    int      depth;
    sq_node *last; /* last statement (for "- k: v" parameter lines) */
    int      params;
    int      col;        /* text column of the current line */
    int      body, alt;  /* as in sq_frame, for the lines outside every block */
    int      step, stmts, returns;
} sq_ps;

static sq_list *sq_cur(sq_ps *s)
{
    return s->depth ? s->st[s->depth - 1].cur : &s->p->top;
}

static sq_node *sq_add(sq_ps *s, int kind)
{
    sq_node *x = sq_alloc(sizeof(*x));
    x->kind = kind;
    x->line = s->line;
    sq_push_node(sq_cur(s), x);
    if (s->depth) {
        s->st[s->depth - 1].count++;
    }
    if (kind != SQ_STEP && kind != SQ_LOAD) {
        s->stmts++;
    }
    return x;
}

static void sq_open(sq_ps *s, int bkind, sq_node *x)
{
    if (s->depth == SQ_MAXD) {
        sq_diag(s->p, s->line, 1, "blocks nested too deeply");
        return;
    }
    sq_frame *f = &s->st[s->depth++];
    memset(f, 0, sizeof(*f));
    f->kind = bkind;
    f->line = f->bline = s->line;
    f->col = s->col;
    f->body = f->alt = -1;
    f->node = x;
    f->cur = bkind == B_IMPORT ? &s->p->top : bkind == B_IF ? &x->br[0].body : &x->body;
}

static const char *sq_bname(int k)
{
    return k == B_FOR ? "FOR EACH" : SQ_BLOCK[k];
}

static void sq_empty(sq_ps *s, const sq_frame *f)
{
    if (!f->count) {
        sq_diag(s->p, f->bline, 1, "this line opens a block, but nothing is indented under it (indent the lines that belong to it)");
    }
}

/* A block ends where its indentation ends. */
static void sq_pop(sq_ps *s)
{
    sq_empty(s, &s->st[s->depth - 1]);
    s->depth--;
}

static int sq_kw_at(const char *t, const char *w)
{
    size_t k = strlen(w);
    return strncmp(t, w, k) == 0 && !isalnum((unsigned char)t[k]) && t[k] != '_';
}

/*
 * Indentation is the block structure: a line at or left of the column of the line that
 * opened a block ends that block; the lines inside one block share one column. raw is
 * the column where the line starts, s->col where its text starts (after "1. " or "- ").
 * Returns 0 if the line is to be skipped.
 */
static int sq_dent(sq_ps *s, const char *t, int raw)
{
    int is_else = sq_kw_at(t, "ELSE");
    while (s->depth) {
        sq_frame *f = &s->st[s->depth - 1];
        if (raw > f->col || (is_else && f->kind == B_IF && raw == f->col)) {
            break;
        }
        sq_pop(s);
    }
    sq_frame *f = s->depth ? &s->st[s->depth - 1] : NULL;
    if (is_else) {
        if (f && f->kind == B_IF && raw != f->col) {
            sq_diag(s->p, s->line, 1, "ELSE lines up with its IF (the IF on line %d), not with the lines inside it", f->line);
            return 0;
        }
        return 1;
    }
    if (sq_kw_at(t, "END") || sq_kw_at(t, "ENDIF") || sq_kw_at(t, "ENDFOR")) {
        return 1; /* reported as a statement; its indentation says nothing */
    }
    int *body = f ? &f->body : &s->body, *alt = f ? &f->alt : &s->alt;
    if (*body < 0) {
        *body = raw;
    } else if (raw != *body && raw != *alt) {
        if (raw > *body) {
            sq_diag(s->p, s->line, 1,
                    "this line is indented deeper than the lines around it, but no block opens above it (only a line "
                    "ending in ':' such as IF ...: or FOR EACH ...: opens one)");
        } else if (f) {
            sq_diag(s->p, s->line, 1,
                    "this line's indentation matches no block: line it up with the lines inside the %s on line %d, "
                    "or with that %s line to end the block",
                    sq_bname(f->kind), f->line, sq_bname(f->kind));
        } else {
            sq_diag(s->p, s->line, 1, "this line's indentation does not match the lines above it");
        }
    }
    if (s->col != raw) {
        *alt = s->col;
    }
    return 1;
}

/* Header of a block: copy of r without its closing ':'. No ':', or a leftover THEN/DO, is an error. */
static char *sq_head(sq_ps *s, const char *r, const char *reads)
{
    size_t n = strlen(r);
    int ok = n && r[n - 1] == ':';
    n -= ok;
    while (n && (r[n - 1] == ' ' || r[n - 1] == '\t')) {
        n--;
    }
    char *h = sq_dup(r, n);
    static const char *const old[] = {"THEN", "DO"};
    for (int i = 0; i < 2; i++) {
        if (sq_ends(h, old[i])) {
            char *c = sq_strip_end(h, old[i]);
            sq_diag(s->p, s->line, 1, "%s  (no %s: the ':' opens the block)", reads, old[i]);
            free(h);
            return c;
        }
    }
    if (!ok) {
        sq_diag(s->p, s->line, 1, "%s  (the ':' opens the block; the lines indented under it belong to it)", reads);
    }
    return h;
}

static int sq_lev(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    int d[24][24];
    if (n > 22 || m > 22) {
        return 99;
    }
    for (size_t i = 0; i <= n; i++) {
        d[i][0] = (int)i;
    }
    for (size_t j = 0; j <= m; j++) {
        d[0][j] = (int)j;
    }
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            int c = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            int x = d[i - 1][j] + 1, y = d[i][j - 1] + 1;
            d[i][j] = c < x ? (c < y ? c : y) : (x < y ? x : y);
        }
    }
    return d[n][m];
}

static const char *const SQ_KW[] = {"STEP", "IMPORT", "CONNECT", "EXECUTE", "INVOKE", "DELEGATE", "SAVE", "SET",    "ASK", "IF",
                                    "ELSE", "FOR",     "WHILE",  "RETRY", "IN",   "RETURN", NULL};

/* An UPPERCASE first word that is no keyword: a typo of one (error), or a shouted word (prose). */
static int sq_unknown(sq_ps *s, const char *w)
{
    if (!strcmp(w, "BREAK") || !strcmp(w, "CONTINUE") || !strcmp(w, "GOTO") || !strcmp(w, "EXIT")) {
        sq_diag(s->p, s->line, 1,
                "FWL has no %s: to skip the rest of an item, put it inside an IF; to stop a loop early, use "
                "WHILE ... AT MOST N TIMES with a condition, or RETURN to end the flow",
                w);
        return 1;
    }
    static const char *const alias[][2] = {{"INVOCATE", "INVOKE"}, {"INVOKES", "INVOKE"}, {"EXEC", "EXECUTE"},
                                           {"CALL", "EXECUTE"},    {"RUN", "EXECUTE"},    {"STORE", "SAVE"},
                                           {"FOREACH", "FOR EACH"}, {"ELIF", "ELSE IF"},  {"ELSEIF", "ELSE IF"},
                                           {"THEN", "IF <condition>:"},
                                           {"PRINT", "RETURN"},    {"REPEAT", "RETRY UP TO N TIMES:"}};
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) {
        if (strcmp(w, alias[i][0]) == 0) {
            sq_diag(s->p, s->line, 1, "unknown keyword %s; did you mean %s?", w, alias[i][1]);
            return 1;
        }
    }
    if (strlen(w) < 4) {
        return 0; /* API, URL, PDF ...: plain English */
    }
    for (int i = 0; SQ_KW[i]; i++) {
        if (strlen(SQ_KW[i]) >= 4 && sq_lev(w, SQ_KW[i]) <= 2) {
            sq_diag(s->p, s->line, 1, "unknown keyword %s; did you mean %s?", w, SQ_KW[i]);
            return 1;
        }
    }
    return 0;
}

static void sq_instr(sq_ps *s, const char *t)
{
    static const char *const prose[] = {"if", "for each", "while", "else", "return", "retry", NULL};
    static const char *const fix[] = {"IF <condition>:", "FOR EACH $x IN ...:", "WHILE ... AT MOST N TIMES:", "ELSE:",
                                      "RETURN", "RETRY UP TO N TIMES:"};
    for (int i = 0; prose[i]; i++) {
        if (sq_word(t, prose[i]) && !isupper((unsigned char)t[1])) {
            sq_diag(s->p, s->line, 0, "write '%s' as an FWL statement in capitals (%s) so the flow can be checked",
                    prose[i], fix[i]);
            break;
        }
    }
    sq_syntax(s->p, s->line, t, 0);
    sq_node *x = sq_add(s, SQ_INSTR);
    x->a = sq_dup(t, strlen(t));
}

/* Placeholder text in a tool's arguments ("Helpful response...", "TODO") would be
 * sent as it is: the real text must be produced first (INVOKE SKILL, SET ... TO). */
static void sq_placeholder(sq_prog *p, int line, const char *v)
{
    int inq = 0;
    for (const char *c = v; *c; c++) { /* "..." outside strings: fields left out ({ "to": $x, ... }) */
        if (*c == '\\' && inq && c[1]) {
            c++;
        } else if (*c == '"') {
            inq = !inq;
        } else if (!inq && !strncmp(c, "...", 3)) {
            sq_diag(p, line, 1, "\"...\" in a tool's arguments: write out every argument the tool needs");
            break;
        }
    }
    for (const char *c = v; (c = strchr(c, '"'));) {
        const char *e = c + 1;
        while (*e && *e != '"') {
            e += *e == '\\' && e[1] ? 2 : 1;
        }
        size_t n = (size_t)(e - c - 1);
        char lit[120];
        snprintf(lit, sizeof(lit), "%.*s", (int)(n < sizeof(lit) - 1 ? n : sizeof(lit) - 1), c + 1);
        if (strstr(lit, "...") || strstr(lit, "\xe2\x80\xa6") || !strcasecmp(lit, "TODO") || !strcasecmp(lit, "TBD") ||
            strcasestr(lit, "placeholder") || strcasestr(lit, "lorem ipsum")) {
            sq_diag(p, line, 1,
                    "\"%s\" is placeholder text and would be sent as it is: produce the real value first "
                    "(INVOKE SKILL ... then SAVE skill_output, or SET $x TO <what it should say>) and pass $x",
                    lit);
        }
        if (!*e) {
            break;
        }
        c = e + 1;
    }
}

/* One statement (list marker and indentation removed). */
static void sq_stmt(sq_ps *s, char *t)
{
    sq_prog *p = s->p;
    char w[32] = "", name[200], var[65];
    if (s->depth && s->st[s->depth - 1].kind == B_IMPORT) { /* skill names, nothing else */
        const char *e = t;
        do {
            e = sq_named(e, '$', name, sizeof(name));
            if (!e) {
                sq_diag(p, s->line, 1, "under IMPORT: write one skill per line as $name (the folder name in skills/)");
                return;
            }
            sq_node *x = sq_add(s, SQ_LOAD);
            x->a = sq_dup(name, strlen(name));
            e = sq_skip(e);
            e += *e == ',';
        } while (*sq_skip(e));
        return;
    }
    size_t wl = 0;
    while (t[wl] && isalpha((unsigned char)t[wl]) && wl < sizeof(w) - 1) {
        w[wl] = t[wl];
        wl++;
    }
    w[wl] = '\0';
    int upper = wl > 1 && !isalnum((unsigned char)t[wl]) && t[wl] != '_';
    for (size_t i = 0; i < wl && upper; i++) {
        upper = isupper((unsigned char)w[i]);
    }
    const char *r = sq_skip(t + wl);
    size_t len = strlen(t);
    int colon = len && t[len - 1] == ':';
    s->last = NULL;

    if (!upper) {
        sq_instr(s, t);
        return;
    }
    if (!strcmp(w, "STEP")) { /* STEP n: NAME, a label numbered in order */
        char *e;
        long k = strtol(r, &e, 10);
        if (e == r || k < 1 || *e != ':' || !*sq_skip(e + 1)) {
            sq_diag(p, s->line, 1, "STEP reads: STEP n: NAME (numbered 1, 2, 3 ...)");
            return;
        }
        if (k != s->step + 1) {
            sq_diag(p, s->line, 0, "STEP %ld follows STEP %d", k, s->step);
        }
        if (s->depth) {
            sq_diag(p, s->line, 0, "a STEP inside an open %s block (opened on line %d)",
                    SQ_BLOCK[s->st[s->depth - 1].kind], s->st[s->depth - 1].line);
        }
        s->step = (int)k;
        const char *nm = sq_skip(e + 1);
        sq_node *x = sq_add(s, SQ_STEP);
        x->a = sq_dup(nm, strlen(nm));
        x->n = (int)k;
        return;
    }
    if (!strcmp(w, "LOAD")) {
        sq_diag(p, s->line, 1, "FWL has no LOAD: list the agent's skills under IMPORT: at the top of the flow, one $name per line");
        return;
    }
    if (!strcmp(w, "IMPORT")) {
        if (strcmp(r, ":") != 0) {
            sq_diag(p, s->line, 1, "IMPORT reads: IMPORT:  with one $skill per line indented under it");
        }
        if (s->depth) {
            sq_diag(p, s->line, 1, "IMPORT belongs at the top of the flow, not inside a block");
            return;
        }
        sq_open(s, B_IMPORT, NULL);
        return;
    }
    if (!strcmp(w, "CONNECT")) {
        sq_diag(p, s->line, 1, "FWL has no CONNECT: list MCP servers in config.json \"mcp\"; their tools are then called as $server_tool");
        return;
    }
    if (!strcmp(w, "EXECUTE")) {
        size_t k = sq_word(r, "tool");
        const char *e = k ? sq_var(r + k, var) : NULL;
        if (!e) {
            sq_diag(p, s->line, 1, "EXECUTE reads: EXECUTE tool $name [with payload { ... } | with parameters:]");
            return;
        }
        sq_syntax(p, s->line, e, 1);
        sq_node *x = sq_add(s, SQ_EXEC);
        x->a = sq_dup(var, strlen(var));
        e = sq_skip(e);
        sq_placeholder(p, s->line, e);
        x->b = sq_dup(e, strlen(e));
        s->last = x;
        s->params = colon;
        return;
    }
    if (!strcmp(w, "INVOKE") || !strcmp(w, "DELEGATE")) {
        int del = w[0] == 'D'; /* DELEGATE TO @agent: the same shape, an agent instead of a skill */
        size_t k = sq_word(r, del ? "TO" : "SKILL");
        const char *e = k ? sq_named(r + k, del ? '@' : '$', name, sizeof(name)) : NULL;
        if (!e) {
            sq_diag(p, s->line, 1, del ? "DELEGATE reads: DELEGATE TO @agent [USING context $x]  (agents are written @name)"
                                       : "INVOKE reads: INVOKE SKILL $name [USING context $x]");
            return;
        }
        sq_syntax(p, s->line, e, 1);
        sq_node *x = sq_add(s, del ? SQ_DELEGATE : SQ_INVOKE);
        x->a = sq_dup(name, strlen(name));
        e = sq_skip(e);
        size_t u = sq_word(e, "USING");
        if (u) {
            e = sq_skip(e + u);
            size_t c = sq_word(e, "context");
            e = c ? sq_skip(e + c) : e;
        }
        x->b = sq_dup(e, strlen(e));
        s->last = x;
        s->params = colon;
        return;
    }
    if (!strcmp(w, "SAVE")) {
        const char *into = NULL, *tofile = NULL;
        for (const char *c = r; *c; c++) {
            if ((c == r || c[-1] == ' ') && sq_word(c, "INTO") && sq_word(sq_skip(c + 4), "VARIABLE")) {
                into = c;
            }
            if ((c == r || c[-1] == ' ') && sq_word(c, "TO") && sq_word(sq_skip(c + 2), "FILE")) {
                tofile = c;
            }
        }
        const char *at = into ? into : tofile;
        if (!at || at == r) {
            sq_diag(p, s->line, 1,
                    "SAVE reads: SAVE result|skill_output|agent_output|answer|$x INTO VARIABLE $name, or SAVE ... TO FILE "
                    "\"path\"");
            return;
        }
        size_t wn = (size_t)(at - r);
        while (wn && r[wn - 1] == ' ') {
            wn--;
        }
        char *what = sq_dup(r, wn);
        sq_syntax(p, s->line, what, 1);
        if (into) {
            const char *e = sq_var(sq_skip(sq_skip(into + 4) + 8), var);
            if (!e || *sq_skip(e)) {
                sq_diag(p, s->line, 1, "SAVE ... INTO VARIABLE needs one $name (letters, digits, _)");
                free(what);
                return;
            }
            sq_node *x = sq_add(s, SQ_SAVE_VAR);
            x->a = what;
            x->b = sq_dup(var, strlen(var));
        } else {
            char path[300] = "";
            const char *e = sq_quoted(sq_skip(tofile + 2) + 4, path, sizeof(path));
            if (!e || !path[0] || path[0] == '/' || strstr(path, "..")) {
                sq_diag(p, s->line, 1, "SAVE ... TO FILE needs a \"relative/path\" inside the working directory");
            }
            sq_node *x = sq_add(s, SQ_SAVE_FILE);
            x->a = what;
            x->b = sq_dup(path, strlen(path));
        }
        return;
    }
    if (!strcmp(w, "SET")) {
        const char *e = sq_var(r, var);
        size_t k = e ? sq_word(sq_skip(e), "TO") : 0;
        const char *v = k ? sq_skip(sq_skip(e) + k) : NULL;
        if (!v || !*v) {
            sq_diag(p, s->line, 1, "SET reads: SET $name TO <value>");
            return;
        }
        sq_syntax(p, s->line, v, 1);
        sq_node *x = sq_add(s, SQ_SET);
        x->a = sq_dup(var, strlen(var));
        x->b = sq_dup(v, strlen(v));
        return;
    }
    if (!strcmp(w, "ASK")) {
        size_t k = sq_word(r, "USER");
        const char *q = k ? sq_skip(r + k) : NULL;
        if (!q || !*q) {
            sq_diag(p, s->line, 1, "ASK reads: ASK USER \"question\"  (the reply is $answer)");
            return;
        }
        sq_syntax(p, s->line, q, 1);
        sq_node *x = sq_add(s, SQ_ASK);
        x->a = sq_dup(q, strlen(q));
        return;
    }
    if (!strcmp(w, "IF")) {
        char *c = sq_head(s, r, "IF reads: IF <condition>:");
        if (!*c) {
            sq_diag(p, s->line, 1, "IF needs a condition: IF <condition>:");
        }
        sq_syntax(p, s->line, c, 0);
        sq_node *x = sq_add(s, SQ_IF);
        x->br = sq_alloc(sizeof(*x->br));
        x->nbr = 1;
        x->br[0].cond = c;
        x->br[0].line = s->line;
        sq_open(s, B_IF, x);
        return;
    }
    if (!strcmp(w, "ELSE")) {
        int elif = sq_word(r, "IF") > 0;
        sq_frame *f = s->depth ? &s->st[s->depth - 1] : NULL;
        if (!f || f->kind != B_IF) {
            sq_diag(p, s->line, 1, "%s without an open IF", elif ? "ELSE IF" : "ELSE");
            return;
        }
        if (f->has_else) {
            sq_diag(p, s->line, 1, "%s after ELSE: ELSE must be the last branch of an IF", elif ? "ELSE IF" : "ELSE");
        } else {
            sq_empty(s, f);
        }
        char *c = NULL;
        if (elif) {
            c = sq_head(s, sq_skip(r + 2), "ELSE IF reads: ELSE IF <condition>:");
            sq_syntax(p, s->line, c, 0);
        } else if (strcmp(r, ":") != 0) {
            sq_diag(p, s->line, 1, "ELSE reads: ELSE:  (for another condition write ELSE IF <condition>:)");
        }
        sq_node *x = f->node;
        x->br = realloc(x->br, (size_t)(x->nbr + 1) * sizeof(*x->br));
        if (!x->br) {
            abort();
        }
        sq_branch *b = &x->br[x->nbr++];
        memset(b, 0, sizeof(*b));
        b->cond = c;
        b->line = f->bline = s->line;
        f->cur = &b->body;
        f->has_else |= !elif;
        f->count = 0;
        f->body = f->alt = -1; /* each branch sets its own column */
        return;
    }
    if (!strcmp(w, "END") || !strcmp(w, "ENDIF") || !strcmp(w, "ENDFOR")) {
        sq_diag(p, s->line, 1, "FWL has no %s line: a block ends where its indentation ends (delete this line)", t);
        return;
    }
    if (!strcmp(w, "FOR")) {
        char *h = sq_head(s, r, "FOR reads: FOR EACH $item IN $collection:");
        size_t k = sq_word(h, "EACH");
        const char *e = k ? sq_var(h + k, var) : NULL;
        size_t in = e ? sq_word(sq_skip(e), "IN") : 0;
        const char *coll = in ? sq_skip(sq_skip(e) + in) : "";
        sq_node *x = sq_add(s, SQ_FOR);
        if (!e || !in || !*coll) {
            sq_diag(p, s->line, 1, "FOR reads: FOR EACH $item IN $collection:");
        } else {
            x->a = sq_dup(var, strlen(var));
            x->b = sq_dup(coll, strlen(coll));
            sq_syntax(p, s->line, x->b, 1);
        }
        free(h);
        sq_open(s, B_FOR, x);
        return;
    }
    if (!strcmp(w, "WHILE")) {
        char *c = sq_head(s, r, "WHILE reads: WHILE <condition> AT MOST N TIMES:");
        sq_syntax(p, s->line, c, 0);
        sq_node *x = sq_add(s, SQ_WHILE);
        char *am = strstr(c, "AT MOST");
        if (am) {
            char *end;
            long n = strtol(am + 7, &end, 10);
            if (n < 1 || n > 1000 || !sq_word(sq_skip(end), "TIMES")) {
                sq_diag(p, s->line, 1, "WHILE ... AT MOST N TIMES: N must be 1..1000");
            }
            x->n = n > 0 ? (int)n : 0;
            while (am > c && am[-1] == ' ') {
                am--;
            }
            *am = '\0';
        } else {
            sq_diag(p, s->line, 0, "give the WHILE loop a bound: WHILE <condition> AT MOST N TIMES:");
        }
        x->a = c;
        sq_open(s, B_WHILE, x);
        return;
    }
    if (!strcmp(w, "RETRY")) {
        char *h = sq_head(s, r, "RETRY reads: RETRY UP TO N TIMES:");
        size_t k = sq_word(h, "UP");
        const char *q = k ? sq_skip(h + k) : h;
        size_t t2 = k ? sq_word(q, "TO") : 0;
        if (k && t2) {
            q = sq_skip(q + t2);
        }
        char *end;
        long n = strtol(q, &end, 10);
        if (end == q || !sq_word(sq_skip(end), "TIMES") || sq_skip(end)[5]) {
            sq_diag(p, s->line, 1, "RETRY reads: RETRY UP TO N TIMES:");
        } else if (n < 1 || n > 20) {
            sq_diag(p, s->line, 1, "RETRY UP TO %ld TIMES: N must be 1..20", n);
        }
        free(h);
        sq_node *x = sq_add(s, SQ_RETRY);
        x->n = n >= 1 && n <= 20 ? (int)n : 1;
        sq_open(s, B_RETRY, x);
        return;
    }
    if (!strcmp(w, "IN")) {
        char *h = sq_head(s, r, "IN PARALLEL reads: IN PARALLEL:");
        if (strcmp(h, "PARALLEL") != 0) {
            sq_diag(p, s->line, 1, "IN PARALLEL reads: IN PARALLEL:");
        }
        free(h);
        sq_node *x = sq_add(s, SQ_PAR);
        sq_open(s, B_PAR, x);
        return;
    }
    if (!strcmp(w, "RETURN")) {
        if (!*r) {
            sq_diag(p, s->line, 1, "RETURN needs a value: RETURN \"text\" or RETURN $x (or a plain-English summary)");
            return;
        }
        sq_syntax(p, s->line, r, 1);
        sq_node *x = sq_add(s, SQ_RETURN);
        x->a = sq_dup(r, strlen(r));
        s->returns++;
        return;
    }
    if (sq_unknown(s, w)) {
        return;
    }
    /* Two capital words that are no keyword ("SEARCH LEADS", "SEND EMAILS"): pseudo-code
     * that looks like a statement but would only reach the model as vague prose. */
    size_t k = 0;
    while (isupper((unsigned char)r[k]) || r[k] == '-') {
        k++;
    }
    if (k > 1 && !isalnum((unsigned char)r[k])) {
        sq_diag(p, s->line, 1,
                "\"%s %.*s\" is not an FWL statement: use EXECUTE, INVOKE SKILL, ASK USER, SET, IF, FOR EACH, "
                "WHILE, RETRY or RETURN, or write the step as a plain-English sentence (\"Check Gmail for replies.\")",
                w, (int)k, r);
        return;
    }
    sq_instr(s, t); /* a shouted plain-English line ("PDF files go to out/") */
}

/* Frontmatter: spec-version "fwl-1" required, title recommended. Offset after it, or 0. */
static size_t sq_front(sq_ps *s, const char *src, size_t n)
{
    sq_prog *p = s->p;
    s->line++;
    if (n < 4 || strncmp(src, "---", 3) != 0 || (src[3] != '\n' && src[3] != '\r')) {
        sq_diag(p, s->line, 1, "the frontmatter comes right after ```fwl: ---, spec-version: \"fwl-1\", title: \"...\", ---");
        return 0;
    }
    int ver = 0;
    size_t i = (size_t)(strchr(src, '\n') - src) + 1;
    while (i < n) {
        s->line++;
        const char *l = src + i;
        const char *nl = memchr(l, '\n', n - i);
        size_t ll = nl ? (size_t)(nl - l) : n - i;
        i += ll + (nl != NULL);
        if (ll >= 3 && strncmp(l, "---", 3) == 0) {
            if (!ver) {
                sq_diag(p, s->line, 1, "the frontmatter needs spec-version: \"fwl-1\"");
            }
            if (!p->title[0]) {
                sq_diag(p, s->line, 0, "the frontmatter should have a title: \"...\"");
            }
            return i;
        }
        char line[400];
        snprintf(line, sizeof(line), "%.*s", (int)(ll < 399 ? ll : 399), l);
        line[strcspn(line, "\r")] = '\0';
        char *c = strchr(line, ':');
        if (!c) {
            if (*sq_skip(line)) {
                sq_diag(p, s->line, 1, "frontmatter lines read key: value");
            }
            continue;
        }
        *c = '\0';
        const char *v = sq_skip(c + 1);
        char val[300];
        size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
            snprintf(val, sizeof(val), "%.*s", (int)(vl - 2), v + 1);
        } else {
            snprintf(val, sizeof(val), "%s", v);
        }
        if (!strcmp(line, "spec-version")) {
            ver = 1;
            if (strcmp(val, "fwl-1") != 0) {
                sq_diag(p, s->line, 1, "spec-version is \"%s\"; this botter understands \"fwl-1\"", val);
            }
        } else if (!strcmp(line, "title")) {
            snprintf(p->title, sizeof(p->title), "%.199s", val);
        } else if (strcmp(line, "description") != 0) {
            sq_diag(p, s->line, 0, "unknown frontmatter key '%s' (known: spec-version, title, description)", line);
        }
    }
    sq_diag(p, s->line, 1, "the frontmatter is not closed with ---");
    return 0;
}

int sq_parse(const char *src, size_t n, sq_prog *p)
{
    sq_ps s;
    memset(&s, 0, sizeof(s));
    s.p = p;
    s.body = s.alt = -1;
    /* The whole file is one ```fwl block: frontmatter, then statements, then the closing ```. */
    size_t first = strcspn(src, "\n");
    const char *info = sq_skip(src + 3);
    if (n < 3 || strncmp(src, "```", 3) != 0 || !sq_word(info, "fwl")) {
        sq_diag(p, 1, 1, n >= 3 && strncmp(src, "```", 3) == 0 && sq_word(info, "owl")
                             ? "Owl is now called FWL: start FLOW.md with ```fwl and use spec-version: \"fwl-1\""
                             : strncmp(src, "---", 3) == 0
                             ? "FLOW.md starts with ```fwl on its first line; the frontmatter goes inside the block"
                             : "FLOW.md starts with ```fwl on its first line and ends with ``` (the whole file is "
                               "one FWL block)");
        return p->errors;
    }
    s.line = 1;
    size_t i = first < n ? first + 1 : n;
    size_t fm = sq_front(&s, src + i, n - i);
    if (!fm) {
        return p->errors;
    }
    i += fm;
    int closed = 0;
    while (i < n) {
        s.line++;
        const char *l = src + i;
        const char *nl = memchr(l, '\n', n - i);
        size_t ll = nl ? (size_t)(nl - l) : n - i;
        i += ll + (nl != NULL);
        char buf[2048];
        if (ll >= sizeof(buf)) {
            sq_diag(p, s.line, 1, "line too long (max %zu characters)", sizeof(buf) - 1);
            continue;
        }
        memcpy(buf, l, ll);
        buf[ll] = '\0';
        buf[strcspn(buf, "\r")] = '\0';
        char *t = (char *)sq_skip(buf);
        for (size_t k = strlen(t); k && (t[k - 1] == ' ' || t[k - 1] == '\t'); k--) {
            t[k - 1] = '\0';
        }
        if (closed) {
            if (*t) {
                sq_diag(p, s.line, 1, "nothing may follow the closing ``` of the FWL block");
                break;
            }
            continue;
        }
        if (!*t) {
            continue;
        }
        int raw = (int)(t - buf);
        if (memchr(buf, '\t', (size_t)raw)) {
            sq_diag(p, s.line, 1, "indent with spaces, not tabs (indentation is what puts a line inside a block)");
        }
        if (strncmp(t, "```", 3) == 0) {
            if (t[3]) {
                sq_diag(p, s.line, 1, "a code block cannot start inside the FWL block");
                continue;
            }
            closed = 1;
            s.params = 0;
            while (s.depth) {
                sq_pop(&s);
            }
            continue;
        }
        if (*t == '#' || strncmp(t, "<!--", 4) == 0) { /* Markdown is not part of FWL */
            const char *h = t + strspn(t, "# ");
            if (*t == '#' && strncmp(h, "STEP ", 5) == 0) {
                sq_diag(p, s.line, 1, "write the step label as FWL, without #: STEP n: NAME");
            } else {
                sq_diag(p, s.line, 1, "Markdown (headings, <!-- comments -->) is not part of FWL; write plain "
                                      "English or an FWL statement");
            }
            continue;
        }
        int dash = (t[0] == '-' || t[0] == '*') && t[1] == ' ';
        if (dash) {
            t = (char *)sq_skip(t + 2);
        } else if (isdigit((unsigned char)t[0])) {
            char *e = t;
            while (isdigit((unsigned char)*e)) {
                e++;
            }
            if (*e == '.' && (e[1] == ' ' || e[1] == '\0')) {
                t = (char *)sq_skip(e + 1);
            }
        }
        if (dash && s.params && s.last) { /* "- name: value" under "with parameters:" */
            char *c = strchr(t, ':');
            if (!c || !sq_ident(t, (size_t)(c - t))) {
                sq_diag(p, s.line, 1, "a parameter line reads: - name: value");
            } else {
                const char *v = sq_skip(c + 1);
                sq_syntax(p, s.line, v, 1);
                sq_node *x = s.last;
                if (x->kind == SQ_EXEC) {
                    sq_placeholder(p, s.line, v);
                }
                x->pk = realloc(x->pk, (size_t)(x->np + 1) * sizeof(char *));
                x->pv = realloc(x->pv, (size_t)(x->np + 1) * sizeof(char *));
                x->pl = realloc(x->pl, (size_t)(x->np + 1) * sizeof(int));
                if (!x->pk || !x->pv || !x->pl) {
                    abort();
                }
                x->pl[x->np] = s.line;
                x->pk[x->np] = sq_dup(t, (size_t)(c - t));
                x->pv[x->np++] = sq_dup(v, strlen(v));
            }
            continue;
        }
        s.params = 0;
        s.col = (int)(t - buf);
        if (*t && sq_dent(&s, t, raw)) {
            sq_stmt(&s, t);
        }
    }
    if (!closed) {
        sq_diag(p, s.line, 1, "the FWL block is never closed: end the file with ```");
    }
    while (s.depth) {
        sq_pop(&s);
    }
    if (!s.stmts) {
        sq_diag(p, s.line, 1, "the flow has no statements");
    } else if (!s.returns) {
        sq_diag(p, s.line, 0, "the flow never RETURNs: end it with RETURN and what the user gets");
    }
    return p->errors;
}

static void sq_free_list(sq_list *l);

static void sq_free_node(sq_node *x)
{
    free(x->a);
    free(x->b);
    for (int i = 0; i < x->np; i++) {
        free(x->pk[i]);
        free(x->pv[i]);
    }
    free(x->pk);
    free(x->pv);
    free(x->pl);
    sq_free_list(&x->body);
    for (int i = 0; i < x->nbr; i++) {
        free(x->br[i].cond);
        sq_free_list(&x->br[i].body);
    }
    free(x->br);
    free(x);
}

static void sq_free_list(sq_list *l)
{
    for (int i = 0; i < l->n; i++) {
        sq_free_node(l->v[i]);
    }
    free(l->v);
    memset(l, 0, sizeof(*l));
}

void sq_free(sq_prog *p)
{
    sq_free_list(&p->top);
}

/* ------------------------------------------------------------ build checks */

#define SQ_MAXV 256

typedef struct {
    sq_prog *p;
    int (*has_tool)(const char *);
    int (*has_skill)(const char *);
    int (*has_agent)(const char *);
    char vars[SQ_MAXV][65];
    int  nvars;
    char dead[SQ_MAXV][65]; /* loop variables whose FOR EACH ended */
    int  ndead;
    char loaded[SQ_MAXV][65];
    int  nloaded;
} sq_cs;

static int sq_in(char (*set)[65], int n, const char *v)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(set[i], v) == 0) {
            return 1;
        }
    }
    return 0;
}

static void sq_def(sq_cs *c, const char *v)
{
    if (!sq_in(c->vars, c->nvars, v) && c->nvars < SQ_MAXV) {
        snprintf(c->vars[c->nvars++], 65, "%s", v);
    }
}

/* Every $ref in text (outside strings) must be set earlier. */
static void sq_refs(sq_cs *c, int line, const char *s)
{
    for (const char *q = s; q && *q; q++) {
        if (*q == '"') {
            const char *e = q + 1;
            while (*e && *e != '"') {
                e += *e == '\\' && e[1] ? 2 : 1;
            }
            q = *e ? e : e - 1;
            continue;
        }
        size_t dn = *q == '$' ? sq_dollar(q) : 0;
        if (!dn) {
            continue;
        }
        const char *e = q + dn - 1;
        char root[65];
        if (sq_ref(q + 1, dn - 1, root)) {
            if (sq_in(c->dead, c->ndead, root) && !sq_in(c->vars, c->nvars, root)) {
                sq_diag(c->p, line, 1, "$%s only exists inside its FOR EACH loop", root);
            } else if (!sq_in(c->vars, c->nvars, root)) {
                sq_diag(c->p, line, 1,
                        "$%s is used before it is set (SAVE ... INTO VARIABLE $%s, SET $%s TO ..., or FOR EACH "
                        "$%s IN ...)",
                        root, root, root, root);
            }
        }
        q = e;
    }
}

static void sq_check_list(sq_cs *c, sq_list *l)
{
    static const char *const implicit[] = {"result", "skill_output", "answer", "agent_output"};
    static const char *const from[] = {"EXECUTE tool", "INVOKE SKILL", "ASK USER", "DELEGATE TO"};
    for (int i = 0; i < l->n; i++) {
        sq_node *x = l->v[i];
        switch (x->kind) {
        case SQ_INSTR:
            sq_refs(c, x->line, x->a);
            break;
        case SQ_LOAD:
            if (!c->has_skill(x->a)) {
                sq_diag(c->p, x->line, 1, "IMPORT $%s: there is no skills/%s/SKILL.md in this agent", x->a, x->a);
            }
            if (sq_in(c->loaded, c->nloaded, x->a)) {
                sq_diag(c->p, x->line, 0, "skill \"%s\" is loaded twice", x->a);
            } else if (c->nloaded < SQ_MAXV) {
                snprintf(c->loaded[c->nloaded++], 65, "%.64s", x->a);
            }
            break;
        case SQ_EXEC:
            if (!c->has_tool(x->a)) {
                sq_diag(c->p, x->line, 1, "EXECUTE tool $%s: no such tool (built-in, or tools/bin/%s with tools/doc/%s.json)",
                        x->a, x->a, x->a);
            }
            sq_refs(c, x->line, x->b);
            for (int k = 0; k < x->np; k++) {
                sq_refs(c, x->pl[k], x->pv[k]);
            }
            sq_def(c, "result");
            break;
        case SQ_INVOKE:
            if (!sq_in(c->loaded, c->nloaded, x->a)) {
                sq_diag(c->p, x->line, 1, "INVOKE SKILL $%s: list $%s under IMPORT: at the top of the flow first", x->a, x->a);
            }
            sq_refs(c, x->line, x->b);
            for (int k = 0; k < x->np; k++) {
                sq_refs(c, x->pl[k], x->pv[k]);
            }
            sq_def(c, "skill_output");
            break;
        case SQ_DELEGATE:
            if (!c->has_agent(x->a)) {
                sq_diag(c->p, x->line, 1, "DELEGATE TO @%s: no such agent (a sub-agent agents/%s/agent.md, or one listed in config.json \"agents\")",
                        x->a, x->a);
            }
            sq_refs(c, x->line, x->b);
            for (int k = 0; k < x->np; k++) {
                sq_refs(c, x->pl[k], x->pv[k]);
            }
            sq_def(c, "agent_output");
            break;
        case SQ_ASK:
            sq_refs(c, x->line, x->a);
            sq_def(c, "answer");
            break;
        case SQ_SAVE_VAR:
        case SQ_SAVE_FILE:
            for (int k = 0; k < 4; k++) {
                size_t n = strlen(implicit[k]);
                if (strncmp(x->a, implicit[k], n) == 0 && (x->a[n] == ' ' || x->a[n] == '\0' || x->a[n] == '.') &&
                    !sq_in(c->vars, c->nvars, implicit[k])) {
                    sq_diag(c->p, x->line, 1, "SAVE %s: there is no %s yet (it comes from %s)", implicit[k], implicit[k],
                            from[k]);
                }
            }
            sq_refs(c, x->line, x->a);
            if (x->kind == SQ_SAVE_VAR) {
                sq_def(c, x->b);
            }
            break;
        case SQ_SET:
            sq_refs(c, x->line, x->b);
            sq_def(c, x->a);
            break;
        case SQ_IF:
            for (int k = 0; k < x->nbr; k++) {
                if (x->br[k].cond) {
                    sq_refs(c, x->br[k].line, x->br[k].cond);
                }
                sq_check_list(c, &x->br[k].body);
            }
            break;
        case SQ_FOR:
            sq_refs(c, x->line, x->b);
            if (x->a) {
                int had = sq_in(c->vars, c->nvars, x->a);
                sq_def(c, x->a);
                sq_check_list(c, &x->body);
                if (!had) { /* the loop variable ends with its loop */
                    for (int k = 0; k < c->nvars; k++) {
                        if (strcmp(c->vars[k], x->a) == 0) {
                            memmove(c->vars[k], c->vars[k + 1], (size_t)(c->nvars - k - 1) * 65);
                            c->nvars--;
                            break;
                        }
                    }
                    if (c->ndead < SQ_MAXV) {
                        snprintf(c->dead[c->ndead++], 65, "%s", x->a);
                    }
                }
            } else {
                sq_check_list(c, &x->body);
            }
            break;
        case SQ_WHILE:
            sq_refs(c, x->line, x->a);
            sq_check_list(c, &x->body);
            break;
        case SQ_RETRY:
        case SQ_PAR:
            sq_check_list(c, &x->body);
            break;
        case SQ_RETURN:
            sq_refs(c, x->line, x->a);
            break;
        }
    }
}

void sq_check(sq_prog *p, int (*has_tool)(const char *), int (*has_skill)(const char *), int (*has_agent)(const char *))
{
    sq_cs *c = sq_alloc(sizeof(*c));
    c->p = p;
    c->has_tool = has_tool;
    c->has_skill = has_skill;
    c->has_agent = has_agent;
    sq_check_list(c, &p->top);
    free(c);
}
