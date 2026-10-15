#ifndef BC_BOTLUA_H
#define BC_BOTLUA_H

#include "cJSON.h"
#include <stddef.h>

/*
 * An agent's functions: functions/<name>.lua, run by the vendored Lua 5.4 (lib/lua). Shared by botcore
 * (functions.c: the functions become tools) and packer (#includes this file's .c: checks them and
 * runs functions/tests/<name>.lua at build time).
 *
 * A function is a tool when a `---` doc comment sits directly above `function name(...)` at column 0:
 *
 *   --- Days between two dates.
 *   ---@param from string  first date, YYYY-MM-DD
 *   ---@param to string  second date, YYYY-MM-DD
 *   ---@return integer  to - from, in days
 *   function days_between(from, to) ... end
 *
 * Types: string number integer boolean table any, T[] (a list), "a"|"b" (one of these words);
 * `name?` or `type?` = optional. Other functions (no doc comment, or local) are helpers.
 *
 * Lua gets base (without dofile/loadfile/load; print goes to stderr), string, table, math (without
 * random) and utf8. Nothing else reaches the machine except the `bot` module:
 *   bot.json.encode(v) / bot.json.decode(s)
 *   bot.csv.parse(text) -> rows keyed by the header / bot.csv.encode(rows, columns) -> text
 *   bot.date.add_days(d, n) / add_months(d, n) / diff_days(from, to) / weekday(d) / is_valid(d)
 *   bot.today()                               (bl_io.today; BOTTER_TODAY pins it)
 *   bot.fs.read / write / append / list / exists   (bl_io: botcore keeps them in the working folder)
 *   bot.http.get(url [, headers]) / bot.http.post(url, body [, headers]) -> {status, body, json}
 *   bot.shell(command [, timeout_s]) -> output, exit code    (the shell_exec tool: same rules, same asking)
 *   bot.mcp(server, tool, args) -> text      bot.agent(name, task) -> text      bot.secret(name) -> value
 */

typedef struct {
    /* NULL = not available here; the Lua call then fails with a clear message. All paths are relative
     * to whatever folder the host decides (botcore: the working directory). err gets a message. */
    char *(*fs_read)(void *ud, const char *path, size_t *len, char *err, size_t el);  /* malloc'd */
    int   (*fs_write)(void *ud, const char *path, const char *data, size_t len, int append, char *err, size_t el);
    char *(*fs_list)(void *ud, const char *path, char *err, size_t el);  /* malloc'd: one name per line, dirs end in / */
    int   (*fs_exists)(void *ud, const char *path);
    int   (*today)(void *ud, char out[11]);  /* 0 ok */
    int   (*interrupted)(void *ud);          /* 1 = stop the running function (Ctrl-C) */
    /* Run one of the agent's tools (shell_exec, an MCP server's tool, delegate): malloc'd result text. */
    char *(*tool)(void *ud, const char *name, const char *args_json);
    /* GET (body NULL) or POST (JSON body); headers: "Name: value\r\n" lines. malloc'd body, NULL + err on failure. */
    char *(*http)(void *ud, const char *url, const char *body, const char *headers, int *status, char *err, size_t el);
    const char *(*secret)(void *ud, const char *name); /* a line of .env or the sealed vault, or NULL */
    void  *ud;
} bl_io;

typedef struct {
    char  *name;
    char  *file;     /* functions/x.lua */
    int    line;     /* of `function name(` */
    char  *desc;     /* what the model reads: the doc text (+ "Returns: ...") */
    cJSON *params;   /* JSON Schema object for the arguments */
    char **order;    /* parameter names, in the order the function takes them */
    int    nparams;
    char   uses[80]; /* "" = pure; else what its own code reaches: "reads files, the clock" */
} bl_fn;

typedef struct bl_state bl_state;

/* Diagnostics: file, line (0 = none), error (1) or warning (0), message. */
typedef void (*bl_diag)(void *ud, const char *file, int line, int error, const char *msg);

bl_state    *bl_open(const bl_io *io);
void         bl_close(bl_state *s);
/* Run one file (chunk name "functions/x.lua") and collect its documented functions. 0 ok, -1 errors (reported). */
int          bl_load(bl_state *s, const char *file, const char *src, size_t len, int collect, bl_diag diag, void *ud);
/* Syntax only (a test file, which needs the other files to run). 0 ok, -1 errors (reported). */
int          bl_check_syntax(const char *file, const char *src, size_t len, bl_diag diag, void *ud);
int          bl_count(const bl_state *s);
const bl_fn *bl_get(const bl_state *s, int i);
const bl_fn *bl_find(const bl_state *s, const char *name);
/* Append an OpenAI "function" tool for every documented function. */
void         bl_schema_append(const bl_state *s, cJSON *arr);
/* Call a documented function with a JSON object of arguments: malloc'd text ("ERROR: ..." on failure). */
char        *bl_call(bl_state *s, const char *name, const char *args_json);
/* Call a global function with no arguments (tests): NULL = it returned, else a malloc'd error message. */
char        *bl_call_plain(bl_state *s, const char *name);
/* Call a global function with one text argument, if it exists: malloc'd text it returned, or NULL when
 * it does not exist or returned nothing. An error is returned as "ERROR: ...". */
char        *bl_call_hook(bl_state *s, const char *name, const char *arg);
/* Names of the global functions whose names start with prefix, sorted; caller frees each and the array. */
char       **bl_globals(bl_state *s, const char *prefix, int *n);

#endif
