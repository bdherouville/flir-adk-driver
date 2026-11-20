/*
 * cci_util.h — big-endian packing helpers for the src/cci_*.c wrappers.
 * Header-only (static inline) so each wrapper file stays independent.
 * Boson CCI payloads are big-endian; FLOAT is IEEE-754 single, big-endian.
 */
#ifndef FLIR_CCI_UTIL_H
#define FLIR_CCI_UTIL_H

#include "flir_adk.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void cci_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void cci_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint16_t cci_get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t cci_get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline void cci_putf(uint8_t *p, float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	cci_put32(p, u);
}

static inline float cci_getf(const uint8_t *p)
{
	uint32_t u = cci_get32(p);
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

/* Get command without input payload: 0 only if >= need bytes came back. */
static inline int cci_get(struct flir_ctx *ctx, uint32_t id, uint8_t *resp,
			  size_t need)
{
	size_t n = need;
	if (flir_cci_raw(ctx, id, NULL, 0, resp, &n) || n < need)
		return -1;
	return 0;
}

#endif
