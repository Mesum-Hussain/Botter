#define _GNU_SOURCE
#include "botlua.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BL_MEM_MAX    (512u * 1024 * 1024) /* what all of an agent's Lua may hold at once */
#define BL_CALL_S     60                   /* one function call */
#define BL_LOAD_S     10                   /* running a file's top level */
#define BL_JSON_DEPTH 100

/* Files a test wrote: tests read the real folder but never write it; their writes live here, and a later
 * bot.fs.read/exists of the same path sees them. */
typedef struct scratch {
    char           *path;
    char           *data;
    size_t          n;
    struct scratch *next;
} scratch;

struct bl_state {
    lua_State *L;
    bl_io      io;
    scratch   *scratch;
    bl_fn     *fn;
    int        nfn, cap;
    size_t     mem;
    double     deadline; /* CLOCK_MONOTONIC seconds; 0 = none */
    int        limit_s;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bl_state *state_of(lua_State *L)
{
    void *ud = NULL;
    lua_getallocf(L, &ud);
    return ud;
}

/* Every allocation goes through here, so a runaway table cannot take the machine's memory. */
static void *bl_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    bl_state *s = ud;
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        free(ptr);
        s->mem -= old;
        return NULL;
    }
    if (nsize > old && s->mem + (nsize - old) > BL_MEM_MAX) {
        return NULL; /* Lua raises "not enough memory" */
    }
    void *p = realloc(ptr, nsize);
    if (p) {
        s->mem = s->mem - old + nsize;
    }
    return p;
}

/* Every 10000 instructions: Ctrl-C, or out of time. */
static void bl_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    bl_state *s = state_of(L);
    if (s->io.interrupted && s->io.interrupted(s->io.ud)) {
        luaL_error(L, "stopped: interrupted by the user");
    }
    if (s->deadline > 0 && now_s() > s->deadline) {
        luaL_error(L, "stopped: ran longer than %d seconds", s->limit_s);
    }
}

/* ---- JSON <-> Lua ------------------------------------------------------- */

static int key_cmp(const void *a, const void *b)
{
    return strcmp((*(cJSON *const *)a)->string, (*(cJSON *const *)b)->string);
}

/* Keys in sorted order: Lua's table order changes from run to run, and the same table must give the same
 * text (prompts stay cacheable, outputs reproducible). */
static void sort_keys(cJSON *o)
{
    int n = cJSON_GetArraySize(o);
    if (n < 2) {
        return;
    }
    cJSON **v = malloc(sizeof(*v) * (size_t)n);
    if (!v) {
        return;
    }
    for (int i = 0; i < n; i++) {
        v[i] = cJSON_DetachItemViaPointer(o, o->child);
    }
    qsort(v, (size_t)n, sizeof(*v), key_cmp);
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(o, v[i]); /* keeps the item's key */
    }
    free(v);
}

static cJSON *to_json(lua_State *L, int idx, int depth)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER: {
        double v = lua_isinteger(L, idx) ? (double)lua_tointeger(L, idx) : lua_tonumber(L, idx);
        return isfinite(v) ? cJSON_CreateNumber(v) : cJSON_CreateNull();
    }
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        if (depth > BL_JSON_DEPTH) {
            return NULL;
        }
        lua_Integer n = (lua_Integer)lua_rawlen(L, idx), count = 0;
        int seq = 1;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            count++;
            if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n) {
                seq = 0;
            }
            lua_pop(L, 1);
        }
        if (seq && count == n) { /* a list (an empty table is an empty list) */
            cJSON *a = cJSON_CreateArray();
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, i);
                cJSON *v = to_json(L, -1, depth + 1);
                lua_pop(L, 1);
                if (!v) {
                    cJSON_Delete(a);
                    return NULL;
                }
                cJSON_AddItemToArray(a, v);
            }
            return a;
        }
        cJSON *o = cJSON_CreateObject();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            char key[64];
            const char *k = key;
            if (lua_type(L, -2) == LUA_TSTRING) {
                k = lua_tostring(L, -2);
            } else if (lua_isinteger(L, -2)) {
                snprintf(key, sizeof(key), "%lld", (long long)lua_tointeger(L, -2));
            } else if (lua_type(L, -2) == LUA_TNUMBER) {
                snprintf(key, sizeof(key), "%.15g", lua_tonumber(L, -2));
            } else {
                lua_pop(L, 1);
                continue; /* a boolean or table key has no JSON form */
            }
            cJSON *v = to_json(L, -1, depth + 1);
            lua_pop(L, 1);
            if (!v) {
                lua_pop(L, 1);
                cJSON_Delete(o);
                return NULL;
            }
            cJSON_AddItemToObject(o, k, v);
        }
        sort_keys(o);
        return o;
    }
    default:
        return cJSON_CreateNull(); /* nil, functions */
    }
}

static void push_json(lua_State *L, const cJSON *j)
{
    luaL_checkstack(L, 4, "JSON too deep");
    if (cJSON_IsBool(j)) {
        lua_pushboolean(L, cJSON_IsTrue(j));
    } else if (cJSON_IsNumber(j)) {
        double v = j->valuedouble;
        if (v == floor(v) && fabs(v) < 9007199254740992.0) {
            lua_pushinteger(L, (lua_Integer)v);
        } else {
            lua_pushnumber(L, v);
        }
    } else if (cJSON_IsString(j)) {
        lua_pushstring(L, j->valuestring);
    } else if (cJSON_IsArray(j)) {
        lua_createtable(L, cJSON_GetArraySize(j), 0);
        lua_Integer i = 1;
        for (const cJSON *e = j->child; e; e = e->next, i++) {
            push_json(L, e);
            lua_rawseti(L, -2, i);
        }
    } else if (cJSON_IsObject(j)) {
        lua_createtable(L, 0, cJSON_GetArraySize(j));
        for (const cJSON *e = j->child; e; e = e->next) {
            if (!cJSON_IsNull(e)) {
                push_json(L, e);
                lua_setfield(L, -2, e->string);
            }
        }
    } else {
        lua_pushnil(L);
    }
}

static int l_json_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    cJSON *j = to_json(L, 1, 0);
    char *s = j ? (lua_toboolean(L, 2) ? cJSON_Print(j) : cJSON_PrintUnformatted(j)) : NULL;
    cJSON_Delete(j);
    if (!s) {
        return luaL_error(L, "bot.json.encode: this value has no JSON form (nested too deeply, or a loop)");
    }
    lua_pushstring(L, s);
    free(s);
    return 1;
}

static int l_json_decode(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    cJSON *j = cJSON_ParseWithLength(s, n);
    if (!j) {
        return luaL_error(L, "bot.json.decode: not valid JSON near: %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : s);
    }
    push_json(L, j);
    cJSON_Delete(j);
    return 1;
}

/* ---- CSV (RFC 4180) ----------------------------------------------------- */

/* One field starting at *p; advances *p past it and its separator. *end_row = the row ended. */
static void csv_field(lua_State *L, luaL_Buffer *b, const char **p, const char *e, int *end_row)
{
    const char *s = *p;
    luaL_buffinit(L, b);
    *end_row = 0;
    if (s < e && *s == '"') {
        for (s++; s < e; s++) {
            if (*s == '"') {
                if (s + 1 < e && s[1] == '"') {
                    luaL_addchar(b, '"');
                    s++;
                } else {
                    s++;
                    break;
                }
            } else {
                luaL_addchar(b, *s);
            }
        }
        while (s < e && *s != ',' && *s != '\n' && *s != '\r') { /* junk after the closing quote */
            luaL_addchar(b, *s++);
        }
    } else {
        const char *f = s;
        while (s < e && *s != ',' && *s != '\n' && *s != '\r') {
            s++;
        }
        luaL_addlstring(b, f, (size_t)(s - f));
    }
    if (s >= e) {
        *end_row = 1;
    } else if (*s == ',') {
        s++;
    } else {
        *end_row = 1;
        s += (*s == '\r' && s + 1 < e && s[1] == '\n') ? 2 : 1;
    }
    *p = s;
}

/* bot.csv.parse(text [, {header = false}]): with a header (default) a list of rows keyed by column
 * name, every column present ("" when empty); without, a list of lists. Blank lines are skipped. */
static int l_csv_parse(lua_State *L)
{
    size_t n;
    const char *p = luaL_checklstring(L, 1, &n), *e = p + n;
    int header = 1;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "header");
        header = lua_isnil(L, -1) || lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
        p += 3; /* byte order mark */
    }
    lua_newtable(L); /* rows */
    int rows = lua_gettop(L), names = 0, ncols = 0;
    lua_Integer nrows = 0;
    while (p < e) {
        if (*p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        lua_newtable(L);
        int row = lua_gettop(L), col = 0, end = 0;
        while (!end) {
            luaL_Buffer b;
            csv_field(L, &b, &p, e, &end);
            luaL_pushresult(&b);
            lua_rawseti(L, row, ++col);
        }
        if (header && !names) {
            names = row; /* the header row stays on the stack */
            ncols = col;
            continue;
        }
        if (header) {
            lua_createtable(L, 0, ncols);
            for (int c = 1; c <= ncols; c++) {
                lua_rawgeti(L, names, c);
                if (c <= col) {
                    lua_rawgeti(L, row, c);
                } else {
                    lua_pushliteral(L, "");
                }
                lua_settable(L, -3);
            }
            lua_remove(L, row);
        }
        lua_rawseti(L, rows, ++nrows);
    }
    lua_settop(L, rows);
    return 1;
}

