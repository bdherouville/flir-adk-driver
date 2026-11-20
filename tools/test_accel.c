/* test_accel — self-check for the generic HEQ AGC (make test) */
#include "flir_adk.h"
#include <assert.h>
#include <stdio.h>

extern const struct flir_accel flir_accel_generic;

int main(void)
{
	enum { W = 640, H = 512 };
	static uint16_t raw[W * H];
	static uint8_t out[W * H];

	/* Ramp straddling 16384: the old 14-bit mask wrapped here */
	for (int i = 0; i < W * H; i++)
		raw[i] = (uint16_t)(16000 + i * 800 / (W * H));
	flir_accel_generic.agc_process(raw, out, W, H, NULL);
	for (int i = 1; i < W * H; i++)
		assert(out[i] >= out[i - 1]);           /* monotonic: hotter never darker */
	assert(out[0] < 5 && out[W * H - 1] > 250);  /* uses the full gray range */

	/* Flat frame → mid gray, no divide by zero */
	for (int i = 0; i < W * H; i++)
		raw[i] = 22000;
	flir_accel_generic.agc_process(raw, out, W, H, NULL);
	assert(out[0] == 128);

	/* gray_to_raw: labels follow the AGC mapping, monotonic, ends inside data */
	for (int i = 0; i < W * H; i++)
		raw[i] = (uint16_t)(22000 + i * 1000 / (W * H));
	flir_accel_generic.agc_process(raw, out, W, H, NULL);
	static uint16_t g2r[256];
	flir_gray_to_raw(raw, out, W * H, g2r);
	for (int g = 1; g < 256; g++)
		assert(g2r[g] >= g2r[g - 1]);
	assert(g2r[0] >= 22000 && g2r[255] <= 23000 && g2r[255] - g2r[0] > 900);

	/* Linear-percent blend: 95% in a narrow cold band, 5% far hotter. Pure HEQ
	 * puts the two modes nearly adjacent in gray; the linear share must open a
	 * gap of roughly LINEAR*255. Run 30 frames so the smoothed history settles. */
	for (int i = 0; i < W * H; i++)
		raw[i] = (uint16_t)(i < W * H / 20 ? 23000 + i % 11 : 22000 + i % 51);
	for (int f = 0; f < 30; f++)
		flir_accel_generic.agc_process(raw, out, W, H, NULL);
	int cold_max = 0, hot_min = 255;
	for (int i = 0; i < W * H; i++) {
		if (raw[i] < 22500 && out[i] > cold_max) cold_max = out[i];
		if (raw[i] > 22500 && out[i] < hot_min) hot_min = out[i];
	}
	printf("bimodal gap: cold_max=%d hot_min=%d\n", cold_max, hot_min);
	assert(hot_min - cold_max >= 40);

	/* denoise: static noise shrinks, a real step passes through immediately */
	static int32_t acc[W * H];
	static uint16_t den[W * H];
	double var_in = 0, var_out = 0;
	for (int f = 0; f < 200; f++) {
		for (int i = 0; i < W * H; i++)
			raw[i] = (uint16_t)(22000 + (int)(((unsigned)i * 7919u + (unsigned)f * 104729u) % 13u) - 6); /* +-6 noise */
		flir_denoise(raw, den, acc, W * H, 20, f == 0);
		if (f >= 20) {
			var_in += (raw[1000] - 22000.0) * (raw[1000] - 22000.0);
			var_out += (den[1000] - 22000.0) * (den[1000] - 22000.0);
		}
	}
	assert(var_out < var_in / 3);
	raw[1000] = 22500;
	flir_denoise(raw, den, acc, W * H, 20, 0);
	assert(den[1000] == 22500);

	/* overlay hot/cold markers: red under the hot pixel, blue above the cold one, off by default */
	{
		enum { OW = W + 100 };
		static uint8_t src[W * H * 3], dst[OW * H * 3];
		struct flir_overlay_opts ov = { .raw_min = 22000, .raw_max = 23000, .cross_hide = 1 };
		flir_overlay(src, raw, W, H, &ov, dst);
		assert(dst[((100 - 3) * OW + 200) * 3] == 0);           /* off: image untouched */
		ov.minmax_show = 1;
		ov.hot_x = 200; ov.hot_y = 100; ov.cold_x = 400; ov.cold_y = 300;
		flir_overlay(src, raw, W, H, &ov, dst);
		const uint8_t *hot = &dst[((100 - 3) * OW + 200) * 3];  /* inside the hot triangle */
		const uint8_t *cold = &dst[((300 + 3) * OW + 400) * 3]; /* inside the cold triangle */
		assert(hot[0] > 200 && hot[2] < 100);
		assert(cold[2] > 200 && cold[0] < 100);
	}

	puts("test_accel: ok");
	return 0;
}
