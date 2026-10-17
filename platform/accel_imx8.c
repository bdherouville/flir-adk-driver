/*
 * accel_imx8.c — NXP i.MX8 GPU/VPU accelerated processing (stub)
 *
 * The i.MX8 has a Vivante GC7000 GPU (OpenCL capable) and a VPU.
 * Real implementation would use OpenCL or G2D for AGC/colorize.
 *
 * ponytail: stub, implement OpenCL path when i.MX8 BSP is available
 */

#include "flir_adk.h"

static int imx8_agc(const uint16_t *raw, uint8_t *out,
		    int w, int h, void *priv)
{
	(void)priv;
	uint16_t lo = 0xFFFF, hi = 0;
	int n = w * h;
	for (int i = 0; i < n; i++) {
		if (raw[i] < lo) lo = raw[i];
		if (raw[i] > hi) hi = raw[i];
	}
	if (hi == lo) hi = lo + 1;
	uint32_t range = hi - lo;
	for (int i = 0; i < n; i++)
		out[i] = (uint8_t)(((uint32_t)(raw[i] - lo) * 255) / range);
	return 0;
}

const struct flir_accel flir_accel_imx8 = {
	.name        = "imx8/vivante",
	.agc_process = imx8_agc,
	.colorize    = NULL,
	.priv        = NULL,
};
