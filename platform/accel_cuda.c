/*
 * accel_cuda.c — Jetson Orin / CUDA accelerated AGC + colorizer (stub)
 *
 * Build with: nvcc -c accel_cuda.cu, link against -lcuda -lcudart
 * Or use NPP (NVIDIA Performance Primitives) for histogram-EQ AGC.
 *
 * ponytail: stub, implement CUDA kernels when camera hardware is available
 */

#include "flir_adk.h"

static int cuda_agc(const uint16_t *raw, uint8_t *out,
		    int w, int h, void *priv)
{
	(void)priv;
	/* TODO: upload raw to GPU, run histogram-equalization AGC via NPP,
	 * download result. For now, fall back to CPU linear AGC. */
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

const struct flir_accel flir_accel_cuda = {
	.name        = "cuda/jetson-orin",
	.agc_process = cuda_agc,
	.colorize    = NULL,
	.priv        = NULL,
};
