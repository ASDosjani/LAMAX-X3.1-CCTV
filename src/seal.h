/* Encrypted envelope for camcfg <-> LamaxTool (the config.txt contents):
 *
 *   master  = PBKDF2-HMAC-SHA256(admin password, "lamax-x31-camcfg-v1", 20000 rounds, 32 bytes)
 *   salt    = 16 random bytes per message
 *   enc_key = HMAC-SHA256(master, "enc" || salt)      (ChaCha20 key, RFC 8439, nonce 0, counter 1)
 *   mac_key = HMAC-SHA256(master, "mac" || salt)
 *   message = "LXE1" || salt[16] || time[8, big endian unix seconds] || ciphertext || tag[32]
 *   tag     = HMAC-SHA256(mac_key, everything before the tag)
 *
 * Opening checks the tag (constant time) and that the time is within 5 minutes. */
#ifndef SEAL_H
#define SEAL_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SEAL_OVERHEAD (4 + 16 + 8 + 32)

void sha256(const void *data, size_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
void pbkdf2_sha256(const char *pass, const uint8_t *salt, size_t slen, unsigned rounds, uint8_t out[32]);
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *buf, size_t len);

/* master key from the admin password (slow on purpose: ~20000 HMACs) */
void seal_master(const char *password, uint8_t master[32]);
/* out must hold len + SEAL_OVERHEAD bytes; returns the sealed length */
size_t seal(const uint8_t master[32], const uint8_t *plain, size_t len, uint8_t *out, time_t now);
/* opens in place: returns the plaintext length (at buf + 28), or -1 (bad tag / format),
 * -2 (too old or from the future) */
long seal_open(const uint8_t master[32], uint8_t *buf, size_t len, time_t now);

#endif
