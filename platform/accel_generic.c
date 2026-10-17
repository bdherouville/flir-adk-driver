/*
 * accel_generic.c — pure-C software AGC and colorizer (no HW accel)
 *
 * Works everywhere: OpenWrt/QCA9533, generic ARM, x86.
 */

#include "flir_adk.h"
#include <string.h>

/*
 * Histogram-equalization AGC for RAW16 thermal data.
 *
 * Better than linear stretch — spreads contrast across the actual
 * distribution of pixel values rather than just min/max.
 * Clips top/bottom 0.5% to reject hot/dead pixels.
 */
static int heq_agc(const uint16_t *raw, uint8_t *out, int w, int h, void *priv)
{
	(void)priv;
	int n = w * h;

	/* Build histogram over 14-bit range (Boson uses 14 bits in 16-bit container) */
	#define HIST_BINS 16384
	static uint32_t hist[HIST_BINS]; /* ponytail: static to avoid 64KB stack alloc */
	memset(hist, 0, sizeof(hist));

	uint16_t lo = 0xFFFF, hi = 0;
	for (int i = 0; i < n; i++) {
		uint16_t v = raw[i] & 0x3FFF; /* mask to 14 bits */
		hist[v]++;
		if (raw[i] < lo) lo = raw[i];
		if (raw[i] > hi) hi = raw[i];
	}
	if (hi == lo) {
		memset(out, 128, n);
		return 0;
	}

	/* Clip top/bottom 0.5% for dead/hot pixel rejection */
	int clip = n / 200;
	uint16_t clip_lo = 0, clip_hi = HIST_BINS - 1;
	int acc = 0;
	for (int i = 0; i < HIST_BINS && acc < clip; i++) {
		acc += hist[i];
		clip_lo = i;
	}
	acc = 0;
	for (int i = HIST_BINS - 1; i >= 0 && acc < clip; i--) {
		acc += hist[i];
		clip_hi = i;
	}
	if (clip_hi <= clip_lo) {
		clip_lo = lo & 0x3FFF;
		clip_hi = hi & 0x3FFF;
	}

	/* Build CDF (cumulative distribution function) as a 14-bit → 8-bit LUT */
	static uint8_t cdf_lut[HIST_BINS];
	uint32_t cdf = 0;
	uint32_t range_pixels = 0;
	for (int i = clip_lo; i <= clip_hi; i++)
		range_pixels += hist[i];
	if (range_pixels == 0) range_pixels = 1;

	for (int i = 0; i < HIST_BINS; i++) {
		if (i < clip_lo) {
			cdf_lut[i] = 0;
		} else if (i > clip_hi) {
			cdf_lut[i] = 255;
		} else {
			cdf += hist[i];
			cdf_lut[i] = (uint8_t)((uint64_t)cdf * 255 / range_pixels);
		}
	}

	for (int i = 0; i < n; i++)
		out[i] = cdf_lut[raw[i] & 0x3FFF];

	return 0;
	#undef HIST_BINS
}

/*
 * Iron-bow colorizer: black → blue → red → yellow → white
 * Standard thermal palette, same as Boson's built-in LUT 1.
 */
static const uint8_t ironbow[256][3] = {
	/* Generated: 4-stop gradient, pre-computed for zero runtime cost */
	#define IB(i) { \
		(i) < 64 ? 0 : (i) < 128 ? ((i)-64)*200/64 : (i) < 192 ? 200+((i)-128)*55/64 : 255, \
		(i) < 128 ? 0 : (i) < 192 ? ((i)-128)*200/64 : 200+((i)-192)*55/64, \
		(i) < 64  ? (i)*128/64 : (i) < 128 ? 128-((i)-64)*128/64 : (i) < 192 ? 0 : ((i)-192)*255/64 \
	}
	/* Unroll all 256 entries via groups of 16 */
	#define IB16(b) IB(b+0),IB(b+1),IB(b+2),IB(b+3),IB(b+4),IB(b+5),IB(b+6),IB(b+7), \
		IB(b+8),IB(b+9),IB(b+10),IB(b+11),IB(b+12),IB(b+13),IB(b+14),IB(b+15)
	IB16(0),IB16(16),IB16(32),IB16(48),IB16(64),IB16(80),IB16(96),IB16(112),
	IB16(128),IB16(144),IB16(160),IB16(176),IB16(192),IB16(208),IB16(224),IB16(240)
	#undef IB16
	#undef IB
};

static int sw_colorize(const uint8_t *gray, uint8_t *rgb,
		       int w, int h, int lut, void *priv)
{
	(void)lut; /* ponytail: only iron-bow for now, add more LUTs when needed */
	(void)priv;
	int n = w * h;
	for (int i = 0; i < n; i++) {
		uint8_t g = gray[i];
		rgb[i * 3]     = ironbow[g][0];
		rgb[i * 3 + 1] = ironbow[g][1];
		rgb[i * 3 + 2] = ironbow[g][2];
	}
	return 0;
}

const struct flir_accel flir_accel_generic = {
	.name        = "generic",
	.agc_process = heq_agc,
	.colorize    = sw_colorize,
	.priv        = NULL,
};
