/*
 * flir_adk.h — FLIR ADK 75° (Boson 640) portable driver
 *
 * V4L2 capture + CCI serial control.
 * 640x512 LWIR, 8-14µm, 12µm VOx microbolometer.
 */

#ifndef FLIR_ADK_H
#define FLIR_ADK_H

#include <stdint.h>
#include <stddef.h>

/* Video modes —— the camera presents as a UVC device */
enum flir_video_mode {
	FLIR_MODE_AGC8,   /* on-camera AGC, YUV/I420, 8-bit, display-ready */
	FLIR_MODE_RAW16,  /* raw radiometric, Y16/GRAY16_LE, 14-bit in 16 */
};

/* AGC mode (Boson SDK: FLR_AGC_MODE_E) */
enum flir_agc_mode {
	FLIR_AGC_NORMAL      = 0,
	FLIR_AGC_HOLD        = 1,
	FLIR_AGC_THRESHOLD   = 2,
	FLIR_AGC_AUTO_BRIGHT = 3,
	FLIR_AGC_AUTO_LINEAR = 4,
	FLIR_AGC_MANUAL      = 5,
};

/* Color LUT (Boson SDK: FLR_COLORLUT_ID_E) */
enum flir_color_lut {
	FLIR_LUT_WHITEHOT    = 0,
	FLIR_LUT_BLACKHOT    = 1,
	FLIR_LUT_RAINBOW     = 2,
	FLIR_LUT_RAINBOW_HC  = 3,
	FLIR_LUT_IRONBOW     = 4,
	FLIR_LUT_LAVA        = 5,
	FLIR_LUT_ARCTIC      = 6,
	FLIR_LUT_GLOBOW      = 7,
	FLIR_LUT_GRADEDFIRE  = 8,
	FLIR_LUT_HOTTEST     = 9,
};

/* Gain mode (Boson SDK: FLR_BOSON_GAINMODE_E) */
enum flir_gain_mode {
	FLIR_GAIN_HIGH   = 0,
	FLIR_GAIN_LOW    = 1,
	FLIR_GAIN_AUTO   = 2,
	FLIR_GAIN_DUAL   = 3,
	FLIR_GAIN_MANUAL = 4,
};

/* Frame handed to the caller — zero-copy from mmap'd V4L2 buffer */
struct flir_frame {
	const void *data;
	size_t      len;
	uint32_t    width;      /* 640 */
	uint32_t    height;     /* 512 */
	uint32_t    seq;        /* V4L2 sequence number */
	uint64_t    timestamp;  /* µs, CLOCK_MONOTONIC */
};

/* Camera handle (opaque) */
struct flir_ctx;

/*
 * Lifecycle
 */
struct flir_ctx *flir_open(const char *video_dev, const char *cci_dev,
			   enum flir_video_mode mode);
void             flir_close(struct flir_ctx *ctx);

/*
 * Streaming — start/stop V4L2 MMAP streaming
 */
int flir_stream_start(struct flir_ctx *ctx);
int flir_stream_stop(struct flir_ctx *ctx);

/*
 * Grab one frame (blocks until ready, or timeout_ms elapsed).
 * Returns 0 on success, -1 on error/timeout.
 * The frame data is valid until the next flir_grab() call.
 */
int flir_grab(struct flir_ctx *ctx, struct flir_frame *out, int timeout_ms);

/*
 * CCI — camera control via FSLP serial protocol (921600 8N1)
 *
 * Function IDs from Boson+ SDK IDD Rev 410.
 * FSLP: start(0x8E), channel, seq(4), cmd(4), status(4), payload, CRC16, end(0xAE)
 */

/* Calibration */
int flir_cci_ffc_trigger(struct flir_ctx *ctx);
int flir_cci_get_fpa_temp(struct flir_ctx *ctx, float *celsius);
int flir_cci_set_gain_mode(struct flir_ctx *ctx, int mode);

/* Image processing */
int flir_cci_set_agc_mode(struct flir_ctx *ctx, int agc_type);
int flir_cci_set_color_lut(struct flir_ctx *ctx, int lut_id);
int flir_cci_set_zoom(struct flir_ctx *ctx, int factor);
int flir_cci_tlinear_enable(struct flir_ctx *ctx, int enable);

