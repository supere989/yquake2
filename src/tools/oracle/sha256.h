#ifndef Q2_ORACLE_SHA256_H
#define Q2_ORACLE_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
	uint8_t data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} q2_sha256_ctx_t;

void q2_sha256_init(q2_sha256_ctx_t *ctx);
void q2_sha256_update(q2_sha256_ctx_t *ctx, const void *data, size_t len);
void q2_sha256_final(q2_sha256_ctx_t *ctx, uint8_t hash[32]);
void q2_sha256_hex(const uint8_t hash[32], char out[65]);

#endif
