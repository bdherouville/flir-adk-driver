/*
 * cci_symbology.c — on-camera symbology (vector objects only).
 *
 * Wraps the create/show/delete/move Sets for text, rectangle and line objects
 * drawn into the camera's 8-bit video. Colour is 0xAARRGGBB where A 0 is
 * opaque and 127 is fully transparent.
 *
 * Intentionally NOT wrapped: bitmap / PNG / send-data uploads, groups, and
 * save/reload configuration (those write camera flash). Symbology enable and
 * disable live in the "sym" param in cci_params.c.
 */
#include "flir_adk.h"
#include "cci_util.h"
#include <string.h>

#define CCI_SYM_CREATE_TEXT      0x00140006 /* 146 B: id[0] x[1:3] y[3:5] w[5:7] h[7:9] font[9] size[10:12] align[12:14] color[14:18] text[18:146] */
#define CCI_SYM_UPDATE_AND_SHOW  0x0014000A /* 2 B: id[0] visible[1] (0|1) */
#define CCI_SYM_DELETE           0x0014000C /* 1 B: id[0] */
#define CCI_SYM_FILLED_RECT      0x0014000E /* 13 B: id[0] x[1:3] y[3:5] w[5:7] h[7:9] color[9:13] */
#define CCI_SYM_OUTLINED_RECT    0x00140010 /* 13 B: same as filled */
#define CCI_SYM_MOVE_BY_OFFSET   0x00140019 /* 5 B: id[0] dx[1:3] dy[3:5] */
#define CCI_SYM_CREATE_LINE      0x0014001C /* 13 B: id[0] x1[1:3] y1[3:5] x2[5:7] y2[7:9] color[9:13] */

static int shape13(struct flir_ctx *ctx, uint32_t fn, uint8_t id, int16_t x,
		   int16_t y, int16_t a, int16_t b, uint32_t argb)
{
	uint8_t p[13] = {0};
	p[0] = id;
	cci_put16(p + 1, (uint16_t)x);
	cci_put16(p + 3, (uint16_t)y);
	cci_put16(p + 5, (uint16_t)a);
	cci_put16(p + 7, (uint16_t)b);
	cci_put32(p + 9, argb);
	return flir_cci_raw(ctx, fn, p, sizeof(p), NULL, NULL);
}

int flir_cci_sym_create_text(struct flir_ctx *ctx, uint8_t id, int16_t x,
			     int16_t y, int16_t w, int16_t h, int16_t size,
			     int16_t align, uint32_t argb, const char *text)
{
	size_t len = text ? strlen(text) : 0;
	if (!text || len > 127)
		return -1;

	uint8_t p[146] = {0};
	p[0] = id;
	cci_put16(p + 1, (uint16_t)x);
	cci_put16(p + 3, (uint16_t)y);
	cci_put16(p + 5, (uint16_t)w);
	cci_put16(p + 7, (uint16_t)h);
	p[9] = 1; /* font 1 = system font */
	cci_put16(p + 10, (uint16_t)size);
	cci_put16(p + 12, (uint16_t)align);
	cci_put32(p + 14, argb);
	memcpy(p + 18, text, len); /* rest stays NUL: 128 text bytes, NUL padded */

	return flir_cci_raw(ctx, CCI_SYM_CREATE_TEXT, p, sizeof(p), NULL, NULL);
}

int flir_cci_sym_create_rect(struct flir_ctx *ctx, uint8_t id, int16_t x,
			     int16_t y, int16_t w, int16_t h, uint32_t argb,
			     int filled)
{
	uint32_t fn = filled ? CCI_SYM_FILLED_RECT : CCI_SYM_OUTLINED_RECT;
	return shape13(ctx, fn, id, x, y, w, h, argb);
}

int flir_cci_sym_create_line(struct flir_ctx *ctx, uint8_t id, int16_t x1,
			     int16_t y1, int16_t x2, int16_t y2, uint32_t argb)
{
	return shape13(ctx, CCI_SYM_CREATE_LINE, id, x1, y1, x2, y2, argb);
}

int flir_cci_sym_show(struct flir_ctx *ctx, uint8_t id, int visible)
{
	uint8_t p[2] = { id, (uint8_t)(visible ? 1 : 0) };
	return flir_cci_raw(ctx, CCI_SYM_UPDATE_AND_SHOW, p, sizeof(p), NULL, NULL);
}

int flir_cci_sym_delete(struct flir_ctx *ctx, uint8_t id)
{
	uint8_t p[1] = { id };
	return flir_cci_raw(ctx, CCI_SYM_DELETE, p, sizeof(p), NULL, NULL);
}

int flir_cci_sym_move_by(struct flir_ctx *ctx, uint8_t id, int16_t dx, int16_t dy)
{
	uint8_t p[5] = {0};
	p[0] = id;
	cci_put16(p + 1, (uint16_t)dx);
	cci_put16(p + 3, (uint16_t)dy);
	return flir_cci_raw(ctx, CCI_SYM_MOVE_BY_OFFSET, p, sizeof(p), NULL, NULL);
}
