#include "flir_adk.h"
#include <string.h>
#include <stdio.h>

#define BAR_W    100
#define GRAD_W   16
#define GRAD_X   8
#define MARGIN_Y 20
#define FONT_S   2   /* 2x scale: each font pixel = 2x2 output pixels */
#define CHAR_W   (5 * FONT_S + 1)
#define CHAR_H   (7 * FONT_S)

/* 5x7 bitmap font for 0-9 and punctuation */
static const uint8_t font5x7[][7] = {
	[0]  = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
	[1]  = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
	[2]  = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
	[3]  = {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* 3 */
	[4]  = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
	[5]  = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
	[6]  = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
	[7]  = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
	[8]  = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
	[9]  = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
	[10] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
	[11] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
	[12] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
	[13] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
	[14] = {0x06,0x09,0x09,0x06,0x00,0x00,0x00}, /* degree ° */
	[15] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
	[16] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
	[17] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
	[18] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
	[19] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
	[20] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
	[21] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
};

static int char_idx(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c == '-') return 10;
	if (c == '.') return 11;
	if (c == 'C') return 13;
	if (c == '^') return 14; /* degree symbol */
	if (c == 'E') return 15;
	if (c == 'R') return 16;
	if (c == 'O') return 17;
	if (c == 'I') return 18;
	if (c == 'F') return 19;
	if (c == 'P') return 20;
	if (c == 'A') return 21;
	return 12;
}

static void draw_char_s(uint8_t *rgb, int img_w, int img_h,
			int x0, int y0, char c, int scale,
			uint8_t r, uint8_t g, uint8_t b)
{
	int ci = char_idx(c);
	for (int row = 0; row < 7; row++)
		for (int col = 0; col < 5; col++)
			if (font5x7[ci][row] & (0x10 >> col))
				for (int sy = 0; sy < scale; sy++)
					for (int sx = 0; sx < scale; sx++) {
						int px = x0 + col * scale + sx;
						int py = y0 + row * scale + sy;
						if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;
						int off = (py * img_w + px) * 3;
						rgb[off] = r; rgb[off+1] = g; rgb[off+2] = b;
					}
}

static void draw_text_s(uint8_t *rgb, int img_w, int img_h,
			int x, int y, const char *s, int scale,
			uint8_t r, uint8_t g, uint8_t b)
{
	int cw = 5 * scale + (scale > 1 ? 1 : 0);
	for (int i = 0; s[i]; i++)
		draw_char_s(rgb, img_w, img_h, x + i * cw, y, s[i], scale, r, g, b);
}

static void draw_text(uint8_t *rgb, int img_w, int img_h,
		      int x, int y, const char *s,
		      uint8_t r, uint8_t g, uint8_t b)
{
	draw_text_s(rgb, img_w, img_h, x, y, s, FONT_S, r, g, b);
}

static void set_pixel(uint8_t *rgb, int img_w, int img_h,
		      int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	if (x < 0 || x >= img_w || y < 0 || y >= img_h) return;
	int off = (y * img_w + x) * 3;
	rgb[off] = r; rgb[off+1] = g; rgb[off+2] = b;
}

/* Iron-bow LUT — must match accel_generic.c */
static void ironbow_rgb(int idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = idx < 64 ? 0 : idx < 128 ? (idx-64)*200/64 : idx < 192 ? 200+(idx-128)*55/64 : 255;
	*g = idx < 128 ? 0 : idx < 192 ? (idx-128)*200/64 : 200+(idx-192)*55/64;
	*b = idx < 64 ? idx*128/64 : idx < 128 ? 128-(idx-64)*128/64 : idx < 192 ? 0 : (idx-192)*255/64;
}

