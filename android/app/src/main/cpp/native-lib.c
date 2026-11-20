/*
 * native-lib.c — Boson RAW16 over libusb (UVC bulk, done like Linux uvcvideo)
 *                -> flir_loop pipeline -> RGBA 740x512
 *
 * Mirrors tools/flir_loop.c: freeze/FFC hold, denoise (thresh 20), HEQ AGC,
 * palette (iron-bow via accel colorize, white/black-hot, rainbow, lava, arctic, hot spot via tables),
 * flir_gray_to_raw labels, overlay with FPA-referenced temperatures.
 * Output goes to an ANativeWindow (SurfaceView) and is kept for snapshots.
 */
#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <libusb.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "flir_adk.h"

#define TAG "BosonNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define W 640
#define H 512
#define OW (W + FLIR_COLORBAR_WIDTH)
/* Measured on this unit (android/README.md, USB layout) */
#define BOSON_IF_VIDEO 1        /* UVC streaming interface */
#define BOSON_EP_VIDEO 0x81     /* its bulk IN endpoint */
#define BOSON_FMT_Y16 2         /* bFormatIndex: 1 I420, 2 Y16, 3 NV12, 4 NV21 */
#define BOSON_INTERVAL 166666   /* 100 ns units: 60 fps, Y16's default interval */
#define VS_PROBE_CONTROL 0x01   /* UVC VideoStreaming control selectors */
#define VS_COMMIT_CONTROL 0x02
#define UVC_SET_CUR 0x01
#define UVC_GET_CUR 0x81
/* Like uvcvideo: 5 bulk URBs of 32 x 512 B in flight. libuvc queued 8 x 656 KB, which the
 * Lenovo TB-X6C6F (MediaTek MUSB) answered with kernel memory corruption and panics. */
#define NXFER 5
#define XFER_SIZE (32 * 512)
#define FRAME_BYTES (W * H * 2)

extern const struct flir_accel flir_accel_generic;

static libusb_context *g_usb;
static libusb_device_handle *g_uh;
static pthread_t g_event_thread, g_worker;
static atomic_int g_run_events, g_run_worker;
static int g_threads;

/* USB streaming state: only touched by the libusb event thread */
static struct libusb_transfer *g_xfer[NXFER];
static atomic_int g_in_flight, g_streaming;
static uint32_t g_max_payload;           /* negotiated dwMaxPayloadTransferSize */
static uint32_t pay_bytes, got;          /* bytes in the current payload / frame */
static int at_payload_start = 1, fid = -1, eof_pending;
static uint8_t asm_buf[FRAME_BYTES];     /* frame being assembled */

/* hand-off to the worker: one ready frame, newest wins */
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_frame_cond = PTHREAD_COND_INITIALIZER;
static uint8_t ready_buf[FRAME_BYTES];
static int ready;

static atomic_int g_lut = FLIR_LUT_IRONBOW;
static atomic_int g_unit = FLIR_UNIT_RAW;
static atomic_int g_spot = 1;
static atomic_int g_nr = 1;
static atomic_int g_minmax = 0;    /* hot/cold markers on the image */
static atomic_int g_fpa_cx10 = 0;
/* spot crosshair in sensor pixels (tap to move); reading = 5x5 mean around it */
static atomic_int g_spot_x = W / 2, g_spot_y = H / 2;
static atomic_int g_spot_raw;      /* last 5x5 raw mean at the spot, for nativeCalibrate */
static atomic_int g_cal_cx100;     /* user calibration offset, 0.01 degC, added to every reading */

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER; /* g_win, g_rgba, g_have_out */
static ANativeWindow *g_win;
static uint8_t g_rgba[OW * H * 4];
static int g_have_out;

/* pipeline state (only touched by the worker thread) */
static uint16_t prev[W * H], clean[W * H], g2r[256];
static int32_t nr_acc[W * H];
static int have_prev, frozen, nr_was, settle;
/* frames still processed but not shown after an FFC, while the denoise and
 * HEQ history re-converge on the new offsets; this hides the post-FFC flash */