/* A growing C string: the Lua stack is free to use while it fills (unlike a luaL_Buffer). */
typedef struct {
    char  *p;
    size_t len, cap;
} bl_sbuf_t;

static void bl_sb_add(bl_sbuf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) {
            nc *= 2;
        }
        char *np = realloc(b->p, nc);
        if (!np) {
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void csv_add_value(lua_State *L, bl_sbuf_t *b, int idx)
{
    char num[64];
    const char *s = "";
    size_t n = 0;
    if (lua_isinteger(L, idx)) {
        n = (size_t)snprintf(num, sizeof(num), "%lld", (long long)lua_tointeger(L, idx));
        s = num;
    } else if (lua_type(L, idx) == LUA_TNUMBER) {
        double v = lua_tonumber(L, idx);
        n = (size_t)snprintf(num, sizeof(num), v == floor(v) && fabs(v) < 1e15 ? "%.0f" : "%.15g", v);
        s = num;
    } else if (lua_type(L, idx) == LUA_TBOOLEAN) {
        s = lua_toboolean(L, idx) ? "true" : "false";
        n = strlen(s);
    } else if (lua_type(L, idx) == LUA_TSTRING) {
        s = lua_tolstring(L, idx, &n);
    }
    if (!memchr(s, ',', n) && !memchr(s, '"', n) && !memchr(s, '\n', n) && !memchr(s, '\r', n)) {
        bl_sb_add(b, s, n);
        return;
    }
    bl_sb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        bl_sb_add(b, s + i, 1);
        if (s[i] == '"') {
            bl_sb_add(b, "\"", 1);
        }
    }
    bl_sb_add(b, "\"", 1);
}

/* bot.csv.encode(rows, columns): a header line, then each row's values in the columns' order. */
static int l_csv_encode(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer nc = (lua_Integer)lua_rawlen(L, 2), nr = (lua_Integer)lua_rawlen(L, 1);
    if (nc == 0) {
        return luaL_error(L, "bot.csv.encode: columns must be a list of column names");
    }
    bl_sbuf_t b = {0};
    for (lua_Integer c = 1; c <= nc; c++) {
        lua_rawgeti(L, 2, c);
        bl_sb_add(&b, ",", c > 1);
        csv_add_value(L, &b, -1);
        lua_pop(L, 1);
    }
    bl_sb_add(&b, "\n", 1);
    for (lua_Integer r = 1; r <= nr; r++) {
        lua_rawgeti(L, 1, r);
        if (!lua_istable(L, -1)) {
            free(b.p);
            return luaL_error(L, "bot.csv.encode: row %d is not a table", (int)r);
        }
        for (lua_Integer c = 1; c <= nc; c++) {
            lua_rawgeti(L, 2, c);
            lua_gettable(L, -2);
            bl_sb_add(&b, ",", c > 1);
            csv_add_value(L, &b, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        bl_sb_add(&b, "\n", 1);
    }
    lua_pushlstring(L, b.p ? b.p : "", b.len);
    free(b.p);
    return 1;
}

/* ---- dates (YYYY-MM-DD, proleptic Gregorian; pure arithmetic) ---------- */

static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (long long)yoe + era * 400 + (*m <= 2);
}

static unsigned days_in_month(long long y, unsigned m)
{
    static const unsigned dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : dm[m - 1];
}

/* "YYYY-MM-DD" (a time after it is allowed: "2026-09-07T10:00" or "2026-09-07 10:00"). */
static int parse_date(const char *s, long long *y, unsigned *m, unsigned *d)
{
    if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-' || (s[10] && s[10] != 'T' && s[10] != ' ')) {
        return -1;
    }
    for (int i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && !isdigit((unsigned char)s[i])) {
            return -1;
        }
    }
    *y = atoll(s);
    *m = (unsigned)atoi(s + 5);
    *d = (unsigned)atoi(s + 8);
    return *m >= 1 && *m <= 12 && *d >= 1 && *d <= days_in_month(*y, *m) ? 0 : -1;
}

static long long check_date(lua_State *L, int idx)
{
    const char *s = luaL_checkstring(L, idx);
    long long y;
    unsigned m, d;
    if (parse_date(s, &y, &m, &d) != 0) {
        luaL_error(L, "not a date (YYYY-MM-DD): %s", s);
    }
    return days_from_civil(y, m, d);
}

static int push_date(lua_State *L, long long days)
{
    long long y;
    unsigned m, d;
    char out[32];
    civil_from_days(days, &y, &m, &d);
    snprintf(out, sizeof(out), "%04lld-%02u-%02u", y, m, d);
    lua_pushstring(L, out);
    return 1;
}

static int l_date_add_days(lua_State *L)
{
    long long z = check_date(L, 1);
    return push_date(L, z + luaL_checkinteger(L, 2));
}

/* Same day n months later; the 31st becomes the month's last day when it is shorter. */
static int l_date_add_months(lua_State *L)
{
    long long y;
    unsigned m, d;
    const char *s = luaL_checkstring(L, 1);
    if (parse_date(s, &y, &m, &d) != 0) {
        return luaL_error(L, "not a date (YYYY-MM-DD): %s", s);
    }
    long long total = y * 12 + (m - 1) + luaL_checkinteger(L, 2);
    long long ny = total >= 0 ? total / 12 : (total - 11) / 12;
    unsigned nm = (unsigned)(total - ny * 12) + 1, last = days_in_month(ny, nm);
    return push_date(L, days_from_civil(ny, nm, d > last ? last : d));
}

static int l_date_diff_days(lua_State *L)
{
    long long a = check_date(L, 1), b = check_date(L, 2);
    lua_pushinteger(L, (lua_Integer)(b - a));
    return 1;
}

/* 1 = Monday ... 7 = Sunday */
static int l_date_weekday(lua_State *L)
{
    long long z = check_date(L, 1);
    lua_pushinteger(L, (lua_Integer)(((z % 7) + 7 + 3) % 7 + 1));
    return 1;
}

static int l_date_is_valid(lua_State *L)
{
    long long y;
    unsigned m, d;
    lua_pushboolean(L, lua_type(L, 1) == LUA_TSTRING && parse_date(lua_tostring(L, 1), &y, &m, &d) == 0);
    return 1;
}

/* ---- the clock and files (through the host) ----------------------------- */

static int l_today(lua_State *L)
{
    bl_state *s = state_of(L);
    char out[11];
    if (!s->io.today || s->io.today(s->io.ud, out) != 0) {
        return luaL_error(L, "bot.today is not available here");
    }
    lua_pushstring(L, out);
    return 1;
}

static scratch *scratch_find(bl_state *s, const char *path)
{
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    for (scratch *f = s->scratch; f; f = f->next) {
        if (!strcmp(f->path, path)) {
            return f;
        }
    }
    return NULL;
}

/* A test's write: kept in memory (no fs_write here, but files can be read: the host runs tests). */
static int scratch_put(bl_state *s, const char *path, const char *data, size_t n, int append)
{
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    scratch *f = scratch_find(s, path);
    if (!f) {
        f = calloc(1, sizeof(*f));
        if (!f || !(f->path = strdup(path))) {
            free(f);
            return -1;
        }
        if (append && s->io.fs_read) { /* appending to a real file starts from its content */
            char err[8];
            f->data = s->io.fs_read(s->io.ud, path, &f->n, err, sizeof(err));
            if (!f->data) {
                f->n = 0;
            }
        }
        f->next = s->scratch;
        s->scratch = f;
    }
    size_t base = append ? f->n : 0;
    char *d = realloc(f->data, base + n + 1);
    if (!d) {
        return -1;
    }
    memcpy(d + base, data, n);
    d[base + n] = 0;
    f->data = d;
    f->n = base + n;
    return 0;
}

