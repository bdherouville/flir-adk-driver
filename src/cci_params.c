/*
 * cci_params.c — table-driven scalar Get/Set (struct flir_param_info).
 * IDs / types: Boson Software IDD 102-2013-44; every Get verified on a
 * 20640A075-6PADK, fw 2.0.15223. Ranges: IDD min/max where given, FLIR
 * AGC application note for agc-*, else the wire type's range.
 */
#include "cci_util.h"
#include <string.h>

const struct flir_param_info flir_params[] = {
	/* FFC */
	{ "ffc-mode",            0x00050013, 0x00050012, 0, 'e', 0, 2,      "FFC mode: 0 manual, 1 auto, 2 external" },
	{ "ffc-temp",            0x00050009, 0x00050008, 0, 'w', 1, 1000,   "auto-FFC trigger: FPA drift since last FFC, degC x10" },
	{ "ffc-period",          0x0005000B, 0x0005000A, 0, 'u', 1, 86400,  "auto-FFC trigger: seconds since last FFC" },
	{ "ffc-in-progress",     0x0005000C, 0, 0, 'h', 0, 0, "1 while an FFC runs" },
	{ "ffc-status",          0x00050054, 0, 0, 'e', 0, 0, "0 none yet, 1 imminent, 2 in progress, 3 complete" },
	{ "ffc-desired",         0x00050055, 0, 0, 'u', 0, 0, "1 when the camera wants an FFC" },
	{ "ffc-last-frame",      0x0005005D, 0, 0, 'u', 0, 0, "frame count at the last FFC" },
	{ "ffc-last-temp-kx10",  0x0005005E, 0, 0, 'w', 0, 0, "FPA temperature at the last FFC, K x10" },
	/* camera / system */
	{ "gain",                0x00050015, 0x00050014, 0, 'e', 0, 4, "gain mode: 0 high, 1 low, 2 auto, 3 dual, 4 manual" },
	{ "lut",                 0x000B0004, 0x000B0003, 0, 'e', 0, 9, "on-camera colour LUT 0..9 (8-bit stream)" },
	{ "telemetry",           0x00040002, 0x00040001, 0, 'e', 0, 1, "telemetry line 0|1" },
	{ "freeze",              0x000E0002, 0x000E0001, 0, 'e', 0, 1, "freeze the video pipeline 0|1" },
	{ "fps",                 0x000E0007, 0, 0, 'u', 0, 0, "camera frame rate" },
	{ "frame-count",         0x00020002, 0, 0, 'u', 0, 0, "frames since power-up" },
	{ "myriad-temp",         0x00050068, 0, 0, 'f', 0, 0, "processor (Myriad) temperature, degC" },
	{ "low-power",           0x00050062, 0, 0, 'w', 0, 0, "1 = low-power state after over-temperature" },
	{ "overtemp",            0x00050063, 0, 0, 'w', 0, 0, "1 = an over-temperature event occurred" },
	{ "overtemp-count",      0x00050076, 0, 0, 'u', 0, 0, "number of over-temperature events" },
	{ "lens",                0x00050039, 0, 0, 'u', 0, 0, "current lens number" },
	{ "table",               0x0005003B, 0, 0, 'u', 0, 0, "current NUC table number" },
	{ "jffs2-state",         0x00170007, 0, 0, 'e', 0, 0, "flash file system state, 3 = mounted" },
	/* on-camera noise filters */
	{ "tf",                  0x000A0002, 0x000A0001, 0, 'e', 0, 1, "temporal noise filter 0|1" },
	{ "tf-delta-nf",         0x000A0004, 0x000A0003, 0, 'w', 0, 255, "TF strength: delta_nf (scales wLUT index)" },
	{ "tf-delta-motion",     0x000A0006, 0x000A0005, 0, 'w', 0, 65535, "TF per-pixel motion threshold" },
	{ "tf-motion-threshold", 0x000A000F, 0x000A000E, 0, 'u', 0, 327680, "moving pixels needed to call a frame motion" },
	{ "scnr",                0x00080002, 0x00080001, 0, 'e', 0, 1, "column noise filter 0|1" },
	{ "scnr-th-col-sum",     0x00080004, 0x00080003, 0, 'w', 0, 65535, "SCNR column-sum threshold" },
	{ "scnr-th-pixel",       0x00080006, 0x00080005, 0, 'w', 0, 65535, "SCNR neighbour pixel threshold" },
	{ "scnr-max-corr",       0x00080008, 0x00080007, 0, 'w', 0, 65535, "SCNR maximum correction" },
	{ "spnr",                0x000C0002, 0x000C0001, 0, 'e', 0, 1, "spatial noise filter 0|1" },
	{ "spnr-sf-min",         0x000C001D, 0x000C001C, 0, 'f', 0, 10, "SPNR scale factor, static scene" },
	{ "spnr-sf-max",         0x000C001F, 0x000C001E, 0, 'f', 0, 10, "SPNR scale factor, moving scene" },
	{ "bpr",                 0x00030001, 0x00030002, 0, 'e', 0, 1, "bad pixel replacement 0|1" },
	/* camera AGC: shapes the 8-bit AGC8/NV12 stream, never RAW16 */
	{ "agc-percent-per-bin", 0x00090002, 0x00090001, 0, 'f', 0, 100, "plateau: max percent of pixels per histogram bin" },
	{ "agc-linear-percent",  0x00090004, 0x00090003, 0, 'f', 0, 100, "0 histogram-shaped .. 100 linear" },
	{ "agc-outlier-cut",     0x00090006, 0x00090005, 0, 'f', 0, 49, "percent of histogram tails ignored" },
	{ "agc-max-gain",        0x0009000A, 0x00090009, 0, 'f', 0.25, 8, "maximum transfer-function slope" },
	{ "agc-df",              0x0009000C, 0x0009000B, 0, 'f', 0, 100, "damping factor (100 = frozen)" },
	{ "agc-gamma",           0x0009000E, 0x0009000D, 0, 'f', 0.5, 4, "gamma (<1 more contrast in hot range)" },
	{ "agc-detail-headroom", 0x00090014, 0x00090013, 0, 'f', 0, 127, "8-bit shades reserved for DDE detail" },
	{ "agc-d2br",            0x00090016, 0x00090015, 0, 'f', 0, 8, "DDE detail-to-background ratio" },
	{ "agc-sigma-r",         0x00090018, 0x00090017, 0, 'f', 1, 8191, "DDE smoothing factor (FLIR: keep default)" },
	{ "agc-entropy",         0x0009001F, 0x0009001E, 0, 'e', 0, 1, "1 information-based equalization, 0 plateau" },
	/* image statistics (read-only) */
	{ "stats-mean",          0x001D0007, 0, 0, 'w', 0, 0, "image mean, counts" },
	{ "stats-first-bin",     0x001D0005, 0, 0, 'w', 0, 0, "lowest populated histogram bin" },
	{ "stats-last-bin",      0x001D0006, 0, 0, 'w', 0, 0, "highest populated histogram bin" },
	{ "stats-mean-roi",      0x001D000A, 0, 0, 'w', 0, 0, "mean inside stats-roi" },
	{ "stats-first-bin-roi", 0x001D0008, 0, 0, 'w', 0, 0, "lowest bin inside stats-roi" },
	{ "stats-last-bin-roi",  0x001D0009, 0, 0, 'w', 0, 0, "highest bin inside stats-roi" },
	/* DVO: CMOS / analog output only, USB video unaffected */
	{ "dvo-analog",          0x00060005, 0x00060004, 0, 'e', 0, 1, "analog video output 0|1" },
	{ "dvo-format",          0x00060007, 0x00060006, 0x0006000C, 'e', 0, 2, "CMOS output 0 RGB, 1 YCbCr, 2 default (applied)" },
	{ "dvo-display-mode",    0x0006000E, 0x0006000D, 0, 'e', 0, 1, "0 continuous, 1 one-shot" },
	/* symbology */
	{ "sym",                 0x00140020, 0x00140000, 0, 'e', 0, 1, "on-camera symbology overlay 0|1" },
	/* radiometric (R) / Boson+ only: status 0x161 on this 6P unit */
	{ "tlinear",             0x003E0002, 0x003E0001, 0, 'e', 0, 1, "TLinear 0|1 (radiometric only)" },
	{ "radiometry",          0x00420001, 0x00420000, 0, 'e', 0, 1, "radiometry temp-stable 0|1 (radiometric only)" },
	{ "spotmeter",           0x00430001, 0x00430000, 0, 'e', 0, 1, "spot meter 0|1 (radiometric only)" },
	{ NULL, 0, 0, 0, 0, 0, 0, NULL }
};

