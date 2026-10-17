/*
 * cci.c — Boson CCI via FSLP serial protocol
 *
 * FSLP frame (unstuffed):
 *   [0]      0x8E start flag
 *   [1]      channel (0x00)
 *   [2..5]   sequence (big-endian, only byte [5] changes, cycles 0-9)
 *   [6..9]   function ID (big-endian)
 *   [10..13] status (TX: 0xFFFFFFFF, RX: 0x00000000 = OK)
 *   [14..N]  payload
 *   [N+1..2] CRC-16 (big-endian)
 *   [last]   0xAE end flag
 *
 * Byte-stuffing (between start and end flags):
 *   0x8E → 0x9E 0x81
 *   0x9E → 0x9E 0x91
 *   0xAE → 0x9E 0xA1
 *   Unstuff: escaped_byte + 0x0D
 *
 * UART: 921600 baud, 8N1
 */

#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

#define FSLP_START  0x8E
#define FSLP_END    0xAE
#define FSLP_ESCAPE 0x9E

#define FSLP_HDR_SIZE 14 /* channel(1) + seq(4) + cmd(4) + status(4) + CRC(2) - wait no */
/* Frame content (inside start/end): channel(1) + seq(4) + cmd(4) + status(4) + payload + CRC(2) */
#define FSLP_OVERHEAD 15 /* 1+4+4+4+2 = 15 bytes of framing around payload */
#define FSLP_MAX_PAYLOAD 256

/*
 * Boson CCI Function IDs — from Boson+ SDK IDD Rev 410
 */
#define CCI_BOSON_GET_CAMERA_SN    0x00050002
#define CCI_BOSON_GET_CAMERA_PN    0x00050004
#define CCI_BOSON_GET_SENSOR_SN    0x00050006
#define CCI_BOSON_RUN_FFC          0x00050007
#define CCI_BOSON_SET_GAIN_MODE    0x00050014
#define CCI_BOSON_GET_GAIN_MODE    0x00050015
#define CCI_BOSON_FPA_TEMP_C_X10   0x00050030
#define CCI_BOSON_FPA_TEMP_K_X10   0x00050031

#define CCI_ROIC_GET_FPA_TEMP      0x00020001

#define CCI_AGC_GET_MODE           0x00090032
#define CCI_AGC_SET_MODE           0x00090033

#define CCI_COLORLUT_SET_ID        0x000B0003
#define CCI_COLORLUT_GET_ID        0x000B0004

#define CCI_TELEMETRY_SET_STATE    0x00040001
#define CCI_TELEMETRY_GET_STATE    0x00040002

#define CCI_SCALER_SET_ZOOM        0x000D0002

#define CCI_TLINEAR_SET_CONTROL    0x003E0001
#define CCI_TLINEAR_GET_CONTROL    0x003E0002

#define CCI_RADIOMETRY_SET_TEMP_STABLE 0x00420000
#define CCI_RADIOMETRY_GET_TEMP_FROM_COUNTS 0x00420038
#define CCI_RADIOMETRY_GET_COUNTS_FROM_TEMP 0x00420037

#define CCI_SYSCTRL_SET_USB_IR16_MODE 0x000E000D

#define CCI_SPOTMETER_SET_ENABLE   0x00430000
#define CCI_SPOTMETER_GET_ENABLE   0x00430001
#define CCI_SPOTMETER_SET_ROI      0x00430003
#define CCI_SPOTMETER_GET_ROI      0x00430004
#define CCI_SPOTMETER_GET_STATS    0x00430005
#define CCI_SPOTMETER_SET_STATSMODE 0x00430006
#define CCI_SPOTMETER_GET_TEMP_STATS 0x00430008

/* CRC-16/AUG-CCITT: init 0x1D0F, poly 0x1021 */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
	uint16_t crc = 0x1D0F;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021
					     : (crc << 1);
	}
	return crc;
}

static uint8_t fslp_seq_counter;

static void put32be(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8) & 0xFF;
	p[3] = v & 0xFF;
}

