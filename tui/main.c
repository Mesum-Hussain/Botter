/*
 * botter TUI: the chat front end for the botter agent, inline in the terminal.
 *
 * The agent itself (botcore + botter pack, a normal .bot) runs as a child in frontend mode
 * (BOTCORE_FRONTEND=1, see botcore src/front.h); this program owns the terminal. The .bot is appended to
 * this executable by `botter-tui --bundle <agent.bot> <out>`, or given as argv[1] while developing. It is
 * executed from a memfd, so agents that botter builds get the plain botcore runtime (and its REPL).
 *
 * No screen of its own: what has been said is printed once into the terminal's scrollback (so scrolling,
 * selecting and copying are the terminal's), and only the live part at the bottom is redrawn: a reply being
 * streamed, the spinner, the input box and a hint line. Colours are the terminal's own (default, dim, bold
 * and its 16 theme colours); only the logo has fixed colours.
 *
 * Not a tty, or BOTTER_TUI=0: the agent is executed directly (plain REPL).
 */
#define _GNU_SOURCE
#include "tui.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef BOTTER_VERSION
#define BOTTER_VERSION "dev"
#endif

extern char **environ;

/* ================= terminal ================= */

static int            g_tty = -1; /* output fd (the terminal) */
static struct termios g_saved;
static int            g_raw;
static int            g_W = 80, g_H = 24;
static int            g_truecolor; /* for the logo only */
static int            g_sigpipe[2] = {-1, -1};
static volatile sig_atomic_t g_winch, g_quit;
static int            g_live_up; /* rows from the top of the live area down to the cursor */

static void write_all(const char *s, size_t n)
{
    for (size_t off = 0; off < n;) {
        ssize_t w = write(g_tty, s + off, n - off);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return;
        }
        off += (size_t)w;
    }
}

/* Back to the terminal as it was; the live area is wiped, the conversation stays in the scrollback. */
static void tty_restore(void)
{
    if (g_raw) {
        char s[80];
        int n = g_live_up ? snprintf(s, sizeof(s), "\033[%dA", g_live_up) : 0; /* to the top of the live area */
        n += snprintf(s + n, sizeof(s) - (size_t)n, "\r\033[J\033[0m\033[?2004l\033[?25h");
        write_all(s, (size_t)n);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = 0;
    }
}

static void on_crash(int sig)
{
    tty_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH) {
        g_winch = 1;
    } else {
        g_quit = 1;
    }
    int e = errno;
    (void)!write(g_sigpipe[1], "x", 1);
    errno = e;
}

static void tty_size(void)
{
    struct winsize ws;
    if (ioctl(g_tty, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_W = ws.ws_col;
        g_H = ws.ws_row;
    }
}

static int tty_setup(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        return -1;
    }
    struct termios t = g_saved;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) {
        return -1;
    }
    g_raw = 1;
    write_all("\033[?2004h", 8); /* bracketed paste */
    atexit(tty_restore);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_crash;
    sa.sa_flags = SA_RESETHAND;
    int crash[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    for (size_t i = 0; i < sizeof(crash) / sizeof(crash[0]); i++) {
        sigaction(crash[i], &sa, NULL);
    }
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    tty_size();
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ================= styled output ================= */

/* 0xRRGGBB on a 256-colour terminal (the logo). */
static int rgb256(uint32_t c)
{
    int r = (int)(c >> 16) & 255, g = (int)(c >> 8) & 255, b = (int)c & 255;
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
#define Q(v) ((v) < 48 ? 0 : (v) < 115 ? 1 : ((v) - 35) / 40)
    int qr = Q(r), qg = Q(g), qb = Q(b);
#undef Q
    int dc = (r - lv[qr]) * (r - lv[qr]) + (g - lv[qg]) * (g - lv[qg]) + (b - lv[qb]) * (b - lv[qb]);
    int gi = (r + g + b) / 3 > 238 ? 23 : ((r + g + b) / 3 - 3) / 10;
    gi = gi < 0 ? 0 : gi;
    int gv = 8 + gi * 10;
    int dg = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);
    return dg < dc ? 232 + gi : 16 + 36 * qr + 6 * qg + qb;
}

static void put_color(tsb_t *o, int fg, uint32_t c)
{
    if (c == TC_NONE || c == TC_DIM) {
        return; /* the reset before it left the default */
    }
    if ((c & 0xFF000000u) == 0xFD000000u) {
        int n = (int)(c & 15);
        tsb_fmt(o, ";%d", (fg ? 30 : 40) + (n & 7) + (n > 7 ? 60 : 0));
    } else if (g_truecolor) {
        tsb_fmt(o, ";%d;2;%u;%u;%u", fg ? 38 : 48, (c >> 16) & 255, (c >> 8) & 255, c & 255);
    } else {
        tsb_fmt(o, ";%d;5;%d", fg ? 38 : 48, rgb256(c));
    }
}

static void sgr(tsb_t *o, tsty_t s)
{
    tsb_str(o, "\033[0");
    if (s.at & TA_BOLD) {
        tsb_str(o, ";1");
    }
    if (s.fg == TC_DIM) {
        tsb_str(o, ";2");
    }
    if (s.at & TA_ITALIC) {
        tsb_str(o, ";3");
    }
    if (s.at & TA_UNDER) {
        tsb_str(o, ";4");
    }
    if (s.at & TA_STRIKE) {
        tsb_str(o, ";9");
    }
    put_color(o, 1, s.fg);
    put_color(o, 0, s.bg);
    tsb_str(o, "m");
}

/* One laid-out line, cut at the terminal's width (a wrapped row would throw off the redraw). */
static void put_line(tsb_t *o, const tline_t *l)
{
    int w = 0;
    for (int i = 0; i < l->n && w < g_W; i++) {
        const trun_t *r = &l->r[i];
        sgr(o, r->s);
        if (w + r->w <= g_W) {
            tsb_str(o, r->t);
            w += r->w;
            continue;
        }
        size_t n = strlen(r->t);
        for (size_t k = 0; k < n;) {
            int len, cw = tu_width(tu_decode(r->t + k, n - k, &len));
            if (w + cw > g_W) {
                break;
            }
            tsb_add(o, r->t + k, (size_t)len);
            w += cw;
            k += (size_t)len;
        }
    }
    tsb_str(o, "\033[0m");
}

/* ================= the conversation ================= */