#define SETTLE_FRAMES 8
static uint16_t work_buf[W * H];
static uint8_t gray[W * H], rgb[W * H * 3], out_rgb[OW * H * 3], out_rgba[OW * H * 4];
static uint8_t grey_lut[256][3], inv_lut[256][3];
/* extra palettes, built once by the worker (build_palettes) */
static uint8_t rainbow_lut[256][3], lava_lut[256][3], arctic_lut[256][3], hotspot_lut[256][3];
static pthread_once_t g_pal_once = PTHREAD_ONCE_INIT;

/* piecewise-linear gradient through n evenly spaced RGB stops */
static void gradient(uint8_t lut[256][3], const uint8_t (*stops)[3], int n)
{
	for (int i = 0; i < 256; i++) {
		int seg = i * (n - 1) / 256, t0 = seg * 255 / (n - 1), t1 = (seg + 1) * 255 / (n - 1);
		for (int c = 0; c < 3; c++)
			lut[i][c] = (uint8_t)(stops[seg][c] + (stops[seg + 1][c] - stops[seg][c]) * (i - t0) / (t1 - t0));
	}
}

static void build_palettes(void)
{
	static const uint8_t rainbow[][3] = { {0,0,128}, {0,0,255}, {0,255,255}, {0,255,0}, {255,255,0}, {255,0,0} };
	static const uint8_t lava[][3]    = { {0,0,0}, {80,0,90}, {200,0,0}, {255,120,0}, {255,230,0}, {255,255,255} };
	static const uint8_t arctic[][3]  = { {0,0,40}, {0,40,160}, {40,140,230}, {180,230,255}, {255,200,60}, {255,255,220} };
	gradient(rainbow_lut, rainbow, 6);
	gradient(lava_lut, lava, 6);
	gradient(arctic_lut, arctic, 6);
	for (int i = 0; i < 256; i++) { /* grey, top ~10 % in red */
		int hot = i >= 230;
		hotspot_lut[i][0] = hot ? (uint8_t)(200 + (i - 230) * 55 / 25) : (uint8_t)i;
		hotspot_lut[i][1] = hotspot_lut[i][2] = hot ? 0 : (uint8_t)i;
	}
}

/* caller holds g_out_lock */
static void post_locked(void)
{
	ANativeWindow_Buffer b;
	if (!g_win || !g_have_out || ANativeWindow_lock(g_win, &b, NULL) != 0)
		return;
	if (b.width == OW && b.height == H)
		for (int y = 0; y < H; y++)
			memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4,
			       g_rgba + (size_t)y * OW * 4, OW * 4);
	ANativeWindow_unlockAndPost(g_win);
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* logcat: fps and pipeline cost, every 600 frames (~10 s at 60 fps) */
static void stats(double t0)
{
	static double win0, busy;
	static int n;
	double t1 = now_ms();
	if (!n) win0 = t0;
	busy += t1 - t0;
	if (++n == 600) {
		LOGI("%.1f fps, pipeline %.1f ms/frame", 600e3 / (t1 - win0), busy / 600);
		n = 0;
		busy = 0;
	}
}