static void draw_crosshair(uint8_t *rgb, int img_w, int img_h, int cx, int cy)
{
	int arm = 20, gap = 6;
	/* Horizontal arms */
	for (int x = cx - arm; x <= cx + arm; x++) {
		if (x >= cx - gap && x <= cx + gap) continue;
		set_pixel(rgb, img_w, img_h, x, cy, 255, 255, 255);
		set_pixel(rgb, img_w, img_h, x, cy-1, 0, 0, 0);
		set_pixel(rgb, img_w, img_h, x, cy+1, 0, 0, 0);
	}
	/* Vertical arms */
	for (int y = cy - arm; y <= cy + arm; y++) {
		if (y >= cy - gap && y <= cy + gap) continue;
		set_pixel(rgb, img_w, img_h, cx, y, 255, 255, 255);
		set_pixel(rgb, img_w, img_h, cx-1, y, 0, 0, 0);
		set_pixel(rgb, img_w, img_h, cx+1, y, 0, 0, 0);
	}
	/* Center dot */
	set_pixel(rgb, img_w, img_h, cx, cy, 255, 255, 255);
}

static void draw_colorbar(uint8_t *rgb, int ow, int w, int h,
			   uint16_t raw_min, uint16_t raw_max)
{
	int grad_top = MARGIN_Y;
	int grad_bot = h - MARGIN_Y;
	int grad_h = grad_bot - grad_top;

	/* Gradient */
	for (int y = grad_top; y < grad_bot; y++) {
		int idx = 255 - (y - grad_top) * 255 / (grad_h - 1);
		uint8_t r, g, b;
		ironbow_rgb(idx, &r, &g, &b);
		for (int x = w + GRAD_X; x < w + GRAD_X + GRAD_W; x++)
			set_pixel(rgb, ow, h, x, y, r, g, b);
	}

	/* Border */
	for (int x = w + GRAD_X - 1; x <= w + GRAD_X + GRAD_W; x++) {
		set_pixel(rgb, ow, h, x, grad_top - 1, 180, 180, 180);
		set_pixel(rgb, ow, h, x, grad_bot, 180, 180, 180);
	}
	for (int y = grad_top - 1; y <= grad_bot; y++) {
		set_pixel(rgb, ow, h, w + GRAD_X - 1, y, 180, 180, 180);
		set_pixel(rgb, ow, h, w + GRAD_X + GRAD_W, y, 180, 180, 180);
	}

	/* Labels */
	int text_x = w + GRAD_X + GRAD_W + 6;
	uint16_t raw_mid = (raw_min + raw_max) / 2;
	char label[16];

	snprintf(label, sizeof(label), "%u", raw_max);
	draw_text(rgb, ow, h, text_x, grad_top, label, 255, 255, 255);

	snprintf(label, sizeof(label), "%u", raw_mid);
	draw_text(rgb, ow, h, text_x, (grad_top + grad_bot) / 2 - CHAR_H / 2,
		  label, 200, 200, 200);

	snprintf(label, sizeof(label), "%u", raw_min);
	draw_text(rgb, ow, h, text_x, grad_bot - CHAR_H, label, 255, 255, 255);

	/* Tick marks */
	int ticks[3] = { grad_top, (grad_top + grad_bot) / 2, grad_bot - 1 };
	for (int t = 0; t < 3; t++)
		for (int x = w + GRAD_X + GRAD_W + 1; x < text_x - 1; x++)
			set_pixel(rgb, ow, h, x, ticks[t], 180, 180, 180);
}

int flir_colorbar(const uint8_t *src_rgb, int w, int h,
		  uint16_t raw_min, uint16_t raw_max,
		  uint8_t *dst_rgb)
{
	if (!src_rgb || !dst_rgb) return -1;
	int ow = w + BAR_W;

	for (int y = 0; y < h; y++) {
		memcpy(dst_rgb + y * ow * 3, src_rgb + y * w * 3, w * 3);
		memset(dst_rgb + (y * ow + w) * 3, 0, BAR_W * 3);
	}

	draw_colorbar(dst_rgb, ow, w, h, raw_min, raw_max);
	return 0;
}

