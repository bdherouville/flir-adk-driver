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

/* Read-only: sends Get commands only. */
int main(int argc, char **argv)
{
	const char *cdev = "/dev/ttyACM0";
	int opt;
	while ((opt = getopt(argc, argv, "d:c:")) != -1)
		switch (opt) {
		case 'd': break; /* accepted for compatibility, ignored */
		case 'c': cdev = optarg; break;
		default: fprintf(stderr, "Usage: %s [-c cci] (-d is ignored: CCI only)\n", argv[0]); return 1;
		}

	struct flir_ctx *ctx = flir_open(NULL, cdev, FLIR_MODE_RAW16);
	if (!ctx) { fprintf(stderr, "Failed to open camera\n"); return 1; }

	printf("=== FLIR Boson CCI capability probe ===\n\n");

	printf("Core:\n");
	probe_get(ctx, "bosonGetCameraSN",    0x00050002);
	probe_get(ctx, "bosonGetSensorSN",    0x00050006);
	probe_get(ctx, "bosonGetCameraPN",    0x00050004);
	probe_get(ctx, "bosonFPATempDegCx10", 0x00050030);
	probe_get(ctx, "bosonGetGainMode",    0x00050015);

	printf("\nAGC:\n");
	probe_get(ctx, "agcGetMode (Boson+)", 0x00090032);

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

	printf("\nSysCtrl:\n");
	probe_get(ctx, "sysctrlGetUsbVideoIR16Mode", 0x000E000E);

	printf("\nTelemetry:\n");
	probe_get(ctx, "telemetryGetState",   0x00040002);

	printf("\nScaler:\n");
	probe_get(ctx, "scalerGetMaxZoom",    0x000D0001);
	probe_get(ctx, "scalerGetZoom",       0x000D0003);

	printf("\nFFC:\n");
	probe_get(ctx, "bosonGetFFCMode",           0x00050013);
	probe_get(ctx, "bosonGetFFCTempThreshold",  0x00050009);
	probe_get(ctx, "bosonGetFFCFrameThreshold", 0x0005000B);
	probe_get(ctx, "bosonGetFfcStatus",         0x00050054);

	printf("\nFilters:\n");
	probe_get(ctx, "tfGetEnableState",   0x000A0002);
	probe_get(ctx, "scnrGetEnableState", 0x00080002);
	probe_get(ctx, "spnrGetEnableState", 0x000C0002);
	probe_get(ctx, "bprGetState",        0x00030001);

	printf("\nCamera AGC:\n");
	probe_get(ctx, "agcGetLinearPercent", 0x00090004);
	probe_get(ctx, "agcGetGamma",         0x0009000E);
	probe_get(ctx, "agcGetUseEntropy",    0x0009001F);

	printf("\nSystem:\n");
	probe_get(ctx, "bosonGetSoftwareRev",       0x00050022);
	probe_get(ctx, "sysctrlGetCameraFrameRate", 0x000E0007);
	probe_get(ctx, "sysctrlGetFreezeState",     0x000E0002);
	probe_get(ctx, "bosonGetMyriadTemp",        0x00050068);

	flir_close(ctx);
	return 0;
}
