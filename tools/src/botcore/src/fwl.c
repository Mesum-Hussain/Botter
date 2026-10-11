/*
 * FWL front end: parser (syntax tree, syntax errors, which sentence fills which value) and
 * build-time checks. Shared by botcore (fwl_run.c runs the tree) and botter_pack (which
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
#define SQ_MAXV 256

static const char *const SQ_BLOCK[] = {"", "IF", "FOR EACH", "WHILE", "RETRY"};
enum { B_IF = 1, B_FOR, B_WHILE, B_RETRY };

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

static size_t sq_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncasecmp(s, w, n) == 0 && !isalnum((unsigned char)s[n]) && s[n] != '_' ? n : 0;
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

/* {name}, {name.field}, {list[0]} at c: its length, or 0. */
size_t sq_val(const char *c)
{
    char root[65];
    const char *e = c[0] == '{' ? strchr(c, '}') : NULL;
    return e && e - c <= 200 && sq_ref(c + 1, (size_t)(e - c - 1), root) ? (size_t)(e - c) + 1 : 0;
}

int sq_marks(const char *s, char mark, char out[][65], int max)
{
    int n = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '"') { /* text in quotes is said, not meant */
            const char *q = c + 1;
            while (*q && *q != '"') {
                q += *q == '\\' && q[1] ? 2 : 1;
            }
            if (!*q) {
                break;
            }
            c = q;
        } else if (*c == mark && (c == s || !isalnum((unsigned char)c[-1])) && (isalpha((unsigned char)c[1]) || c[1] == '_')) {
            size_t l = 1;
            while (isalnum((unsigned char)c[l]) || c[l] == '_' || c[l] == '-') {
                l++;
            }
            while (c[l - 1] == '-') {
                l--;
            }
            int dup = 0;
            for (int i = 0; i < n; i++) {
                dup |= strlen(out[i]) == l - 1 && !strncmp(out[i], c + 1, l - 1);
            }
            if (!dup && n < max && l <= 65) {
                snprintf(out[n++], 65, "%.*s", (int)(l - 1), c + 1);
            }
            c += l - 1;
        }
    }
    return n;
}

/* Strings closed, and no names in the old spellings. 0 if fine. */
static int sq_syntax(sq_prog *p, int line, const char *s)
{
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
        } else if (*c == '`') {
            const char *q = strchr(c + 1, '`');
            char root[65];
            if (q && sq_ref(c + 1, (size_t)(q - c - 1), root)) {
                sq_diag(p, line, 1, "write {%.*s}, not `%.*s`: a remembered value goes in { }", (int)(q - c - 1), c + 1,
                        (int)(q - c - 1), c + 1);
                return -1;
            }
        }
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
        } else if (sq_val(c)) {
            c += sq_val(c);
            any = 1;
        } else if (*c == ' ' || *c == '\t' || *c == '+') {
            c++;
        } else { /* a number, true, false or null; anything else is prose */
            size_t l = strcspn(c, " \t+");
            char w[32], *end;
            snprintf(w, sizeof(w), "%.*s", (int)(l < 31 ? l : 31), c);
            strtod(w, &end);
            if (l > 31 || (*end && strcmp(w, "true") && strcmp(w, "false") && strcmp(w, "null"))) {
                return 0;
            }
            c += l;
            any = 1;
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
    int      col;       /* text column of the current line */
    int      body, alt; /* as in sq_frame, for the lines outside every block */
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
    s->stmts += kind != SQ_STEP;
    return x;
}