static int l_fs_read(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *path = luaL_checkstring(L, 1);
    scratch *f = scratch_find(s, path);
    if (f) {
        lua_pushlstring(L, f->data ? f->data : "", f->n);
        return 1;
    }
    char err[300] = "";
    size_t n = 0;
    char *d = s->io.fs_read ? s->io.fs_read(s->io.ud, path, &n, err, sizeof(err)) : NULL;
    if (!d) {
        return luaL_error(L, "bot.fs.read %s: %s", path, err[0] ? err : "files are not available here");
    }
    lua_pushlstring(L, d, n);
    free(d);
    return 1;
}

static int fs_put(lua_State *L, int append)
{
    bl_state *s = state_of(L);
    const char *path = luaL_checkstring(L, 1);
    size_t n;
    const char *data = luaL_checklstring(L, 2, &n);
    char err[300] = "";
    if (!s->io.fs_write && s->io.fs_read) {
        if (scratch_put(s, path, data, n, append) != 0) {
            return luaL_error(L, "bot.fs.%s %s: out of memory", append ? "append" : "write", path);
        }
        return 0;
    }
    if (!s->io.fs_write || s->io.fs_write(s->io.ud, path, data, n, append, err, sizeof(err)) != 0) {
        return luaL_error(L, "bot.fs.%s %s: %s", append ? "append" : "write", path,
                          err[0] ? err : "files are not available here");
    }
    return 0;
}

static int l_fs_write(lua_State *L) { return fs_put(L, 0); }
static int l_fs_append(lua_State *L) { return fs_put(L, 1); }

static int l_fs_list(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *path = luaL_optstring(L, 1, ".");
    char err[300] = "";
    char *d = s->io.fs_list ? s->io.fs_list(s->io.ud, path, err, sizeof(err)) : NULL;
    if (!d) {
        return luaL_error(L, "bot.fs.list %s: %s", path, err[0] ? err : "files are not available here");
    }
    lua_newtable(L);
    lua_Integer i = 0;
    for (char *sv = NULL, *t = strtok_r(d, "\n", &sv); t; t = strtok_r(NULL, "\n", &sv)) {
        lua_pushstring(L, t);
        lua_rawseti(L, -2, ++i);
    }
    free(d);
    return 1;
}

static int l_fs_exists(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, scratch_find(s, path) || (s->io.fs_exists && s->io.fs_exists(s->io.ud, path)));
    return 1;
}

/* ---- the network, commands, MCP servers, other agents: through the host's own rules ---- */

static int l_http(lua_State *L, int post)
{
    bl_state *s = state_of(L);
    const char *url = luaL_checkstring(L, 1);
    int hi = post ? 3 : 2;
    char *body = NULL;
    if (post) {
        if (lua_type(L, 2) == LUA_TSTRING) {
            body = strdup(lua_tostring(L, 2));
        } else { /* a table goes as JSON */
            cJSON *j = to_json(L, 2, 0);
            body = j ? cJSON_PrintUnformatted(j) : NULL;
            cJSON_Delete(j);
        }
        if (!body) {
            return luaL_error(L, "bot.http.post: the body must be a text or a table");
        }
    }
    char *hdr = NULL;
    size_t hl = 0;
    FILE *hs = open_memstream(&hdr, &hl);
    if (lua_istable(L, hi) && hs) {
        lua_pushnil(L);
        while (lua_next(L, hi)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING &&
                !strpbrk(lua_tostring(L, -2), "\r\n:") && !strpbrk(lua_tostring(L, -1), "\r\n")) {
                fprintf(hs, "%s: %s\r\n", lua_tostring(L, -2), lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    }
    if (hs) {
        fclose(hs);
    }
    char err[300] = "";
    int status = 0;
    char *resp = s->io.http ? s->io.http(s->io.ud, url, body, hdr, &status, err, sizeof(err)) : NULL;
    free(body);
    free(hdr);
    if (!resp) {
        return luaL_error(L, "bot.http %s: %s", url, err[0] ? err : "the network is not available here");
    }
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, resp);
    lua_setfield(L, -2, "body");
    cJSON *j = cJSON_Parse(resp);
    if (j) { /* a JSON answer is also there already decoded */
        push_json(L, j);
        lua_setfield(L, -2, "json");
        cJSON_Delete(j);
    }
    free(resp);
    return 1;
}

static int l_http_get(lua_State *L) { return l_http(L, 0); }
static int l_http_post(lua_State *L) { return l_http(L, 1); }

/* One of the agent's tools, as the model would call it. NULL-safe: an "ERROR..." result raises. */
static char *call_tool(lua_State *L, const char *what, const char *name, cJSON *args)
{
    bl_state *s = state_of(L);
    char *a = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    char *r = s->io.tool && a ? s->io.tool(s->io.ud, name, a) : NULL;
    free(a);
    if (!r) {
        luaL_error(L, "%s is not available here", what);
    }
    if (!strncmp(r, "ERROR", 5)) {
        lua_pushfstring(L, "%s: %s", what, r + 5 + strspn(r + 5, ": "));
        free(r);
        lua_error(L);
    }
    return r;
}

/* bot.shell(command [, timeout_s]) -> output, exit code (nil when it was killed or timed out) */
static int l_shell(lua_State *L)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "command", luaL_checkstring(L, 1));
    if (lua_isinteger(L, 2)) {
        cJSON_AddNumberToObject(a, "timeout_s", (double)lua_tointeger(L, 2));
    }
    char *r = call_tool(L, "bot.shell", "shell_exec", a);
    size_t n = strlen(r);
    const char *tail = n ? r + n - 1 : r;
    while (tail > r && tail[-1] != '\n') {
        tail--;
    }
    int code = 0, has = sscanf(tail, "[exit code %d]", &code) == 1;
    size_t keep = has ? (size_t)(tail - r) : n;
    if (keep && r[keep - 1] == '\n') {
        keep--;
    }
    lua_pushlstring(L, r, keep);
    if (has) {
        lua_pushinteger(L, code);
    } else {
        lua_pushnil(L);
    }
    free(r);
    return 2;
}

/* bot.mcp(server, tool, args): a tool of one of config.json's MCP servers -> its text */
static int l_mcp(lua_State *L)
{
    const char *srv = luaL_checkstring(L, 1), *tool = luaL_checkstring(L, 2);
    cJSON *a = lua_istable(L, 3) ? to_json(L, 3, 0) : cJSON_CreateObject();
    if (!cJSON_IsObject(a)) {
        cJSON_Delete(a);
        return luaL_error(L, "bot.mcp: the arguments must be a table of names and values");
    }
    char name[200];
    snprintf(name, sizeof(name), "%s_%s", srv, tool);
    char *r = call_tool(L, "bot.mcp", name, a);
    lua_pushstring(L, r);
    free(r);
    return 1;
}

/* bot.agent(name, task): hand a task to a sub-agent or a remote (A2A) agent -> its answer */
static int l_agent(lua_State *L)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "agent", luaL_checkstring(L, 1));
    cJSON_AddStringToObject(a, "task", luaL_checkstring(L, 2));
    char *r = call_tool(L, "bot.agent", "delegate", a);
    lua_pushstring(L, r);
    free(r);
    return 1;
}

/* bot.tool(name, args): any of the agent's tools as the model would call it -> its text */
static int l_tool(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    cJSON *a = lua_istable(L, 2) ? to_json(L, 2, 0) : cJSON_CreateObject();
    if (!cJSON_IsObject(a)) {
        cJSON_Delete(a);
        return luaL_error(L, "bot.tool: the arguments must be a table of names and values");
    }
    char what[120];
    snprintf(what, sizeof(what), "bot.tool %s", name);
    char *r = call_tool(L, what, name, a);
    lua_pushstring(L, r);
    free(r);
    return 1;
}

