/* test_cci — CCI payload encoding self-check over a socketpair (make test).
 * No hardware: a fake camera reply is queued before each call. */
#define _GNU_SOURCE
#include "../src/internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int peer;
static struct flir_ctx ctx;
static uint8_t wire[700];
static size_t wire_len;

static uint16_t crc16(const uint8_t *d, size_t n)
{
	uint16_t crc = 0x1D0F;
	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)(d[i] << 8);
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
					     : (uint16_t)(crc << 1);
	}
	return crc;
}

/* Queue one stuffed FSLP reply for the next command. */
static void reply(uint32_t status, const uint8_t *pl, size_t n)
{
	uint8_t raw[300], out[700];
	size_t r = 0, o = 0;
	memset(raw, 0, 9);            /* channel, sequence, function id */
	r = 9;
	raw[r++] = (uint8_t)(status >> 24);
	raw[r++] = (uint8_t)(status >> 16);
	raw[r++] = (uint8_t)(status >> 8);
	raw[r++] = (uint8_t)status;
	if (n)
		memcpy(raw + r, pl, n);
	r += n;
	uint16_t c = crc16(raw, r);
	raw[r++] = (uint8_t)(c >> 8);
	raw[r++] = (uint8_t)c;
	out[o++] = 0x8E;
	for (size_t i = 0; i < r; i++) {
		if (raw[i] == 0x8E || raw[i] == 0x9E || raw[i] == 0xAE) {
			out[o++] = 0x9E;
			out[o++] = (uint8_t)(raw[i] - 0x0D);
		} else {
			out[o++] = raw[i];
		}
	}
	out[o++] = 0xAE;
	assert(write(peer, out, o) == (ssize_t)o);
}

/* Decode the request the driver sent: function id + payload. */
static size_t sent(uint32_t *id, uint8_t *pl)
{
	uint8_t raw[300];
	size_t r = 0;
	ssize_t n = read(peer, wire, sizeof(wire));
	assert(n > 2);
	wire_len = (size_t)n;
	assert(wire[0] == 0x8E && wire[wire_len - 1] == 0xAE);
	for (size_t i = 1; i < wire_len - 1; i++)
		raw[r++] = wire[i] == 0x9E ? (uint8_t)(wire[++i] + 0x0D) : wire[i];
	assert(r >= 15);
	assert(crc16(raw, r - 2) == (uint16_t)((raw[r - 2] << 8) | raw[r - 1]));
	*id = (uint32_t)raw[5] << 24 | (uint32_t)raw[6] << 16 |
	      (uint32_t)raw[7] << 8 | raw[8];
	memcpy(pl, raw + 13, r - 15);
	return r - 15;
}

static int nothing_sent(void)
{
	uint8_t b[1];
	return recv(peer, b, 1, MSG_DONTWAIT) < 0;
}

int main(void)
{
	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	ctx.vfd = -1;
	ctx.cfd = sv[0];
	peer = sv[1];
	uint32_t id;
	uint8_t pl[300];
	size_t n;
	double v = 0;

	/* 1. scalerSetZoom: 12-byte FLR_SCALER_ZOOM_PARAMS_T about the centre */
	static const uint8_t zoom[12] = { 0,0,0,2, 0,0,1,0x40, 0,0,1,0 };
	reply(0, NULL, 0);
	assert(flir_cci_set_zoom(&ctx, 2) == 0);
	n = sent(&id, pl);
	assert(id == 0x000D0002 && n == 12 && !memcmp(pl, zoom, 12));

	/* 2. FLOAT set: big-endian IEEE-754 */
	const struct flir_param_info *g = flir_param_find("agc-gamma");
	assert(g && g->set_id == 0x0009000D && g->get_id == 0x0009000E && g->type == 'f');
	static const uint8_t f097[4] = { 0x3f, 0x78, 0x51, 0xec };
	reply(0, NULL, 0);
	assert(flir_cci_param_set(&ctx, g, 0.97) == 0);
	n = sent(&id, pl);
	assert(id == 0x0009000D && n == 4 && !memcmp(pl, f097, 4));

	/* 3. FLOAT get decodes */
	reply(0, f097, 4);
	assert(flir_cci_param_get(&ctx, g, &v) == 0 && v > 0.9699 && v < 0.9701);
	assert(flir_cci_last_status() == 0);
	n = sent(&id, pl);
	assert(id == 0x0009000E && n == 0);

	/* 4. out of range, non-integer, read-only, NULL: rejected, no I/O */
	assert(flir_cci_param_set(&ctx, g, 5.0) == -1);
	assert(flir_cci_param_set(&ctx, flir_param_find("ffc-mode"), 1.5) == -1);
	assert(flir_cci_param_set(&ctx, flir_param_find("fps"), 30) == -1);
	assert(flir_cci_param_set(&ctx, NULL, 0) == -1);
	assert(nothing_sent());

	/* 5. UINT_16 set is 2 bytes */
	reply(0, NULL, 0);
	assert(flir_cci_param_set(&ctx, flir_param_find("ffc-temp"), 10) == 0);
	n = sent(&id, pl);
	assert(id == 0x00050008 && n == 2 && pl[0] == 0 && pl[1] == 10);

	/* 6. byte stuffing of 0x8E / 0x9E inside the payload */
	static const uint8_t stuffed[4] = { 0x9E, 0x81, 0x9E, 0x91 };
	reply(0, NULL, 0);
	assert(flir_cci_param_set(&ctx, flir_param_find("ffc-period"), 0x8E9E) == 0);
	n = sent(&id, pl);
	assert(id == 0x0005000A && n == 4 && pl[2] == 0x8E && pl[3] == 0x9E);
	assert(memmem(wire, wire_len, stuffed, 4));

	/* 7. camera error status is reported */
	reply(0x161, NULL, 0);
	assert(flir_cci_param_get(&ctx, flir_param_find("tlinear"), &v) == -1);
	assert(flir_cci_last_status() == 0x161);
	sent(&id, pl);

	/* 8. symbologyCreateText: 146 bytes, font 1, NUL padded text */
	reply(0, NULL, 0);
	assert(flir_cci_sym_create_text(&ctx, 3, 10, 20, 200, 30, 24, 17, 0x00FF0000u, "Hi") == 0);
	n = sent(&id, pl);
	assert(id == 0x00140006 && n == 146);
	assert(pl[0] == 3 && pl[1] == 0 && pl[2] == 10 && pl[9] == 1 && pl[13] == 17);
	assert(pl[14] == 0 && pl[15] == 0xFF && pl[18] == 'H' && pl[19] == 'i');
	assert(pl[20] == 0 && pl[145] == 0);

	/* 9. FLR_ROI_T: row start/end then column start/end, UINT_16 each */
	static const uint8_t roi[8] = { 0,0, 1,0xff, 0,0, 2,0x7f };
	reply(0, NULL, 0);
	assert(flir_cci_agc_set_roi(&ctx, 0, 511, 0, 639) == 0);
	n = sent(&id, pl);
	assert(id == 0x00090020 && n == 8 && !memcmp(pl, roi, 8));

	/* 10. parameter table sanity */
	size_t count = 0;
	for (const struct flir_param_info *p = flir_params; p->name; p++, count++) {
		assert(p->get_id && p->type && strchr("euhwf", p->type) && p->help);
		assert(p->lo <= p->hi);
		assert(flir_param_find(p->name) == p);   /* unique names */
	}
	assert(count >= 50 && !flir_param_find("nope"));

	puts("test_cci: ok");
	return 0;
}