static uint32_t get32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t get16be(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

/* Byte-stuff one byte into output buffer. Returns bytes written (1 or 2). */
static int stuff_byte(uint8_t *out, uint8_t c)
{
	switch (c) {
	case 0x8E: out[0] = 0x9E; out[1] = 0x81; return 2;
	case 0x9E: out[0] = 0x9E; out[1] = 0x91; return 2;
	case 0xAE: out[0] = 0x9E; out[1] = 0xA1; return 2;
	default:   out[0] = c; return 1;
	}
}

/* Build a complete FSLP frame. Returns total frame length, or -1 on error. */
static int fslp_build(uint8_t *buf, size_t bufsz,
		      uint32_t cmd_id, const void *payload, size_t plen)
{
	if (plen > FSLP_MAX_PAYLOAD)
		return -1;

	/* Assemble unstuffed content: channel + seq + cmd + status + payload */
	uint8_t raw[FSLP_OVERHEAD + FSLP_MAX_PAYLOAD];
	size_t rlen = 0;

	raw[rlen++] = 0x00; /* channel: 0 = FLIR Binary Protocol */

	/* Sequence: 4 bytes, only last byte matters */
	raw[rlen++] = 0x00;
	raw[rlen++] = 0x00;
	raw[rlen++] = 0x00;
	raw[rlen++] = fslp_seq_counter;
	fslp_seq_counter = (fslp_seq_counter + 1) % 10;

	/* Function ID */
	put32be(raw + rlen, cmd_id);
	rlen += 4;

	/* Status: 0xFFFFFFFF for TX */
	raw[rlen++] = 0xFF;
	raw[rlen++] = 0xFF;
	raw[rlen++] = 0xFF;
	raw[rlen++] = 0xFF;

	/* Payload */
	if (payload && plen) {
		memcpy(raw + rlen, payload, plen);
		rlen += plen;
	}

	/* CRC over everything (channel through payload) */
	uint16_t crc = crc16_ccitt(raw, rlen);
	raw[rlen++] = (crc >> 8) & 0xFF;
	raw[rlen++] = crc & 0xFF;

	/* Byte-stuff into output buffer */
	size_t o = 0;
	if (o >= bufsz) return -1;
	buf[o++] = FSLP_START;

	for (size_t i = 0; i < rlen; i++) {
		if (o + 2 > bufsz) return -1;
		o += stuff_byte(buf + o, raw[i]);
	}

	if (o >= bufsz) return -1;
	buf[o++] = FSLP_END;
	return (int)o;
}

/*
 * Read and un-stuff one FSLP response frame.
 * Returns payload length on success, -1 on error/timeout.
 * Status code is written to *status if non-NULL.
 */
static int fslp_read_response(int fd, void *out, size_t outsz,
			      uint32_t *status, int timeout_ms)
{
	uint8_t raw[1024];
	size_t rlen = 0;
	int in_frame = 0, escaped = 0;

	struct timeval tv = { .tv_sec = timeout_ms / 1000,
			      .tv_usec = (timeout_ms % 1000) * 1000 };
	fd_set fds;

	for (;;) {
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0)
			return -1;

		uint8_t c;
		if (read(fd, &c, 1) != 1)
			return -1;

		if (c == FSLP_START) {
			in_frame = 1;
			rlen = 0;
			escaped = 0;
			continue;
		}
		if (!in_frame)
			continue;

		if (c == FSLP_END) {
			/* Parse: channel(1) + seq(4) + cmd(4) + status(4) + payload + CRC(2) */
			if (rlen < 15) return -1; /* minimum: 13 header + 2 CRC, no payload */

			/* Verify CRC */
			uint16_t got_crc = get16be(raw + rlen - 2);
			uint16_t exp_crc = crc16_ccitt(raw, rlen - 2);
			if (got_crc != exp_crc)
				return -1;

			/* Check status field at bytes [9..12] */
			uint32_t st = get32be(raw + 9);
			if (status) *status = st;
			if (st != 0)
				return -1;

			/* Payload starts at byte 13, ends before CRC */
			size_t plen = rlen - 15; /* total - (1+4+4+4+2) */
			if (plen > outsz) plen = outsz;
			if (plen > 0)
				memcpy(out, raw + 13, plen);
			return (int)plen;
		}

		if (c == FSLP_ESCAPE) {
			escaped = 1;
			continue;
		}

		if (escaped) {
			c = c + 0x0D;
			escaped = 0;
		}

		if (rlen < sizeof(raw))
			raw[rlen++] = c;
	}
}