/* bot.worker{task = ..., system?, context?, skills?, tools?, writes?, max_rounds?} -> the worker's reply */
static int l_worker(lua_State *L)
{
    bl_state *s = state_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "task");
    if (lua_type(L, -1) != LUA_TSTRING || !*lua_tostring(L, -1)) {
        return luaL_error(L, "bot.worker: give it a task (bot.worker{task = \"...\"})");
    }
    lua_pop(L, 1);
    if (!s->io.worker) {
        return luaL_error(L, "bot.worker is not available here");
    }
    cJSON *spec = to_json(L, 1, 0);
    char *j = spec ? cJSON_PrintUnformatted(spec) : NULL, err[600] = "";
    cJSON_Delete(spec);
    char *r = j ? s->io.worker(s->io.ud, j, err, sizeof(err)) : NULL;
    free(j);
    if (!r) {
        return luaL_error(L, "bot.worker: %s", err[0] ? err : "the worker failed");
    }
    lua_pushstring(L, r);
    free(r);
    return 1;
}

/* bot.llm(prompt) or bot.llm{prompt, system?, schema?, images?, audio?} -> text, or a table when there is a schema */
static int l_llm(lua_State *L)
{
    bl_state *s = state_of(L);
    cJSON *spec;
    if (lua_type(L, 1) == LUA_TSTRING) {
        spec = cJSON_CreateObject();
        cJSON_AddStringToObject(spec, "prompt", lua_tostring(L, 1));
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        spec = to_json(L, 1, 0);
    }
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(spec, "prompt");
    if (!cJSON_IsString(p) || !*p->valuestring) {
        cJSON_Delete(spec);
        return luaL_error(L, "bot.llm: give it a prompt");
    }
    int structured = cJSON_GetObjectItemCaseSensitive(spec, "schema") != NULL;
    char *j = cJSON_PrintUnformatted(spec), err[600] = "";
    cJSON_Delete(spec);
    char *r = s->io.llm && j ? s->io.llm(s->io.ud, j, err, sizeof(err)) : NULL;
    free(j);
    if (!r) {
        return luaL_error(L, "bot.llm: %s", err[0] ? err : "not available here");
    }
    cJSON *v = structured ? cJSON_Parse(r) : NULL;
    if (v) {
        push_json(L, v);
        cJSON_Delete(v);
    } else {
        lua_pushstring(L, r);
    }
    free(r);
    return 1;
}

