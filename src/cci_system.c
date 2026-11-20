/*
 * cci_system.c — multi-field CCI wrappers: identification, persistence, ROIs, splash screen, read-only fileOps (Boson Software IDD 102-2013-44).
 */
#include "cci_util.h"
#include <string.h>

#define CCI_BOSON_REBOOT           0x00050010 /* no payload */
#define CCI_BOSON_WRITE_DYN_HEADER 0x00050018 /* no payload, writes flash */
#define CCI_BOSON_RESTORE_FACTORY  0x0005001B /* no payload */
#define CCI_BOSON_GET_SW_REV       0x00050022 /* out: major, minor, patch UINT_32 (12 B) */
#define CCI_BOSON_GET_SENSOR_PN    0x0005003F /* out: UCHAR[32], e.g. "500-0773" NUL padded */
#define CCI_AGC_SET_ROI            0x00090020 /* in: FLR_ROI_T 8 B */
#define CCI_AGC_GET_ROI            0x00090021 /* out: FLR_ROI_T 8 B */
#define CCI_STATS_SET_ROI          0x001D0003
#define CCI_STATS_GET_ROI          0x001D0004
#define CCI_SPLASH_SET_DURATION    0x001A0000 /* in: screen UINT_32, periodMs UINT_32 */
#define CCI_SPLASH_SET_BACKGROUND  0x001A0002 /* in: screen UINT_32, 0xRRGGBB UINT_32 */
#define CCI_SPLASH_GET_DURATION    0x001A0003 /* in: screen UINT_32; out: periodMs UINT_32 */
#define CCI_SPLASH_GET_BACKGROUND  0x001A0005 /* in: screen UINT_32; out: colour UINT_32 */
#define CCI_FILEOPS_DIR            0x00160000 /* out: dirent UCHAR[128], "" = end */
#define CCI_FILEOPS_GET_FILE_SIZE  0x0016000D /* in: path UCHAR[128]; out: UINT_32 */

int flir_cci_get_software_rev(struct flir_ctx *ctx, uint32_t *major,
			      uint32_t *minor, uint32_t *patch)
{
	uint8_t r[12];
	if (cci_get(ctx, CCI_BOSON_GET_SW_REV, r, 12))
		return -1;
	*major = cci_get32(r);
	*minor = cci_get32(r + 4);
	*patch = cci_get32(r + 8);
	return 0;
}

int flir_cci_get_sensor_pn(struct flir_ctx *ctx, char *pn, size_t pn_sz)
{
	uint8_t r[32];
	size_t n = sizeof(r);
	size_t len;

	if (pn_sz == 0)
		return -1;
	if (flir_cci_raw(ctx, CCI_BOSON_GET_SENSOR_PN, NULL, 0, r, &n))
		return -1;

	len = n < pn_sz - 1 ? n : pn_sz - 1;
	memcpy(pn, r, len);
	pn[len] = '\0';
	len = strlen(pn); /* cut at first NUL */
	while (len && pn[len - 1] == ' ')
		len--;
	pn[len] = '\0';
	return 0;
}

/* Send a no-payload command with a response buffer, so the 2 s timeout
 * applies (flash writes can be slow). */
static int cci_cmd_noarg(struct flir_ctx *ctx, uint32_t id)
{
	uint8_t r[4];
	size_t n = sizeof(r);
	return flir_cci_raw(ctx, id, NULL, 0, r, &n);
}

int flir_cci_save_settings(struct flir_ctx *ctx)
{
	return cci_cmd_noarg(ctx, CCI_BOSON_WRITE_DYN_HEADER);
}

int flir_cci_restore_factory_defaults(struct flir_ctx *ctx)
{
	return cci_cmd_noarg(ctx, CCI_BOSON_RESTORE_FACTORY);
}

/* reboot may return -1 with flir_cci_last_status() == 0xFFFFFFFF when the
 * camera resets before replying. */
int flir_cci_reboot(struct flir_ctx *ctx)
{
	return cci_cmd_noarg(ctx, CCI_BOSON_REBOOT);
}

static int roi_set(struct flir_ctx *ctx, uint32_t id, uint16_t row_start,
		   uint16_t row_end, uint16_t col_start, uint16_t col_end)
{
	uint8_t p[8];
	cci_put16(p, row_start);
	cci_put16(p + 2, row_end);
	cci_put16(p + 4, col_start);
	cci_put16(p + 6, col_end);
	return flir_cci_raw(ctx, id, p, 8, NULL, NULL);
}

