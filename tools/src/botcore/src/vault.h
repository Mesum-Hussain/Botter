#ifndef BC_VAULT_H
#define BC_VAULT_H

#include "cJSON.h"

/*
 * The sealed vault (opt-in): an agent's keys, encrypted with a password and embedded in the
 * .bot as secrets.vault. Whoever runs the agent types the password; nothing is stored outside
 * the file. Without a vault the agent asks for the model key as before (kernel keyring).
 *
 * The file is JSON: {"v":1,"kdf":"pbkdf2-sha256","iter":N,"salt":hex,"nonce":hex,"data":hex,
 * "tag":hex}. The key is PBKDF2-HMAC-SHA256(password, salt, iter); data is the secrets (a JSON
 * object of NAME: value, the names an .env would use) under ChaCha20-Poly1305. A wrong
 * password and a damaged file look the same: the tag does not match.
 */
#define VAULT_FILE     "secrets.vault"
#define VAULT_MIN_PASS 10

int vault_present(void);
/*
 * Open the embedded vault. password NULL = with the key remembered in the kernel keyring by
 * an earlier run this boot (for --acp / --a2a, where nobody can type). 0 ok, -1 wrong
 * password, damaged, or nothing remembered.
 */
int vault_open(const char *password);
/* A secret of the opened vault, or NULL. */
const char *vault_get(const char *name);
/* Encrypt secrets (an object of strings) into the file at path. 0 ok. */
int vault_seal(const char *path, const cJSON *secrets, const char *password);

#endif