/* bot.ask(question) -> the person's answer, or nil when nobody is there */
static int l_ask(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *q = luaL_checkstring(L, 1);
    char *a = s->io.ask ? s->io.ask(s->io.ud, q) : NULL;
    if (a) {
        lua_pushstring(L, a);
        free(a);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* bot.say(text): a progress line for the person (never part of any conversation) */
static int l_say(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *t = luaL_checkstring(L, 1);
    if (s->io.say) {
        s->io.say(s->io.ud, t);
    }
    return 0;
}

/* bot.usage() -> {calls, input_tokens, output_tokens} */
static int l_usage(lua_State *L)
{
    bl_state *s = state_of(L);
    long calls = 0;
    long long in = 0, out = 0;
    if (!s->io.usage || s->io.usage(s->io.ud, &calls, &in, &out) != 0) {
        return luaL_error(L, "bot.usage is not available here");
    }
    lua_newtable(L);
    lua_pushinteger(L, calls);
    lua_setfield(L, -2, "calls");
    lua_pushinteger(L, in);
    lua_setfield(L, -2, "input_tokens");
    lua_pushinteger(L, out);
    lua_setfield(L, -2, "output_tokens");
    return 1;
}

/* ---- bot.test: the project's Lua, loaded fresh from the working folder, and its tests run ---- */

static int bl_cmp_str(const void *a, const void *b);

typedef struct {
    char  *p;
    size_t n;
    int    errors;
} test_rep_t;

static void rep_add(test_rep_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rep_add(test_rep_t *r, const char *fmt, ...)
{
    char *t = NULL;
    va_list ap;
    va_start(ap, fmt);
    int w = vasprintf(&t, fmt, ap);
    va_end(ap);
    char *np = w > 0 ? realloc(r->p, r->n + (size_t)w + 1) : NULL;
    if (np) {
        memcpy(np + r->n, t, (size_t)w + 1);
        r->p = np;
        r->n += (size_t)w;
    }
    free(t);
}

static void test_diag(void *ud, const char *file, int line, int error, const char *msg)
{
    test_rep_t *r = ud;
    if (line) {
        rep_add(r, "%s:%d: %s: %s\n", file, line, error ? "error" : "warning", msg);
    } else {
        rep_add(r, "%s: %s: %s\n", file, error ? "error" : "warning", msg);
    }
    r->errors += error;
}

/* Files ending in .lua in a folder (io.fs_list: one name per line), as dir/name, sorted. */
static int list_lua(bl_state *s, const char *dir, char ***out)
{
    char err[200] = "", *names = s->io.fs_list ? s->io.fs_list(s->io.ud, dir, err, sizeof(err)) : NULL;
    int n = 0;
    *out = NULL;
    for (char *save = NULL, *l = names ? strtok_r(names, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
        size_t k = strlen(l);
        char *full = NULL;
        if (k > 4 && !strcmp(l + k - 4, ".lua") && asprintf(&full, "%s/%s", dir, l) >= 0) {
            char **nv = realloc(*out, sizeof(char *) * (size_t)(n + 1));
            if (!nv) {
                free(full);
                break;
            }
            *out = nv;
            (*out)[n++] = full;
        }
    }
    free(names);
    if (n) {
        qsort(*out, (size_t)n, sizeof(char *), bl_cmp_str);
    }
    return n;
}

static void add_name(char ***v, int *n, char *name)
{
    char **nv = realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (nv) {
        *v = nv;
        nv[(*n)++] = name;
    } else {
        free(name);
    }
}

/* Load the project's Lua (functions/, skills/<n>/functions/, FLOW.lua) into t. */
static void load_project(bl_state *s, bl_state *t, test_rep_t *r)
{
    char **files = NULL, **sk = NULL, err[200] = "";
    int n = list_lua(s, "functions", &files);
    char *skills = s->io.fs_list(s->io.ud, "skills", err, sizeof(err));
    for (char *save = NULL, *l = skills ? strtok_r(skills, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
        size_t k = strlen(l);
        char d[300];
        if (k > 1 && l[k - 1] == '/' && snprintf(d, sizeof(d), "skills/%sfunctions", l) < (int)sizeof(d)) {
            int m = list_lua(s, d, &sk);
            for (int i = 0; i < m; i++) {
                add_name(&files, &n, sk[i]);
            }
            free(sk);
        }
    }
    free(skills);
    if (s->io.fs_exists && s->io.fs_exists(s->io.ud, "FLOW.lua")) {
        add_name(&files, &n, strdup("FLOW.lua"));
    }
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        char *src = s->io.fs_read(s->io.ud, files[i], &len, err, sizeof(err));
        if (src) {
            bl_load(t, files[i], src, len, strcmp(files[i], "FLOW.lua") != 0, test_diag, r);
        }
        free(src);
        free(files[i]);
    }
    free(files);
}

/* A score's count: an integer when it is whole (so it prints 3, not 3.0) */
static void push_count(lua_State *L, double v)
{
    if (v > -1e15 && v < 1e15 && v == (double)(lua_Integer)v) {
        lua_pushinteger(L, (lua_Integer)v);
    } else {
        lua_pushnumber(L, v);
    }
}

static int l_test(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *path = luaL_optstring(L, 1, "functions/tests");
    if (!s->io.fs_read || !s->io.fs_list) {
        return luaL_error(L, "bot.test is not available here");
    }
    char **tests = NULL;
    size_t pl = strlen(path);
    int nt = 0;
    if (pl > 4 && !strcmp(path + pl - 4, ".lua")) {
        add_name(&tests, &nt, strdup(path));
    } else {
        nt = list_lua(s, path, &tests);
    }
    bl_io io = {.fs_read = s->io.fs_read, .fs_list = s->io.fs_list, .fs_exists = s->io.fs_exists, .today = s->io.today,
                .interrupted = s->io.interrupted, .skill_dir = s->io.skill_dir, .ud = s->io.ud}; /* no writes, no network */
    test_rep_t r = {NULL, 0, 0};
    int passed = 0, failed = 0;
    lua_newtable(L); /* the result */
    int res = lua_gettop(L);
    lua_newtable(L); /* its scores */
    int scores = lua_gettop(L), nscore = 0;
    if (!nt) {
        rep_add(&r, "%s: no test files\n", path);
    }
    for (int i = 0; i < nt; i++) {
        bl_state *t = bl_open(&io);
        char err[200] = "";
        size_t len = 0;
        char *src = t ? s->io.fs_read(s->io.ud, tests[i], &len, err, sizeof(err)) : NULL;
        int before = r.errors;
        if (t) {
            load_project(s, t, &r);
        }
        if (!src) {
            rep_add(&r, "%s: cannot read: %s\n", tests[i], err);
            failed++;
        } else if (r.errors != before || bl_load(t, tests[i], src, len, 0, test_diag, &r) != 0) {
            failed++; /* the project or the test file does not load: the report says why */
        } else {
            int n = 0;
            char **names = bl_globals(t, "score_", &n);
            for (int k = 0; k < n; k++) {
                double got = 0, total = 0;
                char *details = NULL, *why = bl_call_score(t, names[k], &got, &total, &details);
                if (why) {
                    rep_add(&r, "%s: %s FAILED: %s\n", tests[i], names[k], why);
                    failed++;
                } else {
                    lua_newtable(L);
                    lua_pushstring(L, names[k] + 6);
                    lua_setfield(L, -2, "name");
                    push_count(L, got);
                    lua_setfield(L, -2, "passed");
                    push_count(L, total);
                    lua_setfield(L, -2, "total");
                    lua_pushstring(L, details ? details : "");
                    lua_setfield(L, -2, "details");
                    lua_rawseti(L, scores, ++nscore);
                    rep_add(&r, "score %s: %g/%g\n%s%s", names[k] + 6, got, total, details ? details : "",
                            details && *details && details[strlen(details) - 1] != '\n' ? "\n" : "");
                }
                free(why);
                free(details);
                free(names[k]);
            }
            free(names);
            names = bl_globals(t, "test_", &n);
            for (int k = 0; k < n; k++) {
                char *why = bl_call_plain(t, names[k]);
                if (why) {
                    rep_add(&r, "%s: test %s FAILED: %s\n", tests[i], names[k], why);
                    failed++;
                } else {
                    passed++;
                }
                free(why);
                free(names[k]);
            }
            free(names);
        }
        free(src);
        bl_close(t);
        free(tests[i]);
    }
    free(tests);
    lua_setfield(L, res, "scores");
    rep_add(&r, "tests: %d passed, %d failed\n", passed, failed);
    lua_pushstring(L, r.p ? r.p : "");
    lua_setfield(L, res, "report");
    free(r.p);
    lua_pushinteger(L, passed);
    lua_setfield(L, res, "passed");
    lua_pushinteger(L, failed);
    lua_setfield(L, res, "failed");
    return 1;
}

/* bot.skill_dir(name): the real folder of skills/<name> (scripts/, assets/ ...), or nil */
static int l_skill_dir(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *v = s->io.skill_dir ? s->io.skill_dir(s->io.ud, luaL_checkstring(L, 1)) : NULL;
    if (v) {
        lua_pushstring(L, v);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* bot.secret(name): a key from .env or the sealed vault (never return it to the model) */
static int l_secret(lua_State *L)
{
    bl_state *s = state_of(L);
    const char *v = s->io.secret ? s->io.secret(s->io.ud, luaL_checkstring(L, 1)) : NULL;
    if (v) {
        lua_pushstring(L, v);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* print() writes to stderr: stdout belongs to the conversation (and to --run's results). */
static int l_print(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *t = luaL_tolstring(L, i, &len);
        if (i > 1) {
            fputc('\t', stderr);
        }
        fwrite(t, 1, len, stderr);
        lua_pop(L, 1);
    }
    fputc('\n', stderr);
    return 0;
}

static void set_funcs(lua_State *L, const char *name, const luaL_Reg *r)
{
    lua_newtable(L);
    luaL_setfuncs(L, r, 0);
    lua_setfield(L, -2, name);
}

bl_state *bl_open(const bl_io *io)
{
    bl_state *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    if (io) {
        s->io = *io;
    }
    s->L = lua_newstate(bl_alloc, s);
    if (!s->L) {
        free(s);
        return NULL;
    }
    lua_State *L = s->L;
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string},
                                    {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
                                    {LUA_UTF8LIBNAME, luaopen_utf8}, {NULL, NULL}};
    for (const luaL_Reg *l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    /* nothing that loads code from elsewhere, and nothing random: same input, same output (TEACH below
     * replaces each with a function that says what to use instead) */
    lua_register(L, "print", l_print);
    /* what models reach for out of habit: say what to use instead of "attempt to index a nil value" */
    static const char TEACH[] =
        "local function refuse(what, instead)\n"
        "  return function() error(what .. ' is not available in functions: ' .. instead, 2) end\n"
        "end\n"
        "require = refuse('require', 'every file in functions/ is already loaded; call its global functions directly')\n"
        "dofile = refuse('dofile', 'every file in functions/ is already loaded; read data files with bot.fs.read')\n"
        "loadfile, load = dofile, refuse('load', 'write the code as a function instead')\n"
        "local function guarded(name, instead)\n"
        "  return setmetatable({}, { __index = function(_, k) error(name .. '.' .. tostring(k) .. "
        "' is not available in functions: ' .. instead, 2) end })\n"
        "end\n"
        "io = guarded('io', 'read and write files with bot.fs.read(path), bot.fs.write(path, text), bot.fs.append, bot.fs.list')\n"
        "os = guarded('os', 'use bot.today() for today, bot.date.add_days / add_months / diff_days / weekday for dates, "
        "and bot.shell(command) for commands')\n"
        "package = guarded('package', 'every file in functions/ is already loaded')\n"
        "math.random = refuse('math.random', 'functions give the same answer every time; pass any choice in as a parameter')\n"
        "math.randomseed = math.random\n";
    if (luaL_dostring(L, TEACH) != LUA_OK) {
        lua_pop(L, 1);
    }

    lua_newtable(L); /* bot */
    static const luaL_Reg json[] = {{"encode", l_json_encode}, {"decode", l_json_decode}, {NULL, NULL}};
    static const luaL_Reg csv[] = {{"parse", l_csv_parse}, {"encode", l_csv_encode}, {NULL, NULL}};
    static const luaL_Reg date[] = {{"add_days", l_date_add_days},   {"add_months", l_date_add_months},
                                    {"diff_days", l_date_diff_days}, {"weekday", l_date_weekday},
                                    {"is_valid", l_date_is_valid},   {NULL, NULL}};
    static const luaL_Reg fs[] = {{"read", l_fs_read}, {"write", l_fs_write},   {"append", l_fs_append},
                                  {"list", l_fs_list}, {"exists", l_fs_exists}, {NULL, NULL}};
    set_funcs(L, "json", json);
    set_funcs(L, "csv", csv);
    set_funcs(L, "date", date);
    static const luaL_Reg http[] = {{"get", l_http_get}, {"post", l_http_post}, {NULL, NULL}};
    set_funcs(L, "fs", fs);
    set_funcs(L, "http", http);
    static const luaL_Reg top[] = {{"today", l_today}, {"shell", l_shell},   {"mcp", l_mcp},  {"agent", l_agent},
                                   {"secret", l_secret}, {"tool", l_tool}, {"skill_dir", l_skill_dir},
                                   {"worker", l_worker}, {"ask", l_ask}, {"say", l_say}, {"usage", l_usage}, {"test", l_test},
                                   {"llm", l_llm}, {NULL, NULL}};
    luaL_setfuncs(L, top, 0);
    lua_setglobal(L, "bot");
    lua_sethook(L, bl_hook, LUA_MASKCOUNT, 10000);
    return s;
}

static void fn_free(bl_fn *f)
{
    free(f->name);
    free(f->file);
    free(f->desc);
    cJSON_Delete(f->params);
    for (int i = 0; i < f->nparams; i++) {
        free(f->order[i]);
    }
    free(f->order);
}

void bl_close(bl_state *s)
{
    if (!s) {
        return;
    }
    for (int i = 0; i < s->nfn; i++) {
        fn_free(&s->fn[i]);
    }
    free(s->fn);
    lua_close(s->L);
    for (scratch *f = s->scratch, *nx; f; f = nx) {
        nx = f->next;
        free(f->path);
        free(f->data);
        free(f);
    }
    free(s);
}

int bl_count(const bl_state *s) { return s ? s->nfn : 0; }
const bl_fn *bl_get(const bl_state *s, int i) { return s && i >= 0 && i < s->nfn ? &s->fn[i] : NULL; }

const bl_fn *bl_find(const bl_state *s, const char *name)
{
    for (int i = 0; s && i < s->nfn; i++) {
        if (!strcmp(s->fn[i].name, name)) {
            return &s->fn[i];
        }
    }
    return NULL;
}

/* ---- doc comments -------------------------------------------------------- */

typedef struct {
    bl_diag     diag;
    void       *ud;
    const char *file;
    int         errors;
} rep_t;

static void report(rep_t *r, int line, int error, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void report(rep_t *r, int line, int error, const char *fmt, ...)
{
    char m[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    r->errors += error;
    if (r->diag) {
        r->diag(r->ud, r->file, line, error, m);
    }
}

/* A Lua error message "functions/x.lua:12: text" -> its line and text. */
static void report_lua(rep_t *r, const char *msg)
{
    size_t fl = strlen(r->file);
    if (!strncmp(msg, r->file, fl) && msg[fl] == ':' && isdigit((unsigned char)msg[fl + 1])) {
        char *end;
        long line = strtol(msg + fl + 1, &end, 10);
        report(r, (int)line, 1, "%s", *end == ':' ? end + 1 + (end[1] == ' ') : end);
    } else {
        report(r, 0, 1, "%s", msg);
    }
}

static char *trim_dup(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) {
        s++, n--;
    }
    while (n && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    return strndup(s, n);
}

/* A @param / @return type -> JSON Schema (NULL = not a type we know; *why says so). */
static cJSON *type_schema(const char *t, const char **why)
{
    size_t n = strlen(t);
    if (n > 2 && !strcmp(t + n - 2, "[]")) {
        char *inner = strndup(t, n - 2);
        cJSON *items = inner ? type_schema(inner, why) : NULL;
        free(inner);
        if (!items) {
            return NULL;
        }
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "type", "array");
        cJSON_AddItemToObject(a, "items", items);
        return a;
    }
    if (*t == '"' || *t == '\'') { /* "a"|"b": one of these words */
        cJSON *o = cJSON_CreateObject(), *e = cJSON_AddArrayToObject(o, "enum");
        cJSON_AddStringToObject(o, "type", "string");
        for (const char *p = t; *p;) {
            char q = *p;
            const char *end = (q == '"' || q == '\'') ? strchr(p + 1, q) : NULL;
            if (!end) {
                *why = "write a choice as \"a\"|\"b\", each word in quotes";
                cJSON_Delete(o);
                return NULL;
            }
            char *w = strndup(p + 1, (size_t)(end - p - 1));
            cJSON_AddItemToArray(e, cJSON_CreateString(w ? w : ""));
            free(w);
            p = end + 1;
            if (*p == '|') {
                p++;
            } else if (*p) {
                *why = "write a choice as \"a\"|\"b\", each word in quotes";
                cJSON_Delete(o);
                return NULL;
            }
        }
        return o;
    }
    static const char *const map[][2] = {{"string", "string"},   {"number", "number"}, {"integer", "integer"},
                                         {"boolean", "boolean"}, {"table", "object"},  {"object", "object"}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(t, map[i][0])) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", map[i][1]);
            return o;
        }
    }
    if (!strcmp(t, "any")) {
        return cJSON_CreateObject();
    }
    *why = strchr(t, '|') ? "use one type (or \"a\"|\"b\" for a choice of words)"
                          : "types are string, number, integer, boolean, table, any, T[] or \"a\"|\"b\"";
    return NULL;
}

/* What a function's own code reaches, from its text (from `function` to the `end` at column 0). */
static void scan_uses(const char *body, size_t n, char *out, size_t cap)
{
    static const char *const pat[][2] = {
        {"bot.fs.read", "reads files"},   {"bot.fs.list", "reads files"},  {"bot.fs.exists", "reads files"},
        {"bot.fs.write", "writes files"}, {"bot.fs.append", "writes files"}, {"bot.today", "the clock"},
        {"bot.http", "the network"},      {"bot.shell", "commands"},      {"bot.mcp", "MCP servers"},
        {"bot.agent", "other agents"},    {"bot.secret", "secrets"},     {"bot.tool", "tools"},
    };
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(pat) / sizeof(pat[0]); i++) {
        if (memmem(body, n, pat[i][0], strlen(pat[i][0])) && !strstr(out, pat[i][1])) {
            size_t l = strlen(out);
            snprintf(out + l, cap - l, "%s%s", l ? ", " : "", pat[i][1]);
        }
    }
}

static int is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Code that only looks done: a placeholder word in a comment, or a loop with nothing but comments in it.
 * Weak models write these and call the job finished; the warning makes the build say otherwise. */
static void scan_unfinished(rep_t *r, const char *name, const char *body, size_t n, int first_line)
{
    static const char *const words[] = {"todo", "fixme", "placeholder", "simplified", "for demo", "demo only",
                                        "not implemented", "stub", "mock", "dummy", "hard-coded", "hardcoded",
                                        "for this test", "in a real", "implement later", "to be implemented", "under construction", NULL};
    int line = first_line, loop_line = 0, loop_empty = 0;
    for (const char *p = body, *e = body + n; p < e; line++) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(e - p);
        char low[400];
        size_t k = 0;
        for (size_t i = 0; i < ll && k < sizeof(low) - 1; i++) {
            low[k++] = (char)tolower((unsigned char)p[i]);
        }
        low[k] = '\0';
        const char *t = low + strspn(low, " \t");
        const char *comment = strstr(low, "--");
        for (int w = 0; comment && words[w]; w++) {
            if (strstr(comment, words[w])) {
                report(r, line, 0, "%s looks unfinished (\"%s\"): finish the real logic, or remove the note if it is done",
                       name, words[w]);
                break;
            }
        }
        size_t tl = strlen(t);
        int opens = (!strncmp(t, "for ", 4) || !strncmp(t, "while ", 6)) && tl >= 2 && !strcmp(t + tl - 2, "do");
        if (opens) {
            loop_line = line, loop_empty = 1;
        } else if (loop_line && (!strncmp(t, "end", 3) && (t[3] == '\0' || !is_ident(t[3])))) {
            if (loop_empty) {
                report(r, loop_line, 0, "%s has a loop that does nothing (only comments inside): write what it should do", name);
            }
            loop_line = 0;
        } else if (loop_line && *t && strncmp(t, "--", 2) != 0) {
            loop_empty = 0;
        }
        p = nl ? nl + 1 : e;
    }
}



/* The documented function that starts at `line`; doc = its comment lines. */
static void add_function(bl_state *s, rep_t *r, const char *line, size_t ll, int lno, char **doc, int *docl, int nd,
                         const char *rest, size_t rest_n)
{
    const char *p = line + 8;
    while (*p == ' ') {
        p++;
    }
    const char *nm = p;
    while (is_ident(*p)) {
        p++;
    }
    if (p == nm || (*p == '.' || *p == ':')) {
        report(r, lno, 0, "only a global function (function name(...)) becomes a tool; this doc comment is ignored");
        return;
    }
    char *name = strndup(nm, (size_t)(p - nm));
    while (*p == ' ') {
        p++;
    }
    const char *close = *p == '(' ? memchr(p, ')', ll - (size_t)(p - line)) : NULL;
    if (!name || !close) {
        report(r, lno, 1, "write the function on one line: function %s(a, b)", name ? name : "name");
        free(name);
        return;
    }
    if (bl_find(s, name)) {
        report(r, lno, 1, "%s is defined twice (another file or earlier in this one)", name);
        free(name);
        return;
    }
    bl_fn f = {0};
    f.name = name;
    f.file = strdup(r->file);
    f.line = lno;
    f.params = cJSON_CreateObject();
    cJSON_AddStringToObject(f.params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(f.params, "properties"), *req = cJSON_CreateArray();
    /* the signature's parameters, in order */
    for (const char *q = p + 1; q < close;) {
        while (q < close && (*q == ' ' || *q == ',')) {
            q++;
        }
        const char *b = q;
        while (q < close && *q != ',') {
            q++;
        }
        char *pn = trim_dup(b, (size_t)(q - b));
        if (pn && *pn) {
            if (!strcmp(pn, "...")) {
                report(r, lno, 1, "%s: a tool takes named parameters, not ...", name);
            }
            f.order = realloc(f.order, sizeof(char *) * (size_t)(f.nparams + 1));
            f.order[f.nparams++] = pn;
        } else {
            free(pn);
        }
    }
    char *desc = NULL, *ret = NULL;
    size_t dl = 0;
    FILE *ds = open_memstream(&desc, &dl);
    int seen[64] = {0};
    for (int i = 0; i < nd; i++) {
        const char *d = doc[i] + 3; /* after --- */
        if (*d == '@') {
            char tag[16] = "", pname[65] = "", ptype[200] = "";
            int used = 0;
            sscanf(d + 1, "%15s%n", tag, &used);
            const char *t = d + 1 + used;
            if (!strcmp(tag, "param")) {
                int u2 = 0;
                if (sscanf(t, " %64s %199s%n", pname, ptype, &u2) < 2) {
                    report(r, docl[i], 1, "write ---@param name type description");
                    continue;
                }
                int opt = 0;
                size_t pl = strlen(pname), tl = strlen(ptype);
                if (pl && pname[pl - 1] == '?') {
                    pname[--pl] = '\0', opt = 1;
                }
                if (tl > 1 && ptype[tl - 1] == '?') {
                    ptype[--tl] = '\0', opt = 1;
                }
                if (tl > 4 && !strcmp(ptype + tl - 4, "|nil")) {
                    ptype[tl - 4] = '\0', opt = 1;
                }
                int k = -1;
                for (int j = 0; j < f.nparams; j++) {
                    if (!strcmp(f.order[j], pname)) {
                        k = j;
                    }
                }
                if (k < 0) {
                    report(r, docl[i], 1, "@param %s: %s has no parameter called %s", pname, name, pname);
                    continue;
                }
                const char *why = "";
                cJSON *sch = type_schema(ptype, &why);
                if (!sch) {
                    report(r, docl[i], 1, "@param %s: unknown type %s (%s)", pname, ptype, why);
                    continue;
                }
                char *pd = trim_dup(t + u2, strlen(t + u2));
                if (pd && *pd) {
                    cJSON_AddStringToObject(sch, "description", pd);
                }
                free(pd);
                cJSON_AddItemToObject(props, pname, sch);
                if (!opt) {
                    cJSON_AddItemToArray(req, cJSON_CreateString(pname));
                }
                if (k < 64) {
                    seen[k] = 1;
                }
            } else if (!strcmp(tag, "return")) {
                free(ret);
                ret = trim_dup(t, strlen(t));
            } else {
                report(r, docl[i], 0, "@%s is not used (only @param and @return)", tag);
            }
            continue;
        }
        char *text = trim_dup(d, strlen(d));
        if (ds) {
            fputs(text && *text ? text : "", ds);
            fputc('\n', ds);
        }
        free(text);
    }
    if (ds) {
        fclose(ds);
    }
    for (int j = 0; j < f.nparams && j < 64; j++) {
        if (!seen[j]) {
            report(r, lno, 1, "%s: parameter %s has no ---@param line (the model needs its type)", name, f.order[j]);
        }
    }
    char *d = desc ? trim_dup(desc, strlen(desc)) : NULL;
    free(desc);
    if (!d || !*d) {
        report(r, lno, 1, "%s: the doc comment needs a sentence saying what the function does", name);
    }
    if (ret && *ret) {
        char *full = NULL;
        if (asprintf(&full, "%s\nReturns: %s", d ? d : "", ret) >= 0) {
            free(d);
            d = full;
        }
    }
    free(ret);
    f.desc = d ? d : strdup("");
    if (cJSON_GetArraySize(req)) {
        cJSON_AddItemToObject(f.params, "required", req);
    } else {
        cJSON_Delete(req);
    }
    scan_uses(rest, rest_n, f.uses, sizeof(f.uses));
    scan_unfinished(r, name, rest, rest_n, lno + 1);
    if (s->nfn == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->fn = realloc(s->fn, sizeof(*s->fn) * (size_t)s->cap);
    }
    s->fn[s->nfn++] = f;
}

/* Walk the source: `---` lines directly above `function name(` at column 0 document it. */
static void collect(bl_state *s, rep_t *r, const char *src, size_t len)
{
    char *doc[200];
    int docl[200], nd = 0, lno = 0;
    for (const char *p = src, *e = src + len; p < e;) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(e - p);
        lno++;
        int is_doc = ll >= 3 && !strncmp(p, "---", 3) && (ll == 3 || p[3] != '-');
        if (is_doc) {
            if (nd < 200) {
                docl[nd] = lno;
                doc[nd++] = strndup(p, ll);
            }
        } else {
            if (nd && ll > 9 && !strncmp(p, "function ", 9)) {
                /* the body: up to the first `end` at column 0 */
                const char *b = nl ? nl + 1 : e, *stop = b;
                while (stop < e) {
                    const char *n2 = memchr(stop, '\n', (size_t)(e - stop));
                    size_t l2 = n2 ? (size_t)(n2 - stop) : (size_t)(e - stop);
                    if (l2 >= 3 && !strncmp(stop, "end", 3) && (l2 == 3 || !is_ident(stop[3]))) {
                        break;
                    }
                    stop = n2 ? n2 + 1 : e;
                }
                char *line = strndup(p, ll);
                if (line) {
                    add_function(s, r, line, ll, lno, doc, docl, nd, b, (size_t)(stop - b));
                }
                free(line);
            } else if (nd && ll > 15 && !strncmp(p, "local function ", 15)) {
                report(r, lno, 0, "a local function is a helper, never a tool; drop `local` to make it one");
            }
            for (int i = 0; i < nd; i++) {
                free(doc[i]);
            }
            nd = 0;
        }
        p = nl ? nl + 1 : e;
    }
    for (int i = 0; i < nd; i++) {
        free(doc[i]);
    }
}

static int msgh(lua_State *L)
{
    if (!lua_isstring(L, 1)) {
        lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    return 1;
}

/* A call with a time limit (0 = none). Calls can nest (a function asks a sub-agent, whose model calls a
 * function): the inner call gets its own limit, and the outer one's is back when it returns. */
static int pcall_limited(bl_state *s, int nargs, int nres, int seconds)
{
    int base = lua_gettop(s->L) - nargs;
    double outer = s->deadline;
    int outer_s = s->limit_s;
    lua_pushcfunction(s->L, msgh);
    lua_insert(s->L, base);
    s->limit_s = seconds;
    s->deadline = seconds ? now_s() + seconds : 0;
    int rc = lua_pcall(s->L, nargs, nres, base);
    s->deadline = outer;
    s->limit_s = outer_s;
    lua_remove(s->L, base);
    return rc;
}

int bl_load(bl_state *s, const char *file, const char *src, size_t len, int collect_fns, bl_diag diag, void *ud)
{
    rep_t r = {diag, ud, file, 0};
    char chunk[300];
    snprintf(chunk, sizeof(chunk), "@%s", file);
    int first = s->nfn;
    if (luaL_loadbufferx(s->L, src, len, chunk, "t") != LUA_OK) {
        report_lua(&r, lua_tostring(s->L, -1));
        lua_settop(s->L, 0);
        return -1;
    }
    if (pcall_limited(s, 0, 0, BL_LOAD_S) != LUA_OK) {
        report_lua(&r, lua_tostring(s->L, -1));
        lua_settop(s->L, 0);
        return -1;
    }
    if (collect_fns) {
        collect(s, &r, src, len);
        for (int i = first; i < s->nfn; i++) { /* documented, but not what the file defined */
            int t = lua_getglobal(s->L, s->fn[i].name);
            lua_pop(s->L, 1);
            if (t != LUA_TFUNCTION) {
                report(&r, s->fn[i].line, 1, "%s is not a global function once the file has run (is it inside a block?)",
                       s->fn[i].name);
            }
        }
    }
    lua_settop(s->L, 0);
    return r.errors ? -1 : 0;
}

int bl_check_syntax(const char *file, const char *src, size_t len, bl_diag diag, void *ud)
{
    bl_state *s = bl_open(NULL);
    if (!s) {
        return -1;
    }
    rep_t r = {diag, ud, file, 0};
    char chunk[300];
    snprintf(chunk, sizeof(chunk), "@%s", file);
    if (luaL_loadbufferx(s->L, src, len, chunk, "t") != LUA_OK) {
        report_lua(&r, lua_tostring(s->L, -1));
    }
    bl_close(s);
    return r.errors ? -1 : 0;
}

void bl_schema_append(const bl_state *s, cJSON *arr, const char *prefix)
{
    for (int i = 0; s && i < s->nfn; i++) {
        if (prefix && strncmp(s->fn[i].file, prefix, strlen(prefix)) != 0) {
            continue;
        }
        cJSON *t = cJSON_CreateObject(), *fn = cJSON_AddObjectToObject(t, "function");
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddStringToObject(fn, "name", s->fn[i].name);
        cJSON_AddStringToObject(fn, "description", s->fn[i].desc);
        cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(s->fn[i].params, 1));
        cJSON_AddItemToArray(arr, t);
    }
}

/* An argument as the model sent it, as the parameter's type wants it ("5" for a number is 5). */
static void push_arg(lua_State *L, const cJSON *v, const cJSON *schema)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(schema, "type"));
    if (type && cJSON_IsString(v) && (!strcmp(type, "number") || !strcmp(type, "integer"))) {
        char *end;
        double d = strtod(v->valuestring, &end);
        if (end != v->valuestring && !*end) {
            if (d == floor(d) && fabs(d) < 9007199254740992.0) {
                lua_pushinteger(L, (lua_Integer)d);
            } else {
                lua_pushnumber(L, d);
            }
            return;
        }
    }
    if (type && !strcmp(type, "string") && cJSON_IsNumber(v)) {
        char num[64];
        snprintf(num, sizeof(num), "%.15g", v->valuedouble);
        lua_pushstring(L, num);
        return;
    }
    push_json(L, v);
}

char *bl_call(bl_state *s, const char *name, const char *args_json)
{
    const bl_fn *f = bl_find(s, name);
    char *out = NULL;
    if (!f) {
        return strdup("ERROR: no such function");
    }
    cJSON *a = cJSON_Parse(args_json && *args_json ? args_json : "{}");
    if (!cJSON_IsObject(a)) {
        cJSON_Delete(a);
        return strdup("ERROR: arguments must be a JSON object");
    }
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(f->params, "required"), *rq;
    cJSON_ArrayForEach(rq, req)
    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, rq->valuestring);
        if (!v || cJSON_IsNull(v)) {
            if (asprintf(&out, "ERROR: %s needs the argument \"%s\"", name, rq->valuestring) < 0) {
                out = NULL;
            }
            cJSON_Delete(a);
            return out;
        }
    }
    lua_State *L = s->L;
    int top = lua_gettop(L); /* not 0 when this call runs inside another one */
    lua_getglobal(L, name);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(f->params, "properties");
    for (int i = 0; i < f->nparams; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, f->order[i]);
        if (v && !cJSON_IsNull(v)) {
            push_arg(L, v, cJSON_GetObjectItemCaseSensitive(props, f->order[i]));
        } else {
            lua_pushnil(L);
        }
    }
    cJSON_Delete(a);
    int r1 = top + 1, nres;
    if (pcall_limited(s, f->nparams, LUA_MULTRET, BL_CALL_S) != LUA_OK) {
        if (asprintf(&out, "ERROR: %s", lua_tostring(L, -1)) < 0) {
            out = NULL;
        }
    } else if ((nres = lua_gettop(L) - top) == 0 || lua_isnil(L, r1)) { /* nothing, or the Lua way of failing: nil, "why" */
        if (nres >= 2 && lua_type(L, r1 + 1) == LUA_TSTRING) {
            if (asprintf(&out, "ERROR: %s", lua_tostring(L, r1 + 1)) < 0) {
                out = NULL;
            }
        } else {
            out = strdup("(no result)");
        }
    } else if (lua_type(L, r1) == LUA_TSTRING) {
        size_t n;
        const char *t = lua_tolstring(L, r1, &n);
        out = strndup(t, n);
    } else {
        cJSON *j = to_json(L, r1, 0);
        out = j ? cJSON_PrintUnformatted(j) : NULL;
        cJSON_Delete(j);
        if (!out) {
            out = strdup("ERROR: the result has no JSON form (nested too deeply, or a loop)");
        }
    }
    lua_settop(L, top);
    return out ? out : strdup("ERROR: out of memory");
}

