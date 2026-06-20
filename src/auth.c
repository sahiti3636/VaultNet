/*
 * WARNING: NOT production-grade crypto.  See auth.h for details.
 */

#include "auth.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* FNV-1a 64-bit constants */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t fnv1a_block(const uint8_t *data, size_t len, uint64_t h) {
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= FNV_PRIME;
    }
    return h;
}

/*
 * 10 000-round iterated FNV-1a with salt: provides mild key-stretching.
 * Output is a 128-bit value (two 64-bit halves) encoded as 32 hex chars.
 * Still NOT suitable for production — single 64-bit FNV is not collision-
 * resistant; use this only as a teaching example.
 */
void hash_password(const char *password, const char *salt,
                   char *out_hash, size_t hash_len) {
    uint64_t h1 = FNV_OFFSET;
    uint64_t h2 = FNV_OFFSET ^ 0xdeadbeefcafe1234ULL;

    const uint8_t *pw   = (const uint8_t *)password;
    const uint8_t *sa   = (const uint8_t *)salt;
    size_t pw_len = strlen(password);
    size_t sa_len = strlen(salt);

    /* Initial salt mix */
    h1 = fnv1a_block(sa, sa_len, h1);
    h2 = fnv1a_block(sa, sa_len, h2);

    for (int round = 0; round < 10000; round++) {
        h1 = fnv1a_block(pw, pw_len, h1);
        h1 = fnv1a_block(sa, sa_len, h1);
        /* Cross-feed the two lanes for extra diffusion */
        h2 = fnv1a_block((uint8_t *)&h1, sizeof(h1), h2);
        h2 = fnv1a_block(pw, pw_len, h2);
    }

    snprintf(out_hash, hash_len, "%016llx%016llx",
             (unsigned long long)h1, (unsigned long long)h2);
}

void generate_salt(char *salt, size_t len) {
    /* Try /dev/urandom first; fall back to time-based PRNG. */
    int fd = open("/dev/urandom", O_RDONLY);
    uint8_t raw[SALT_LEN];

    if (fd >= 0) {
        ssize_t n = 0;
        while (n < (ssize_t)SALT_LEN) {
            ssize_t r = read(fd, raw + n, (size_t)(SALT_LEN - n));
            if (r < 0) break;
            n += r;
        }
        close(fd);
    } else {
        /* Fallback: mix time + stack address (weak, for demo only) */
        srand((unsigned)(time(NULL) ^ (uintptr_t)salt));
        for (int i = 0; i < SALT_LEN; i++)
            raw[i] = (uint8_t)(rand() & 0xff);
    }

    /* Encode as hex; write at most len-1 chars */
    size_t out = 0;
    for (int i = 0; i < SALT_LEN && out + 2 < len; i++, out += 2)
        snprintf(salt + out, 3, "%02x", raw[i]);
    salt[out] = '\0';
}

int verify_password(const char *password, const char *salt,
                    const char *stored_hash) {
    char computed[MAX_HASH];
    hash_password(password, salt, computed, sizeof(computed));
    return (strncmp(computed, stored_hash, MAX_HASH) == 0) ? 1 : 0;
}
