/*
 * flir_calibrate — full test/calibration tool
 *
 * Captures RAW16 frames, runs HEQ AGC + colorizer, writes PGM/PPM,
 * exercises CCI commands, and reports calibration stats.
 *
 * Usage: flir_calibrate [-d /dev/video5] [-c /dev/ttyACM0] [-n 10] [-o output_dir] [-u C|F|K]
 *
 * Output: PGM (grayscale) and PPM (iron-bow) files viewable with any image viewer.
 * Convert to PNG: convert output.ppm output.png (ImageMagick)
 */

#define _GNU_SOURCE
#include "flir_adk.h"
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Declared in accel_generic.c */
extern const struct flir_accel flir_accel_generic;

#define W 640
#define H 512

static void write_pgm(const char *path, const uint8_t *gray, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	fprintf(f, "P5\n%d %d\n255\n", w, h);
	fwrite(gray, 1, w * h, f);
	fclose(f);
}

static void write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb, 1, w * h * 3, f);
	fclose(f);
}

static void write_raw16_pgm(const char *path, const uint16_t *raw, int w, int h,
			    uint16_t lo, uint16_t hi)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	int range = hi > lo ? hi - lo : 1;
	fprintf(f, "P5\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		int v = (int)(raw[i] - lo) * 255 / range;
		if (v < 0) v = 0;
		if (v > 255) v = 255;
		uint8_t b = (uint8_t)v;
		fwrite(&b, 1, 1, f);
	}
	fclose(f);
}

static void compute_stats(const uint16_t *px, int n,
			   uint16_t *out_lo, uint16_t *out_hi,
			   double *out_mean, double *out_std)
{
	uint16_t lo = 0xFFFF, hi = 0;
	double sum = 0;
	for (int i = 0; i < n; i++) {
		if (px[i] < lo) lo = px[i];
		if (px[i] > hi) hi = px[i];
		sum += px[i];
	}
	double mean = sum / n;
	double var = 0;
	for (int i = 0; i < n; i++) {
		double d = px[i] - mean;
		var += d * d;
	}
	*out_lo = lo; *out_hi = hi;
	*out_mean = mean; *out_std = sqrt(var / n);
}

static void temporal_noise(const uint16_t *f0, const uint16_t *f1, int n,
			   double *out_mean, double *out_std, int *out_max)
{
	double sum = 0;
	int mx = 0;
	for (int i = 0; i < n; i++) {
		int d = abs((int)f1[i] - (int)f0[i]);
		sum += d;
		if (d > mx) mx = d;
	}
	double mean = sum / n;
	double var = 0;
	for (int i = 0; i < n; i++) {
		double d = abs((int)f1[i] - (int)f0[i]) - mean;
		var += d * d;
	}
	*out_mean = mean; *out_std = sqrt(var / n); *out_max = mx;
}

static int16_t fpa_temp_cx10;

static void cci_report(struct flir_ctx *ctx)
{
	printf("\n--- CCI Report ---\n");

	uint32_t sn;
	if (flir_cci_get_serial_number(ctx, &sn) == 0)
		printf("  Camera S/N: %u (0x%08X)\n", sn, sn);
	else
		printf("  Camera S/N: [read failed]\n");

	if (flir_cci_get_sensor_sn(ctx, &sn) == 0)
		printf("  Sensor S/N: %u (0x%08X)\n", sn, sn);

	char pn[24] = {0};
	if (flir_cci_get_part_number(ctx, pn, sizeof(pn) - 1) == 0)
		printf("  Part number: %s\n", pn);

	float temp;
	if (flir_cci_get_fpa_temp(ctx, &temp) == 0) {
		printf("  FPA temperature: %.1f C\n", temp);
		fpa_temp_cx10 = (int16_t)(temp * 10);
	} else
		printf("  FPA temperature: [read failed]\n");

	/* Try hardware spot meter (radiometric cameras only) */
	int hw_spot = 0;
	if (flir_cci_radiometry_enable(ctx, 1) == 0 &&
	    flir_cci_spotmeter_enable(ctx, 1) == 0) {
		flir_cci_spotmeter_set_statsmode(ctx, 0);
		flir_cci_spotmeter_set_roi(ctx, 206, 306, 220, 420);
		usleep(500000);
		float sm_mean, sm_min, sm_max;
		if (flir_cci_spotmeter_get_temp(ctx, &sm_mean, &sm_min, &sm_max) == 0) {
			printf("  HW spot meter: mean=%.1fC min=%.1fC max=%.1fC\n",
			       sm_mean, sm_min, sm_max);
			hw_spot = 1;
		}
	}
	if (!hw_spot)
		printf("  HW spot meter: not available (non-radiometric camera)\n"
		       "  Software spot meter will run on captured frames.\n");

	printf("  Triggering FFC (flat-field correction)...\n");
	if (flir_cci_ffc_trigger(ctx) == 0)
		printf("  FFC: triggered OK\n");
	else
		printf("  FFC: [trigger failed]\n");

	usleep(1500000); /* FFC shutter cycle ~1.5s */

	if (flir_cci_get_fpa_temp(ctx, &temp) == 0)
		printf("  FPA temperature (post-FFC): %.1f C\n", temp);
}

