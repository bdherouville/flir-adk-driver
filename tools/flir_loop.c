/*
 * flir_loop — render the Boson RAW16 stream into a v4l2loopback device
 *
 * RAW16 → HEQ AGC → palette → OSD overlay → YUYV 740x512 → /dev/videoN
 * VLC / OBS open the loopback device like any webcam.
 *
 * Live settings come from a small control file (re-read when it changes):
 *     lut=4 unit=C spot=1 nr=1
 *   lut : 0 white-hot, 1 black-hot, anything else iron-bow
 *   unit: R (raw counts), C, F, K
 *   spot: 0|1 center spot meter box
 *   nr  : 0|1 temporal noise reduction (default on)
 */

#define _GNU_SOURCE
#include "flir_adk.h"
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define W 640
#define H 512
#define OW (W + FLIR_COLORBAR_WIDTH)
#define FPA_EVERY 300 /* frames between FPA temp reads (~5 s at 60 fps) */

extern const struct flir_accel flir_accel_generic;

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

struct settings { int lut; enum flir_temp_unit unit; int spot; int nr; };

static enum flir_temp_unit parse_unit(char c)
{
	switch (c) {
	case 'C': case 'c': return FLIR_UNIT_C;
	case 'F': case 'f': return FLIR_UNIT_F;
	case 'K': case 'k': return FLIR_UNIT_K;
	default:            return FLIR_UNIT_RAW;
	}
}

/* Re-read control file if its mtime changed. Unknown keys are ignored. */
static void poll_ctl(const char *path, struct timespec *last, struct settings *s)
{
	struct stat st;
	if (stat(path, &st) || (st.st_mtim.tv_sec == last->tv_sec &&
				st.st_mtim.tv_nsec == last->tv_nsec))
		return;
	*last = st.st_mtim;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char key[16], val[16];
	while (fscanf(f, " %15[^=]=%15s", key, val) == 2) {
		if (!strcmp(key, "lut"))       s->lut = atoi(val);
		else if (!strcmp(key, "unit")) s->unit = parse_unit(val[0]);
		else if (!strcmp(key, "spot")) s->spot = atoi(val) != 0;
		else if (!strcmp(key, "nr"))   s->nr = atoi(val) != 0;
	}
	fclose(f);
	fprintf(stderr, "settings: lut=%d unit=%d spot=%d nr=%d\n", s->lut, s->unit, s->spot, s->nr);
}

/* Read FPA temp over a short-lived CCI-only handle so flir_ctl can use the port too.
 * ponytail: no lock on the tty; a flir_ctl call landing in the same ~10 ms can
 * interleave frames. Add flock() on the tty if that ever shows up. */
static int16_t read_fpa_cx10(const char *cdev)
{
	if (!cdev)
		return 0;
	struct flir_ctx *c = flir_open(NULL, cdev, FLIR_MODE_RAW16);
	float t;
	int16_t r = (c && flir_cci_get_fpa_temp(c, &t) == 0) ? (int16_t)(t * 10) : 0;
	if (c)
		flir_close(c);
	return r;
}

static void rgb_to_yuyv(const uint8_t *rgb, uint8_t *out, int n_pixels)
{
	for (int i = 0; i < n_pixels; i += 2, rgb += 6, out += 4) {
		int r0 = rgb[0], g0 = rgb[1], b0 = rgb[2];
		int r1 = rgb[3], g1 = rgb[4], b1 = rgb[5];
		int r = (r0 + r1) / 2, g = (g0 + g1) / 2, b = (b0 + b1) / 2;
		out[0] = (uint8_t)((66 * r0 + 129 * g0 + 25 * b0 + 128) / 256 + 16);
		out[1] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
		out[2] = (uint8_t)((66 * r1 + 129 * g1 + 25 * b1 + 128) / 256 + 16);
		out[3] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
	}
}