static tlines_t g_out;           /* finished lines not yet printed into the scrollback */
static int      g_gap = 1;       /* the last line printed is blank (no second blank line) */
static int      g_expand;        /* ctrl+o: tool calls, results and reasoning are printed too */
static int      g_plan;          /* botcore's mode: 0 = Build, 1 = Plan */
static int      g_plan_want = -1; /* tab while busy: the mode to ask for at the next chat prompt */
static tsb_t    g_stream, g_think; /* a reply / reasoning being streamed */
static tsb_t    g_rawl;          /* the agent's plain output: an unfinished line */
static tsb_t    g_rx;            /* bytes from the agent not handled yet */
static int      g_dirty = 1;

static int text_w(void) { return g_W > 12 ? g_W - 4 : 8; }

static void out_blank(void)
{
    if (!g_gap) {
        tl_blank(&g_out);
        g_gap = 1;
    }
}

/* Lines into the scrollback, each after a prefix (the first line may have its own). */
static void out_lines(tlines_t *L, tsty_t ps, const char *first, const char *rest)
{
    if (L->n) {
        tl_prefix(L, 0, ps, first, ps, rest);
        tl_move(&g_out, L);
        g_gap = 0;
    }
    g_dirty = 1;
}

static void out_text(const char *t, tsty_t s, tsty_t ps, const char *first, const char *rest)
{
    tlines_t L = {0};
    lay_text(&L, t, strlen(t), s, text_w(), TC_NONE);
    out_lines(&L, ps, first, rest);
}

/* Left-truncate ("…tail") into w columns. */
static void put_tail(tline_t *l, tsty_t s, const char *t, int w)
{
    int tw = tu_strwidth(t, strlen(t)), cut = tw > w;
    while (cut && tw > w - 1 && *t) {
        int len;
        tw -= tu_width(tu_decode(t, strlen(t), &len));
        t += len;
    }
    if (cut) {
        tl_put(l, s, "…", strlen("…"));
    }
    tl_put(l, s, t, strlen(t));
}

/* The logo with who and where beside it, at the start (botcore then says which model it uses). */
static void out_header(void)
{
    tlines_t logo = {0}, info = {0}, L = {0};
    lay_logo(&logo);
    int lw = logo_width(), side = g_W >= lw + 30, tw = side ? g_W - lw - 5 : g_W - 2;
    tsty_t mut = sty(T.muted, 0), txt = sty(T.text, 0);
    char cwd[4096];
    tl_blank(&info);
    tline_t *l = tl_new(&info, TC_NONE);
    tl_put(l, sty(T.text, TA_BOLD), "Botter", 6);
    tl_put(l, mut, " " BOTTER_VERSION, strlen(" " BOTTER_VERSION));
    tl_put_trunc(tl_new(&info, TC_NONE), mut, "the agent that builds agents", tw);
    tl_blank(&info);
    put_tail(tl_new(&info, TC_NONE), mut, getcwd(cwd, sizeof(cwd)) ? cwd : "?", tw);
    tl_put_trunc(tl_new(&info, TC_NONE), mut, "enter send · shift+enter or \\ new line · esc interrupt", tw);
    int h = side ? (logo.n > info.n ? logo.n : info.n) : logo.n + info.n;
    for (int y = 0; y < h; y++) {
        l = tl_new(&L, TC_NONE);
        if (side) {
            if (y < logo.n) {
                tl_cat(l, &logo.l[y]);
            }
            tl_pad(l, lw + 3, txt);
            if (y < info.n) {
                tl_cat(l, &info.l[y]);
            }
        } else {
            tl_cat(l, y < logo.n ? &logo.l[y] : &info.l[y - logo.n]);
        }
    }
    tl_free(&logo);
    tl_free(&info);
    out_lines(&L, txt, " ", " ");
    out_blank();
}

/* Plain output of the agent (menus, messages before the model is connected): finished lines go out. */
static void raw_text(const char *s, size_t n)
{
    tsb_add(&g_rawl, s, n);
    char *nl;
    while (g_rawl.p && (nl = memchr(g_rawl.p, '\n', g_rawl.len))) {
        size_t k = (size_t)(nl - g_rawl.p);
        tlines_t L = {0};
        lay_ansi(&L, g_rawl.p, k, text_w());
        if (!L.n || (L.n == 1 && L.l[0].w == 0)) { /* nothing visible on it */
            tl_free(&L);
            out_blank();
        } else {
            out_lines(&L, sty(T.text, 0), "  ", "  ");
        }
        memmove(g_rawl.p, nl + 1, g_rawl.len - k);
        g_rawl.len -= k + 1;
    }
    g_dirty = 1;
}

static void flush_raw(void)
{
    if (g_rawl.len) {
        raw_text("\n", 1);
    }
}

/* A finished reply (markdown). */
static void out_reply(const char *md)
{
    tlines_t L = {0};
    lay_markdown(&L, md, text_w());
    out_blank();
    out_lines(&L, sty(T.text, 0), "  ", "  ");
}

/* Reasoning, shown only with ctrl+o. */
static void out_think(const char *t)
{
    if (g_expand && t[strspn(t, " \n\t")]) {
        out_text(t, sty(T.muted, TA_ITALIC), sty(T.muted, 0), "  ✻ ", "    ");
    }
}

/* ================= the agent process ================= */

static pid_t  g_child = -1;
static int    g_cin = -1, g_cout = -1;
static int    g_await;  /* the agent waits for a line */
static int    g_chat;   /* ... and it is the chat prompt (">>"), not a question */
static int    g_secret; /* ... and it is secret (a key, a password) */
static char   g_label[256];
static int    g_busy;
static char   g_busy_label[96];
static double g_busy_t0;
static char   g_notice[160];
static double g_notice_until;

static void notice(const char *m)
{
    snprintf(g_notice, sizeof(g_notice), "%s", m);
    g_notice_until = now_s() + 3;
    g_dirty = 1;
}

static void send_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    s[n] = '\n';
    for (size_t off = 0; off < n + 1;) {
        ssize_t w = write(g_cin, s + off, n + 1 - off);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    explicit_bzero(s, n + 1);
    free(s);
}

static void send_line(const char *k, const char *v)
{
    cJSON *o = cJSON_CreateObject();
    if (v) {
        cJSON_AddStringToObject(o, k, v);
    } else {
        cJSON_AddTrueToObject(o, k);
    }
    send_json(o);
    cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(x)) {
        explicit_bzero(x->valuestring, strlen(x->valuestring));
    }
    cJSON_Delete(o);
    g_await = 0;
}

