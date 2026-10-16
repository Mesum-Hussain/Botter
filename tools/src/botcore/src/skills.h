#ifndef BC_SKILLS_H
#define BC_SKILLS_H

#include <stddef.h>

/*
 * The agent's skills (progress.md §12.1): skills/<name>/SKILL.md plus anything the skill brings
 * (references/, scripts/, assets/, functions/). At runtime a skill's files appear at a real read-only
 * path, so standard Agent Skills work unmodified: the model reads them with fs_read and runs scripts
 * with shell_exec. The files are written on first use to a private folder in $XDG_RUNTIME_DIR (RAM)
 * or /tmp, and removed when the agent exits.
 *
 * A skill is loaded with the `skill` tool: the model gets SKILL.md and the file list, and the Lua
 * functions in skills/<name>/functions/ become tools of that conversation.
 */

int         sk_count(void);
/* Real path of a skill's folder (the files are written on first use), or NULL when there is no such skill. */
const char *sk_dir(const char *name);
/* 1 when abs (a resolved absolute path) is inside the folder that holds the skills. */
int         sk_inside(const char *abs);

#endif