static void sq_name(sq_node *x, const char *s, size_t n)
{
    x->pk = realloc(x->pk, (size_t)(x->np + 1) * sizeof(char *));
    if (!x->pk) {
        abort();
    }
    x->pk[x->np++] = sq_dup(s, n);
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
    f->cur = bkind == B_IF ? &x->br[0].body : &x->body;
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
    int is_else = sq_kw_at(t, "ELSE") || sq_kw_at(t, "ELIF");
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
    if (sq_kw_at(t, "END")) {
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
                    SQ_BLOCK[f->kind], f->line, SQ_BLOCK[f->kind]);
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
    for (int i = 0; i < 2; i++) {
        const char *old = i ? " DO" : " THEN";
        size_t k = strlen(old);
        if (n > k && !strncmp(r + n - k, old, k)) {
            sq_diag(s->p, s->line, 1, "%s  (no%s: the ':' opens the block)", reads, old);
            return sq_dup(r, n - k);
        }
    }
    if (!ok) {
        sq_diag(s->p, s->line, 1, "%s  (the ':' opens the block; the lines indented under it belong to it)", reads);
    }
    return sq_dup(r, n);
}

/* "at most N times" / "N times" at the end of h: N, with h cut before it; 0 if absent. */
static long sq_times(char *h, int need_at_most)
{
    char *t = strcasestr(h, need_at_most ? "at most" : "times");
    if (need_at_most && t) {
        char *end;
        long n = strtol(t + 7, &end, 10);
        if (strncasecmp(sq_skip(end), "times", 5) || sq_skip(end)[5]) {
            return -1;
        }
        while (t > h && (t[-1] == ' ' || t[-1] == ',' || t[-1] == '(')) {
            t--;
        }
        *t = '\0';
        return n ? n : -1;
    }
    return 0;
}

/* A capital word that used to be a statement, or looks like one: what to write instead. 1 if reported. */
static int sq_gone(sq_ps *s, const char *w, const char *r)
{
    static const char *const old[][2] = {
        {"EXECUTE", "say it in a sentence that names the tool, e.g. \"Find leads for {target} with $scrape_leads and keep them as {leads}.\""},
        {"RUN", "say it in a sentence that names the tool, e.g. \"Find leads for {target} with $scrape_leads and keep them as {leads}.\""},
        {"INVOKE", "say it in a sentence that names the skill, e.g. \"Write a pitch for {lead} using #pitching.\""},
        {"DELEGATE", "say it in a sentence that names the agent, e.g. \"Have @reviewer check {pitch} and keep its verdict as {verdict}.\""},
        {"SAVE", "name the value in the sentence that produces it, e.g. \"... and keep them as {leads}.\" (to write a file, say so in a sentence)"},
        {"SET", "name the value in a sentence, e.g. \"Work out the city named in {clients} and call it {city}.\""},
        {"IMPORT", "nothing is imported any more: delete this line and name skills where they are used, as #skill"},
        {"LOAD", "nothing is imported any more: delete this line and name skills where they are used, as #skill"},
        {"CONNECT", "list MCP servers in config.json \"mcp\"; their tools are then named $server_tool in a sentence"},
        {"ELIF", "write ELSE IF <condition>:"},
        {"BREAK", "to stop a loop early, give WHILE a condition; to end the flow, RETURN"},
        {"CONTINUE", "to skip the rest of an item, put it inside an IF"},
        {"GOTO", "there is no jumping: use IF, FOR EACH and WHILE"},
        {"IN", "IN PARALLEL is gone: the steps of a flow run one after another"},
    };
    for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
        if (!strcmp(w, old[i][0])) {
            sq_diag(s->p, s->line, 1, "FWL has no %s: %s", w, old[i][1]);
            return 1;
        }
    }
    if (!strcmp(w, "END")) {
        sq_diag(s->p, s->line, 1, "FWL has no END line: a block ends where its indentation ends (delete this line)");
        return 1;
    }
    static const char *const kw[] = {"STEP", "ASK", "IF", "ELSE", "FOR", "WHILE", "RETRY", "RETURN", NULL};
    for (int i = 0; kw[i] && strlen(w) >= 4; i++) {
        if (strlen(kw[i]) >= 4 && sq_lev(w, kw[i]) <= 2) {
            sq_diag(s->p, s->line, 1, "unknown keyword %s; did you mean %s?", w, kw[i]);
            return 1;
        }
    }
    size_t k = 0; /* a capital word that is no keyword (CALCULATE, SAY, SEND EMAILS): made-up FWL, vague to the model */
    while (isupper((unsigned char)r[k]) || r[k] == '-') {
        k++;
    }
    k = k > 1 && !isalnum((unsigned char)r[k]) ? k : 0;
    sq_diag(s->p, s->line, 1,
            "\"%s%s%.*s\" is not FWL: only ASK, IF, ELSE, FOR EACH, WHILE, RETRY, STEP and RETURN are written in "
            "capitals; write the step as a plain sentence that starts with an ordinary word (\"Check the inbox for "
            "replies.\")",
            w, k ? " " : "", (int)k, r);
    return 1;
}

