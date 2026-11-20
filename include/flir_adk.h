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
	FLIR_MODE_NV12,   /* on-camera AGC, NV12 (Y plane + interleaved CbCr) */
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
 *
 * video_dev may be NULL for CCI-only access: the V4L2 node is not touched
 * (so this works while another process streams), streaming/grab calls fail,
 * and flir_open() returns NULL if cci_dev is NULL or cannot be opened.
 */
struct flir_ctx *flir_open(const char *video_dev, const char *cci_dev,
			   enum flir_video_mode mode);
/*
 * flir_open() plus a frame rate: fps > 0 asks the driver for 1/fps s per
 * frame (VIDIOC_S_PARM; the Boson USB interface offers 60 and 30), fps <= 0
 * keeps the camera default. Ignored when video_dev is NULL. Returns NULL if
 * VIDIOC_S_PARM fails; prints a warning if the driver picks another rate.
 */
struct flir_ctx *flir_open_ex(const char *video_dev, const char *cci_dev,
			      enum flir_video_mode mode, int fps);
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
 * Function IDs from the Boson Software IDD (102-2013-44), verified on this
 * camera (fw 2.0.15223). IDs marked Boson+ come from the Boson+ SDK IDD
 * Rev 410 and are rejected (status 0x161) by plain Boson firmware.
 * FSLP: start(0x8E), channel, seq(4), cmd(4), status(4), payload, CRC16, end(0xAE)
 */

/* Calibration */
int flir_cci_ffc_trigger(struct flir_ctx *ctx);
int flir_cci_get_fpa_temp(struct flir_ctx *ctx, float *celsius);
int flir_cci_set_gain_mode(struct flir_ctx *ctx, int mode);

/* Image processing */
/* Boson+ agcSetMode (0x00090033): plain Boson answers 0x161. The AGC
 * parameters ("agc-*" in flir_params[]) work on every Boson. */
int flir_cci_set_agc_mode(struct flir_ctx *ctx, int agc_type);
int flir_cci_set_color_lut(struct flir_ctx *ctx, int lut_id);
/* scalerSetZoom 0x000D0002 takes FLR_SCALER_ZOOM_PARAMS_T: zoom, xCenter,
 * yCenter (3 x UINT_32). flir_cci_set_zoom() zooms about the centre (320,256). */
int flir_cci_set_zoom(struct flir_ctx *ctx, int factor);
int flir_cci_set_zoom_params(struct flir_ctx *ctx, uint32_t zoom,
			     uint32_t x_center, uint32_t y_center);
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

/*
 * Default two-point calibration (this unit, high gain, measured 2026-11-18):
 * ceiling @ 21°C = 22383 counts, skin @ ~34°C = 23313 counts, FPA 37.9°C.
 *   temp_C = fpa_C + (raw - FLIR_CAL_RAW_AT_FPA) / FLIR_CAL_COUNTS_PER_DEG
 * Assumes the post-FFC offset tracks FPA temp. Re-measure per unit / gain mode.
 */
#define FLIR_CAL_COUNTS_PER_DEG 71.5f
#define FLIR_CAL_RAW_AT_FPA     23592

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
	float    counts_per_deg;    /* raw counts per °C (FLIR_CAL_COUNTS_PER_DEG) */
	float    ref_temp_c;        /* reference temp in °C (e.g. FPA temp) */
	uint16_t ref_raw;           /* raw count at ref_temp_c (FLIR_CAL_RAW_AT_FPA) */
	const uint8_t (*palette)[3];  /* color bar LUT, 256 entries; NULL = iron-bow */
	const uint16_t *gray_to_raw;  /* 256 entries from flir_gray_to_raw(); NULL = linear min..max labels */
};

/*
 * Map each gray level of an AGC'd frame back to the raw count it represents,
 * so color bar labels stay correct for non-linear AGC (HEQ). Endpoints are the
 * AGC clip boundaries, so outlier pixels don't stretch the labels.
 */
void flir_gray_to_raw(const uint16_t *raw, const uint8_t *gray, int n,
		      uint16_t out[256]);

/*
 * Motion-adaptive temporal denoise on RAW16. Static pixels are averaged over
 * frames (measured: static-area noise 3.3 -> 1.8 counts); a pixel that changes by more than `thresh` counts
 * jumps straight to the new value, so movement doesn't smear.
 * acc: caller-owned n-entry buffer. reset=1 on the first frame.
 */
void flir_denoise(const uint16_t *in, uint16_t *out, int32_t *acc, int n,
		  int thresh, int reset);

int flir_overlay(const uint8_t *src_rgb, const uint16_t *raw,
		 int w, int h, const struct flir_overlay_opts *opts,
		 uint8_t *dst_rgb);

/*
 * Scalar settings and readouts (src/cci_params.c). One table drives the C
 * API, flir_ctl (`flir_ctl NAME` reads, `flir_ctl NAME VALUE` writes),
 * `flir_ctl status` and the web panel. Each entry is one CCI Get/Set pair.
 * type = wire type of the value, big-endian:
 *   'e' INT_32 (enum, FLR_ENABLE_E)   'u' UINT_32
 *   'h' INT_16                        'w' UINT_16
 *   'f' FLOAT (IEEE-754 single)
 */
