#ifndef BC_FWL_RUN_H
#define BC_FWL_RUN_H

#include "fwl.h"

/*
 * The FWL interpreter: runs an agent's FLOW.fwl line by line. The structure is botcore's own
 * (IF / ELSE IF / ELSE, FOR EACH, WHILE, RETRY, ASK, RETURN, and conditions that compare
 * values); every sentence, and every condition that needs judgement, goes to the model.
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
    /* What this tool returned the last time it ran in the turn just finished (malloc'd), or NULL. */
    char *(*last_result)(void *ud, const char *tool);
    void *ud;
} sq_io;

#define SQ_DONE    0 /* ran to the end or RETURN */
#define SQ_ABORTED 1 /* the user interrupted or quit */

int fwl_run(sq_prog *prog, sq_io *io);

#endif
