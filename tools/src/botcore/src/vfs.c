#define _GNU_SOURCE
#include "vfs.h"
#include "blob.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static vfs_entry_t *g_tab;
static size_t       g_n;
static const char  *g_err;

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static uint32_t crc32_ieee(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

const char *vfs_error(void)
{
    return g_err;
}

/* Patched by packer in every .bot (see blob.h); all zero after the magic in plain botcore. */
__attribute__((used, aligned(8))) static volatile const unsigned char g_packref[PACKREF_SIZE] = PACKREF_MAGIC;

static char g_errbuf[200];

static int fail(const char *why)
{
    g_err = why;
    free(g_tab);
    g_tab = NULL;
    g_n = 0;
    return -1;
}

int vfs_init(void)
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const char *p = (const char *)getauxval(AT_EXECFN);
        fd = p ? open(p, O_RDONLY | O_CLOEXEC) : -1;
    }
    unsigned char ref[PACKREF_SIZE];
    for (size_t i = 0; i < PACKREF_SIZE; i++) {
        ref[i] = g_packref[i];
    }
    uint64_t want_size = rd64(ref + 16);
    if (fd < 0) {
        return want_size ? fail("cannot read this agent's own executable (/proc/self/exe)") : 0;
    }
    struct stat st;
    unsigned char ft[FOOTER_SIZE];
    int has_footer = fstat(fd, &st) == 0 && st.st_size >= FOOTER_SIZE &&
                     pread(fd, ft, FOOTER_SIZE, st.st_size - FOOTER_SIZE) == FOOTER_SIZE &&
                     memcmp(ft + 24, FOOTER_MAGIC, 8) == 0;
    uint64_t fsz = (uint64_t)st.st_size;
    if (want_size && fsz != want_size) {
        close(fd);
        snprintf(g_errbuf, sizeof(g_errbuf),
                 "this agent file is %s: it should be %llu bytes but is %llu. Copy or download it again.",
                 fsz < want_size ? "truncated" : "damaged", (unsigned long long)want_size, (unsigned long long)fsz);
        return fail(g_errbuf);
    }
    if (!has_footer) {
        close(fd);
        return 0; /* plain botcore, no pack */
    }
    uint64_t boff = rd64(ft), blen = rd64(ft + 8);
    uint32_t crc = rd32(ft + 16);
    if (want_size && boff != rd64(ref + 8)) {
        close(fd);
        return fail("embedded agent pack does not match this executable (bad offset)");
    }
    if (blen < BLOB_HEADER_SIZE || boff > fsz || blen > fsz - boff || boff + blen + FOOTER_SIZE != fsz) {
        close(fd);
        return fail("embedded agent pack is damaged (bad footer)");
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) {
        pg = 4096;
    }
    uint64_t base = boff - boff % (uint64_t)pg, delta = boff - base;
    unsigned char *map = mmap(NULL, (size_t)(blen + delta), PROT_READ, MAP_PRIVATE, fd, (off_t)base);
    close(fd);
    if (map == MAP_FAILED) {
        return fail("cannot map embedded agent pack");
    }
    const unsigned char *b = map + delta;
    if (crc32_ieee(b, (size_t)blen) != crc) {
        return fail("embedded agent pack is damaged (checksum mismatch)");
    }
    if (memcmp(b, BLOB_MAGIC, 8) != 0) {
        return fail("embedded agent pack has an unknown format");
    }
    uint32_t count = rd32(b + 8);
    if ((uint64_t)count * BLOB_ENTRY_SIZE + BLOB_HEADER_SIZE > blen) {
        return fail("embedded agent pack is damaged (bad index)");
    }
    vfs_entry_t *tab = calloc(count ? count : 1, sizeof(*tab));
    if (!tab) {
        return fail("out of memory");
    }
    g_tab = tab;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = b + BLOB_HEADER_SIZE + (size_t)i * BLOB_ENTRY_SIZE;
        uint64_t po = rd32(e), pl = rd32(e + 4), dof = rd64(e + 8), dl = rd64(e + 16);
        uint32_t kind = rd32(e + 24);
        if (po + pl + 1 > blen || dof > blen || dl > blen - dof || blen - dof - dl < 1 || kind > VFS_EXEC_SCRIPT ||
            b[po + pl] != 0 || b[dof + dl] != 0 || pl == 0) {
            return fail("embedded agent pack is damaged (bad entry)");
        }
        tab[i].path = (const char *)b + po;
        tab[i].data = (const char *)b + dof;
        tab[i].len = (size_t)dl;
        tab[i].kind = (int)kind;
        if (i > 0 && strcmp(tab[i - 1].path, tab[i].path) >= 0) {
            return fail("embedded agent pack is damaged (unsorted index)");
        }
    }
    g_n = count;
    return 0;
}

const vfs_entry_t *vfs_table(size_t *count)
{
    *count = g_n;
    return g_tab;
}

size_t vfs_count(void)
{
    return g_n;
}

static int cmp_path(const void *key, const void *elem)
{
    return strcmp((const char *)key, ((const vfs_entry_t *)elem)->path);
}

const vfs_entry_t *vfs_find(const char *path)
{
    if (!g_tab || g_n == 0 || !path) {
        return NULL;
    }
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    while (path[0] == '/') {
        path++;
    }
    return bsearch(path, g_tab, g_n, sizeof(*g_tab), cmp_path);
}