static void process_frame(const uint16_t *cam)
{
	double t0 = now_ms();

	/* FFC / freeze: identical interior rows -> hold last output */
	if (have_prev && !memcmp(cam + W, prev + W, (size_t)W * (H - 2) * 2)) {
		if (frozen++ == 0)
			LOGI("input frozen (FFC or freeze): holding output");
		pthread_mutex_lock(&g_out_lock);
		post_locked();
		pthread_mutex_unlock(&g_out_lock);
		return;
	}
	if (frozen) {
		LOGI("input resumed after %d frozen frames", frozen);
		frozen = 0;
		nr_was = 0;
		settle = SETTLE_FRAMES;
	}
	memcpy(prev, cam, sizeof(prev));
	have_prev = 1;

	int nr = atomic_load(&g_nr), lut = atomic_load(&g_lut);
	int spot_on = atomic_load(&g_spot), unit = atomic_load(&g_unit);
	int fpa = atomic_load(&g_fpa_cx10);

	const uint16_t *raw = cam;
	if (nr) {
		flir_denoise(raw, clean, nr_acc, W * H, 20, !nr_was);
		raw = clean;
	}
	nr_was = nr;

	{
		int sx = atomic_load(&g_spot_x), sy = atomic_load(&g_spot_y), sum = 0, n = 0;
		for (int y = sy - 2; y <= sy + 2; y++)
			for (int x = sx - 2; x <= sx + 2; x++)
				if (x >= 0 && x < W && y >= 0 && y < H) { sum += raw[y * W + x]; n++; }
		atomic_store(&g_spot_raw, n ? sum / n : 0);
	}

	uint16_t lo = 0xFFFF, hi = 0;
	int lo_i = 0, hi_i = 0;
	for (int i = 0; i < W * H; i++) {
		if (raw[i] < lo) { lo = raw[i]; lo_i = i; }
		if (raw[i] > hi) { hi = raw[i]; hi_i = i; }
	}
	flir_accel_generic.agc_process(raw, gray, W, H, NULL);

	pthread_once(&g_pal_once, build_palettes);
	const uint8_t (*pal)[3];
	switch (lut) {
	case FLIR_LUT_WHITEHOT: pal = (const uint8_t (*)[3])grey_lut; break;
	case FLIR_LUT_BLACKHOT: pal = (const uint8_t (*)[3])inv_lut; break;
	case FLIR_LUT_RAINBOW:  pal = (const uint8_t (*)[3])rainbow_lut; break;
	case FLIR_LUT_LAVA:     pal = (const uint8_t (*)[3])lava_lut; break;
	case FLIR_LUT_ARCTIC:   pal = (const uint8_t (*)[3])arctic_lut; break;
	case FLIR_LUT_HOTTEST:  pal = (const uint8_t (*)[3])hotspot_lut; break;
	default:                pal = NULL; /* iron-bow */
	}
	if (pal)
		for (int i = 0; i < W * H; i++)
			memcpy(&rgb[i * 3], pal[gray[i]], 3);
	else
		flir_accel_generic.colorize(gray, rgb, W, H, FLIR_LUT_IRONBOW, NULL);

	struct flir_overlay_opts ov = {
		.raw_min = lo, .raw_max = hi,
		.fpa_temp_cx10 = (int16_t)fpa,
		.cross_x = (uint16_t)atomic_load(&g_spot_x),
		.cross_y = (uint16_t)atomic_load(&g_spot_y),
		.cross_hide = !spot_on,
		.temp_unit = fpa ? (enum flir_temp_unit)unit : FLIR_UNIT_RAW,
		.counts_per_deg = FLIR_CAL_COUNTS_PER_DEG,
		.ref_temp_c = fpa / 10.0f + atomic_load(&g_cal_cx100) / 100.0f,
		.ref_raw = FLIR_CAL_RAW_AT_FPA,
		.palette = pal,
		.gray_to_raw = g2r,
		.minmax_show = atomic_load(&g_minmax),
		.hot_x = (uint16_t)(hi_i % W), .hot_y = (uint16_t)(hi_i / W),
		.cold_x = (uint16_t)(lo_i % W), .cold_y = (uint16_t)(lo_i / W),
	};
	flir_gray_to_raw(raw, gray, W * H, g2r);
	flir_overlay(rgb, raw, W, H, &ov, out_rgb);

	for (int i = 0; i < OW * H; i++) { /* outside the lock: the UI thread waits on it */
		out_rgba[i * 4]     = out_rgb[i * 3];
		out_rgba[i * 4 + 1] = out_rgb[i * 3 + 1];
		out_rgba[i * 4 + 2] = out_rgb[i * 3 + 2];
		out_rgba[i * 4 + 3] = 255;
	}
	pthread_mutex_lock(&g_out_lock);
	if (settle > 0)
		settle--;           /* keep showing the pre-FFC image */
	else {
		memcpy(g_rgba, out_rgba, sizeof g_rgba);
		g_have_out = 1;
	}
	post_locked();
	pthread_mutex_unlock(&g_out_lock);
	stats(t0);
}