struct flir_param_info {
	const char *name;     /* flir_ctl command, e.g. "agc-gamma" */
	uint32_t    get_id;   /* Get function ID (every entry has one) */
	uint32_t    set_id;   /* Set function ID, 0 = read-only */
	uint32_t    post_id;  /* sent without payload after a successful Set, 0 = none */
	char        type;     /* 'e' 'u' 'h' 'w' 'f' */
	double      lo, hi;   /* accepted Set range, inclusive */
	const char *help;     /* one line for usage text / UI */
};

/* Terminated by an entry whose name is NULL. */
extern const struct flir_param_info flir_params[];

/* NULL if unknown. */
const struct flir_param_info *flir_param_find(const char *name);

/*
 * 0 on success, -1 on failure. flir_cci_param_set() returns -1 without any
 * serial I/O when p is NULL or read-only, v is NaN or outside [lo, hi], or
 * v is not a whole number for a non-'f' type.
 */
int flir_cci_param_get(struct flir_ctx *ctx, const struct flir_param_info *p,
		       double *v);
int flir_cci_param_set(struct flir_ctx *ctx, const struct flir_param_info *p,
		       double v);

/* Identification (src/cci_system.c) */
int flir_cci_get_software_rev(struct flir_ctx *ctx, uint32_t *major,
			      uint32_t *minor, uint32_t *patch);
int flir_cci_get_sensor_pn(struct flir_ctx *ctx, char *pn, size_t pn_sz);

/*
 * Persistence — tools must require explicit confirmation before calling.
 * save: bosonWriteDynamicHeaderToFlash, current settings become power-up
 * defaults (writes flash). factory: bosonRestoreFactoryDefaultsFromFlash,
 * loads factory settings into the running camera (call save to keep them).
 * reboot: bosonReboot; the camera may reset before replying (-1, status
 * 0xFFFFFFFF).
 */
int flir_cci_save_settings(struct flir_ctx *ctx);
int flir_cci_restore_factory_defaults(struct flir_ctx *ctx);
int flir_cci_reboot(struct flir_ctx *ctx);

/* Regions of interest, FLR_ROI_T: roi[4] = row_start, row_end, col_start, col_end */
int flir_cci_agc_set_roi(struct flir_ctx *ctx, uint16_t row_start, uint16_t row_end,
			 uint16_t col_start, uint16_t col_end);
int flir_cci_agc_get_roi(struct flir_ctx *ctx, uint16_t roi[4]);
int flir_cci_stats_set_roi(struct flir_ctx *ctx, uint16_t row_start, uint16_t row_end,
			   uint16_t col_start, uint16_t col_end);
int flir_cci_stats_get_roi(struct flir_ctx *ctx, uint16_t roi[4]);

/* Splash screen: duration / background only (no image upload) */
int flir_cci_splash_get_duration(struct flir_ctx *ctx, uint32_t screen, uint32_t *ms);
int flir_cci_splash_set_duration(struct flir_ctx *ctx, uint32_t screen, uint32_t ms);
int flir_cci_splash_get_background(struct flir_ctx *ctx, uint32_t screen, uint32_t *rgb);
int flir_cci_splash_set_background(struct flir_ctx *ctx, uint32_t screen, uint32_t rgb);

/*
 * fileOps — READ-ONLY subset on purpose. Write / remove / rename / erase are
 * not wrapped: a bad write to the camera's flash file system can brick it.
 * fileops_dir: next entry of the camera's current directory, "" at the end.
 */
int flir_cci_fileops_dir(struct flir_ctx *ctx, char *name, size_t name_sz);
int flir_cci_fileops_file_size(struct flir_ctx *ctx, const char *path, uint32_t *size);

/*
 * On-camera symbology (src/cci_symbology.c): vector objects drawn into the
 * camera's 8-bit video (not RAW16). Enable with param "sym". id 0..255;
 * argb = 0xAARRGGBB, A = 0 opaque .. 127 transparent.
 * align (FLR_SYMBOLOGY_TEXT_ALIGNMENT_E): 17/18/19 left/center/right top,
 * 33/34/35 middle, 49/50/51 bottom. text: at most 127 bytes.
 */
int flir_cci_sym_create_text(struct flir_ctx *ctx, uint8_t id, int16_t x, int16_t y,
			     int16_t w, int16_t h, int16_t size, int16_t align,
			     uint32_t argb, const char *text);
int flir_cci_sym_create_rect(struct flir_ctx *ctx, uint8_t id, int16_t x, int16_t y,
			     int16_t w, int16_t h, uint32_t argb, int filled);
int flir_cci_sym_create_line(struct flir_ctx *ctx, uint8_t id, int16_t x1, int16_t y1,
			     int16_t x2, int16_t y2, uint32_t argb);
int flir_cci_sym_show(struct flir_ctx *ctx, uint8_t id, int visible);
int flir_cci_sym_delete(struct flir_ctx *ctx, uint8_t id);
int flir_cci_sym_move_by(struct flir_ctx *ctx, uint8_t id, int16_t dx, int16_t dy);

/*
 * Status of the most recent flir_cci_raw() call (every flir_cci_* wrapper
 * goes through it). 0 = OK; 0xFFFFFFFF = no valid response (device not
 * open, write/read error, timeout, bad CRC); any other value is the camera's
 * FLR_RESULT code, e.g. 0x00000161 = module not available on this variant.
 * Process-global, not thread-safe.
 */
uint32_t flir_cci_last_status(void);

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