const struct flir_param_info *flir_param_find(const char *name)
{
	for (const struct flir_param_info *p = flir_params; name && p->name; p++)
		if (!strcmp(p->name, name))
			return p;
	return NULL;
}

static size_t wire_size(char type)
{
	return (type == 'h' || type == 'w') ? 2 : 4;
}

int flir_cci_param_get(struct flir_ctx *ctx, const struct flir_param_info *p,
		       double *v)
{
	uint8_t r[4];
	if (!p || !p->get_id || cci_get(ctx, p->get_id, r, wire_size(p->type)))
		return -1;
	switch (p->type) {
	case 'e': *v = (int32_t)cci_get32(r); break;
	case 'u': *v = cci_get32(r); break;
	case 'h': *v = (int16_t)cci_get16(r); break;
	case 'w': *v = cci_get16(r); break;
	case 'f': *v = cci_getf(r); break;
	default:  return -1;
	}
	return 0;
}

int flir_cci_param_set(struct flir_ctx *ctx, const struct flir_param_info *p,
		       double v)
{
	uint8_t b[4];
	/* !(a && b) form also rejects NaN */
	if (!p || !p->set_id || !(v >= p->lo && v <= p->hi))
		return -1;
	if (p->type != 'f' && v != (double)(long long)v)
		return -1;
	switch (p->type) {
	case 'e': cci_put32(b, (uint32_t)(int32_t)v); break;
	case 'u': cci_put32(b, (uint32_t)v); break;
	case 'h': cci_put16(b, (uint16_t)(int16_t)v); break;
	case 'w': cci_put16(b, (uint16_t)v); break;
	case 'f': cci_putf(b, (float)v); break;
	default:  return -1;
	}
	if (flir_cci_raw(ctx, p->set_id, b, wire_size(p->type), NULL, NULL))
		return -1;
	return p->post_id ? flir_cci_raw(ctx, p->post_id, NULL, 0, NULL, NULL) : 0;
}
