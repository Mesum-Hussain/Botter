#define _GNU_SOURCE
#include "botlua.h"
#include "chat.h"
#include "guard.h"
#include "memory.h"
#include "skills.h"
#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Resolve + gate a path. 0 = go ahead (resolved path in out); -1 = refused,
 * `result` already holds the message for the LLM.
 */
static int access_path(const char *tool, const char *path, const char *verb, char out[PATH_MAX], char *result,
                       size_t rl)
{
    int inside = 0;
    if (guard_resolve(path, out, &inside) != 0) {
        snprintf(result, rl, "invalid path '%.200s'", path);
        return -1;
    }
    if (!inside && sk_inside(out)) { /* the agent's own skill files: readable, never changed */
        if (strcmp(verb, "read") != 0 && strcmp(verb, "list") != 0) {
            snprintf(result, rl, "%s is one of your skill's files, which are read-only: write in the working directory", out);
            return -1;
        }
        return 0;
    }
    if (!inside && !guard_confirm("%s wants to %s a path OUTSIDE the working dir (%s):\n  %s", tool, verb,
                                  guard_ctx(), out)) {
        snprintf(result, rl, "denied by user: %s is outside the working directory", out);
        return -1;
    }
    return 0;
}

static const char *str_arg(const cJSON *in, const char *key)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(in, key);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

static int cmp_names(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int not_dots(const struct dirent *d)
{
    return strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0;
}

static bool fs_list(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path");
    char res[PATH_MAX];

    if (!path || !*path) {
        path = ".";
    }
    if (access_path("fs_list", path, "list", res, result, rl) != 0) {
        return false;
    }
    struct dirent **names = NULL;
    int n = scandir(res, &names, not_dots, cmp_names);
    if (n < 0) {
        snprintf(result, rl, "cannot list '%s': %s", path, strerror(errno));
        return false;
    }
    size_t off = 0;
    if (n == 0) {
        snprintf(result, rl, "(empty directory)");
    }
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX + 256];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", res, names[i]->d_name);
        int w;
        if (lstat(full, &st) != 0) {
            w = snprintf(result + off, rl - off, "%s\n", names[i]->d_name);
        } else if (S_ISDIR(st.st_mode)) {
            w = snprintf(result + off, rl - off, "%s/\n", names[i]->d_name);
        } else if (S_ISLNK(st.st_mode)) {
            w = snprintf(result + off, rl - off, "%s@\n", names[i]->d_name);
        } else {
            w = snprintf(result + off, rl - off, "%s (%lld bytes)\n", names[i]->d_name, (long long)st.st_size);
        }
        if (w < 0 || (size_t)w >= rl - off) {
            snprintf(result + off, rl - off > 40 ? 40 : rl - off, "...(more entries not shown)\n");
            break;
        }
        off += (size_t)w;
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    return true;
}

static bool fs_read(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path");
    char res[PATH_MAX];

    if (!path) {
        snprintf(result, rl, "'path' (string) is required");
        return false;
    }
    if (access_path("fs_read", path, "read", res, result, rl) != 0) {
        return false;
    }
    const cJSON *oj = cJSON_GetObjectItemCaseSensitive(in, "offset");
    long long off = cJSON_IsNumber(oj) && oj->valuedouble > 0 ? (long long)oj->valuedouble : 0;

    int fd = open(res, O_RDONLY | O_NOCTTY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        snprintf(result, rl, "cannot open '%s': %s", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        snprintf(result, rl, "'%s' is a directory; use fs_list", path);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(result, rl, "'%s' is not a regular file", path);
        return false;
    }
    if (!oj && mem_profile(res, path, (long long)st.st_size, result, rl)) { /* a big data file: its profile */
        close(fd);
        return true;
    }
    size_t cap = rl > 300 ? rl - 300 : rl / 2; /* leave room for the trailer note */
    ssize_t got = pread(fd, result, cap, (off_t)off);
    close(fd);
    if (got < 0) {
        snprintf(result, rl, "read error on '%s': %s", path, strerror(errno));
        return false;
    }
    if (memchr(result, '\0', (size_t)got)) {
        snprintf(result, rl, "'%s' looks like a binary file (%lld bytes); not shown", path, (long long)st.st_size);
        return true;
    }
    size_t len = (size_t)got;
    long long end = off + (long long)len;
    if (end < st.st_size) { /* truncated: do not cut a UTF-8 sequence in half */
        while (len > 0 && ((unsigned char)result[len] & 0xC0) == 0x80) {
            len--;
        }
        end = off + (long long)len;
        result[len] = '\0';
        snprintf(result + len, rl - len,
                 "\n[truncated: showed bytes %lld-%lld of %lld; call fs_read again with offset=%lld for more]", off,
                 end, (long long)st.st_size, end);
    } else {
        result[len] = '\0';
        if (len == 0) {
            snprintf(result, rl, off ? "(offset is at or past end of file; size %lld bytes)" : "(empty file)",
                     (long long)st.st_size);
        }
    }
    return true;
}

static int mkdir_parents(char *path) /* path is modified temporarily */
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0777) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    return 0;
}