static void interrupt_child(void)
{
    if (g_child > 0) {
        kill(g_child, SIGINT);
        notice("interrupting…");
    }
}

/* ================= input line editor ================= */

static tsb_t  E;       /* input text */
static size_t Epos;    /* cursor (byte offset) */
static int    Etop;    /* first visible row of the input */
static tsb_t  g_stash; /* chat draft kept while a question is asked */
static int    g_stashed;
static char  *g_hist[300];
static int    g_nh, g_hidx = -1;
static char  *g_draft;

#define INPUT_ROWS 8 /* the input box grows up to this many rows, then scrolls */

static void ed_clear(void)
{
    if (E.p) {
        explicit_bzero(E.p, E.cap);
        E.p[0] = '\0';
    }
    E.len = 0;
    Epos = 0;
    Etop = 0;
}

static void ed_set(const char *s)
{
    ed_clear();
    tsb_str(&E, s);
    Epos = E.len;
}

static void ed_insert(const char *s, size_t n)
{
    if (!E.p) {
        tsb_add(&E, "", 0);
    }
    if (E.len + n + 1 > E.cap) { /* grow by hand: the old buffer may hold a secret */
        size_t nc = (E.len + n + 1) * 2;
        char *nb = malloc(nc);
        if (!nb) {
            abort();
        }
        memcpy(nb, E.p, E.len + 1);
        explicit_bzero(E.p, E.cap);
        free(E.p);
        E.p = nb;
        E.cap = nc;
    }
    memmove(E.p + Epos + n, E.p + Epos, E.len - Epos + 1);
    memcpy(E.p + Epos, s, n);
    E.len += n;
    Epos += n;
    g_hidx = -1;
}

static void ed_delete(size_t a, size_t b)
{
    if (b <= a || b > E.len) {
        return;
    }
    memmove(E.p + a, E.p + b, E.len - b + 1);
    E.len -= b - a;
    Epos = a;
}

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t ed_prev(size_t i)
{
    while (i > 0 && is_cont((unsigned char)E.p[--i])) {
    }
    return i;
}

static size_t ed_next(size_t i)
{
    if (i < E.len) {
        i++;
    }
    while (i < E.len && is_cont((unsigned char)E.p[i])) {
        i++;
    }
    return i;
}

static size_t ed_word_left(size_t i)
{
    while (i > 0 && isspace((unsigned char)E.p[i - 1])) {
        i--;
    }
    while (i > 0 && !isspace((unsigned char)E.p[i - 1])) {
        i--;
    }
    return i;
}

static size_t ed_word_right(size_t i)
{
    while (i < E.len && isspace((unsigned char)E.p[i])) {
        i++;
    }
    while (i < E.len && !isspace((unsigned char)E.p[i])) {
        i++;
    }
    return i;
}

typedef struct {
    size_t a, b;
} vl_t;

static int chw(size_t i, int *len)
{
    uint32_t cp = tu_decode(E.p + i, E.len - i, len);
    return g_secret ? 1 : tu_width(cp);
}

static int cols_between(size_t a, size_t b)
{
    int c = 0;
    for (size_t i = a; i < b;) {
        int l;
        c += chw(i, &l);
        i += (size_t)l;
    }
    return c;
}

/* Rows of the input at text width tw (word wrap + hard newlines). */
static int ed_lines(vl_t **out, int tw)
{
    static vl_t *v;
    static int   cap;
    int n = 0, col = 0;
    size_t a = 0, i = 0, brk = 0; /* brk: just after the last space on this row (0 = none) */
#define PUSH(x, y)                                                                                                \
    do {                                                                                                          \
        if (n == cap) {                                                                                           \
            cap = cap ? cap * 2 : 16;                                                                             \
            v = realloc(v, sizeof(*v) * (size_t)cap);                                                             \
            if (!v) {                                                                                             \
                abort();                                                                                          \
            }                                                                                                     \
        }                                                                                                         \
        v[n].a = (x);                                                                                             \
        v[n].b = (y);                                                                                             \
        n++;                                                                                                      \
    } while (0)
    while (i < E.len) {
        if (E.p[i] == '\n') {
            PUSH(a, i);
            a = ++i;
            col = 0;
            brk = 0;
            continue;
        }
        int l, w = chw(i, &l);
        if (col + w > tw && col > 0) {
            if (brk > a && !g_secret && E.p[i] != ' ') { /* word wrap: the partial word moves down */
                PUSH(a, brk);
                col = cols_between(brk, i);
                a = brk;
            } else {
                PUSH(a, i);
                a = i;
                col = 0;
            }
            brk = 0;
            continue;
        }
        col += w;
        if (E.p[i] == ' ') {
            brk = i + 1;
        }
        i += (size_t)l;
    }
    PUSH(a, E.len);
#undef PUSH
    *out = v;
    return n;
}

static void ed_cursor(const vl_t *v, int n, int *row, int *col)
{
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (Epos >= v[i].a && Epos <= v[i].b) {
            r = i;
            if (!(Epos == v[i].b && i + 1 < n && v[i + 1].a == v[i].b)) {
                break; /* at a soft wrap the cursor belongs to the next row */
            }
        }
    }
    *row = r;
    *col = cols_between(v[r].a, Epos);
}

static size_t pos_at_col(const vl_t *v, int r, int col)
{
    size_t i = v[r].a;
    int c = 0;
    while (i < v[r].b) {
        int l, w = chw(i, &l);
        if (c + w > col) {
            break;
        }
        c += w;
        i += (size_t)l;
    }
    return i;
}

static void hist_add(const char *s)
{
    if (!*s || (g_nh && strcmp(g_hist[g_nh - 1], s) == 0)) {
        return;
    }
    if (g_nh == (int)(sizeof(g_hist) / sizeof(g_hist[0]))) {
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, sizeof(g_hist[0]) * (size_t)(g_nh - 1));
        g_nh--;
    }
    g_hist[g_nh++] = strdup(s);
}

static void hist_move(int dir)
{
    if (!g_chat || g_secret || g_nh == 0) {
        return;
    }
    int ni = (g_hidx < 0 ? g_nh : g_hidx) + dir;
    if (ni < 0 || ni > g_nh) {
        return;
    }
    if (g_hidx < 0) {
        free(g_draft);
        g_draft = strdup(E.p ? E.p : "");
    }
    ed_set(ni == g_nh ? (g_draft ? g_draft : "") : g_hist[ni]);
    g_hidx = ni == g_nh ? -1 : ni;
}