static void sq_sentence(sq_ps *s, const char *t)
{
    static const char *const prose[] = {"if ", "for each ", "while ", "else", "return ", "retry ", NULL};
    static const char *const fix[] = {"IF <condition>:", "FOR EACH {x} in {list}:", "WHILE <condition>, at most N times:",
                                      "ELSE:", "RETURN", "RETRY N times:"};
    for (int i = 0; prose[i]; i++) {
        if (!strncmp(t, prose[i], strlen(prose[i]))) {
            sq_diag(s->p, s->line, 0, "write '%.*s' as FWL structure in capitals (%s) so the flow follows it exactly",
                    (int)strcspn(prose[i], " "), prose[i], fix[i]);
            break;
        }
    }
    sq_syntax(s->p, s->line, t);
    sq_node *x = sq_add(s, SQ_INSTR);
    x->a = sq_dup(t, strlen(t));
}

/* ASK <question> [choice, "choice two"] into {name} */
static void sq_ask(sq_ps *s, const char *r)
{
    static const char READS[] = "ASK reads: ASK \"question\" [choice, choice] into {name}  (the choices and 'into' are optional)";
    char *q = sq_dup(r, strlen(r)), root[65];
    size_t n = strlen(q);
    sq_node *x = sq_add(s, SQ_ASK);
    char *into = NULL;
    for (char *c = q; (c = strcasestr(c, " into ")); c++) {
        into = c;
    }
    if (into && sq_val(sq_skip(into + 6)) && !sq_skip(into + 6)[sq_val(sq_skip(into + 6))]) {
        const char *v = sq_skip(into + 6);
        if (!sq_ref(v + 1, sq_val(v) - 2, root) || strlen(root) != sq_val(v) - 2) {
            sq_diag(s->p, s->line, 1, "ASK ... into {name}: one plain name (letters, digits, _)");
        }
        x->b = sq_dup(root, strlen(root));
        *into = '\0';
        n = strlen(q);
    }
    while (n && q[n - 1] == ' ') {
        q[--n] = '\0';
    }
    char *lb = n && q[n - 1] == ']' ? strrchr(q, '[') : NULL;
    if (lb && lb > q && lb[-1] == ' ') { /* the choices */
        q[n - 1] = '\0';
        for (char *c = lb + 1; *c;) {
            c += strspn(c, " ,");
            size_t l = *c == '"' ? strcspn(c + 1, "\"") : strcspn(c, ",");
            const char *v = *c == '"' ? c + 1 : c;
            size_t vl = l;
            while (vl && v[vl - 1] == ' ') {
                vl--;
            }
            if (vl) {
                sq_name(x, v, vl);
            }
            c += l + (*c == '"');
            c += *c == '"';
        }
        if (x->np < 2) {
            sq_diag(s->p, s->line, 1, "ASK [...] needs at least two choices, separated by commas");
        }
        *lb = '\0';
        n = strlen(q);
        while (n && q[n - 1] == ' ') {
            q[--n] = '\0';
        }
    }
    if (!strncmp(q, "USER ", 5) || !*q) {
        sq_diag(s->p, s->line, 1, "%s", READS);
    }
    sq_syntax(s->p, s->line, q);
    x->a = q;
}

