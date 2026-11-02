#include "auth.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define PBKDF2_ITERS   100000
#define PBKDF2_KEYLEN  32     /* 32 bytes → 64 hex chars; fits in MAX_HASH=129 */

int hash_password(const char *password, const char *salt,
                  char *out_hash, size_t hash_len) {
    if (!password || !salt || !out_hash || hash_len < PBKDF2_KEYLEN * 2 + 1)
        return -1;
    uint8_t dk[PBKDF2_KEYLEN];

    if (PKCS5_PBKDF2_HMAC(
        password, (int)strlen(password),
        (const unsigned char *)salt, (int)strlen(salt),
        PBKDF2_ITERS, EVP_sha256(),
        PBKDF2_KEYLEN, dk) != 1) {
        out_hash[0] = '\0';
        return -1;
    }

    for (int i = 0; i < PBKDF2_KEYLEN; i++)
        snprintf(out_hash + i * 2, 3, "%02x", dk[i]);
    OPENSSL_cleanse(dk, sizeof(dk));
    return 0;
}

int generate_salt(char *salt, size_t len) {
    if (!salt || len < SALT_HEX_LEN) return -1;
    uint8_t raw[SALT_LEN];

    if (RAND_bytes(raw, SALT_LEN) != 1) {
        salt[0] = '\0';
        return -1;
    }

    size_t out = 0;
    for (int i = 0; i < SALT_LEN && out + 2 < len; i++, out += 2)
        snprintf(salt + out, 3, "%02x", raw[i]);
    salt[out] = '\0';
    OPENSSL_cleanse(raw, sizeof(raw));
    return 0;
}

int verify_password(const char *password, const char *salt,
                    const char *stored_hash) {
    char computed[MAX_HASH];
    if (!stored_hash || strnlen(stored_hash, MAX_HASH) != PBKDF2_KEYLEN * 2 ||
        hash_password(password, salt, computed, sizeof(computed)) != 0)
        return 0;
    /* Constant-time comparison to resist timing attacks */
    int match = CRYPTO_memcmp(computed, stored_hash, PBKDF2_KEYLEN * 2) == 0;
    OPENSSL_cleanse(computed, sizeof(computed));
    return match;
}