/* ---- worker: runs the pipeline so the USB event thread never waits on it ---- */

static void *worker_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_frame_lock);
	while (atomic_load(&g_run_worker)) {
		if (!ready) {
			pthread_cond_wait(&g_frame_cond, &g_frame_lock);
			continue;
		}
		memcpy(work_buf, ready_buf, FRAME_BYTES);
		ready = 0;
		pthread_mutex_unlock(&g_frame_lock);
		process_frame(work_buf);
		pthread_mutex_lock(&g_frame_lock);
	}
	pthread_mutex_unlock(&g_frame_lock);
	return NULL;
}

static void frame_done(void)
{
	if (got == FRAME_BYTES) {
		pthread_mutex_lock(&g_frame_lock);
		memcpy(ready_buf, asm_buf, FRAME_BYTES); /* worker busy: newest frame wins */
		ready = 1;
		pthread_cond_signal(&g_frame_cond);
		pthread_mutex_unlock(&g_frame_lock);
	} else if (got) {
		static int bad;
		if (bad++ % 600 == 0) /* e.g. 491520 = I420: the format switch did not take */
			LOGE("dropping %u-byte frame (want %d)", got, FRAME_BYTES);
	}
	got = 0;
}

/*
 * UVC bulk payloads, as uvcvideo parses them: a payload starts with a header (length,
 * flags: bit0 FID, bit1 EOF) and ends with a short transfer or after dwMaxPayloadTransferSize
 * bytes. A frame ends on EOF or when FID toggles.
 */
static void on_bulk(const uint8_t *p, int n)
{
	if (n <= 0)
		return;
	pay_bytes += n;
	if (at_payload_start) {
		at_payload_start = 0;
		int hl = p[0];
		if (hl < 2 || hl > n) { /* not a header: resync on the next payload */
			got = 0;
			fid = -1;
			goto end;
		}
		int f = p[1] & 1;
		if (fid >= 0 && f != fid && got)
			frame_done();
		fid = f;
		eof_pending = p[1] & 2;
		p += hl;
		n -= hl;
	}
	if (got + n > FRAME_BYTES)
		n = FRAME_BYTES - got;
	memcpy(asm_buf + got, p, n);
	got += n;
end:
	return;
}

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *t)
{
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		on_bulk(t->buffer, t->actual_length);
		if (t->actual_length < t->length || pay_bytes >= g_max_payload) { /* payload end */
			at_payload_start = 1;
			pay_bytes = 0;
			if (eof_pending)
				frame_done();
			eof_pending = 0;
		}
	}
	else if (t->status != LIBUSB_TRANSFER_CANCELLED && t->status != LIBUSB_TRANSFER_TIMED_OUT)
		LOGE("bulk transfer ended: status %d", t->status); /* stall, error, device gone */
	if (atomic_load(&g_streaming) &&
	    (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) &&
	    libusb_submit_transfer(t) == 0)
		return;
	atomic_fetch_sub(&g_in_flight, 1);
}