/* ================= the live area ================= */

static int input_tw(void) { return g_W > 10 ? g_W - 6 : 4; } /* "│ > " text " │" */

static void box_edge(tlines_t *L, const char *a, const char *b)
{
    tline_t *l = tl_new(L, TC_NONE);
    tsty_t s = sty(T.muted, 0);
    tl_put(l, s, a, strlen(a));
    for (int x = 0; x < g_W - 2; x++) {
        tl_put(l, s, "─", strlen("─"));
    }
    tl_put(l, s, b, strlen(b));
}

/* The live lines; *crow / *ccol: where the cursor goes (row within them, column). */
static void build_live(tlines_t *L, int *crow, int *ccol)
{
    static const char *const SPIN[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    tsty_t mut = sty(T.muted, 0), txt = sty(T.text, 0);
    vl_t *v;
    int tw = input_tw(), n = ed_lines(&v, tw), r, c;
    ed_cursor(v, n, &r, &c);
    int rows = n < INPUT_ROWS ? n : INPUT_ROWS;
    rows = rows > g_H - 8 ? (g_H > 9 ? g_H - 8 : 1) : rows;
    if (r < Etop) {
        Etop = r;
    } else if (r >= Etop + rows) {
        Etop = r - rows + 1;
    }
    int fixed = rows + 4 + (g_busy && !g_await) + (!g_chat && g_await && g_label[0]); /* box, hint, spinner, label */

    /* a reply on its way: its last lines, as many as fit */
    if (g_stream.len || g_rawl.len) {
        tlines_t S = {0};
        if (g_stream.len) {
            lay_markdown(&S, g_stream.p, text_w());
        }
        if (g_rawl.len) {
            lay_ansi(&S, g_rawl.p, g_rawl.len, text_w());
        }
        int room = g_H - fixed - 2, from = S.n > room ? S.n - room : 0;
        tl_blank(L);
        for (int i = from; i < S.n && room > 0; i++) {
            tline_t *l = tl_new(L, TC_NONE);
            tl_put(l, txt, "  ", 2);
            tl_cat(l, &S.l[i]);
        }
        tl_free(&S);
    }
    if (g_busy && !g_await) {
        int secs = (int)(now_s() - g_busy_t0);
        tline_t *l = tl_new(L, TC_NONE);
        char t[160];
        tl_put(l, txt, "  ", 2);
        tl_put(l, sty(T.info, 0), SPIN[(int)(now_s() * 10) % 10], strlen(SPIN[0]));
        snprintf(t, sizeof(t), " %s", g_busy_label[0] ? g_busy_label : "Working");
        tl_put_trunc(l, txt, t, g_W - 30);
        snprintf(t, sizeof(t), "  %ds · esc to interrupt", secs);
        tl_put(l, mut, t, strlen(t));
    }
    tl_blank(L);
    if (!g_chat && g_await && g_label[0]) { /* a question: its words above the box */
        tl_put_trunc(tl_new(L, TC_NONE), sty(T.text, TA_BOLD), g_label, g_W - 2);
    }
    box_edge(L, "╭", "╮");
    *crow = L->n + r - Etop;
    *ccol = 4 + c;
    for (int i = Etop; i < Etop + rows; i++) {
        tline_t *l = tl_new(L, TC_NONE);
        tl_put(l, mut, "│ ", strlen("│ "));
        tl_put(l, g_await ? sty(T.text, TA_BOLD) : mut, i == 0 ? "> " : "  ", 2);
        if (E.len == 0 && i == 0) {
            const char *ph = !g_await ? "Botter is working" : g_chat ? "Describe the agent you want, or ask a question" : "";
            tl_put_trunc(l, mut, ph, tw);
        } else if (g_secret) {
            for (int k = cols_between(v[i].a, v[i].b); k > 0; k--) {
                tl_put(l, txt, "•", strlen("•"));
            }
        } else {
            tl_put(l, txt, E.p + v[i].a, v[i].b - v[i].a);
        }
        tl_pad(l, g_W - 1, txt);
        tl_put(l, mut, "│", strlen("│"));
    }
    box_edge(L, "╰", "╯");
    tline_t *l = tl_new(L, TC_NONE);
    if (g_notice[0]) {
        tl_put(l, txt, "  ", 2);
        tl_put_trunc(l, sty(T.info, 0), g_notice, g_W - 4);
    } else {
        char t[160];
        int plan = g_plan_want >= 0 ? g_plan_want : g_plan;
        snprintf(t, sizeof(t), "  %s · tab %s · ctrl+o %s", plan ? "plan mode" : "build mode", plan ? "build" : "plan",
                 g_expand ? "less" : "details");
        tl_put_trunc(l, mut, t, g_W - 2);
    }
}

/* Print the finished lines into the scrollback, then draw the live area over the old one. */
static void render(void)
{
    tsb_t o = {0};
    tlines_t L = {0};
    int crow, ccol;
    build_live(&L, &crow, &ccol);
    tsb_str(&o, "\033[?25l");
    if (g_live_up) {
        tsb_fmt(&o, "\033[%dA", g_live_up);
    }
    tsb_str(&o, "\r\033[J");
    for (int i = 0; i < g_out.n; i++) {
        put_line(&o, &g_out.l[i]);
        tsb_str(&o, "\r\n");
    }
    tl_free(&g_out);
    for (int i = 0; i < L.n; i++) {
        put_line(&o, &L.l[i]);
        if (i + 1 < L.n) {
            tsb_str(&o, "\r\n");
        }
    }
    if (L.n - 1 - crow > 0) {
        tsb_fmt(&o, "\033[%dA", L.n - 1 - crow);
    }
    tsb_fmt(&o, "\r\033[%dC%s", ccol, g_await ? "\033[?25h" : "");
    g_live_up = crow;
    tl_free(&L);
    write_all(o.p, o.len);
    tsb_free(&o);
    g_dirty = 0;
}

/* ================= events from the agent ================= */

static const char *jstr(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void strip_sgr(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '\033') {
            if (s[1] == '[') {
                for (s += 2; *s && !(*s >= '@' && *s <= '~'); s++) {
                }
                if (!*s) {
                    break;
                }
            }
            continue;
        }
        if ((unsigned char)*s >= 0x20 || *s == '\t') {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
    while (o > 0 && out[o - 1] == ' ') {
        out[--o] = '\0';
    }
    memmove(out, out + strspn(out, " "), strlen(out + strspn(out, " ")) + 1);
}

/* Ask botcore to switch Plan/Build (a command line; it answers with a "mode" event and a new prompt). */
static void send_mode(int plan)
{
    send_line("line", plan ? "/plan" : "/build");
    g_plan_want = -1;
    g_plan = plan;
}

/* tab: switch Plan/Build now, or at the next chat prompt if Botter is busy or asking something. */
static void toggle_mode(void)
{
    int want = !(g_plan_want >= 0 ? g_plan_want : g_plan);
    if (g_await && g_chat) {
        send_mode(want);
    } else {
        g_plan_want = want;
        notice(want ? "plan mode after this turn" : "build mode after this turn");
    }
}

static void on_record(const char *json)
{
    cJSON *j = cJSON_Parse(json);
    const char *ev = jstr(j, "ev");
    if (!strcmp(ev, "prompt")) {
        flush_raw();
        char lab[256];
        strip_sgr(jstr(j, "text"), lab, sizeof(lab));
        g_await = 1;
        g_secret = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "secret"));
        g_chat = !g_secret && strcmp(lab, ">>") == 0;
        snprintf(g_label, sizeof(g_label), "%s", g_chat ? "" : lab);
        if (g_chat) {
            if (g_stream.len) { /* a turn that failed sends no final text: what came stays */
                out_reply(g_stream.p);
                tsb_free(&g_stream);
            }
            if (g_stashed) {
                ed_set(g_stash.p ? g_stash.p : "");
                tsb_free(&g_stash);
                g_stashed = 0;
            }
            if (g_plan_want >= 0 && g_plan_want != g_plan) {
                send_mode(g_plan_want);
            }
        } else {
            if (!g_stashed) {
                tsb_free(&g_stash);
                tsb_str(&g_stash, E.p ? E.p : "");
                g_stashed = 1;
            }
            ed_clear();
        }
    } else if (!strcmp(ev, "mode")) {
        g_plan = !strcmp(jstr(j, "mode"), "plan");
        g_plan_want = g_plan_want == g_plan ? -1 : g_plan_want;
    } else if (!strcmp(ev, "busy")) {
        g_busy_t0 = g_busy ? g_busy_t0 : now_s();
        g_busy = 1;
        snprintf(g_busy_label, sizeof(g_busy_label), "%s", jstr(j, "label"));
    } else if (!strcmp(ev, "idle")) {
        g_busy = 0;
    } else if (!strcmp(ev, "delta")) {
        if (!strcmp(jstr(j, "kind"), "thinking")) {
            tsb_str(&g_think, jstr(j, "text"));
            snprintf(g_busy_label, sizeof(g_busy_label), "Thinking");
        } else {
            tsb_str(&g_stream, jstr(j, "text"));
            snprintf(g_busy_label, sizeof(g_busy_label), "Writing");
        }
    } else if (!strcmp(ev, "thinking")) {
        out_think(jstr(j, "text"));
        tsb_free(&g_think);
    } else if (!strcmp(ev, "reply")) { /* the whole answer: replaces what was streamed */
        flush_raw();
        if (g_think.len) {
            out_think(g_think.p);
            tsb_free(&g_think);
        }
        out_reply(jstr(j, "text"));
        tsb_free(&g_stream);
    } else if (!strcmp(ev, "text")) { /* words between tool calls, or a progress line: one bullet */
        flush_raw();
        tsb_free(&g_stream);
        const char *t = jstr(j, "text");
        if (t[strspn(t, " \n\t")]) {
            out_text(t, sty(T.text, 0), sty(T.muted, 0), "  · ", "    ");
        }
    } else if (!strcmp(ev, "tool")) {
        const char *st = jstr(j, "status");
        snprintf(g_busy_label, sizeof(g_busy_label), "%s", *st ? st : "Working");
        if (g_expand) {
            char line[400];
            snprintf(line, sizeof(line), "%s %.300s", jstr(j, "name"), jstr(j, "args"));
            out_text(line, sty(T.muted, 0), sty(T.muted, 0), "  ⏺ ", "    ");
        }
    } else if (!strcmp(ev, "tool_result")) {
        const char *r = jstr(j, "text");
        int err = !strncmp(r, "ERROR", 5);
        if (g_expand || err) {
            char first[300];
            snprintf(first, sizeof(first), "%.*s%s", (int)strcspn(r, "\n") > 200 ? 200 : (int)strcspn(r, "\n"), r,
                     r[strcspn(r, "\n")] ? " …" : "");
            out_text(first, sty(err ? T.err : T.muted, 0), sty(T.muted, 0), "    ⎿ ", "      ");
        }
        snprintf(g_busy_label, sizeof(g_busy_label), "Thinking");
    }
    cJSON_Delete(j);
    g_dirty = 1;
}

