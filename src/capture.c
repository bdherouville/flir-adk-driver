/*
 * capture.c — V4L2 MMAP capture for FLIR ADK / Boson UVC
 */

#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
	return r;
}

static uint32_t mode_to_v4l2(enum flir_video_mode m)
{
	return m == FLIR_MODE_RAW16
		? V4L2_PIX_FMT_Y16
		: V4L2_PIX_FMT_YUV420;
}

struct flir_ctx *flir_open(const char *video_dev, const char *cci_dev,
			   enum flir_video_mode mode)
{
	struct flir_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->mode = mode;
	ctx->cfd = -1;

	ctx->vfd = open(video_dev, O_RDWR);
	if (ctx->vfd < 0) {
		perror(video_dev);
		free(ctx);
		return NULL;
	}

	/* Set format */
	struct v4l2_format fmt = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = {
			.width       = FLIR_WIDTH,
			.height      = FLIR_HEIGHT,
			.pixelformat = mode_to_v4l2(mode),
			.field       = V4L2_FIELD_NONE,
		},
	};
	if (xioctl(ctx->vfd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		goto fail;
	}
	if (fmt.fmt.pix.pixelformat != mode_to_v4l2(mode)) {
		fprintf(stderr, "Camera rejected format 0x%08x, got 0x%08x\n",
			mode_to_v4l2(mode), fmt.fmt.pix.pixelformat);
		goto fail;
	}

	/* Request MMAP buffers */
	struct v4l2_requestbuffers req = {
		.count  = NUM_BUFS,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (xioctl(ctx->vfd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		goto fail;
	}
	ctx->n_bufs = req.count;

	for (int i = 0; i < ctx->n_bufs; i++) {
		struct v4l2_buffer buf = {
			.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index  = i,
		};
		if (xioctl(ctx->vfd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			goto fail;
		}
		ctx->bufs[i].length = buf.length;
		ctx->bufs[i].start = mmap(NULL, buf.length,
					  PROT_READ | PROT_WRITE,
					  MAP_SHARED, ctx->vfd,
					  buf.m.offset);
		if (ctx->bufs[i].start == MAP_FAILED) {
			perror("mmap");
			goto fail;
		}
	}

	/* Open CCI serial port if provided */
	if (cci_dev) {
		ctx->cfd = open(cci_dev, O_RDWR | O_NOCTTY);
		if (ctx->cfd >= 0)
			flir_cci_init_serial(ctx->cfd);
	}

	return ctx;

fail:
	close(ctx->vfd);
	free(ctx);
	return NULL;
}

void flir_close(struct flir_ctx *ctx)
{
	if (!ctx) return;
	if (ctx->streaming)
		flir_stream_stop(ctx);
	for (int i = 0; i < ctx->n_bufs; i++)
		munmap(ctx->bufs[i].start, ctx->bufs[i].length);
	close(ctx->vfd);
	if (ctx->cfd >= 0)
		close(ctx->cfd);
	free(ctx);
}

int flir_stream_start(struct flir_ctx *ctx)
{
	for (int i = 0; i < ctx->n_bufs; i++) {
		struct v4l2_buffer buf = {
			.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index  = i,
		};
		if (xioctl(ctx->vfd, VIDIOC_QBUF, &buf) < 0)
			return -1;
	}
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(ctx->vfd, VIDIOC_STREAMON, &type) < 0)
		return -1;
	ctx->streaming = 1;
	return 0;
}

int flir_stream_stop(struct flir_ctx *ctx)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(ctx->vfd, VIDIOC_STREAMOFF, &type) < 0)
		return -1;
	ctx->streaming = 0;
	return 0;
}

int flir_grab(struct flir_ctx *ctx, struct flir_frame *out, int timeout_ms)
{
	struct pollfd pfd = { .fd = ctx->vfd, .events = POLLIN };
	if (poll(&pfd, 1, timeout_ms) <= 0)
		return -1;

	struct v4l2_buffer buf = {
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (xioctl(ctx->vfd, VIDIOC_DQBUF, &buf) < 0)
		return -1;

	out->data      = ctx->bufs[buf.index].start;
	out->len       = buf.bytesused;
	out->width     = FLIR_WIDTH;
	out->height    = FLIR_HEIGHT;
	out->seq       = buf.sequence;
	out->timestamp = (uint64_t)buf.timestamp.tv_sec * 1000000
			 + buf.timestamp.tv_usec;

	/* Re-queue the buffer for the next frame */
	if (xioctl(ctx->vfd, VIDIOC_QBUF, &buf) < 0)
		return -1;

	return 0;
}

int flir_set_accel(struct flir_ctx *ctx, const struct flir_accel *accel)
{
	ctx->accel = accel;
	return 0;
}
