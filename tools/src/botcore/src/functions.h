#ifndef BC_FUNCTIONS_H
#define BC_FUNCTIONS_H

#include "cJSON.h"

/*
 * The agent's functions/<name>.lua (botlua.h): each documented function is a tool the model can call.
 * Files reached from Lua (bot.fs) stay inside the working directory; writes are refused in Plan mode.
 */
void        fn_init(void);                   /* load the pack's functions; print errors */
void        fn_schema_append(cJSON *arr);
int         fn_is(const char *name);
char       *fn_call(const char *name, const char *args_json); /* malloc'd result text */
int         fn_count(void);
/* The agent's on_turn_end(reply) hook, if its functions define one: what to tell the model next (malloc'd),
 * or NULL when the turn may end. */
char       *fn_turn_end(const char *reply);
/* How bot.shell / bot.mcp / bot.agent run a tool: main.c's runner (shows it, asks when risky). */
void        fn_set_tool_runner(char *(*run)(const char *name, const char *args_json));

#endif
