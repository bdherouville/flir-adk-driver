/*
 * flir_grab — minimal tool: open camera, grab N frames, dump raw to stdout
 *
 * Usage: flir_grab [-d /dev/video0] [-c /dev/ttyACM0] [-m raw16|agc8] [-n 1]
 */

#include "flir_adk.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-d video_dev] [-c cci_dev] [-m raw16|agc8] [-n frames]\n", prog);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *vdev = "/dev/video0";
	const char *cdev = NULL;
	enum flir_video_mode mode = FLIR_MODE_AGC8;
	int nframes = 1;
	int opt;

	while ((opt = getopt(argc, argv, "d:c:m:n:h")) != -1) {
		switch (opt) {
		case 'd': vdev = optarg; break;
		case 'c': cdev = optarg; break;
		case 'm':
			if (strcmp(optarg, "raw16") == 0)
				mode = FLIR_MODE_RAW16;
			else if (strcmp(optarg, "agc8") == 0)
				mode = FLIR_MODE_AGC8;
			else usage(argv[0]);
			break;
		case 'n': nframes = atoi(optarg); break;
		default:  usage(argv[0]);
		}
	}

	struct flir_ctx *ctx = flir_open(vdev, cdev, mode);
	if (!ctx) {
		fprintf(stderr, "Failed to open %s\n", vdev);
		return 1;
	}

	if (cdev) {
		uint32_t sn;
		if (flir_cci_get_serial_number(ctx, &sn) == 0)
			fprintf(stderr, "Camera S/N: %u\n", sn);
		float temp;
		if (flir_cci_get_fpa_temp(ctx, &temp) == 0)
			fprintf(stderr, "FPA temp: %.1f°C\n", temp);
	}

	if (flir_stream_start(ctx)) {
		fprintf(stderr, "Stream start failed\n");
		flir_close(ctx);
		return 1;
	}

	size_t expected = (mode == FLIR_MODE_RAW16)
		? (size_t)640 * 512 * 2
		: (size_t)640 * 512 * 3 / 2;

	struct flir_frame frame;
	int got = 0;
	for (int attempt = 0; got < nframes && attempt < nframes + 4; attempt++) {
		if (flir_grab(ctx, &frame, 5000)) {
			fprintf(stderr, "Grab timeout\n");
			break;
		}
		if (frame.len != expected) {
			fprintf(stderr, "Skip frame %u: %zu bytes (expected %zu)\n",
				frame.seq, frame.len, expected);
			continue;
		}
		fprintf(stderr, "Frame %u: %ux%u, %zu bytes, ts=%lu\n",
			frame.seq, frame.width, frame.height,
			frame.len, (unsigned long)frame.timestamp);
		fwrite(frame.data, 1, frame.len, stdout);
		got++;
	}

	flir_stream_stop(ctx);
	flir_close(ctx);
	return 0;
}
