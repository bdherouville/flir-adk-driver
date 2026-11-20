/*
 * accel_generic.c — pure-C software AGC and colorizer (no HW accel)
 *
 * Works everywhere: OpenWrt/QCA9533, generic ARM, x86.
 */

#include "flir_adk.h"
#include <string.h>

/*
 * Plateau histogram-equalization AGC for RAW16 thermal data.
 *
 * - Full 16-bit range (Boson Y16 values sit around 20000+, not 14-bit).
 * - Clips top/bottom 0.5% to reject hot/dead pixels.
 * - Plateau: caps each histogram bin at PLATEAU x the mean bin height, so large
 *   flat areas (walls, ceiling) don't eat the gray range and amplify noise.
 * - Temporal: the histogram is IIR-smoothed across frames to stop flicker.
 *   Tune SMOOTH (0 = no memory, ->1 = slow) and PLATEAU to taste.
 * - Linear percent (like Boson agcSetLinearPercent, default 20%): the curve is
 *   (1-LINEAR) x equalized + LINEAR x a straight ramp over the smoothed
 *   histogram's own 0.5% tails, so sparse hot/cold objects keep separation.
 */
#define HIST_BINS 65536
#define PLATEAU   4.0f
#define SMOOTH    0.8f
#define LINEAR    0.20f

static int heq_agc(const uint16_t *raw, uint8_t *out, int w, int h, void *priv)
{
	(void)priv;
	int n = w * h;
	/* ponytail: static state, one camera per process; move to priv for multi-cam */
	static uint32_t hist[HIST_BINS];
	static float smooth[HIST_BINS];
	static uint8_t lut[HIST_BINS];
	static int have_prev;
	memset(hist, 0, sizeof(hist));

	for (int i = 0; i < n; i++)
		hist[raw[i]]++;

	/* 0.5% tails */
	int clip = n / 200, acc = 0, lo = 0, hi = HIST_BINS - 1;
	while (lo < HIST_BINS - 1 && (acc += hist[lo]) <= clip) lo++;
	acc = 0;
	while (hi > lo && (acc += hist[hi]) <= clip) hi--;
	if (hi <= lo) {
		memset(out, 128, n);
		return 0;
	}

	/* Plateau-clip, then blend into the running histogram */
	float cap = PLATEAU * (float)n / (float)(hi - lo + 1);
	float a = have_prev ? SMOOTH : 0.0f;
	for (int i = 0; i < HIST_BINS; i++) {
		float v = (i < lo || i > hi) ? 0.0f : (hist[i] > cap ? cap : (float)hist[i]);
		smooth[i] = a * smooth[i] + (1.0f - a) * v;
	}
	have_prev = 1;

	/* CDF over the smoothed histogram (its support may be wider than [lo,hi]) */
	double total = 0;
	for (int i = 0; i < HIST_BINS; i++)
		total += smooth[i];
	/* Linear ramp endpoints: 0.5% tails of the smoothed histogram (stable) */
	double cdf = 0;
	int slo = -1, shi = 0;
	for (int i = 0; i < HIST_BINS; i++) {
		cdf += smooth[i];
		if (slo < 0 && cdf > total * 0.005) slo = i;
		if (cdf < total * 0.995) shi = i + 1;
	}
	if (shi <= slo) shi = slo + 1;
	cdf = 0;
	for (int i = 0; i < HIST_BINS; i++) {
		cdf += smooth[i];
		double lin = i <= slo ? 0.0 : i >= shi ? 255.0 : 255.0 * (i - slo) / (shi - slo);
		lut[i] = (uint8_t)((1.0 - LINEAR) * (cdf * 255.0 / total) + LINEAR * lin + 0.5);
	}

	for (int i = 0; i < n; i++)
		out[i] = lut[raw[i]];
	return 0;
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