/* One statement (list marker and indentation removed). */
static void sq_stmt(sq_ps *s, char *t)
{
    sq_prog *p = s->p;
    char w[32] = "", var[65];
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
    if (!upper) {
        sq_sentence(s, t);
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
            sq_diag(p, s->line, 0, "a STEP inside an open %s block (opened on line %d)", SQ_BLOCK[s->st[s->depth - 1].kind],
                    s->st[s->depth - 1].line);
        }
        s->step = (int)k;
        sq_node *x = sq_add(s, SQ_STEP);
        x->a = sq_dup(sq_skip(e + 1), strlen(sq_skip(e + 1)));
        x->n = (int)k;
    } else if (!strcmp(w, "ASK")) {
        sq_ask(s, r);
    } else if (!strcmp(w, "IF")) {
        char *c = sq_head(s, r, "IF reads: IF <condition>:");
        if (!*c) {
            sq_diag(p, s->line, 1, "IF needs a condition: IF <condition>:");
        }
        sq_syntax(p, s->line, c);
        sq_node *x = sq_add(s, SQ_IF);
        x->br = sq_alloc(sizeof(*x->br));
        x->nbr = 1;
        x->br[0].cond = c;
        x->br[0].line = s->line;
        sq_open(s, B_IF, x);
    } else if (!strcmp(w, "ELSE")) {
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
            sq_syntax(p, s->line, c);
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
    } else if (!strcmp(w, "FOR")) {
        static const char READS[] = "FOR reads: FOR EACH {item} in {list}:";
        char *h = sq_head(s, r, READS);
        size_t k = sq_word(h, "EACH");
        const char *v = k ? sq_skip(h + k) : "";
        size_t vl = sq_val(v), in = vl ? sq_word(sq_skip(v + vl), "IN") : 0;
        const char *coll = in ? sq_skip(sq_skip(v + vl) + in) : "";
        sq_node *x = sq_add(s, SQ_FOR);
        if (!in || !*coll || !sq_ref(v + 1, vl - 2, var) || strlen(var) != vl - 2) {
            sq_diag(p, s->line, 1, "%s", READS);
        } else {
            x->a = sq_dup(var, strlen(var));
            x->b = sq_dup(coll, strlen(coll));
            sq_syntax(p, s->line, x->b);
        }
        free(h);
        sq_open(s, B_FOR, x);
    } else if (!strcmp(w, "WHILE")) {
        char *c = sq_head(s, r, "WHILE reads: WHILE <condition>, at most N times:");
        long n = sq_times(c, 1);
        sq_syntax(p, s->line, c);
        sq_node *x = sq_add(s, SQ_WHILE);
        if (n < 0 || n > 1000) {
            sq_diag(p, s->line, 1, "WHILE ..., at most N times: N must be 1..1000");
        } else if (!n) {
            sq_diag(p, s->line, 0, "give the WHILE loop a bound: WHILE <condition>, at most N times:");
        }
        x->n = n > 0 && n <= 1000 ? (int)n : 0;
        x->a = c;
        sq_open(s, B_WHILE, x);
    } else if (!strcmp(w, "RETRY")) {
        char *h = sq_head(s, r, "RETRY reads: RETRY N times:"), *end;
        const char *q = strncasecmp(h, "up to ", 6) ? h : h + 6;
        long n = strtol(q, &end, 10);
        if (end == q || strcasecmp(sq_skip(end), "times")) {
            sq_diag(p, s->line, 1, "RETRY reads: RETRY N times:");
        } else if (n < 1 || n > 20) {
            sq_diag(p, s->line, 1, "RETRY %ld times: N must be 1..20", n);
        }
        free(h);
        sq_node *x = sq_add(s, SQ_RETRY);
        x->n = n >= 1 && n <= 20 ? (int)n : 1;
        sq_open(s, B_RETRY, x);
    } else if (!strcmp(w, "RETURN")) {
        if (!*r) {
            sq_diag(p, s->line, 1, "RETURN needs what the user gets: RETURN \"text\", or a sentence saying what to tell them");
            return;
        }
        sq_syntax(p, s->line, r);
        sq_node *x = sq_add(s, SQ_RETURN);
        x->a = sq_dup(r, strlen(r));
        s->returns++;
    } else {
        sq_gone(s, w, r);
    }
}

/* --------------------------------------------- which sentence fills which value */

