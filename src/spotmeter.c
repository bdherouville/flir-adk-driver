#include "flir_adk.h"
#include <math.h>

int flir_sw_spotmeter(const uint16_t *raw, int w, int h,
		      uint16_t row_start, uint16_t row_end,
		      uint16_t col_start, uint16_t col_end,
		      struct flir_spot_result *out)
{
	if (!raw || !out) return -1;
	if (row_end > h) row_end = h;
	if (col_end > w) col_end = w;
	if (row_start >= row_end || col_start >= col_end) return -1;

	uint16_t lo = 0xFFFF, hi = 0;
	uint16_t lo_r = 0, lo_c = 0, hi_r = 0, hi_c = 0;
	double sum = 0;
	int n = 0;

	for (uint16_t r = row_start; r < row_end; r++) {
		for (uint16_t c = col_start; c < col_end; c++) {
			uint16_t v = raw[r * w + c];
			sum += v;
			if (v < lo) { lo = v; lo_r = r; lo_c = c; }
			if (v > hi) { hi = v; hi_r = r; hi_c = c; }
			n++;
		}
	}

	double mean = sum / n;
	double var = 0;
	for (uint16_t r = row_start; r < row_end; r++)
		for (uint16_t c = col_start; c < col_end; c++) {
			double d = raw[r * w + c] - mean;
			var += d * d;
		}

	out->mean = mean;
	out->std = sqrt(var / n);
	out->min = lo;  out->min_row = lo_r; out->min_col = lo_c;
	out->max = hi;  out->max_row = hi_r; out->max_col = hi_c;
	return 0;
}
