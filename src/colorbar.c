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

/* 5x7 bitmap font */
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
	[22] = {0x11,0x11,0x15,0x15,0x15,0x0A,0x0A}, /* W (for raw) */
	[23] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
	[24] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
	[25] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
};

static int char_idx(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c == '-') return 10;
	if (c == '.') return 11;
	if (c == 'C') return 13;
	if (c == '^') return 14;
	if (c == 'E') return 15;
	if (c == 'R') return 16;
	if (c == 'O') return 17;
	if (c == 'I') return 18;
	if (c == 'F') return 19;
	if (c == 'P') return 20;
	if (c == 'A') return 21;
	if (c == 'W') return 22;
	if (c == 'Y') return 23;
	if (c == 'N') return 24;
	if (c == 'K') return 25;
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

static void draw_text(uint8_t *rgb, int img_w, int img_h,
		      int x, int y, const char *s,
		      uint8_t r, uint8_t g, uint8_t b)
{
	for (int i = 0; s[i]; i++)
		draw_char_s(rgb, img_w, img_h, x + i * CHAR_W, y, s[i], FONT_S, r, g, b);
}

static void set_pixel(uint8_t *rgb, int img_w, int img_h,
		      int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
	if (x < 0 || x >= img_w || y < 0 || y >= img_h) return;
	int off = (y * img_w + x) * 3;
	rgb[off] = r; rgb[off+1] = g; rgb[off+2] = b;
}

static void draw_box(uint8_t *rgb, int img_w, int img_h,
		     int x, int y, int w, int h,
		     uint8_t r, uint8_t g, uint8_t b)
{
	for (int by = y; by < y + h; by++)
		for (int bx = x; bx < x + w; bx++)
			set_pixel(rgb, img_w, img_h, bx, by, r, g, b);
}

/* Iron-bow LUT — must match accel_generic.c */
static void ironbow_rgb(int idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = idx < 64 ? 0 : idx < 128 ? (idx-64)*200/64 : idx < 192 ? 200+(idx-128)*55/64 : 255;
	*g = idx < 128 ? 0 : idx < 192 ? (idx-128)*200/64 : 200+(idx-192)*55/64;
	*b = idx < 64 ? idx*128/64 : idx < 128 ? 128-(idx-64)*128/64 : idx < 192 ? 0 : (idx-192)*255/64;
}

/* Convert raw count to temperature in the requested unit */
static float raw_to_temp(uint16_t raw, const struct flir_overlay_opts *o)
{
	float c = o->ref_temp_c + (float)((int)raw - (int)o->ref_raw) / o->counts_per_deg;
	switch (o->temp_unit) {
	case FLIR_UNIT_F: return c * 9.0f / 5.0f + 32.0f;
	case FLIR_UNIT_K: return c + 273.15f;
	default:          return c; /* Celsius */
	}
}

static float celsius_to_unit(float c, enum flir_temp_unit u)
{
	switch (u) {
	case FLIR_UNIT_F: return c * 9.0f / 5.0f + 32.0f;
	case FLIR_UNIT_K: return c + 273.15f;
	default:          return c;
	}
}

static const char *unit_suffix(enum flir_temp_unit u)
{
	switch (u) {
	case FLIR_UNIT_F: return "^F";
	case FLIR_UNIT_K: return "K";
	default:          return "^C";
	}
}

static int has_temp_cal(const struct flir_overlay_opts *o)
{
	return o->temp_unit != FLIR_UNIT_RAW && o->counts_per_deg > 0;
}

/* Format a value: either raw count or temperature with unit */
static void fmt_value(char *buf, size_t sz, uint16_t raw,
		      const struct flir_overlay_opts *o)
{
	if (has_temp_cal(o)) {
		float t = raw_to_temp(raw, o);
		snprintf(buf, sz, "%.1f%s", t, unit_suffix(o->temp_unit));
	} else {
		snprintf(buf, sz, "%u", raw);
	}
}