int flir_overlay(const uint8_t *src_rgb, const uint16_t *raw,
		 int w, int h, const struct flir_overlay_opts *opts,
		 uint8_t *dst_rgb)
{
	if (!src_rgb || !raw || !opts || !dst_rgb) return -1;
	int ow = w + BAR_W;

	/* Copy source + black bar area */
	for (int y = 0; y < h; y++) {
		memcpy(dst_rgb + y * ow * 3, src_rgb + y * w * 3, w * 3);
		memset(dst_rgb + (y * ow + w) * 3, 0, BAR_W * 3);
	}

	/* Color bar */
	draw_colorbar(dst_rgb, ow, w, h, opts->raw_min, opts->raw_max);

	/* Center crosshair */
	int cx = w / 2, cy = h / 2;
	draw_crosshair(dst_rgb, ow, h, cx, cy);

	/* Center spot value (average 5x5 around center) */
	long sum = 0; int cnt = 0;
	for (int dy = -2; dy <= 2; dy++)
		for (int dx = -2; dx <= 2; dx++) {
			int px = cx + dx, py = cy + dy;
			if (px >= 0 && px < w && py >= 0 && py < h) {
				sum += raw[py * w + px];
				cnt++;
			}
		}
	uint16_t center_val = cnt ? (uint16_t)(sum / cnt) : 0;

	/* Draw center value label with background box */
	char label[24];
	snprintf(label, sizeof(label), "%u", center_val);
	int lx = cx + 24, ly = cy - 20;
	int label_len = (int)strlen(label);
	int box_w = label_len * CHAR_W + 6;
	int box_h = CHAR_H + 6;
	/* Dark background */
	for (int by = ly - 2; by < ly + box_h - 2; by++)
		for (int bx = lx - 3; bx < lx + box_w - 3; bx++)
			set_pixel(dst_rgb, ow, h, bx, by, 0, 0, 0);
	draw_text(dst_rgb, ow, h, lx, ly, label, 255, 255, 255);

	/* FPA temperature (top-left corner) */
	if (opts->fpa_temp_cx10 != 0) {
		int t = opts->fpa_temp_cx10;
		char fpa[24];
		snprintf(fpa, sizeof(fpa), "FPA %d.%d^C", t / 10, t < 0 ? -(t % 10) : t % 10);
		/* Background box */
		int fl = (int)strlen(fpa);
		for (int by = 4; by < 4 + CHAR_H + 4; by++)
			for (int bx = 4; bx < 4 + fl * CHAR_W + 4; bx++)
				set_pixel(dst_rgb, ow, h, bx, by, 0, 0, 0);
		draw_text(dst_rgb, ow, h, 6, 6, fpa, 220, 220, 220);
	}

	/* Spot meter box outline (center ROI) */
	if (opts->spot_enabled) {
		int sr = opts->spot_row_start, er = opts->spot_row_end;
		int sc = opts->spot_col_start, ec = opts->spot_col_end;
		for (int x = sc; x < ec; x++) {
			set_pixel(dst_rgb, ow, h, x, sr, 255, 255, 255);
			set_pixel(dst_rgb, ow, h, x, er - 1, 255, 255, 255);
		}
		for (int y = sr; y < er; y++) {
			set_pixel(dst_rgb, ow, h, sc, y, 255, 255, 255);
			set_pixel(dst_rgb, ow, h, ec - 1, y, 255, 255, 255);
		}

		/* Spot meter mean value below the box */
		char spot[24];
		snprintf(spot, sizeof(spot), "ROI %u", opts->spot_mean);
		int sl = (int)strlen(spot);
		int spot_x = (sc + ec) / 2 - sl * CHAR_W / 2;
		int spot_y = er + 4;
		for (int by = spot_y - 1; by < spot_y + CHAR_H + 1; by++)
			for (int bx = spot_x - 2; bx < spot_x + sl * CHAR_W + 2; bx++)
				set_pixel(dst_rgb, ow, h, bx, by, 0, 0, 0);
		draw_text(dst_rgb, ow, h, spot_x, spot_y, spot, 255, 255, 0);
	}

	return 0;
}