/* Public: send a raw CCI command */
int flir_cci_raw(struct flir_ctx *ctx, uint32_t cmd_id,
		 const void *payload, size_t payload_len,
		 void *resp, size_t *resp_len)
{
	int fd = ctx ? ctx->cfd : -1;
	if (fd < 0) return -1;

	uint8_t frame[1024];
	int flen = fslp_build(frame, sizeof(frame), cmd_id, payload, payload_len);
	if (flen < 0) return -1;

	if (write(fd, frame, flen) != flen)
		return -1;

	if (resp && resp_len) {
		uint32_t status;
		int n = fslp_read_response(fd, resp, *resp_len, &status, 2000);
		if (n < 0) return -1;
		*resp_len = n;
	} else {
		/* SET commands: consume the response and check status */
		uint8_t discard[64];
		uint32_t status;
		int n = fslp_read_response(fd, discard, sizeof(discard), &status, 1000);
		if (n < 0) return -1;
	}
	return 0;
}

/* Configure serial port for Boson CCI */
void flir_cci_init_serial(int fd)
{
	struct termios tty;
	tcgetattr(fd, &tty);
	cfmakeraw(&tty);
	cfsetispeed(&tty, B921600);
	cfsetospeed(&tty, B921600);
	tty.c_cflag = CS8 | CLOCAL | CREAD;
	tty.c_iflag = IGNPAR;
	tty.c_oflag = 0;
	tty.c_lflag = 0;
	tty.c_cc[VMIN]  = 0;
	tty.c_cc[VTIME] = 10; /* 1s byte-level timeout */
	tcflush(fd, TCIOFLUSH);
	tcsetattr(fd, TCSANOW, &tty);
}

/*
 * CCI command wrappers
 */

int flir_cci_ffc_trigger(struct flir_ctx *ctx)
{
	return flir_cci_raw(ctx, CCI_BOSON_RUN_FFC, NULL, 0, NULL, NULL);
}

int flir_cci_get_fpa_temp(struct flir_ctx *ctx, float *celsius)
{
	uint8_t resp[4];
	size_t rlen = sizeof(resp);
	if (flir_cci_raw(ctx, CCI_BOSON_FPA_TEMP_C_X10, NULL, 0, resp, &rlen))
		return -1;
	if (rlen < 2) return -1;
	/* Response: INT_16, degrees Celsius * 10 */
	int16_t raw = (int16_t)get16be(resp);
	*celsius = raw / 10.0f;
	return 0;
}

int flir_cci_set_agc_mode(struct flir_ctx *ctx, int agc_type)
{
	uint8_t payload[4];
	put32be(payload, (uint32_t)agc_type);
	return flir_cci_raw(ctx, CCI_AGC_SET_MODE, payload, 4, NULL, NULL);
}

int flir_cci_set_color_lut(struct flir_ctx *ctx, int lut_id)
{
	uint8_t payload[4];
	put32be(payload, (uint32_t)lut_id);
	return flir_cci_raw(ctx, CCI_COLORLUT_SET_ID, payload, 4, NULL, NULL);
}

int flir_cci_enable_telemetry(struct flir_ctx *ctx, int enable)
{
	uint8_t payload[4];
	put32be(payload, enable ? 1 : 0);
	return flir_cci_raw(ctx, CCI_TELEMETRY_SET_STATE, payload, 4, NULL, NULL);
}

int flir_cci_get_serial_number(struct flir_ctx *ctx, uint32_t *sn)
{
	uint8_t resp[4];
	size_t rlen = sizeof(resp);
	if (flir_cci_raw(ctx, CCI_BOSON_GET_CAMERA_SN, NULL, 0, resp, &rlen))
		return -1;
	if (rlen < 4) return -1;
	*sn = get32be(resp);
	return 0;
}

int flir_cci_set_zoom(struct flir_ctx *ctx, int factor)
{
	uint8_t payload[4];
	put32be(payload, (uint32_t)factor);
	return flir_cci_raw(ctx, CCI_SCALER_SET_ZOOM, payload, 4, NULL, NULL);
}

int flir_cci_get_sensor_sn(struct flir_ctx *ctx, uint32_t *sn)
{
	uint8_t resp[4];
	size_t rlen = sizeof(resp);
	if (flir_cci_raw(ctx, CCI_BOSON_GET_SENSOR_SN, NULL, 0, resp, &rlen))
		return -1;
	if (rlen < 4) return -1;
	*sn = get32be(resp);
	return 0;
}