typedef struct {
    sq_prog *p;
    char     name[SQ_MAXV][65];
    int      line[SQ_MAXV], uses[SQ_MAXV], dead[SQ_MAXV], n;
} sq_bs;

static int sq_find(sq_bs *b, const char *name)
{
    for (int i = 0; i < b->n; i++) {
        if (!strcmp(b->name[i], name)) {
            return i;
        }
    }
    return -1;
}

static int sq_define(sq_bs *b, const char *name, int line)
{
    int i = sq_find(b, name);
    if (i < 0 && b->n < SQ_MAXV) {
        i = b->n++;
        snprintf(b->name[i], 65, "%s", name);
        b->line[i] = line;
        b->uses[i] = 0;
    }
    if (i >= 0) {
        b->dead[i] = 0;
    }
    return i;
}

/* Every {value} in text: known ones are used; new ones are filled here (fill) or an error. */
static void sq_values(sq_bs *b, int line, const char *s, sq_node *fill)
{
    for (const char *c = s; s && *c; c++) {
        size_t n = sq_val(c);
        char root[65];
        if (!n || !sq_ref(c + 1, n - 2, root)) {
            continue;
        }
        int i = sq_find(b, root);
        if (i >= 0 && b->dead[i]) {
            sq_diag(b->p, line, 1, "{%s} only exists inside its FOR EACH loop", root);
        } else if (i >= 0) {
            b->uses[i] += !(fill && b->line[i] == line);
        } else if (fill) {
            sq_define(b, root, line);
            sq_name(fill, root, strlen(root));
        } else {
            sq_diag(b->p, line, 1, "{%s} has no value here: a sentence or an ASK above must fill it first (a value is "
                                   "filled by the first sentence that mentions it)", root);
        }
        c += n - 1;
    }
}

static void sq_bind(sq_bs *b, sq_list *l)
{
    for (int i = 0; i < l->n; i++) {
        sq_node *x = l->v[i];
        switch (x->kind) {
        case SQ_INSTR:
            sq_values(b, x->line, x->a, x);
            break;
        case SQ_ASK:
            sq_values(b, x->line, x->a, NULL);
            sq_define(b, x->b ? x->b : "answer", x->line);
            break;
        case SQ_IF:
            for (int k = 0; k < x->nbr; k++) {
                sq_values(b, x->br[k].line, x->br[k].cond, NULL);
                sq_bind(b, &x->br[k].body);
            }
            break;
        case SQ_FOR: {
            sq_values(b, x->line, x->b, NULL);
            int v = x->a ? sq_define(b, x->a, x->line) : -1;
            sq_bind(b, &x->body);
            if (v >= 0) {
                b->dead[v] = 1;
                b->uses[v]++; /* the loop itself uses it */
            }
            break;
        }
        case SQ_WHILE:
            sq_bind(b, &x->body); /* the loop's own steps may fill what its condition reads */
            sq_values(b, x->line, x->a, NULL);
            break;
        case SQ_RETRY:
            sq_bind(b, &x->body);
            break;
        case SQ_RETURN:
            sq_values(b, x->line, x->a, NULL);
            break;
        }
    }
}

/* Optional frontmatter (--- title: "..." description: "..." ---). Bytes it takes; lines counted in s. */
static size_t sq_front(sq_ps *s, const char *src, size_t n)
{
    if (n < 4 || strncmp(src, "---", 3) != 0) {
        return 0;
    }
    const char *end = NULL;
    for (const char *l = memchr(src, '\n', n); l && l + 1 < src + n; l = memchr(l + 1, '\n', (size_t)(src + n - l - 1))) {
        s->line++;
        if (!strncmp(l + 1, "---", 3)) {
            end = memchr(l + 1, '\n', (size_t)(src + n - l - 1));
            end = end ? end + 1 : src + n;
            break;
        }
        if (!strncmp(l + 1, "title:", 6)) {
            const char *t = sq_skip(l + 7);
            t += *t == '"';
            snprintf(s->p->title, sizeof(s->p->title), "%.*s", (int)strcspn(t, "\"\r\n"), t);
        }
    }
    if (!end) {
        sq_diag(s->p, 1, 1, "the frontmatter that starts with --- on line 1 is never closed: end it with ---");
        return n;
    }
    s->line++;
    return (size_t)(end - src);
}

