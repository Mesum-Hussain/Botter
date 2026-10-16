#ifndef BC_MEMORY_H
#define BC_MEMORY_H

#include "cJSON.h"
#include <stddef.h>

/*
 * Context + memory engine (every agent; RAM only). The transcript is not sent as it grew:
 *   - archive: past FADE_HIGH bytes, tool results and long tool-call arguments older than the recent
 *     window become one-line stubs with an id, in one batch (so provider prompt caching holds between
 *     batches); recall(id) brings one back.
 *   - working memory: the model's own notes (note tool), capped.
 *   - file ledger: every file written this session (size, lines, Lua check, tools it defines).
 *   - data profiles: fs_read of a big CSV/TSV returns a profile (rows, columns, types, samples), kept here.
 * Notes, ledger and profiles are shown to the model at the end of the latest message of each request.
 */

/* History size that starts a batch of archiving, and what stays raw at the end. */
#define MEM_FADE_HIGH  (64 * 1024)
#define MEM_KEEP_BYTES (24 * 1024)
#define MEM_KEEP_TOOLS 6
/* Text shorter than this is never archived (a stub would not save much). */
#define MEM_FADE_MIN   400
/* fs_read of a .csv/.tsv bigger than this returns a profile (unless an offset is given). */
#define MEM_PROFILE_MIN (16 * 1024)

/* Bytes of message text in a history: contents and tool-call arguments (the system prompt included). */
size_t mem_hist_bytes(const cJSON *hist);
size_t mem_msg_bytes(const cJSON *m);

/* Archive old tool traffic in hist when it is over MEM_FADE_HIGH. Originals go to `archive` (an array;
 * id = index + 1). Returns how many pieces were archived. */
int    mem_compact(cJSON *hist, cJSON *archive);

/* The memory block for the end of the next request, or NULL when there is nothing to show. malloc'd. */
char  *mem_state(const cJSON *notes, const cJSON *archive);

/* A file was written (path as resolved; check = its Lua check in a few words, or NULL to keep the last). */
void   mem_file_written(const char *res, const char *check);

/* fs_read of a data file: 1 = result holds a profile instead of the raw text, 0 = read it normally. */
int    mem_profile(const char *res, const char *path, long long size, char *result, size_t rl);

/* Forget the ledger and profiles (a new conversation). */
void   mem_reset(void);

#endif