/* Files this session wrote: rewriting them needs no confirmation, they are the agent's own work. */
static char *g_written[512];
static int   g_nwritten;

static int written_here(const char *path)
{
    for (int i = 0; i < g_nwritten; i++) {
        if (!strcmp(g_written[i], path)) {
            return 1;
        }
    }
    return 0;
}

static void note_written(const char *path)
{
    if (!written_here(path) && g_nwritten < (int)(sizeof(g_written) / sizeof(g_written[0]))) {
        g_written[g_nwritten++] = strdup(path);
    }
}

typedef struct {
    char  *p;
    size_t len, cap;
} text_t;

static void text_add(text_t *t, const char *s)
{
    size_t n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        size_t nc = (t->cap ? t->cap * 2 : 1024) + n;
        char *np = realloc(t->p, nc);
        if (!np) {
            return;
        }
        t->p = np;
        t->cap = nc;
    }
    memcpy(t->p + t->len, s, n + 1);
    t->len += n;
}

static void lua_diag(void *ud, const char *file, int line, int error, const char *msg)
{
    char l[800];
    if (line) {
        snprintf(l, sizeof(l), "  %s:%d: %s: %s\n", file, line, error ? "error" : "warning", msg);
    } else {
        snprintf(l, sizeof(l), "  %s: %s: %s\n", file, error ? "error" : "warning", msg);
    }
    text_add(ud, l);
}

/* After a write to functions/<name>.lua: check it as the build will, so a mistake shows at once
 * (like an editor's diagnostics). The note goes after the write's own result. */
static void lua_check(const char *res, const char *content, size_t n, char *result, size_t rl, char *sum, size_t sl)
{
    const char *f = strstr(res, "/functions/");
    size_t pl = strlen(res);
    if (!f || pl < 5 || strcmp(res + pl - 4, ".lua") != 0) {
        return;
    }
    const char *file = f + 1, *rest = file + 10;
    int test = !strncmp(rest, "tests/", 6) && !strchr(rest + 6, '/');
    if (!test && strchr(rest, '/')) {
        return;
    }
    text_t diags = {0};
    char note[1200] = "";
    if (test) {
        if (bl_check_syntax(file, content, n, lua_diag, &diags) == 0) {
            snprintf(note, sizeof(note), "\nLua check of %s: the syntax is fine (the build runs its test_ functions).", file);
            snprintf(sum, sl, "test file, syntax ok");
        }
    } else {
        bl_state *s = bl_open(NULL); /* no files and no network while checking: only the code itself */
        if (s && bl_load(s, file, content, n, 1, lua_diag, &diags) == 0) {
            char names[700] = "";
            for (int i = 0; i < bl_count(s); i++) {
                size_t l = strlen(names);
                snprintf(names + l, sizeof(names) - l, "%s%s", l ? ", " : "", bl_get(s, i)->name);
            }
            snprintf(note, sizeof(note),
                     names[0] ? "\nLua check of %s: ok. Tools the model can call: %s."
                              : "\nLua check of %s: ok, but no function here has a --- doc comment directly above it, so "
                                "the model cannot call any of them%s (fine for helpers that another file's tools use).",
                     file, names);
            snprintf(sum, sl, names[0] ? "Lua ok; tools: %.200s" : "Lua ok; helpers only (no tools)", names);
        }
        bl_close(s);
    }
    size_t used = strlen(result);
    if (diags.len) {
        snprintf(result + used, rl - used, "\nLua check of %s found problems; fix them before building:\n%s", file, diags.p);
        snprintf(sum, sl, "Lua check FAILED (see the last write's result)");
    } else {
        snprintf(result + used, rl - used, "%s", note);
    }
    free(diags.p);
}

/* Write content to the resolved path res (asking before replacing a file this session did not write,
 * unless ask is 0). */
