#define _GNU_SOURCE
#include "skills.h"
#include "chat.h"
#include "functions.h"
#include "tools.h"
#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char  g_root[PATH_MAX]; /* "" until the files are written */
static pid_t g_owner;

int sk_count(void)
{
    size_t n = 0;
    int c = 0;
    const vfs_entry_t *t = vfs_table(&n);
    for (size_t i = 0; i < n; i++) {
        char nm[65];
        c += sscanf(t[i].path, "skills/%64[^/]/SKILL.m%c", nm, &(char){0}) == 2;
    }
    return c;
}

/* Remove a tree we wrote (folders were made read-only: make them writable first). */
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    chmod(path, 0700);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char p[PATH_MAX];
        if (snprintf(p, sizeof(p), "%s/%s", path, e->d_name) >= (int)sizeof(p)) {
            continue;
        }
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p);
        } else {
            unlink(p);
        }
    }
    if (d) {
        closedir(d);
    }
    rmdir(path);
}

static void cleanup(void)
{
    if (g_root[0] && getpid() == g_owner) { /* not in a forked child that calls exit() */
        remove_tree(g_root);
        g_root[0] = '\0';
    }
}

/* Folders left by agents that were killed (botcore-skills-<pid>-*, pid no longer running). */
static void remove_stale(const char *base)
{
    DIR *d = opendir(base);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        long pid;
        char p[PATH_MAX];
        if (sscanf(e->d_name, "botcore-skills-%ld-", &pid) == 1 && pid > 0 && kill((pid_t)pid, 0) != 0 &&
            errno == ESRCH && snprintf(p, sizeof(p), "%s/%s", base, e->d_name) < (int)sizeof(p)) {
            remove_tree(p);
        }
    }
    if (d) {
        closedir(d);
    }
}

static int mkdirs(char *path) /* every parent of path; path is modified temporarily */
{
    for (char *p = path + strlen(g_root) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            int r = mkdir(path, 0700);
            *p = '/';
            if (r != 0 && errno != EEXIST) {
                return -1;
            }
        }
    }
    return 0;
}

static void lock_tree(const char *path) /* folders read-only once everything is written */
{
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[PATH_MAX];
        struct stat st;
        if (e->d_name[0] != '.' && snprintf(p, sizeof(p), "%s/%s", path, e->d_name) < (int)sizeof(p) &&
            lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            lock_tree(p);
        }
    }
    if (d) {
        closedir(d);
    }
    chmod(path, 0500);
}

/* Write every skills/ file of the pack under a new private folder. 0 ok. */
static int materialize(void)
{
    if (g_root[0]) {
        return 0;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    const char *base = rt && *rt && stat(rt, &st) == 0 && S_ISDIR(st.st_mode) && access(rt, W_OK | X_OK) == 0 ? rt : "/tmp";
    remove_stale(base);
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%s/botcore-skills-%ld-XXXXXX", base, (long)getpid());
    if (!mkdtemp(tmpl) || !realpath(tmpl, g_root)) {
        g_root[0] = '\0';
        return -1;
    }
    g_owner = getpid();
    atexit(cleanup);
    size_t n = 0;
    const vfs_entry_t *t = vfs_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(t[i].path, "skills/", 7) != 0) {
            continue;
        }
        char p[PATH_MAX];
        if (snprintf(p, sizeof(p), "%s/%s", g_root, t[i].path + 7) >= (int)sizeof(p) || mkdirs(p) != 0) {
            continue;
        }
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, t[i].kind == VFS_DATA ? 0444 : 0555);
        size_t done = 0;
        while (fd >= 0 && done < t[i].len) {
            ssize_t w = write(fd, t[i].data + done, t[i].len - done);
            if (w <= 0 && errno != EINTR) {
                break;
            }
            done += w > 0 ? (size_t)w : 0;
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    lock_tree(g_root);
    return 0;
}

const char *sk_dir(const char *name)
{
    static char dir[PATH_MAX];
    char md[200];
    if (!name || !*name || strchr(name, '/') || snprintf(md, sizeof(md), "skills/%s/SKILL.md", name) >= (int)sizeof(md) ||
        !vfs_find(md) || materialize() != 0) {
        return NULL;
    }
    return snprintf(dir, sizeof(dir), "%s/%s", g_root, name) < (int)sizeof(dir) ? dir : NULL;
}

int sk_inside(const char *abs)
{
    size_t n = strlen(g_root);
    return n && !strncmp(abs, g_root, n) && (abs[n] == '/' || abs[n] == '\0');
}

/* ---- the skill tool ---------------------------------------------------------- */

static bool skill_tool(const cJSON *in, char *result, size_t rl)
{
    const cJSON *nj = cJSON_GetObjectItemCaseSensitive(in, "name");
    const char *name = cJSON_IsString(nj) ? nj->valuestring : "";
    char md[200];
    snprintf(md, sizeof(md), "skills/%.100s/SKILL.md", name);
    const vfs_entry_t *e = vfs_find(md);
    const char *dir = e ? sk_dir(name) : NULL;
    if (!e) {
        snprintf(result, rl, "there is no skill \"%.100s\" (the system prompt lists your skills)", name);
        return false;
    }
    if (!dir) {
        snprintf(result, rl, "could not lay out the files of skill \"%s\": %s", name, strerror(errno));
        return false;
    }
    size_t used = 0;
#define OUT(...)                                                                   \
    do {                                                                           \
        int w_ = snprintf(result + used, rl - used, __VA_ARGS__);                  \
        used += w_ > 0 ? ((size_t)w_ < rl - used ? (size_t)w_ : rl - used - 1) : 0; \
    } while (0)
    OUT("Skill %s is loaded. Its files are at %s/ (read-only: read them with fs_read, run scripts with shell_exec "
        "there; write output in the working directory).",
        name, dir);
    size_t n = 0, files = 0, pl = strlen(md) - 8; /* "skills/<name>/" */
    const vfs_entry_t *t = vfs_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(t[i].path, md, pl) && strcmp(t[i].path, md) != 0 && strncmp(t[i].path + pl, "functions/", 10) != 0) {
            OUT("%s %s (%zu bytes%s)", files++ ? "," : "\nFiles:", t[i].path + pl, t[i].len,
                t[i].kind == VFS_DATA ? "" : ", runnable");
        }
    }
    chat_t *c = chat_current();
    cJSON *added = cJSON_CreateArray(), *a;
    fn_skill_schema_append(name, added);
    int nadded = 0;
    while (c && (a = cJSON_DetachItemFromArray(added, 0))) {
        const char *fn = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(a, "function"), "name"));
        OUT("%s %s", nadded++ ? "," : "\nNew tools from this skill:", fn ? fn : "?");
        if (fn && !chat_has_tool(c, fn)) {
            if (!c->tools) {
                c->tools = cJSON_CreateArray();
            }
            cJSON_AddItemToArray(c->tools, a);
        } else {
            cJSON_Delete(a);
        }
    }
    cJSON_Delete(added);
    OUT("\n\n%s", e->data);
#undef OUT
    return true;
}

const tool_t TOOLS_SKILL[] = {
    {"skill",
     "Load one of your skills before the task it covers: returns its instructions (SKILL.md), where its files "
     "are, and makes its functions callable.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"The skill's name, as "
     "listed in the system prompt\"}},\"required\":[\"name\"]}",
     skill_tool},
};
const size_t TOOLS_SKILL_N = sizeof(TOOLS_SKILL) / sizeof(TOOLS_SKILL[0]);
