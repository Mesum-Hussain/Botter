#define _GNU_SOURCE
#include "vault.h"
#include "keystore.h"
#include "vfs.h"

#include "bearssl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define VAULT_ITER 600000u
#define VAULT_AAD  "botter-vault-1"

static cJSON *g_open; /* the secrets, once the password was right */

static void kdf(const char *pw, const unsigned char *salt, size_t sl, uint32_t iter, unsigned char key[32])
{
    br_hmac_key_context kc;
    br_hmac_context     hc;
    unsigned char       u[32];
    br_hmac_key_init(&kc, &br_sha256_vtable, pw, strlen(pw));
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, salt, sl);
    br_hmac_update(&hc, "\0\0\0\1", 4);
    br_hmac_out(&hc, u);
    memcpy(key, u, 32);
    for (uint32_t i = 1; i < iter; i++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, u, 32);
        br_hmac_out(&hc, u);
        for (int j = 0; j < 32; j++) {
            key[j] ^= u[j];
        }
    }
    explicit_bzero(&kc, sizeof(kc));
    explicit_bzero(&hc, sizeof(hc));
    explicit_bzero(u, sizeof(u));
}

static void to_hex(const unsigned char *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
}

/* hex -> malloc'd bytes, *n = how many; NULL if it is not hex. */
static unsigned char *from_hex(const char *h, size_t *n)
{
    size_t l = h ? strlen(h) : 1;
    unsigned char *b = l % 2 ? NULL : malloc(l / 2 + 1);
    for (size_t i = 0; b && i < l / 2; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) {
            free(b);
            return NULL;
        }
        b[i] = (unsigned char)v;
    }
    *n = l / 2;
    return b;
}

int vault_present(void)
{
    const vfs_entry_t *e = vfs_find(VAULT_FILE);
    return e && e->kind == VFS_DATA && e->len > 0;
}

int vault_open(const char *password)
{
    const vfs_entry_t *e = vfs_find(VAULT_FILE);
    cJSON *j = e ? cJSON_ParseWithLength(e->data, e->len) : NULL;
    const cJSON *it = cJSON_GetObjectItem(j, "iter");
    size_t sl = 0, nl = 0, dl = 0, tl = 0, kl = 0;
    unsigned char *salt = from_hex(cJSON_GetStringValue(cJSON_GetObjectItem(j, "salt")), &sl);
    unsigned char *nonce = from_hex(cJSON_GetStringValue(cJSON_GetObjectItem(j, "nonce")), &nl);
    unsigned char *data = from_hex(cJSON_GetStringValue(cJSON_GetObjectItem(j, "data")), &dl);
    unsigned char *tag = from_hex(cJSON_GetStringValue(cJSON_GetObjectItem(j, "tag")), &tl);
    unsigned char key[32], got[16], *cached = NULL;
    char name[80] = "vault:", hex[65];
    int rc = -1;
    if (salt && nonce && data && tag && sl >= 8 && nl == 12 && tl == 16 && cJSON_IsNumber(it) && it->valuedouble >= 1000 &&
        it->valuedouble <= 100000000) {
        to_hex(salt, 8, name + 6); /* the remembered key belongs to this vault */
        if (password) {
            kdf(password, salt, sl, (uint32_t)it->valuedouble, key);
        } else if (ks_get(name, hex, sizeof(hex)) != 64 || !(cached = from_hex(hex, &kl)) || kl != 32) {
            goto out;
        } else {
            memcpy(key, cached, 32);
        }
        br_poly1305_ctmul_run(key, nonce, data, dl, VAULT_AAD, sizeof(VAULT_AAD) - 1, got, br_chacha20_ct_run, 0);
        unsigned diff = 0;
        for (int i = 0; i < 16; i++) {
            diff |= got[i] ^ tag[i];
        }
        data[dl] = '\0';
        cJSON *s = diff ? NULL : cJSON_Parse((char *)data);
        if (cJSON_IsObject(s)) {
            cJSON_Delete(g_open);
            g_open = s;
            rc = 0;
            to_hex(key, 32, hex);
            ks_put(name, hex); /* until reboot: lets --acp / --a2a start without a prompt */
        } else {
            cJSON_Delete(s);
        }
    }
out:
    explicit_bzero(key, sizeof(key));
    explicit_bzero(hex, sizeof(hex));
    if (data) {
        explicit_bzero(data, dl);
    }
    free(cached);
    free(salt);
    free(nonce);
    free(data);
    free(tag);
    cJSON_Delete(j);
    return rc;
}

const char *vault_get(const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_open, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

int vault_seal(const char *path, const cJSON *secrets, const char *password)
{
    unsigned char salt[16], nonce[12], key[32], tag[16];
    char *plain = cJSON_PrintUnformatted(secrets);
    size_t n = plain ? strlen(plain) : 0;
    char *hex = malloc(2 * n + 1), hs[33], hn[25], ht[33];
    int rc = -1;
    if (plain && hex && getrandom(salt, sizeof(salt), 0) == sizeof(salt) && getrandom(nonce, sizeof(nonce), 0) == sizeof(nonce)) {
        kdf(password, salt, sizeof(salt), VAULT_ITER, key);
        br_poly1305_ctmul_run(key, nonce, plain, n, VAULT_AAD, sizeof(VAULT_AAD) - 1, tag, br_chacha20_ct_run, 1);
        to_hex((unsigned char *)plain, n, hex);
        to_hex(salt, sizeof(salt), hs);
        to_hex(nonce, sizeof(nonce), hn);
        to_hex(tag, sizeof(tag), ht);
        FILE *f = fopen(path, "w");
        if (f) {
            rc = fprintf(f, "{\"v\":1,\"kdf\":\"pbkdf2-sha256\",\"iter\":%u,\"salt\":\"%s\",\"nonce\":\"%s\",\"data\":\"%s\",\"tag\":\"%s\"}\n",
                         VAULT_ITER, hs, hn, hex, ht) > 0 ? 0 : -1;
            rc = fclose(f) == 0 ? rc : -1;
        }
    }
    explicit_bzero(key, sizeof(key));
    if (plain) {
        explicit_bzero(plain, n);
    }
    free(plain);
    free(hex);
    return rc;
}