/* Split the agent's output into plain text and "\x1e{json}\n" records. */
static void process_rx(void)
{
    size_t i = 0;
    while (i < g_rx.len) {
        char *rs = memchr(g_rx.p + i, '\x1e', g_rx.len - i);
        if (!rs) {
            size_t end = g_rx.len, k = end; /* keep an unfinished UTF-8 character for the next read */
            while (k > i && end - k < 4 && is_cont((unsigned char)g_rx.p[k - 1])) {
                k--;
            }
            if (k > i && end - k < 4 && (unsigned char)g_rx.p[k - 1] >= 0xC0) {
                unsigned char lead = (unsigned char)g_rx.p[k - 1];
                int need = (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
                if ((int)(end - k + 1) < need) {
                    end = k - 1;
                }
            }
            raw_text(g_rx.p + i, end - i);
            i = end;
            break;
        }
        size_t at = (size_t)(rs - g_rx.p);
        raw_text(g_rx.p + i, at - i);
        char *nl = memchr(rs, '\n', g_rx.len - at);
        if (!nl) {
            i = at; /* an unfinished record */
            break;
        }
        *nl = '\0';
        on_record(rs + 1);
        i = (size_t)(nl - g_rx.p) + 1;
    }
    memmove(g_rx.p, g_rx.p + i, g_rx.len - i);
    g_rx.len -= i;
    if (g_rx.p) {
        g_rx.p[g_rx.len] = '\0';
    }
    g_dirty = 1;
}

/* ================= keys ================= */

enum {
    K_NONE, K_ENTER, K_NEWLINE, K_BS, K_DEL, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_WLEFT, K_WRIGHT,
    K_WBS, K_WDEL, K_KILLEND, K_KILLSTART, K_ESC, K_CTRLC, K_CTRLD, K_CTRLL, K_CTRLO, K_TAB
};

static void submit(void)
{
    if (!g_await) {
        notice("Botter is working · esc to interrupt");
        return;
    }
    const char *text = E.p ? E.p : "";
    if (g_chat && (!strcmp(text, "/plan") || !strcmp(text, "/build"))) {
        hist_add(text);
        ed_clear();
        send_mode(text[1] == 'p');
        return;
    }
    if (g_chat) {
        if (text[strspn(text, " \n\t")]) {
            out_blank();
            out_text(text, sty(T.text, 0), sty(T.muted, 0), "│ ", "│ "); /* like the input box */
            out_blank();
            hist_add(text);
        }
    } else if (g_label[0]) { /* a question and its answer */
        char line[600];
        snprintf(line, sizeof(line), "%s %s", g_label, g_secret ? "••••••" : text);
        out_text(line, sty(T.muted, 0), sty(T.muted, 0), "  ", "  ");
    }
    send_line("line", text);
    free(g_draft);
    g_draft = NULL;
    g_hidx = -1;
    ed_clear();
}

static void do_key(int k)
{
    vl_t *v;
    int n = ed_lines(&v, input_tw()), r, c;
    ed_cursor(v, n, &r, &c);
    switch (k) {
    case K_ENTER:
        if (Epos == E.len && E.len > 0 && E.p[E.len - 1] == '\\' && !g_secret) {
            E.p[--E.len] = '\0'; /* "\" + enter = newline */
            Epos = E.len;
            ed_insert("\n", 1);
        } else {
            submit();
        }
        break;
    case K_NEWLINE:
        if (!g_secret) {
            ed_insert("\n", 1);
        }
        break;
    case K_TAB:
        if (!g_secret) {
            toggle_mode();
        }
        break;
    case K_BS:
        if (Epos > 0) {
            ed_delete(ed_prev(Epos), Epos);
        }
        break;
    case K_DEL:
        if (Epos < E.len) {
            ed_delete(Epos, ed_next(Epos));
        }
        break;
    case K_LEFT:
        Epos = ed_prev(Epos);
        break;
    case K_RIGHT:
        Epos = ed_next(Epos);
        break;
    case K_WLEFT:
        Epos = ed_word_left(Epos);
        break;
    case K_WRIGHT:
        Epos = ed_word_right(Epos);
        break;
    case K_WBS:
        ed_delete(ed_word_left(Epos), Epos);
        break;
    case K_WDEL:
        ed_delete(Epos, ed_word_right(Epos));
        break;
    case K_HOME:
        Epos = v[r].a;
        break;
    case K_END:
        Epos = v[r].b;
        break;
    case K_KILLEND:
        ed_delete(Epos, v[r].b == Epos && v[r].b < E.len ? v[r].b + 1 : v[r].b);
        break;
    case K_KILLSTART:
        ed_delete(v[r].a, Epos);
        break;
    case K_UP:
        if (r == 0) {
            hist_move(-1);
        } else {
            Epos = pos_at_col(v, r - 1, c);
        }
        break;
    case K_DOWN:
        if (r == n - 1) {
            hist_move(1);
        } else {
            Epos = pos_at_col(v, r + 1, c);
        }
        break;
    case K_ESC:
        if (g_busy && !g_await) {
            interrupt_child();
        }
        break;
    case K_CTRLC:
        if (g_await) {
            if (g_chat && E.len) {
                ed_clear();
            } else {
                send_line("intr", NULL);
                ed_clear();
            }
        } else if (g_busy) {
            interrupt_child();
        }
        break;
    case K_CTRLD:
        if (g_await && E.len == 0) {
            send_line("eof", NULL);
        } else if (Epos < E.len) {
            ed_delete(Epos, ed_next(Epos));
        }
        break;
    case K_CTRLL: /* a clean screen; the scrollback keeps what was said */
        write_all("\033[H\033[2J", 7);
        g_live_up = 0;
        break;
    case K_CTRLO:
        g_expand = !g_expand;
        notice(g_expand ? "details on: tool calls and reasoning are shown from now on" : "details off");
        break;
    }
    g_dirty = 1;
}

/* Paste: CR/CRLF -> LF, tabs -> spaces, other control characters dropped. */
static int g_pasting;

static void paste_text(const char *s, size_t n)
{
    tsb_t t = {0};
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\r') {
            tsb_add(&t, "\n", 1);
            i += i + 1 < n && s[i + 1] == '\n';
        } else if (ch == '\t') {
            tsb_add(&t, "    ", 4);
        } else if ((ch == '\n' && !g_secret) || (ch >= 0x20 && ch != 0x7f)) {
            tsb_add(&t, (const char *)&s[i], 1);
        }
    }
    if (t.len) {
        ed_insert(t.p, t.len);
        explicit_bzero(t.p, t.len);
    }
    tsb_free(&t);
    g_dirty = 1;
}

