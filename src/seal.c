/* SHA-256 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018), ChaCha20 (RFC 8439)
 * and the camcfg envelope (see seal.h). Small and portable, no tables beyond K. */
#include "seal.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ SHA-256 */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha_t *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(sha_t *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv)); s->len = 0; s->n = 0;
}

static void sha_update(sha_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        size_t k = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, k); s->n += k; p += k; len -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_final(sha_t *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, lb[8];
    sha_update(s, &pad, 1);
    while (s->n != 56) sha_update(s, &z, 1);
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lb, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    sha_t s; sha_init(&s); sha_update(&s, data, len); sha_final(&s, out);
}

/* ------------------------------------------------------------------ HMAC, PBKDF2 */

typedef struct { sha_t in, out; } hmac_t;

static void hmac_init(hmac_t *m, const uint8_t *key, size_t klen)
{
    uint8_t k[64] = { 0 }, pad[64];
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&m->in); sha_update(&m->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_init(&m->out); sha_update(&m->out, pad, 64);
}

static void hmac_final(hmac_t *m, uint8_t out[32])
{
    uint8_t ih[32];
    sha_final(&m->in, ih);
    sha_update(&m->out, ih, 32);
    sha_final(&m->out, out);
}

void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32])
{
    hmac_t m; hmac_init(&m, key, klen); sha_update(&m.in, msg, mlen); hmac_final(&m, out);
}

void pbkdf2_sha256(const char *pass, const uint8_t *salt, size_t slen, unsigned rounds, uint8_t out[32])
{
    hmac_t base, m;
    hmac_init(&base, (const uint8_t *)pass, strlen(pass));
    uint8_t u[32], one[4] = { 0, 0, 0, 1 };
    m = base; sha_update(&m.in, salt, slen); sha_update(&m.in, one, 4); hmac_final(&m, u);
    memcpy(out, u, 32);
    for (unsigned r = 1; r < rounds; r++) {
        m = base; sha_update(&m.in, u, 32); hmac_final(&m, u);    /* keyed state reused: 2 blocks per round */
        for (int i = 0; i < 32; i++) out[i] ^= u[i];
    }
}

/* ------------------------------------------------------------------ ChaCha20 */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROL(d, 16); c += d; b ^= c; b = ROL(b, 12); \
                       a += b; d ^= a; d = ROL(d, 8);  c += d; b ^= c; b = ROL(b, 7)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *buf, size_t len)
{
    uint32_t st[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[12] = counter;
    for (int i = 0; i < 3; i++) st[13 + i] = le32(nonce + 4 * i);
    while (len) {
        uint32_t x[16];
        memcpy(x, st, sizeof(x));
        for (int i = 0; i < 10; i++) {
            QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
        }
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) {
            uint32_t v = x[i] + st[i];
            ks[4 * i] = v; ks[4 * i + 1] = v >> 8; ks[4 * i + 2] = v >> 16; ks[4 * i + 3] = v >> 24;
        }
        size_t k = len < 64 ? len : 64;
        for (size_t i = 0; i < k; i++) buf[i] ^= ks[i];
        buf += k; len -= k;
        st[12]++;
    }
}

/* ------------------------------------------------------------------ envelope */

static const char MAGIC[4] = { 'L', 'X', 'E', '1' };

void seal_master(const char *password, uint8_t master[32])
{
    static const char salt[] = "lamax-x31-camcfg-v1";
    pbkdf2_sha256(password, (const uint8_t *)salt, sizeof(salt) - 1, 20000, master);
}

static void subkeys(const uint8_t master[32], const uint8_t salt[16], uint8_t enc[32], uint8_t mac[32])
{
    uint8_t m[19];
    memcpy(m + 3, salt, 16);
    memcpy(m, "enc", 3); hmac_sha256(master, 32, m, sizeof(m), enc);
    memcpy(m, "mac", 3); hmac_sha256(master, 32, m, sizeof(m), mac);
}

static void random_bytes(uint8_t *p, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) { ssize_t r = read(fd, p, n); if (r > 0) got = (size_t)r; close(fd); }
    if (got < n) {                                   /* should not happen; never all-zero */
        struct timeval tv; gettimeofday(&tv, NULL);
        srand48(tv.tv_sec ^ tv.tv_usec ^ getpid());
        for (size_t i = got; i < n; i++) p[i] = (uint8_t)lrand48();
    }
}

size_t seal(const uint8_t master[32], const uint8_t *plain, size_t len, uint8_t *out, time_t now)
{
    static const uint8_t zero_nonce[12];
    uint8_t enc[32], mac[32];
    memcpy(out, MAGIC, 4);
    random_bytes(out + 4, 16);
    for (int i = 0; i < 8; i++) out[20 + i] = (uint8_t)((uint64_t)now >> (56 - 8 * i));
    memcpy(out + 28, plain, len);
    subkeys(master, out + 4, enc, mac);
    chacha20_xor(enc, zero_nonce, 1, out + 28, len);        /* fresh key per message: nonce 0 is fine */
    hmac_sha256(mac, 32, out, 28 + len, out + 28 + len);
    return 28 + len + 32;
}

long seal_open(const uint8_t master[32], uint8_t *buf, size_t len, time_t now)
{
    static const uint8_t zero_nonce[12];
    if (len < SEAL_OVERHEAD || memcmp(buf, MAGIC, 4)) return -1;
    uint8_t enc[32], mac[32], tag[32];
    subkeys(master, buf + 4, enc, mac);
    size_t clen = len - SEAL_OVERHEAD;
    hmac_sha256(mac, 32, buf, 28 + clen, tag);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= tag[i] ^ buf[28 + clen + i];
    if (diff) return -1;
    uint64_t t = 0;
    for (int i = 0; i < 8; i++) t = t << 8 | buf[20 + i];
    if (llabs((long long)((int64_t)t - (int64_t)now)) > 300) return -2;
    chacha20_xor(enc, zero_nonce, 1, buf + 28, clen);
    return (long)clen;
}
