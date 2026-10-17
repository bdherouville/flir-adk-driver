#ifndef FLIR_INTERNAL_H
#define FLIR_INTERNAL_H

#include "flir_adk.h"
#include <stdint.h>

#define FLIR_WIDTH  640
#define FLIR_HEIGHT 512
#define NUM_BUFS    4

struct v4l2_buf_info {
	void   *start;
	size_t  length;
};

struct flir_ctx {
	int                  vfd;
	int                  cfd;
	enum flir_video_mode mode;
	struct v4l2_buf_info bufs[NUM_BUFS];
	int                  n_bufs;
	int                  streaming;
	const struct flir_accel *accel;
};

void flir_cci_init_serial(int fd);

#endif