static int roi_get(struct flir_ctx *ctx, uint32_t id, uint16_t roi[4])
{
	uint8_t r[8];
	int i;
	if (cci_get(ctx, id, r, 8))
		return -1;
	for (i = 0; i < 4; i++)
		roi[i] = cci_get16(r + 2 * i);
	return 0;
}

int flir_cci_agc_set_roi(struct flir_ctx *ctx, uint16_t row_start,
			 uint16_t row_end, uint16_t col_start, uint16_t col_end)
{
	return roi_set(ctx, CCI_AGC_SET_ROI, row_start, row_end, col_start,
		       col_end);
}

int flir_cci_agc_get_roi(struct flir_ctx *ctx, uint16_t roi[4])
{
	return roi_get(ctx, CCI_AGC_GET_ROI, roi);
}

int flir_cci_stats_set_roi(struct flir_ctx *ctx, uint16_t row_start,
			   uint16_t row_end, uint16_t col_start,
			   uint16_t col_end)
{
	return roi_set(ctx, CCI_STATS_SET_ROI, row_start, row_end, col_start,
		       col_end);
}

int flir_cci_stats_get_roi(struct flir_ctx *ctx, uint16_t roi[4])
{
	return roi_get(ctx, CCI_STATS_GET_ROI, roi);
}

/* The getters REQUIRE the screen number input (sending no payload gives
 * status 0x0000017F). */
static int splash_get(struct flir_ctx *ctx, uint32_t id, uint32_t screen,
		      uint32_t *out)
{
	uint8_t in[4], r[4];
	size_t n = sizeof(r);
	cci_put32(in, screen);
	if (flir_cci_raw(ctx, id, in, 4, r, &n) || n < 4)
		return -1;
	*out = cci_get32(r);
	return 0;
}

static int splash_set(struct flir_ctx *ctx, uint32_t id, uint32_t screen,
		      uint32_t value)
{
	uint8_t p[8];
	cci_put32(p, screen);
	cci_put32(p + 4, value);
	return flir_cci_raw(ctx, id, p, 8, NULL, NULL);
}

int flir_cci_splash_get_duration(struct flir_ctx *ctx, uint32_t screen,
				 uint32_t *ms)
{
	return splash_get(ctx, CCI_SPLASH_GET_DURATION, screen, ms);
}

int flir_cci_splash_set_duration(struct flir_ctx *ctx, uint32_t screen,
				 uint32_t ms)
{
	return splash_set(ctx, CCI_SPLASH_SET_DURATION, screen, ms);
}

int flir_cci_splash_get_background(struct flir_ctx *ctx, uint32_t screen,
				   uint32_t *rgb)
{
	return splash_get(ctx, CCI_SPLASH_GET_BACKGROUND, screen, rgb);
}

int flir_cci_splash_set_background(struct flir_ctx *ctx, uint32_t screen,
				   uint32_t rgb)
{
	return splash_set(ctx, CCI_SPLASH_SET_BACKGROUND, screen, rgb);
}

/* READ-ONLY on purpose: fileOps write / remove / rename / truncate and the mem* flash commands are deliberately NOT wrapped — a bad write to the camera's flash file system can brick it. */
int flir_cci_fileops_dir(struct flir_ctx *ctx, char *name, size_t name_sz)
{
	uint8_t r[128];
	size_t n = sizeof(r);
	size_t len;

	if (name_sz == 0)
		return -1;
	if (flir_cci_raw(ctx, CCI_FILEOPS_DIR, NULL, 0, r, &n))
		return -1;

	len = n < name_sz - 1 ? n : name_sz - 1;
	memcpy(name, r, len);
	name[len] = '\0'; /* embedded NUL ends the string */
	return 0;
}

int flir_cci_fileops_file_size(struct flir_ctx *ctx, const char *path,
			       uint32_t *size)
{
	uint8_t p[128] = {0};
	uint8_t r[4];
	size_t len = strlen(path);
	size_t n = sizeof(r);

	if (len > 127)
		return -1;
	memcpy(p, path, len);
	if (flir_cci_raw(ctx, CCI_FILEOPS_GET_FILE_SIZE, p, 128, r, &n) ||
	    n < 4)
		return -1;
	*size = cci_get32(r);
	return 0;
}