/* Decode one key from s[0..n). Returns bytes used, 0 = unfinished (wait for more). */
static size_t parse_input(const unsigned char *s, size_t n, int final)
{
    if (g_pasting) {
        const unsigned char *e = memmem(s, n, "\033[201~", 6);
        if (e) {
            paste_text((const char *)s, (size_t)(e - s));
            g_pasting = 0;
            return (size_t)(e - s) + 6;
        }
        if (n > 6) {
            paste_text((const char *)s, n - 5);
            return n - 5;
        }
        return final ? n : 0;
    }
    unsigned char ch = s[0];
    if (ch == 0x1b) {
        if (n == 1) {
            if (final) {
                do_key(K_ESC);
            }
            return final;
        }
        if (s[1] == '[') {
            size_t i = 2;
            while (i < n && s[i] >= 0x20 && s[i] <= 0x3F) {
                i++;
            }
            while (i < n && s[i] >= 0x20 && s[i] <= 0x2F) {
                i++;
            }
            if (i >= n) {
                if (final) {
                    do_key(K_ESC);
                }
                return final;
            }
            int p[8] = {0}, np = 0;
            for (size_t k = 2; k < i && np < 8;) {
                int val = 0;
                while (k < i && isdigit(s[k])) {
                    val = val * 10 + (s[k++] - '0');
                }
                p[np++] = val;
                if (k < i && (s[k] == ';' || s[k] == ':')) {
                    k++;
                } else {
                    break;
                }
            }
            int m = np >= 2 ? p[1] : 1; /* 2 shift, 3 alt, 5 ctrl */
            switch (s[i]) {
            case 'A': do_key(K_UP); break;
            case 'B': do_key(K_DOWN); break;
            case 'C': do_key(m == 5 || m == 3 ? K_WRIGHT : K_RIGHT); break;
            case 'D': do_key(m == 5 || m == 3 ? K_WLEFT : K_LEFT); break;
            case 'H': do_key(K_HOME); break;
            case 'F': do_key(K_END); break;
            case 'Z': do_key(K_TAB); break; /* shift+tab */
            case 'u': /* kitty keyboard protocol: code;mods */
                do_key(p[0] == 13 ? (m > 1 ? K_NEWLINE : K_ENTER) : p[0] == 27 ? K_ESC : K_NONE);
                break;
            case '~':
                if (p[0] == 200) {
                    g_pasting = 1;
                } else if (p[0] == 27 && np >= 3 && p[2] == 13) { /* xterm modifyOtherKeys */
                    do_key(p[1] > 1 ? K_NEWLINE : K_ENTER);
                } else if (p[0] == 1 || p[0] == 7) {
                    do_key(K_HOME);
                } else if (p[0] == 4 || p[0] == 8) {
                    do_key(K_END);
                } else if (p[0] == 3) {
                    do_key(m == 5 ? K_WDEL : K_DEL);
                }
                break;
            }
            return i + 1;
        }
        if (s[1] == 'O') {
            if (n < 3) {
                return final ? n : 0;
            }
            static const char keys[] = "ABCDHF";
            static const int map[] = {K_UP, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END};
            const char *f = s[2] ? strchr(keys, s[2]) : NULL;
            if (f) {
                do_key(map[f - keys]);
            }
            return 3;
        }
        switch (s[1]) { /* alt + key */
        case '\r':
        case '\n': do_key(K_NEWLINE); break;
        case 0x7f:
        case 8: do_key(K_WBS); break;
        case 'b': do_key(K_WLEFT); break;
        case 'f': do_key(K_WRIGHT); break;
        case 'd': do_key(K_WDEL); break;
        case 0x1b: do_key(K_ESC); return 1;
        }
        return 2;
    }
    if (ch < 0x20 || ch == 0x7f) {
        static const int ctl[32] = {
            [1] = K_HOME, [2] = K_LEFT,     [3] = K_CTRLC,    [4] = K_CTRLD,  [5] = K_END,   [6] = K_RIGHT,
            [8] = K_BS,   [9] = K_TAB,      [10] = K_NEWLINE, [11] = K_KILLEND, [12] = K_CTRLL, [13] = K_ENTER,
            [14] = K_DOWN, [15] = K_CTRLO,  [16] = K_UP,      [21] = K_KILLSTART, [23] = K_WBS,
        };
        do_key(ch == 0x7f ? K_BS : ctl[ch]);
        return 1;
    }
    if (ch >= 0x80) {
        size_t need = (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : (ch >> 3) == 30 ? 4 : 1;
        if (n < need) {
            return final ? n : 0;
        }
        if (need > 1) {
            ed_insert((const char *)s, need);
        }
        g_dirty = 1;
        return need;
    }
    size_t k = 0; /* a run of printable ASCII */
    while (k < n && s[k] >= 0x20 && s[k] < 0x7f) {
        k++;
    }
    ed_insert((const char *)s, k);
    g_dirty = 1;
    return k;
}

/* ================= the embedded agent ================= */

static const char TRAILER_MAGIC[8] = {'B', 'O', 'T', 'T', 'U', 'I', '0', '1'};
#define TRAILER_LEN 24 /* u64 payload offset, u64 payload length, magic */

static int copy_range(int dst, int src, off_t off, uint64_t len)
{
    char buf[1 << 16];
    while (len > 0) {
        ssize_t r = pread(src, buf, len < sizeof(buf) ? (size_t)len : sizeof(buf), off);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        for (ssize_t o = 0; o < r;) {
            ssize_t w = write(dst, buf + o, (size_t)(r - o));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                return -1;
            }
            o += w;
        }
        off += r;
        len -= (uint64_t)r;
    }
    return 0;
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

/* Where the embedded agent lives in our own executable: 0 ok. */
static int find_payload(int self, uint64_t *off, uint64_t *len, uint64_t *self_len)
{
    struct stat st;
    unsigned char t[TRAILER_LEN];
    if (fstat(self, &st) != 0) {
        return -1;
    }
    *self_len = (uint64_t)st.st_size;
    if (st.st_size < TRAILER_LEN || pread(self, t, TRAILER_LEN, st.st_size - TRAILER_LEN) != TRAILER_LEN ||
        memcmp(t + 16, TRAILER_MAGIC, 8) != 0) {
        return -1;
    }
    *off = get64(t);
    *len = get64(t + 8);
    if (*off + *len + TRAILER_LEN != (uint64_t)st.st_size) {
        return -1;
    }
    *self_len = *off;
    return 0;
}

/* `botter-tui --bundle <agent.bot> <out>`: this program + the agent + trailer. */
static int bundle(const char *agent, const char *out)
{
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC), ag = open(agent, O_RDONLY | O_CLOEXEC);
    uint64_t off, len, self_len;
    struct stat st;
    if (self < 0 || ag < 0 || fstat(ag, &st) != 0) {
        fprintf(stderr, "bundle: cannot open %s\n", self < 0 ? "/proc/self/exe" : agent);
        return 1;
    }
    find_payload(self, &off, &len, &self_len); /* self_len excludes any payload already attached */
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", out, (int)getpid());
    int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    unsigned char t[TRAILER_LEN];
    put64(t, self_len);
    put64(t + 8, (uint64_t)st.st_size);
    memcpy(t + 16, TRAILER_MAGIC, 8);
    if (o < 0 || copy_range(o, self, 0, self_len) != 0 || copy_range(o, ag, 0, (uint64_t)st.st_size) != 0 ||
        write(o, t, TRAILER_LEN) != TRAILER_LEN || fchmod(o, 0755) != 0 || close(o) != 0 || rename(tmp, out) != 0) {
        fprintf(stderr, "bundle: writing %s failed: %s\n", out, strerror(errno));
        unlink(tmp);
        return 1;
    }
    return 0;
}