int flir_cci_get_part_number(struct flir_ctx *ctx, char *pn, size_t pn_sz)
{
	size_t rlen = pn_sz > 20 ? 20 : pn_sz;
	if (flir_cci_raw(ctx, CCI_BOSON_GET_CAMERA_PN, NULL, 0, pn, &rlen))
		return -1;
	if (rlen < pn_sz)
		pn[rlen] = '\0';
	return 0;
}

int flir_cci_set_gain_mode(struct flir_ctx *ctx, int mode)
{
	uint8_t payload[4];
	put32be(payload, (uint32_t)mode);
	return flir_cci_raw(ctx, CCI_BOSON_SET_GAIN_MODE, payload, 4, NULL, NULL);
}

int flir_cci_tlinear_enable(struct flir_ctx *ctx, int enable)
{
	uint8_t payload[4];
	put32be(payload, enable ? 1 : 0);
	return flir_cci_raw(ctx, CCI_TLINEAR_SET_CONTROL, payload, 4, NULL, NULL);
}

int flir_cci_spotmeter_enable(struct flir_ctx *ctx, int enable)
{
	uint8_t payload[4];
	put32be(payload, enable ? 1 : 0);
	return flir_cci_raw(ctx, CCI_SPOTMETER_SET_ENABLE, payload, 4, NULL, NULL);
}

int flir_cci_spotmeter_set_roi(struct flir_ctx *ctx,
			       uint16_t row_start, uint16_t row_end,
			       uint16_t col_start, uint16_t col_end)
{
	uint8_t payload[8];
	payload[0] = row_start >> 8; payload[1] = row_start & 0xFF;
	payload[2] = row_end >> 8;   payload[3] = row_end & 0xFF;
	payload[4] = col_start >> 8; payload[5] = col_start & 0xFF;
	payload[6] = col_end >> 8;   payload[7] = col_end & 0xFF;
	return flir_cci_raw(ctx, CCI_SPOTMETER_SET_ROI, payload, 8, NULL, NULL);
}

static float be_to_float(const uint8_t *p)
{
	uint32_t bits = get32be(p);
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

int flir_cci_spotmeter_get_temp(struct flir_ctx *ctx,
				float *mean, float *min, float *max)
{
	uint8_t resp[24];
	size_t rlen = sizeof(resp);
	if (flir_cci_raw(ctx, CCI_SPOTMETER_GET_TEMP_STATS, NULL, 0, resp, &rlen))
		return -1;
	if (rlen < 24) return -1;
	/*
	 * [0:4]   mean (FLOAT)
	 * [4:8]   deviation (FLOAT)
	 * [8:10]  min.row (UINT_16)
	 * [10:12] min.column (UINT_16)
	 * [12:16] min.value (FLOAT)
	 * [16:18] max.row (UINT_16)
	 * [18:20] max.column (UINT_16)
	 * [20:24] max.value (FLOAT)
	 */
	*mean = be_to_float(resp);
	*min  = be_to_float(resp + 12);
	*max  = be_to_float(resp + 20);
	return 0;
}

int flir_cci_spotmeter_set_statsmode(struct flir_ctx *ctx, int mode)
{
	uint8_t payload[4];
	put32be(payload, (uint32_t)mode);
	return flir_cci_raw(ctx, CCI_SPOTMETER_SET_STATSMODE, payload, 4, NULL, NULL);
}

int flir_cci_spotmeter_get_stats(struct flir_ctx *ctx,
				 uint16_t *mean, uint16_t *min, uint16_t *max)
{
	uint8_t resp[16];
	size_t rlen = sizeof(resp);
	if (flir_cci_raw(ctx, CCI_SPOTMETER_GET_STATS, NULL, 0, resp, &rlen))
		return -1;
	if (rlen < 16) return -1;
	/*
	 * [0:2]  mean (UINT_16)
	 * [2:4]  deviation (UINT_16)
	 * [4:6]  min.row, [6:8] min.col, [8:10] min.value
	 * [10:12] max.row, [12:14] max.col, [14:16] max.value
	 */
	*mean = get16be(resp);
	*min  = get16be(resp + 8);
	*max  = get16be(resp + 14);
	return 0;
}

int flir_cci_radiometry_enable(struct flir_ctx *ctx, int enable)
{
	uint8_t payload[4];
	put32be(payload, enable ? 1 : 0);
	return flir_cci_raw(ctx, CCI_RADIOMETRY_SET_TEMP_STABLE, payload, 4, NULL, NULL);
}
