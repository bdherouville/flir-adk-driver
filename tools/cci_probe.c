#define _GNU_SOURCE
#include "flir_adk.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int probe_get(struct flir_ctx *ctx, const char *label, uint32_t cmd)
{
	uint8_t resp[64];
	size_t rlen = sizeof(resp);
	int rc = flir_cci_raw(ctx, cmd, NULL, 0, resp, &rlen);
	printf("  %-35s ", label);
	if (rc != 0) { printf("NOT AVAILABLE\n"); return -1; }
	printf("OK [%zu]", rlen);
	for (size_t i = 0; i < rlen && i < 16; i++) printf(" %02x", resp[i]);
	printf("\n");
	return 0;
}

static int probe_set(struct flir_ctx *ctx, const char *label, uint32_t cmd, uint32_t val)
{
	uint8_t payload[4] = { (val>>24)&0xFF, (val>>16)&0xFF, (val>>8)&0xFF, val&0xFF };
	uint8_t resp[4];
	size_t rlen = sizeof(resp);
	int rc = flir_cci_raw(ctx, cmd, payload, 4, resp, &rlen);
	printf("  %-35s %s\n", label, rc ? "NOT AVAILABLE" : "OK");
	return rc;
}

int main(int argc, char **argv)
{
	const char *vdev = "/dev/video5";
	const char *cdev = "/dev/ttyACM0";
	int opt;
	while ((opt = getopt(argc, argv, "d:c:")) != -1)
		switch (opt) {
		case 'd': vdev = optarg; break;
		case 'c': cdev = optarg; break;
		default: fprintf(stderr, "Usage: %s [-d video] [-c cci]\n", argv[0]); return 1;
		}

	struct flir_ctx *ctx = flir_open(vdev, cdev, FLIR_MODE_RAW16);
	if (!ctx) { fprintf(stderr, "Failed to open camera\n"); return 1; }

	printf("=== FLIR Boson CCI capability probe ===\n\n");

	printf("Core:\n");
	probe_get(ctx, "bosonGetCameraSN",    0x00050002);
	probe_get(ctx, "bosonGetSensorSN",    0x00050006);
	probe_get(ctx, "bosonGetCameraPN",    0x00050004);
	probe_get(ctx, "bosonFPATempDegCx10", 0x00050030);
	probe_get(ctx, "bosonGetGainMode",    0x00050015);

	printf("\nAGC:\n");
	probe_get(ctx, "agcGetMode",          0x00090032);

	printf("\nColor LUT:\n");
	probe_get(ctx, "colorLutGetId",       0x000B0004);

	printf("\nRadiometry:\n");
	probe_get(ctx, "radiometryGetTempStableEnable", 0x00420001);
	probe_get(ctx, "radiometryGetRBFOHighGainDef",  0x0042001B);

	printf("\nTLinear:\n");
	probe_get(ctx, "TLinearGetControl",   0x003E0002);

	printf("\nSpot Meter:\n");
	probe_get(ctx, "spotMeterGetEnable",  0x00430001);
	probe_get(ctx, "spotMeterGetStatsMode", 0x00430007);
	probe_set(ctx, "spotMeterSetEnable(1)", 0x00430000, 1);

	printf("\nSysCtrl:\n");
	probe_get(ctx, "sysctrlGetUsbVideoIR16Mode", 0x000E000E);

	printf("\nTelemetry:\n");
	probe_get(ctx, "telemetryGetState",   0x00040002);

	printf("\nScaler:\n");
	probe_set(ctx, "scalerSetZoom(1)",    0x000D0002, 1);

	flir_close(ctx);
	return 0;
}