static int open_loopback(const char *path)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		return -1;
	}
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	fmt.fmt.pix.width = OW;
	fmt.fmt.pix.height = H;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = OW * 2;
	fmt.fmt.pix.sizeimage = OW * H * 2;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT (is this a v4l2loopback device?)");
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	const char *vdev = NULL, *cdev = NULL, *odev = NULL;
	const char *ctl = "/tmp/flir_loop.ctl";
	float cal_s = FLIR_CAL_COUNTS_PER_DEG;
	uint16_t cal_r = FLIR_CAL_RAW_AT_FPA;
	struct settings s = { .lut = FLIR_LUT_IRONBOW, .unit = FLIR_UNIT_RAW, .spot = 1, .nr = 1 };
	int opt;

	while ((opt = getopt(argc, argv, "d:c:o:k:p:u:s:r:")) != -1) {
		switch (opt) {
		case 'd': vdev = optarg; break;
		case 'c': cdev = optarg; break;
		case 'o': odev = optarg; break;
		case 'k': ctl = optarg; break;
		case 'p': s.lut = atoi(optarg); break;
		case 'u': s.unit = parse_unit(optarg[0]); break;
		case 's': cal_s = strtof(optarg, NULL); break;
		case 'r': cal_r = (uint16_t)atoi(optarg); break;
		default: vdev = NULL; optind = argc; break;
		}
	}
	if (!vdev || !odev) {
		fprintf(stderr,
			"Usage: %s -d /dev/videoX -o /dev/videoLOOP [-c /dev/ttyACM0]\n"
			"          [-k ctlfile] [-p lut] [-u R|C|F|K] [-s counts/degC] [-r raw@FPA]\n"
			"  ctlfile (default /tmp/flir_loop.ctl): lut=N unit=C spot=0|1 nr=0|1\n",
			argv[0]);
		return 2;
	}

	int ofd = open_loopback(odev);
	if (ofd < 0)
		return 1;
	struct flir_ctx *ctx = flir_open(vdev, NULL, FLIR_MODE_RAW16);
	if (!ctx || flir_stream_start(ctx)) {
		fprintf(stderr, "Failed to open/stream %s\n", vdev);
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	static uint16_t clean[W * H], g2r[256];
	static int32_t nr_acc[W * H];
	int nr_was = 0;
	static uint8_t gray[W * H], rgb[W * H * 3], out_rgb[OW * H * 3], yuyv[OW * H * 2];
	static uint8_t grey_lut[256][3], inv_lut[256][3];
	for (int i = 0; i < 256; i++) {
		grey_lut[i][0] = grey_lut[i][1] = grey_lut[i][2] = (uint8_t)i;
		inv_lut[i][0] = inv_lut[i][1] = inv_lut[i][2] = (uint8_t)(255 - i);
	}

	struct timespec ctl_mtime = {0};
	int16_t fpa = read_fpa_cx10(cdev);
	unsigned long n = 0;
	struct flir_frame fr;

	fprintf(stderr, "flir_loop: %s -> %s (%dx%d YUYV), ctl %s\n", vdev, odev, OW, H, ctl);
	while (running) {
		if (flir_grab(ctx, &fr, 2000)) {
			fprintf(stderr, "grab timeout\n");
			continue;
		}
		if (fr.len != (size_t)W * H * 2)
			continue; /* first frame after format switch can be short */
		const uint16_t *raw = fr.data;
		if (s.nr) {
			/* 20 counts ~ 3x measured temporal noise (6.6) */
			flir_denoise(raw, clean, nr_acc, W * H, 20, !nr_was);
			raw = clean;
		}
		nr_was = s.nr;

		poll_ctl(ctl, &ctl_mtime, &s);
		if (++n % FPA_EVERY == 0)
			fpa = read_fpa_cx10(cdev);

		uint16_t lo = 0xFFFF, hi = 0;
		for (int i = 0; i < W * H; i++) {
			if (raw[i] < lo) lo = raw[i];
			if (raw[i] > hi) hi = raw[i];
		}

		struct flir_spot_result spot = {0};
		flir_sw_spotmeter(raw, W, H, 206, 306, 220, 420, &spot);

		flir_accel_generic.agc_process(raw, gray, W, H, NULL);

		const uint8_t (*pal)[3] = s.lut == FLIR_LUT_WHITEHOT ? (const uint8_t (*)[3])grey_lut :
					  s.lut == FLIR_LUT_BLACKHOT ? (const uint8_t (*)[3])inv_lut : NULL;
		if (pal)
			for (int i = 0; i < W * H; i++)
				memcpy(&rgb[i * 3], pal[gray[i]], 3);
		else
			flir_accel_generic.colorize(gray, rgb, W, H, FLIR_LUT_IRONBOW, NULL);

		struct flir_overlay_opts ov = {
			.raw_min = lo, .raw_max = hi,
			.fpa_temp_cx10 = fpa,
			.spot_enabled = s.spot,
			.spot_row_start = 206, .spot_row_end = 306,
			.spot_col_start = 220, .spot_col_end = 420,
			.spot_mean = (uint16_t)spot.mean,
			/* temps need FPA reference; fall back to raw counts without CCI */
			.temp_unit = fpa ? s.unit : FLIR_UNIT_RAW,
			.counts_per_deg = cal_s,
			.ref_temp_c = fpa / 10.0f,
			.ref_raw = cal_r,
			.palette = pal,
			.gray_to_raw = g2r,
		};
		flir_gray_to_raw(raw, gray, W * H, g2r);
		flir_overlay(rgb, raw, W, H, &ov, out_rgb);
		rgb_to_yuyv(out_rgb, yuyv, OW * H);

		if (write(ofd, yuyv, sizeof(yuyv)) != (ssize_t)sizeof(yuyv)) {
			perror("write loopback");
			break;
		}
	}

	flir_stream_stop(ctx);
	flir_close(ctx);
	close(ofd);
	return 0;
}