static void *event_main(void *arg)
{
	(void)arg;
	while (atomic_load(&g_run_events)) {
		struct timeval tv = {0, 100 * 1000};
		libusb_handle_events_timeout_completed(g_usb, &tv, NULL);
	}
	return NULL;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/*
 * Negotiate Y16 640x512 at 60 fps the way Linux uvcvideo does: start from the camera's
 * current PROBE values, change format/frame/interval, SET_CUR, read back, then COMMIT
 * exactly what the camera returned.
 */
static int negotiate_y16(void)
{
	uint8_t pb[26] = {0}; /* bcdUVC 0x0100: 26-byte probe/commit */
	int hr = libusb_control_transfer(g_uh, 0xA1, UVC_GET_CUR, VS_PROBE_CONTROL << 8,
					 BOSON_IF_VIDEO, pb, sizeof pb, 1000);
	if (hr == (int)sizeof pb) {
		pb[0] = 1; pb[1] = 0;                           /* bmHint: keep dwFrameInterval */
		pb[2] = BOSON_FMT_Y16; pb[3] = 1;               /* frame 1 = 640x512 */
		pb[4] = BOSON_INTERVAL & 0xFF; pb[5] = BOSON_INTERVAL >> 8 & 0xFF;
		pb[6] = BOSON_INTERVAL >> 16 & 0xFF; pb[7] = 0;
		hr = libusb_control_transfer(g_uh, 0x21, UVC_SET_CUR, VS_PROBE_CONTROL << 8,
					     BOSON_IF_VIDEO, pb, sizeof pb, 1000);
	}
	if (hr == (int)sizeof pb)
		hr = libusb_control_transfer(g_uh, 0xA1, UVC_GET_CUR, VS_PROBE_CONTROL << 8,
					     BOSON_IF_VIDEO, pb, sizeof pb, 1000);
	if (hr != (int)sizeof pb || pb[2] != BOSON_FMT_Y16) {
		LOGE("Y16 probe failed: %s, format %u", hr < 0 ? libusb_error_name(hr) : "short reply", pb[2]);
		return -1;
	}
	g_max_payload = le32(pb + 22);
	if (!g_max_payload)
		g_max_payload = FRAME_BYTES + 12;
	hr = libusb_control_transfer(g_uh, 0x21, UVC_SET_CUR, VS_COMMIT_CONTROL << 8,
				     BOSON_IF_VIDEO, pb, sizeof pb, 1000);
	if (hr != (int)sizeof pb) {
		LOGE("Y16 commit failed: %s", hr < 0 ? libusb_error_name(hr) : "short");
		return -1;
	}
	return 0;
}

static int stream_on(void)
{
	pay_bytes = got = 0;
	at_payload_start = 1;
	fid = -1;
	eof_pending = 0;
	atomic_store(&g_streaming, 1);
	for (int i = 0; i < NXFER; i++) {
		int r = libusb_submit_transfer(g_xfer[i]);
		if (r) {
			LOGE("submit: %s", libusb_error_name(r));
			atomic_store(&g_streaming, 0);
			return -1;
		}
		atomic_fetch_add(&g_in_flight, 1);
	}
	return 0;
}

/* Linux STREAMOFF: cancel the URBs, wait for them, then CLEAR_HALT the bulk endpoint */
static void stream_off(void)
{
	atomic_store(&g_streaming, 0);
	/* cancel repeatedly: a callback may resubmit between reading g_streaming and our cancel */
	for (int k = 0; k < 400 && atomic_load(&g_in_flight) > 0; k++) { /* up to 2 s */
		for (int i = 0; i < NXFER; i++)
			if (g_xfer[i])
				libusb_cancel_transfer(g_xfer[i]); /* idle transfer: harmless NOT_FOUND */
		usleep(5 * 1000);                          /* the event thread reaps them */
	}
	if (atomic_load(&g_in_flight) > 0)
		LOGE("%d transfers still pending after cancel", atomic_load(&g_in_flight));
	if (g_uh)
		libusb_clear_halt(g_uh, BOSON_EP_VIDEO);
}

static void stop_all(void)
{
	if (g_uh)
		stream_off();
	if (g_threads) {
		atomic_store(&g_run_events, 0);
		pthread_join(g_event_thread, NULL);
		pthread_mutex_lock(&g_frame_lock);
		atomic_store(&g_run_worker, 0);
		pthread_cond_signal(&g_frame_cond);
		pthread_mutex_unlock(&g_frame_lock);
		pthread_join(g_worker, NULL);
		g_threads = 0;
	}
	if (atomic_load(&g_in_flight) > 0) {
		/*
		 * A cancel never completed (seen on the Lenovo when the activity stops). libusb
		 * and the kernel still reference that transfer: freeing it, closing the handle or
		 * exiting the context corrupts the heap (Scudo abort, measured). Leak this
		 * session's objects instead (5 x 16 KB at most); Java closing the fd right after
		 * kills the URB in the kernel, and the joined event thread no longer touches them.
		 */
		LOGE("leaking USB session: %d transfer(s) never completed", atomic_load(&g_in_flight));
		for (int i = 0; i < NXFER; i++)
			g_xfer[i] = NULL;
		atomic_store(&g_in_flight, 0);
		g_uh = NULL;
		g_usb = NULL;
	} else {
		if (g_uh) {
			libusb_release_interface(g_uh, BOSON_IF_VIDEO);
			libusb_release_interface(g_uh, 0);
			libusb_close(g_uh); /* a wrapped handle does not close the fd (Java owns it) */
		}
		g_uh = NULL;
		for (int i = 0; i < NXFER; i++) {
			if (g_xfer[i]) {
				free(g_xfer[i]->buffer);
				libusb_free_transfer(g_xfer[i]);
				g_xfer[i] = NULL;
			}
		}
		if (g_usb)
			libusb_exit(g_usb);
		g_usb = NULL;
	}
	pthread_mutex_lock(&g_frame_lock);
	ready = 0;
	pthread_mutex_unlock(&g_frame_lock);
	pthread_mutex_lock(&g_out_lock);
	g_have_out = 0;             /* never show or snapshot a frame from the last session */
	pthread_mutex_unlock(&g_out_lock);
}

JNIEXPORT jint JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeStart(JNIEnv *env, jclass cls, jint fd)
{
	(void)env; (void)cls;
	stop_all();
	for (int i = 0; i < 256; i++) {
		grey_lut[i][0] = grey_lut[i][1] = grey_lut[i][2] = (uint8_t)i;
		inv_lut[i][0] = inv_lut[i][1] = inv_lut[i][2] = (uint8_t)(255 - i);
	}
	have_prev = frozen = nr_was = settle = 0;

	libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
	int r = libusb_init(&g_usb);
	if (!r)
		r = libusb_wrap_sys_device(g_usb, (intptr_t)fd, &g_uh);
	if (!r) {
		/* take both UVC interfaces from uvcvideo, as libuvc did (control 0, streaming 1) */
		for (int i = 0; !r && i <= BOSON_IF_VIDEO; i++) {
			libusb_detach_kernel_driver(g_uh, i); /* errors ok: not bound */
			r = libusb_claim_interface(g_uh, i);
			if (r)
				LOGE("claim interface %d: %s", i, libusb_error_name(r));
		}
	}
	for (int i = 0; !r && i < NXFER; i++) {
		uint8_t *buf = malloc(XFER_SIZE);
		g_xfer[i] = libusb_alloc_transfer(0);
		if (!buf || !g_xfer[i]) {
			free(buf);
			r = LIBUSB_ERROR_NO_MEM;
			break;
		}
		libusb_fill_bulk_transfer(g_xfer[i], g_uh, BOSON_EP_VIDEO, buf, XFER_SIZE, xfer_cb, NULL, 0);
	}
	if (!r) {
		atomic_store(&g_run_events, 1);
		atomic_store(&g_run_worker, 1);
		if (pthread_create(&g_event_thread, NULL, event_main, NULL)) {
			r = -1;
		} else if (pthread_create(&g_worker, NULL, worker_main, NULL)) {
			atomic_store(&g_run_events, 0);
			pthread_join(g_event_thread, NULL);
			r = -1;
		} else {
			g_threads = 1;
			libusb_clear_halt(g_uh, BOSON_EP_VIDEO);
			r = negotiate_y16();
		}
	}
	if (!r)
		r = stream_on();
	if (!r) {
		/*
		 * The Boson free-runs: it keeps streaming the format it last had (I420 at 60 fps
		 * after uvcvideo, a camera service or an earlier session) and stores a new COMMIT
		 * without applying it. It applies it only after a Linux-style STREAMOFF (cancel
		 * the transfers, then CLEAR_HALT). So: start once, stop, negotiate again, start.
		 * Verified with libuvc on S25 Ultra and Lenovo TB-X6C6F, and with this libusb loop
		 * on the Lenovo TB-X6C6F (frames 491520 -> 655360 B).
		 */
		usleep(300 * 1000);
		stream_off();
		usleep(100 * 1000);
		r = negotiate_y16();
		if (!r)
			r = stream_on();
	}
	if (r) {
		LOGE("start failed (%d)", r);
		stop_all();
		return r ? r : -1;
	}
	LOGI("streaming Y16 %dx%d, %d x %d B transfers, max payload %u", W, H, NXFER, XFER_SIZE, g_max_payload);
	return 0;
}

JNIEXPORT void JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeStop(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	stop_all();
}

JNIEXPORT void JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeSetSurface(JNIEnv *env, jclass cls, jobject surface)
{
	(void)cls;
	pthread_mutex_lock(&g_out_lock);
	if (g_win)
		ANativeWindow_release(g_win);
	g_win = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
	if (g_win) {
		ANativeWindow_setBuffersGeometry(g_win, OW, H, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
		post_locked();
	}
	pthread_mutex_unlock(&g_out_lock);
}

JNIEXPORT void JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeSetSettings(JNIEnv *env, jclass cls, jint lut,
							jint unit, jboolean spot, jboolean nr,
							jboolean minmax)
{
	(void)env; (void)cls;
	atomic_store(&g_lut, lut);
	atomic_store(&g_minmax, minmax ? 1 : 0);
	atomic_store(&g_unit, unit);
	atomic_store(&g_spot, spot ? 1 : 0);
	atomic_store(&g_nr, nr ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeSetSpot(JNIEnv *env, jclass cls, jint x, jint y)
{
	(void)env; (void)cls;
	/* 1..W-1: (0,0) means "centre" to flir_overlay */
	atomic_store(&g_spot_x, x < 1 ? 1 : x > W - 1 ? W - 1 : x);
	atomic_store(&g_spot_y, y < 1 ? 1 : y > H - 1 ? H - 1 : y);
}

JNIEXPORT void JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeSetFpa(JNIEnv *env, jclass cls, jint fpa_cx10)
{
	(void)env; (void)cls;
	atomic_store(&g_fpa_cx10, fpa_cx10);
}

/* Offset so the spot reads true_c now; NaN (offset unchanged) without FPA temp or a frame. */
JNIEXPORT jfloat JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeCalibrate(JNIEnv *env, jclass cls, jfloat true_c)
{
	(void)env; (void)cls;
	int fpa = atomic_load(&g_fpa_cx10), raw = atomic_load(&g_spot_raw);
	if (!fpa || !raw)
		return NAN;
	float off = true_c - (fpa / 10.0f + (raw - FLIR_CAL_RAW_AT_FPA) / FLIR_CAL_COUNTS_PER_DEG);
	atomic_store(&g_cal_cx100, (int)lroundf(off * 100.0f));
	return off;
}

JNIEXPORT void JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeSetCalOffset(JNIEnv *env, jclass cls, jfloat off_c)
{
	(void)env; (void)cls;
	atomic_store(&g_cal_cx100, (int)lroundf(off_c * 100.0f));
}

JNIEXPORT jboolean JNICALL
Java_dev_thermalcam_boson_NativeBridge_nativeSnapshot(JNIEnv *env, jclass cls, jobject bitmap)
{
	(void)cls;
	AndroidBitmapInfo info;
	void *px;
	jboolean ok = JNI_FALSE;
	if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
	    info.width != OW || info.height != H || info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
		return JNI_FALSE;
	if (AndroidBitmap_lockPixels(env, bitmap, &px) != ANDROID_BITMAP_RESULT_SUCCESS)
		return JNI_FALSE;
	pthread_mutex_lock(&g_out_lock);
	if (g_have_out) {
		for (int y = 0; y < H; y++)
			memcpy((uint8_t *)px + (size_t)y * info.stride, g_rgba + (size_t)y * OW * 4, OW * 4);
		ok = JNI_TRUE;
	}
	pthread_mutex_unlock(&g_out_lock);
	AndroidBitmap_unlockPixels(env, bitmap);
	return ok;
}
