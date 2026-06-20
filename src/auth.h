#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>
#include "protocol.h"

/*
 * WARNING: The hash scheme below is NOT production-grade cryptography.
 * It is a simple iterative FNV-1a hash with a random salt, included
 * purely as a learning exercise.  In a real system use bcrypt, scrypt,
 * or Argon2 via a well-audited library.
 */

/* Fill `salt` with SALT_HEX_LEN-1 random hex characters + NUL. */
void generate_salt(char *salt, size_t len);

/* Derive a hex-encoded hash of password+salt; write into out_hash (MAX_HASH bytes). */
void hash_password(const char *password, const char *salt,
                   char *out_hash, size_t hash_len);

/* Return 1 if password matches stored_hash/salt, 0 otherwise. */
int verify_password(const char *password, const char *salt,
                    const char *stored_hash);

#endif /* AUTH_H */
