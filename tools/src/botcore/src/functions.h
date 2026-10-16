#ifndef BC_FUNCTIONS_H
#define BC_FUNCTIONS_H

#include "cJSON.h"
#include <stddef.h>

/*
 * The agent's functions/<name>.lua (botlua.h): each documented function is a tool the model can call.
 * Files reached from Lua (bot.fs) stay inside the working directory; writes are refused in Plan mode.
 */
void        fn_init(void);                   /* load the pack's functions; print errors */
void        fn_schema_append(cJSON *arr);                       /* functions/ (always tools) */
void        fn_skill_schema_append(const char *skill, cJSON *arr); /* skills/<skill>/functions/ */
int         fn_is(const char *name);
char       *fn_call(const char *name, const char *args_json); /* malloc'd result text */
int         fn_count(void);
/* The agent's on_turn_end(reply) hook, if its functions define one: what to tell the model next (malloc'd),
 * or NULL when the turn may end. */
char       *fn_turn_end(const char *reply);
/* An eval case's check: the global Lua function name(json_of_the_run) -> NULL when it passes, else why
 * (malloc'd); "ERROR: no such function" when the agent has none of that name. */
char       *fn_check(const char *name, const char *run_json);
/* FLOW.lua (progress.md §12.3): when the agent has one, every incoming job goes to flow(input) instead of the
 * model. fn_flow returns NULL and the reply (malloc'd, may be NULL), or a malloc'd error. */
int         fn_has_flow(void);
char       *fn_flow(const char *input, char **reply);

/* What main.c gives the functions for orchestration (bot.worker, bot.ask, bot.say). */
typedef struct {
    char *(*worker)(const char *spec_json, char *err, size_t el); /* malloc'd reply, or NULL + err */
    char *(*ask)(const char *question);                          /* malloc'd answer, or NULL */
    void  (*say)(const char *text);
    char *(*llm)(const char *spec_json, char *err, size_t el);   /* malloc'd reply, or NULL + err */
} fn_host_t;
void        fn_set_host(const fn_host_t *h);

/* How bot.shell / bot.mcp / bot.agent run a tool: main.c's runner (shows it, asks when risky). */
void        fn_set_tool_runner(char *(*run)(const char *name, const char *args_json));

#endif
