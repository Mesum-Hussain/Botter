#ifndef BC_FWL_RUN_H
#define BC_FWL_RUN_H

#include "fwl.h"

/*
 * The FWL interpreter: runs an agent's FLOW.md statement by statement.
 * Structure is executed by botcore itself (variables, IF/FOR/WHILE/RETRY,
 * EXECUTE tool, ASK USER, SAVE, comparisons such as IS EQUAL TO); the LLM is
 * used only where a statement needs judgement: plain-English instructions,
 * INVOKE SKILL, DELEGATE TO (a sub-agent), and conditions or values written in prose.
 */
typedef struct {
    /* An LLM turn with tools, shown to the user like a normal reply. 0 ok, 1 interrupted, -1 failed. */
    int (*turn)(void *ud, const char *msg, char **reply);
    /* A quiet side question to the LLM (conversation as context, no tools, not shown). 0 ok, 1 interrupted, -1 failed. */
    int (*ask_llm)(void *ud, const char *question, char **reply);
    /* Ask the user (question shown as the agent's message). 0 ok, 1 the user ended the session. */
    int (*ask_user)(void *ud, const char *question, char **answer);
    /* Show text as the agent's message. */
    void (*say)(void *ud, const char *text);
    /* Show a progress line (STEP headings, retries). */
    void (*status)(void *ud, const char *text);
    /* Run a tool (with the usual guards and status line); malloc'd result, "ERROR: ..." on failure. */
    char *(*tool)(void *ud, const char *name, const char *args_json);
    /* Hand a task to a sub-agent (its own conversation and persona). 0 ok, 1 interrupted, -1 failed. */
    int (*agent)(void *ud, const char *name, const char *task, char **reply);
    void *ud;
} sq_io;

#define SQ_DONE    0 /* ran to the end or RETURN */
#define SQ_ABORTED 1 /* the user interrupted or quit */

int fwl_run(sq_prog *prog, sq_io *io);

#endif