char *bl_call_plain(bl_state *s, const char *name)
{
    lua_settop(s->L, 0);
    lua_getglobal(s->L, name);
    char *err = NULL;
    if (pcall_limited(s, 0, 0, BL_CALL_S) != LUA_OK) {
        err = strdup(lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "failed");
    }
    lua_settop(s->L, 0);
    return err;
}

char *bl_call_score(bl_state *s, const char *name, double *passed, double *total, char **details)
{
    int top = lua_gettop(s->L);
    char *err = NULL;
    *details = NULL;
    lua_getglobal(s->L, name);
    if (pcall_limited(s, 0, 3, BL_CALL_S) != LUA_OK) {
        err = strdup(lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "failed");
    } else if (!lua_isnumber(s->L, top + 1) || !lua_isnumber(s->L, top + 2) || lua_tonumber(s->L, top + 2) <= 0) {
        err = strdup("a score_ function returns passed, total (numbers, total > 0) and optionally a text of details");
    } else {
        *passed = lua_tonumber(s->L, top + 1);
        *total = lua_tonumber(s->L, top + 2);
        if (lua_type(s->L, top + 3) == LUA_TSTRING) {
            *details = strdup(lua_tostring(s->L, top + 3));
        }
    }
    lua_settop(s->L, top);
    return err;
}