/* Telemetry */
int flir_cci_enable_telemetry(struct flir_ctx *ctx, int enable);

/* Identification */
int flir_cci_get_serial_number(struct flir_ctx *ctx, uint32_t *sn);
int flir_cci_get_sensor_sn(struct flir_ctx *ctx, uint32_t *sn);
int flir_cci_get_part_number(struct flir_ctx *ctx, char *pn, size_t pn_sz);

/* Radiometry */
int flir_cci_radiometry_enable(struct flir_ctx *ctx, int enable);

/* Spot meter */
int flir_cci_spotmeter_enable(struct flir_ctx *ctx, int enable);
int flir_cci_spotmeter_set_roi(struct flir_ctx *ctx,
			       uint16_t row_start, uint16_t row_end,
			       uint16_t col_start, uint16_t col_end);
int flir_cci_spotmeter_set_statsmode(struct flir_ctx *ctx, int mode);
int flir_cci_spotmeter_get_stats(struct flir_ctx *ctx,
				 uint16_t *mean, uint16_t *min, uint16_t *max);
int flir_cci_spotmeter_get_temp(struct flir_ctx *ctx,
				float *mean, float *min, float *max);

/*
 * Software spot meter — computes ROI stats from RAW16 frame data.
 * Works on any camera variant including non-radiometric.
 * Values are in raw counts (proportional to scene radiance).
 */
struct flir_spot_result {
	double mean;
	double std;
	uint16_t min;
	uint16_t max;
	uint16_t min_row, min_col;
	uint16_t max_row, max_col;
};

int flir_sw_spotmeter(const uint16_t *raw, int w, int h,
		      uint16_t row_start, uint16_t row_end,
		      uint16_t col_start, uint16_t col_end,
		      struct flir_spot_result *out);

/*
 * Thermal image overlay — color bar, crosshair, spot meter, FPA temp.
 * dst_rgb must be (w + FLIR_COLORBAR_WIDTH) * h * 3 bytes.
 */
#define FLIR_COLORBAR_WIDTH 100

enum flir_temp_unit {
	FLIR_UNIT_RAW = 0,  /* raw counts (default) */
	FLIR_UNIT_C   = 1,  /* Celsius */
	FLIR_UNIT_F   = 2,  /* Fahrenheit */
	FLIR_UNIT_K   = 3,  /* Kelvin */
};

int flir_colorbar(const uint8_t *src_rgb, int w, int h,
		  uint16_t raw_min, uint16_t raw_max,
		  uint8_t *dst_rgb);

struct flir_overlay_opts {
	uint16_t raw_min, raw_max;
	int16_t  fpa_temp_cx10;     /* FPA temp in °C×10, 0 = don't show */
	int      spot_enabled;
	uint16_t spot_row_start, spot_row_end;
	uint16_t spot_col_start, spot_col_end;
	uint16_t spot_mean;
	/* Temperature display: set temp_unit != RAW + provide calibration */
	enum flir_temp_unit temp_unit;
	float    counts_per_deg;    /* raw counts per °C (e.g. 40.0) */
	float    ref_temp_c;        /* reference temp in °C (e.g. FPA temp) */
	uint16_t ref_raw;           /* raw count at ref_temp_c */
};

int flir_overlay(const uint8_t *src_rgb, const uint16_t *raw,
		 int w, int h, const struct flir_overlay_opts *opts,
		 uint8_t *dst_rgb);

/* Raw CCI command for anything not wrapped above */
int flir_cci_raw(struct flir_ctx *ctx, uint32_t cmd_id,
		 const void *payload, size_t payload_len,
		 void *resp, size_t *resp_len);

/*
 * Platform accelerator — optional, set before stream_start.
 * See platform/ for per-SoC implementations.
 */
struct flir_accel {
	const char *name;
	int (*agc_process)(const uint16_t *raw, uint8_t *out,
			   int w, int h, void *priv);
	int (*colorize)(const uint8_t *gray, uint8_t *rgb,
			int w, int h, int lut, void *priv);
	void *priv;
};

int flir_set_accel(struct flir_ctx *ctx, const struct flir_accel *accel);

#endif /* FLIR_ADK_H */