/* The agent as an executable memfd. */
static int load_agent(int argc, char **argv)
{
    int src;
    uint64_t off = 0, len = 0, self_len;
    if (argc > 1) {
        struct stat st;
        src = open(argv[1], O_RDONLY | O_CLOEXEC);
        if (src < 0 || fstat(src, &st) != 0) {
            fprintf(stderr, "botter: cannot open %s: %s\n", argv[1], strerror(errno));
            return -1;
        }
        len = (uint64_t)st.st_size;
    } else {
        src = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (src < 0 || find_payload(src, &off, &len, &self_len) != 0) {
            fprintf(stderr, "botter: no agent embedded (build with `make`, or run: %s <agent.bot>)\n", argv[0]);
            return -1;
        }
    }
    int fd = memfd_create("botter", MFD_CLOEXEC);
    if (fd < 0 || copy_range(fd, src, (off_t)off, len) != 0) {
        fprintf(stderr, "botter: cannot load the agent: %s\n", strerror(errno));
        return -1;
    }
    close(src);
    fchmod(fd, 0700);
    return fd;
}

static int spawn_agent(int fd)
{
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        setenv("BOTCORE_FRONTEND", "1", 1);
        char *av[] = {"botter", NULL};
        fexecve(fd, av, environ);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    g_child = pid;
    g_cin = in[1];
    g_cout = out[0];
    fcntl(g_cout, F_SETFL, fcntl(g_cout, F_GETFL) | O_NONBLOCK);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--bundle") == 0) {
        return bundle(argv[2], argv[3]);
    }
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("Botter %s\n", BOTTER_VERSION);
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: %s [agent.bot]        run (default: the embedded agent)\n"
               "       %s --bundle <agent.bot> <out>\n"
               "BOTTER_TUI=0 runs the plain REPL.\n",
               argv[0], argv[0]);
        return 0;
    }
    int fd = load_agent(argc, argv);
    if (fd < 0) {
        return 1;
    }
    const char *term = getenv("TERM"), *want = getenv("BOTTER_TUI"), *ct = getenv("COLORTERM");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || (term && strcmp(term, "dumb") == 0) ||
        (want && strcmp(want, "0") == 0)) {
        char *av[] = {"botter", NULL};
        fexecve(fd, av, environ);
        fprintf(stderr, "botter: cannot start the agent: %s\n", strerror(errno));
        return 1;
    }
    g_truecolor = (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) ||
                  (term && (strstr(term, "kitty") || strstr(term, "ghostty") || strstr(term, "wezterm") ||
                            strstr(term, "alacritty") || strstr(term, "foot") || strstr(term, "direct")));
    g_tty = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) != 0 || spawn_agent(fd) != 0) {
        fprintf(stderr, "botter: cannot start the agent: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    if (tty_setup() != 0) {
        fprintf(stderr, "botter: cannot set up the terminal\n");
        kill(g_child, SIGTERM);
        return 1;
    }
    out_header();

    unsigned char ib[8192];
    size_t ibn = 0;
    double esc_deadline = 0;
    int child_open = 1;
    while (child_open && !g_quit) {
        if (g_winch) {
            g_winch = 0;
            tty_size();
            write_all("\033[H\033[2J", 7); /* the old live area was reflowed: start clean (scrollback stays) */
            g_live_up = 0;
            g_dirty = 1;
        }
        double t = now_s();
        if (g_notice[0] && t >= g_notice_until) {
            g_notice[0] = '\0';
            g_dirty = 1;
        }
        int spinning = g_busy && !g_await;
        if (g_dirty || spinning) {
            render();
        }
        int timeout = spinning ? 100 : -1;
        if (g_notice[0]) {
            int ms = (int)((g_notice_until - t) * 1000) + 10;
            timeout = timeout < 0 || ms < timeout ? ms : timeout;
        }
        if (ibn > 0) {
            int ms = (int)((esc_deadline - t) * 1000);
            timeout = ms < 0 ? 0 : (timeout < 0 || ms < timeout ? ms : timeout);
        }
        struct pollfd p[3] = {{STDIN_FILENO, POLLIN, 0}, {g_cout, POLLIN, 0}, {g_sigpipe[0], POLLIN, 0}};
        int pr = poll(p, 3, timeout);
        if (pr < 0 && errno != EINTR) {
            break;
        }
        if (pr > 0 && (p[2].revents & POLLIN)) {
            char junk[64];
            while (read(g_sigpipe[0], junk, sizeof(junk)) > 0) {
            }
        }
        if (pr > 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[65536];
            for (;;) {
                ssize_t r = read(g_cout, buf, sizeof(buf));
                if (r > 0) {
                    tsb_add(&g_rx, buf, (size_t)r);
                    continue;
                }
                if (r == 0) {
                    child_open = 0;
                } else if (errno == EINTR) {
                    continue;
                }
                break;
            }
            process_rx();
        }
        if (pr > 0 && (p[0].revents & POLLIN)) {
            ssize_t r = read(STDIN_FILENO, ib + ibn, sizeof(ib) - ibn);
            if (r == 0) {
                break; /* the terminal is gone */
            }
            if (r > 0) {
                ibn += (size_t)r;
                esc_deadline = now_s() + 0.03;
            }
        } else if (pr > 0 && (p[0].revents & (POLLHUP | POLLERR))) {
            break;
        }
        int final = ibn > 0 && now_s() >= esc_deadline;
        size_t used = 0;
        while (used < ibn) {
            size_t k = parse_input(ib + used, ibn - used, final);
            if (k == 0) {
                break;
            }
            used += k;
        }
        memmove(ib, ib + used, ibn - used);
        ibn -= used;
        if (ibn == sizeof(ib)) {
            ibn = 0; /* garbage: drop it */
        }
    }

    int status = 0;
    if (child_open || g_quit) {
        kill(g_child, SIGTERM);
    }
    waitpid(g_child, &status, 0);
    flush_raw();
    if (g_stream.len) {
        out_reply(g_stream.p);
    }
    g_await = 0;
    render(); /* the last words go into the scrollback */
    tty_restore();
    if (E.p) {
        explicit_bzero(E.p, E.cap);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