int bl_has_global(bl_state *s, const char *name)
{
    if (!s) {
        return 0;
    }
    int top = lua_gettop(s->L), f = lua_getglobal(s->L, name) == LUA_TFUNCTION;
    lua_settop(s->L, top);
    return f;
}

char *bl_call_global(bl_state *s, const char *name, const char *input, int json, char **reply)
{
    int top = lua_gettop(s->L);
    char *err = NULL;
    *reply = NULL;
    if (lua_getglobal(s->L, name) != LUA_TFUNCTION) {
        lua_settop(s->L, top);
        if (!strcmp(name, "flow")) {
            return strdup("FLOW.lua has no function flow(input)");
        }
        return asprintf(&err, "no global Lua function %s (define it in FLOW.lua or functions/)", name) < 0 ? NULL : err;
    }
    if (json) { /* --call: each input line is a JSON value, given to the function as a Lua value */
        cJSON *in = cJSON_Parse(input ? input : "");
        if (!in) {
            lua_settop(s->L, top);
            return strdup("the input line is not JSON");
        }
        push_json(s->L, in);
        cJSON_Delete(in);
    } else {
        lua_pushstring(s->L, input ? input : "");
    }
    if (pcall_limited(s, 1, 1, 0) != LUA_OK) {
        err = strdup(lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "failed");
    } else if (!json && (lua_type(s->L, -1) == LUA_TSTRING || lua_type(s->L, -1) == LUA_TNUMBER)) {
        *reply = strdup(lua_tostring(s->L, -1));
    } else if (lua_type(s->L, -1) != LUA_TNIL) {
        cJSON *j = to_json(s->L, -1, 0);
        *reply = j ? (json ? cJSON_PrintUnformatted(j) : cJSON_Print(j)) : NULL;
        cJSON_Delete(j);
    }
    lua_settop(s->L, top);
    return err;
}