int sq_parse(const char *src, size_t n, sq_prog *p)
{
    sq_ps s;
    memset(&s, 0, sizeof(s));
    s.p = p;
    s.body = s.alt = -1;
    if (n >= 3 && !strncmp(src, "```", 3)) {
        sq_diag(p, 1, 1, "FLOW.fwl is plain FWL: no ``` fence around it and no spec-version (start with the first line of the flow)");
        return p->errors;
    }
    size_t i = sq_front(&s, src, n);
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
        if (!*t) {
            continue;
        }
        int raw = (int)(t - buf);
        if (memchr(buf, '\t', (size_t)raw)) {
            sq_diag(p, s.line, 1, "indent with spaces, not tabs (indentation is what puts a line inside a block)");
        }
        if (!strncmp(t, "```", 3) || !strncmp(t, "<!--", 4) || (t[0] == '#' && (t[1] == ' ' || t[1] == '#'))) {
            sq_diag(p, s.line, 1, "Markdown (``` fences, # headings, <!-- comments -->) is not part of FWL: write a "
                                  "sentence, or STEP n: NAME for a heading");
            continue;
        }
        if ((t[0] == '-' || t[0] == '*') && t[1] == ' ') {
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
        s.col = (int)(t - buf);
        if (*t && sq_dent(&s, t, raw)) {
            sq_stmt(&s, t);
        }
    }
    while (s.depth) {
        sq_pop(&s);
    }
    if (!s.stmts) {
        sq_diag(p, s.line ? s.line : 1, 1, "the flow has no steps");
    }
    sq_bs *b = sq_alloc(sizeof(*b));
    b->p = p;
    sq_bind(b, &p->top);
    for (int k = 0; k < b->n; k++) {
        if (!b->uses[k]) {
            sq_diag(p, b->line[k], 0, "{%s} is filled here and never used again: a typo, or drop the name", b->name[k]);
        }
    }
    free(b);
    return p->errors;
}

static void sq_free_list(sq_list *l);

static void sq_free_node(sq_node *x)
{
    free(x->a);
    free(x->b);
    for (int i = 0; i < x->np; i++) {
        free(x->pk[i]);
    }
    free(x->pk);
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

typedef struct {
    sq_prog *p;
    int (*has[3])(const char *);
} sq_cs;

static void sq_check_text(sq_cs *c, int line, const char *s)
{
    static const char marks[] = "$#@";
    static const char *const what[] = {"no such tool (built-in, tools/bin/%s, or <server>_<tool> of a config.json \"mcp\" server)",
                                       "no such skill (there is no skills/%s/SKILL.md)",
                                       "no such agent (a sub-agent agents/%s/agent.md, or one listed in config.json \"agents\")"};
    for (int m = 0; s && m < 3; m++) {
        char names[16][65], why[200];
        int n = sq_marks(s, marks[m], names, 16);
        for (int i = 0; i < n; i++) {
            if (!c->has[m](names[i])) {
                snprintf(why, sizeof(why), what[m], names[i]);
                sq_diag(c->p, line, 1, "%c%s: %s", marks[m], names[i], why);
            }
        }
    }
}

static void sq_check_list(sq_cs *c, sq_list *l)
{
    for (int i = 0; i < l->n; i++) {
        sq_node *x = l->v[i];
        sq_check_text(c, x->line, x->kind == SQ_FOR ? x->b : x->kind == SQ_STEP ? NULL : x->a);
        for (int k = 0; k < x->nbr; k++) {
            sq_check_text(c, x->br[k].line, x->br[k].cond);
            sq_check_list(c, &x->br[k].body);
        }
        sq_check_list(c, &x->body);
    }
}

void sq_check(sq_prog *p, int (*has_tool)(const char *), int (*has_skill)(const char *), int (*has_agent)(const char *))
{
    sq_cs c = {p, {has_tool, has_skill, has_agent}};
    sq_check_list(&c, &p->top);
}