static void draw_crosshair(uint8_t *rgb, int img_w, int img_h, int cx, int cy)
{
	int arm = 20, gap = 6;
	for (int x = cx - arm; x <= cx + arm; x++) {
		if (x >= cx - gap && x <= cx + gap) continue;
		set_pixel(rgb, img_w, img_h, x, cy, 255, 255, 255);
		set_pixel(rgb, img_w, img_h, x, cy-1, 0, 0, 0);
		set_pixel(rgb, img_w, img_h, x, cy+1, 0, 0, 0);
	}
	for (int y = cy - arm; y <= cy + arm; y++) {
		if (y >= cy - gap && y <= cy + gap) continue;
		set_pixel(rgb, img_w, img_h, cx, y, 255, 255, 255);
		set_pixel(rgb, img_w, img_h, cx-1, y, 0, 0, 0);
		set_pixel(rgb, img_w, img_h, cx+1, y, 0, 0, 0);
	}
	set_pixel(rgb, img_w, img_h, cx, cy, 255, 255, 255);
}

static void draw_colorbar_labels(uint8_t *rgb, int ow, int w, int h,
				 const struct flir_overlay_opts *opts)
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
	uint16_t raw_mid = (opts->raw_min + opts->raw_max) / 2;
	char label[24];

	fmt_value(label, sizeof(label), opts->raw_max, opts);
	draw_text(rgb, ow, h, text_x, grad_top, label, 255, 255, 255);

	fmt_value(label, sizeof(label), raw_mid, opts);
	draw_text(rgb, ow, h, text_x, (grad_top + grad_bot) / 2 - CHAR_H / 2,
		  label, 200, 200, 200);

	fmt_value(label, sizeof(label), opts->raw_min, opts);
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

	struct flir_overlay_opts opts = { .raw_min = raw_min, .raw_max = raw_max };
	draw_colorbar_labels(dst_rgb, ow, w, h, &opts);
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

	/* Color bar with labels */
	draw_colorbar_labels(dst_rgb, ow, w, h, opts);

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

	/* Center value label */
	char label[24];
	fmt_value(label, sizeof(label), center_val, opts);
	int lx = cx + 24, ly = cy - 20;
	int ll = (int)strlen(label);
	draw_box(dst_rgb, ow, h, lx - 3, ly - 2, ll * CHAR_W + 6, CHAR_H + 4, 0, 0, 0);
	draw_text(dst_rgb, ow, h, lx, ly, label, 255, 255, 255);

	/* FPA temperature (top-left corner) */
	if (opts->fpa_temp_cx10 != 0) {
		float fpa_c = opts->fpa_temp_cx10 / 10.0f;
		float fpa_display = celsius_to_unit(fpa_c, opts->temp_unit);
		char fpa[24];
		if (opts->temp_unit == FLIR_UNIT_K)
			snprintf(fpa, sizeof(fpa), "FPA %.1fK", fpa_display);
		else
			snprintf(fpa, sizeof(fpa), "FPA %.1f%s",
				 fpa_display, unit_suffix(opts->temp_unit));
		int fl = (int)strlen(fpa);
		draw_box(dst_rgb, ow, h, 4, 4, fl * CHAR_W + 4, CHAR_H + 4, 0, 0, 0);
		draw_text(dst_rgb, ow, h, 6, 6, fpa, 220, 220, 220);
	}

	/* Unit indicator (top-right of image area) */
	if (has_temp_cal(opts)) {
		const char *uname;
		switch (opts->temp_unit) {
		case FLIR_UNIT_F: uname = "^F"; break;
		case FLIR_UNIT_K: uname = "K";  break;
		default:          uname = "^C"; break;
		}
		int ul = (int)strlen(uname);
		int ux = w - ul * CHAR_W - 8;
		draw_box(dst_rgb, ow, h, ux - 2, 4, ul * CHAR_W + 4, CHAR_H + 4, 0, 0, 0);
		draw_text(dst_rgb, ow, h, ux, 6, uname, 255, 255, 0);
	}

	/* Spot meter box outline */
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
		fmt_value(spot, sizeof(spot), opts->spot_mean, opts);
		int sl = (int)strlen(spot);
		int spot_x = (sc + ec) / 2 - sl * CHAR_W / 2;
		int spot_y = er + 4;
		draw_box(dst_rgb, ow, h, spot_x - 2, spot_y - 1,
			 sl * CHAR_W + 4, CHAR_H + 2, 0, 0, 0);
		draw_text(dst_rgb, ow, h, spot_x, spot_y, spot, 255, 255, 0);
	}

	return 0;
}
