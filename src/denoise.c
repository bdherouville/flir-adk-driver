/*
 * denoise.c — motion-adaptive temporal filter for RAW16 frames
 */
#include "flir_adk.h"
#include <stdlib.h>

/* acc holds the running average in 1/16 count units (avoids rounding drift) */
void flir_denoise(const uint16_t *in, uint16_t *out, int32_t *acc, int n,
		  int thresh, int reset)
{
	/* ponytail: hard motion threshold, per-pixel; blend near the threshold if edges shimmer */
	int32_t t = (int32_t)thresh << 4;
	for (int i = 0; i < n; i++) {
		int32_t r = (int32_t)in[i] << 4;
		int32_t d = r - acc[i];
		if (reset || abs(d) > t)
			acc[i] = r;
		else
			acc[i] += d / 8;   /* IIR, alpha = 1/8 (~8 frame time constant) */
		out[i] = (uint16_t)((acc[i] + 8) >> 4);
	}
}