int main(int argc, char **argv)
{
	const char *vdev = "/dev/video5";
	const char *cdev = NULL;
	const char *outdir = "output";
	int nframes = 10;
	enum flir_temp_unit temp_unit = FLIR_UNIT_RAW;
	int opt;

	while ((opt = getopt(argc, argv, "d:c:o:n:u:h")) != -1) {
		switch (opt) {
		case 'd': vdev = optarg; break;
		case 'c': cdev = optarg; break;
		case 'o': outdir = optarg; break;
		case 'n': nframes = atoi(optarg); break;
		case 'u':
			if (optarg[0] == 'C' || optarg[0] == 'c') temp_unit = FLIR_UNIT_C;
			else if (optarg[0] == 'F' || optarg[0] == 'f') temp_unit = FLIR_UNIT_F;
			else if (optarg[0] == 'K' || optarg[0] == 'k') temp_unit = FLIR_UNIT_K;
			else { fprintf(stderr, "Unit must be C, F, or K\n"); return 1; }
			break;
		default:
			fprintf(stderr, "Usage: %s [-d dev] [-c cci] [-o dir] [-n frames] [-u C|F|K]\n",
				argv[0]);
			return 1;
		}
	}

	mkdir(outdir, 0755);

	/* Open in RAW16 mode for full radiometric data */
	struct flir_ctx *ctx = flir_open(vdev, cdev, FLIR_MODE_RAW16);
	if (!ctx) {
		fprintf(stderr, "Failed to open %s\n", vdev);
		return 1;
	}
	flir_set_accel(ctx, &flir_accel_generic);

	/* CCI report */
	if (cdev)
		cci_report(ctx);

	/* Capture RAW16 frames */
	printf("\n--- Capturing %d RAW16 frames ---\n", nframes);
	if (flir_stream_start(ctx)) {
		fprintf(stderr, "Stream start failed\n");
		flir_close(ctx);
		return 1;
	}

	size_t expected = (size_t)W * H * 2;
	uint8_t *gray_buf = malloc(W * H);
	uint8_t *rgb_buf = malloc(W * H * 3);
	int bar_w = W + FLIR_COLORBAR_WIDTH;
	uint8_t *bar_buf = malloc(bar_w * H * 3);
	uint16_t *prev_frame = malloc(W * H * 2);
	int got = 0;
	double noise_sum = 0;
	int noise_count = 0;

	struct flir_frame frame;
	for (int attempt = 0; got < nframes && attempt < nframes + 4; attempt++) {
		if (flir_grab(ctx, &frame, 5000)) {
			fprintf(stderr, "Grab timeout at frame %d\n", got);
			break;
		}
		if (frame.len != expected) {
			fprintf(stderr, "Skip frame %u: %zu bytes (expected %zu)\n",
				frame.seq, frame.len, expected);
			continue;
		}

		const uint16_t *raw = (const uint16_t *)frame.data;

		/* Stats */
		uint16_t lo, hi;
		double mean, std;
		compute_stats(raw, W * H, &lo, &hi, &mean, &std);
		printf("  Frame %d: range=[%u-%u] mean=%.1f std=%.2f",
		       got, lo, hi, mean, std);

		/* Temporal noise (vs previous frame) */
		if (got > 0) {
			double tn_mean, tn_std;
			int tn_max;
			temporal_noise(prev_frame, raw, W * H,
				       &tn_mean, &tn_std, &tn_max);
			printf("  noise: mean=%.2f std=%.2f max=%d", tn_mean, tn_std, tn_max);
			noise_sum += tn_std;
			noise_count++;
		}
		printf("\n");

		/* Software spot meter on center ROI */
		struct flir_spot_result spot;
		if (flir_sw_spotmeter(raw, W, H, 206, 306, 220, 420, &spot) == 0) {
			printf("    spot(center): mean=%.0f std=%.1f range=[%u-%u] "
			       "cold@(%u,%u) hot@(%u,%u)\n",
			       spot.mean, spot.std, spot.min, spot.max,
			       spot.min_row, spot.min_col,
			       spot.max_row, spot.max_col);
		}

		memcpy(prev_frame, raw, W * H * 2);

		/* Run HEQ AGC */
		flir_accel_generic.agc_process(raw, gray_buf, W, H, NULL);

		/* Write outputs */
		char path[256];
		snprintf(path, sizeof(path), "%s/raw16_%03d.pgm", outdir, got);
		write_raw16_pgm(path, raw, W, H, lo, hi);

		snprintf(path, sizeof(path), "%s/heq_%03d.pgm", outdir, got);
		write_pgm(path, gray_buf, W, H);

		/* Run colorizer + full overlay */
		flir_accel_generic.colorize(gray_buf, rgb_buf, W, H, 0, NULL);
		struct flir_overlay_opts ov = {
			.raw_min = lo, .raw_max = hi,
			.fpa_temp_cx10 = fpa_temp_cx10,
			.spot_enabled = 1,
			.spot_row_start = 206, .spot_row_end = 306,
			.spot_col_start = 220, .spot_col_end = 420,
			.spot_mean = (uint16_t)spot.mean,
			.temp_unit = temp_unit,
			.counts_per_deg = 40.0f,
			.ref_temp_c = fpa_temp_cx10 / 10.0f,
			.ref_raw = (uint16_t)mean,
		};
		flir_overlay(rgb_buf, raw, W, H, &ov, bar_buf);
		snprintf(path, sizeof(path), "%s/iron_%03d.ppm", outdir, got);
		write_ppm(path, bar_buf, bar_w, H);

		got++;
	}

	flir_stream_stop(ctx);

	/* Summary */
	printf("\n--- Calibration Summary ---\n");
	printf("  Frames captured: %d\n", got);
	if (noise_count > 0)
		printf("  Average temporal noise: %.2f counts\n", noise_sum / noise_count);
	printf("  Output: %s/ (raw16_*.pgm, heq_*.pgm, iron_*.ppm)\n", outdir);
	printf("  Convert: ffmpeg -i %s/iron_000.ppm %s/iron_000.png\n", outdir, outdir);

	free(gray_buf);
	free(rgb_buf);
	free(bar_buf);
	free(prev_frame);
	flir_close(ctx);
	return 0;
}