static bool write_file(const char *tool, const char *path, const char *res, const char *content, int append, int ask,
                       char *result, size_t rl)
{
    if (!guard_write_ok(res)) {
        snprintf(result, rl, "this task may write only %s; %s is not one of them", guard_writes_text(), path);
        return false;
    }
    struct stat st;
    int exists = stat(res, &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) {
        snprintf(result, rl, "'%s' exists and is not a regular file", path);
        return false;
    }
    if (exists && !append && ask && st.st_size > 0 && !written_here(res) &&
        !guard_confirm_cat(GUARD_CAT_OVERWRITE, "%s would OVERWRITE existing file (%lld bytes):\n  %s", tool,
                           (long long)st.st_size, res)) {
        snprintf(result, rl, "denied by user: not overwriting %s", res);
        return false;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", res);
    if (mkdir_parents(tmp) != 0) {
        snprintf(result, rl, "cannot create parent directories of '%s': %s", path, strerror(errno));
        return false;
    }
    int fd = open(res, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) {
        snprintf(result, rl, "cannot open '%s' for writing: %s", path, strerror(errno));
        return false;
    }
    size_t n = strlen(content), done = 0;
    while (done < n) {
        ssize_t w = write(fd, content + done, n - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(result, rl, "write error on '%s': %s", path, strerror(errno));
            close(fd);
            return false;
        }
        done += (size_t)w;
    }
    close(fd);
    note_written(res);
    snprintf(result, rl, "%s %zu bytes to %s%s", append ? "appended" : "wrote", n, res,
             exists && !append ? " (replaced existing file)" : "");
    return true;
}

static bool fs_write(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path");
    const char *content = str_arg(in, "content");
    int append = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "append"));
    char res[PATH_MAX];

    if (guard_plan()) {
        snprintf(result, rl, "%s", GUARD_PLAN_REFUSAL);
        return false;
    }
    if (!path || !content) {
        snprintf(result, rl, "'path' and 'content' (strings) are required");
        return false;
    }
    if (access_path("fs_write", path, "write", res, result, rl) != 0 ||
        !write_file("fs_write", path, res, content, append, 1, result, rl)) {
        return false;
    }
    char sum[240] = "";
    if (!append) {
        lua_check(res, content, strlen(content), result, rl, sum, sizeof(sum));
    }
    mem_file_written(res, sum[0] ? sum : NULL);
    return true;
}

#define EDIT_MAX (8u * 1024 * 1024)

/* Replace an exact piece of a file: the way to fix a few lines without rewriting the whole file. */
static bool fs_edit(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path"), *old = str_arg(in, "old_text"), *new = str_arg(in, "new_text");
    int all = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "replace_all"));
    char res[PATH_MAX];
    if (guard_plan()) {
        snprintf(result, rl, "%s", GUARD_PLAN_REFUSAL);
        return false;
    }
    if (!path || !old || !new || !*old) {
        snprintf(result, rl, "'path', 'old_text' (not empty) and 'new_text' are required");
        return false;
    }
    if (access_path("fs_edit", path, "change", res, result, rl) != 0) {
        return false;
    }
    FILE *f = fopen(res, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > EDIT_MAX) {
        snprintf(result, rl, "cannot edit '%s': %s", path, !f ? strerror(errno) : "not a regular text file under 8 MB");
        if (f) {
            fclose(f);
        }
        return false;
    }
    char *text = calloc(1, (size_t)st.st_size + 1);
    size_t got = text ? fread(text, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!text) {
        snprintf(result, rl, "out of memory");
        return false;
    }
    text[got] = '\0';
    size_t ol = strlen(old), nl = strlen(new), count = 0;
    for (const char *p = text; (p = strstr(p, old)); p += ol) {
        count++;
    }
    if (count == 0 || (count > 1 && !all)) {
        snprintf(result, rl, count ? "old_text is in '%s' %zu times: include more of the surrounding lines so it is "
                                     "unique, or set replace_all"
                                   : "old_text is not in '%s': read the file again and copy the lines exactly, "
                                     "spaces and line breaks included",
                 path, count);
        free(text);
        return false;
    }
    char *out = malloc(got + count * (nl > ol ? nl - ol : 0) + 1), *w = out;
    if (!out) {
        free(text);
        snprintf(result, rl, "out of memory");
        return false;
    }
    const char *p = text;
    for (const char *hit; (hit = strstr(p, old)); p = hit + ol) {
        memcpy(w, p, (size_t)(hit - p));
        w += hit - p;
        memcpy(w, new, nl);
        w += nl;
    }
    strcpy(w, p);
    free(text);
    bool ok = write_file("fs_edit", path, res, out, 0, 0, result, rl); /* a quoted, exact change: no question */
    if (ok) {
        char sum[240] = "";
        snprintf(result, rl, "replaced %zu place%s in %s", count, count == 1 ? "" : "s", res);
        lua_check(res, out, strlen(out), result, rl, sum, sizeof(sum));
        mem_file_written(res, sum[0] ? sum : NULL);
    }
    free(out);
    return ok;
}