char *bl_call_flow(bl_state *s, const char *input, char **reply) { return bl_call_global(s, "flow", input, 0, reply); }

char *bl_call_hook(bl_state *s, const char *name, const char *arg)
{
    if (!s) {
        return NULL;
    }
    int top = lua_gettop(s->L);
    if (lua_getglobal(s->L, name) != LUA_TFUNCTION) {
        lua_settop(s->L, top);
        return NULL;
    }
    lua_pushstring(s->L, arg ? arg : "");
    char *out = NULL;
    if (pcall_limited(s, 1, 1, BL_CALL_S) != LUA_OK) {
        if (asprintf(&out, "ERROR: %s", lua_tostring(s->L, -1)) < 0) {
            out = NULL;
        }
    } else if (lua_type(s->L, -1) == LUA_TSTRING && *lua_tostring(s->L, -1)) {
        out = strdup(lua_tostring(s->L, -1));
    }
    lua_settop(s->L, top);
    return out;
}

static int bl_cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

char **bl_globals(bl_state *s, const char *prefix, int *n)
{
    char **v = NULL;
    *n = 0;
    lua_pushglobaltable(s->L);
    lua_pushnil(s->L);
    while (lua_next(s->L, -2)) {
        if (lua_type(s->L, -2) == LUA_TSTRING && lua_isfunction(s->L, -1) &&
            !strncmp(lua_tostring(s->L, -2), prefix, strlen(prefix))) {
            v = realloc(v, sizeof(char *) * (size_t)(*n + 1));
            v[(*n)++] = strdup(lua_tostring(s->L, -2));
        }
        lua_pop(s->L, 1);
    }
    lua_settop(s->L, 0);
    if (*n) {
        qsort(v, (size_t)*n, sizeof(char *), bl_cmp_str);
    }
    return v;
}
