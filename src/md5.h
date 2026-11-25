#ifndef MD5_H
#define MD5_H
#include <stddef.h>
#include <stdint.h>

/* Minimal MD5 (RFC 1321) for RTSP Digest authentication. */
void md5(const void *data, size_t len, uint8_t out[16]);
void md5_hex(const char *s, char out[33]);

#endif