#define IMAGE_MAX (20u * 1024 * 1024)

/* An image file, read whole: its bytes (malloc'd) and type. 0 ok, -1 with result = why. */
int fs_load_image(const char *path, unsigned char **data, size_t *len, const char **mime, char *result, size_t rl)
{
    char res[PATH_MAX];
    *data = NULL;
    if (access_path("image_view", path, "read", res, result, rl) != 0) {
        return -1;
    }
    FILE *f = fopen(res, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(result, rl, "cannot open '%s': %s", path, f ? "not a regular file" : strerror(errno));
        if (f) {
            fclose(f);
        }
        return -1;
    }
    if ((size_t)st.st_size > IMAGE_MAX) {
        fclose(f);
        snprintf(result, rl, "'%s' is larger than %u MB", path, IMAGE_MAX >> 20);
        return -1;
    }
    unsigned char *d = malloc((size_t)st.st_size + 1);
    size_t n = d ? fread(d, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    *mime = n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)                ? "image/png"
            : n >= 3 && !memcmp(d, "\xff\xd8\xff", 3)                      ? "image/jpeg"
            : n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)) ? "image/gif"
            : n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)  ? "image/webp"
                                                                            : NULL;
    if (!*mime) {
        free(d);
        snprintf(result, rl, "'%s' is not a PNG, JPEG, GIF or WEBP image", path);
        return -1;
    }
    *data = d;
    *len = n;
    return 0;
}

/* Show a picture to the model: the bytes go into the conversation for this turn (chat_attach_image). */
static bool image_view(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path"), *mime = NULL;
    unsigned char *d = NULL;
    size_t n = 0;
    if (!path) {
        snprintf(result, rl, "'path' (string) is required");
        return false;
    }
    if (fs_load_image(path, &d, &n, &mime, result, rl) != 0) {
        return false;
    }
    int ok = chat_attach_image(path, mime, d, n) == 0;
    free(d);
    if (!ok) {
        snprintf(result, rl, "could not attach '%s'", path);
        return false;
    }
    snprintf(result, rl, "%s (%s, %zu bytes) is attached below: look at it before you answer.", path, mime, n);
    return true;
}

const tool_t TOOLS_FS[] = {
    {"fs_list",
     "List the entries of a directory (default: the working directory). Directories end with '/'. "
     "Paths are relative to the working directory; paths outside it require user approval.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path, default '.'\"}}}",
     fs_list},
    {"fs_read",
     "Read a text file. Output is capped (~32KB); for larger files use 'offset' to continue where the "
     "previous call stopped. A .csv/.tsv file over 16 KB returns a profile instead (rows, columns with types "
     "and ranges, first rows); give offset (0 = start) for its raw text. Binary files are not shown.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"offset\":{\"type\":\"integer\",\"description\":\"Byte offset to start from, default 0\"}},"
     "\"required\":[\"path\"]}",
     fs_read},
    {"fs_write",
     "Create or overwrite a text file (parent directories are created). Replacing a file that was there before "
     "this session asks the user first; files you wrote yourself are replaced without asking. Set append=true to "
     "add to the end instead. For a few changed lines use fs_edit. Writing functions/<name>.lua returns the "
     "same Lua check the build does.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},"
     "\"append\":{\"type\":\"boolean\",\"description\":\"Append instead of overwrite, default false\"}},"
     "\"required\":[\"path\",\"content\"]}",
     fs_write},
    {"fs_edit",
     "Change part of a text file in the working directory without asking: replace old_text (copied exactly from the file, spaces and line breaks "
     "included) with new_text. old_text must appear once, unless replace_all is true. Use it to fix a few "
     "lines instead of rewriting the whole file with fs_write.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},"
     "\"new_text\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\",\"description\":\"Replace every "
     "occurrence, default false\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}",
     fs_edit},
    {"image_view",
     "Look at an image file (PNG, JPEG, GIF or WEBP): it is shown to you right after this call, for this "
     "turn only. Use it to read numbers, text or details in pictures, scans and screenshots.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
     image_view},
};
const size_t TOOLS_FS_N = sizeof(TOOLS_FS) / sizeof(TOOLS_FS[0]);
