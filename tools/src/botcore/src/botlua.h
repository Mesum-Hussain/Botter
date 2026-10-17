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
 *   bot.fs.read / write / append / list / exists   (bl_io: botcore keeps them in the working folder; with
 *       fs_read but no fs_write, as in tests, writes stay in memory and later reads/exists see them)
 *   bot.http.get(url [, headers]) / bot.http.post(url, body [, headers]) -> {status, body, json}
 *   bot.shell(command [, timeout_s]) -> output, exit code    (the shell_exec tool: same rules, same asking)
 *   bot.mcp(server, tool, args) -> text      bot.agent(name, task) -> text      bot.secret(name) -> value
 *   bot.tool(name, args) -> text: any of the agent's tools, as the model would call it (same rules)
 *   bot.skill_dir(name) -> the real folder of skills/<name> (its scripts/ and assets/ are there), or nil
 * Orchestration (FLOW.lua, progress.md §12.3; any function may use them):
 *   bot.worker{task, system?, context?, skills?, tools?, writes?, max_rounds?} -> the worker's reply: one task
 *       in a fresh conversation with the agent's model, narrowed to those skills, tools and writable paths
 *   bot.test(path?) -> {passed, failed, report, scores}: the project's Lua in the working folder, loaded fresh,
 *       and the test_/score_ functions of path (a test file or folder, default functions/tests) run
 *   bot.ask(question) -> the person's answer, or nil when nobody is there (batch, remote)
 *   bot.usage() -> {calls, input_tokens, output_tokens}: the model's use so far in this process (all models)
 *   bot.say(text): a progress line for the person
 *   bot.llm(prompt) / bot.llm{prompt, system?, schema?, images?} -> text, or a table when a schema is given: one
 *       request to the agent's own model, for judgement inside code (reading text or images, wording)
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
    const char *(*skill_dir)(void *ud, const char *name); /* real folder of skills/<name>, or NULL */
    /* bot.worker: one task in a fresh conversation (spec = JSON object); malloc'd reply, or NULL + err */
    char *(*worker)(void *ud, const char *spec_json, char *err, size_t el);
    /* bot.ask: the person's answer (malloc'd), or NULL when nobody can answer here */
    char *(*ask)(void *ud, const char *question);
    void  (*say)(void *ud, const char *text);           /* bot.say: a progress line for the person */
    /* bot.llm: one request to the agent's own model (spec = JSON object); malloc'd reply, or NULL + err */
    char *(*llm)(void *ud, const char *spec_json, char *err, size_t el);
    /* bot.usage: model calls and tokens so far, all models together; 0 ok */
    int   (*usage)(void *ud, long *calls, long long *in, long long *out);
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
/* Append an OpenAI "function" tool for every documented function whose file starts with prefix (NULL = all). */
void         bl_schema_append(const bl_state *s, cJSON *arr, const char *prefix);
/* Call a documented function with a JSON object of arguments: malloc'd text ("ERROR: ..." on failure). */
char        *bl_call(bl_state *s, const char *name, const char *args_json);
/* FLOW.lua: call flow(input) with no time limit (Ctrl-C still stops it). *reply = what it returned (malloc'd,
 * NULL for nil); returns NULL, or a malloc'd error message. */
char        *bl_call_flow(bl_state *s, const char *input, char **reply);
int          bl_has_global(bl_state *s, const char *name);
/* Call a global function with no arguments (tests): NULL = it returned, else a malloc'd error message. */
char        *bl_call_plain(bl_state *s, const char *name);
/* Call a score_ function (tests): it returns passed, total [, details]. NULL ok (*details may be set,
 * malloc'd), else a malloc'd error message. */
char        *bl_call_score(bl_state *s, const char *name, double *passed, double *total, char **details);
/* Call a global function with one text argument, if it exists: malloc'd text it returned, or NULL when
 * it does not exist or returned nothing. An error is returned as "ERROR: ...". */
char        *bl_call_hook(bl_state *s, const char *name, const char *arg);
/* Names of the global functions whose names start with prefix, sorted; caller frees each and the array. */
char       **bl_globals(bl_state *s, const char *prefix, int *n);

#endif
