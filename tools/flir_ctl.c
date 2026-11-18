/*
 * flir_ctl.c — run ONE CCI command on a FLIR Boson, print ONE JSON line.
 *
 * Opens only the CCI serial port (never the V4L2 node), so it works while
 * another process is streaming video.
 */
#define _GNU_SOURCE
#include "flir_adk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cmd {
	const char *name;
	int min, max; /* accepted value range; min < 0 = command takes no value */
};

static const struct cmd cmds[] = {
	{ "ffc",        -1,  -1 },
	{ "fpa",        -1,  -1 },
	{ "sn",         -1,  -1 },
	{ "sensor-sn",  -1,  -1 },
	{ "pn",         -1,  -1 },
	{ "gain",        0,   4 },
	{ "lut",         0,   9 },
	{ "agc",         0,   5 },
	{ "zoom",        0, 100 },
	{ "telemetry",   0,   1 },
	{ "tlinear",     0,   1 },
	{ "radiometry",  0,   1 },
	{ "spotmeter",   0,   1 },
};

/* Print s as a JSON string; '"', '\\' and non-printable bytes become '?'. */
static void json_str(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		putchar((c == '"' || c == '\\' || c < 0x20 || c > 0x7e) ? '?' : c);
	}
	putchar('"');
}

_Noreturn static void fail(const char *cmd, const char *err, int code)
{
	printf("{\"ok\":false,\"cmd\":");
	json_str(cmd);
	printf(",\"error\":\"%s\"", err);
	if (code == 3)
		printf(",\"status\":\"0x%08lx\"",
		       (unsigned long)flir_cci_last_status());
	printf("}\n");
	exit(code);
}

static void usage(void)
{
	printf(
"Usage: flir_ctl [-c CCI_DEV] COMMAND [VALUE]\n"
"Send one CCI command to a FLIR Boson and print one JSON line on stdout.\n"
"  -c CCI_DEV   CCI serial port (default /dev/ttyACM0)\n"
"  -h, --help   show this help\n"
"Commands:\n"
"  ffc            run flat-field correction\n"
"  fpa            read FPA temperature (degrees C)\n"
"  sn             read camera serial number\n"
"  sensor-sn      read sensor serial number\n"
"  pn             read part number\n"
"  gain N         0=high 1=low 2=auto 3=dual 4=manual\n"
"  lut N          0=whitehot 1=blackhot 2=rainbow 3=rainbow_hc 4=ironbow\n"
"                 5=lava 6=arctic 7=globow 8=gradedfire 9=hottest\n"
"  agc N          0=normal 1=hold 2=threshold 3=auto_bright 4=auto_linear 5=manual\n"
"  zoom N         0..100\n"
"  telemetry 0|1\n"
"  tlinear 0|1\n"
"  radiometry 0|1\n"
"  spotmeter 0|1\n"
"Exit codes: 0 ok, 1 open_failed, 2 usage/unknown_command/bad_value, 3 cci_failed\n");
}

int main(int argc, char **argv)
{
	const char *cci = "/dev/ttyACM0";

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		}

	int opt;
	while ((opt = getopt(argc, argv, "c:")) != -1) {
		if (opt == 'c')
			cci = optarg;
		else
			fail("", "usage", 2);
	}

	int nargs = argc - optind;
	if (nargs < 1)
		fail("", "usage", 2);

	const char *name = argv[optind];
	const struct cmd *c = NULL;
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (!strcmp(cmds[i].name, name))
			c = &cmds[i];
	if (!c)
		fail(name, "unknown_command", 2);

	long val = 0;
	if (c->min < 0) {
		if (nargs != 1)
			fail(name, "usage", 2);
	} else {
		if (nargs != 2)
			fail(name, "usage", 2);
		const char *arg = argv[optind + 1];
		char *end;
		errno = 0;
		val = strtol(arg, &end, 10);
		if (errno || end == arg || *end || val < c->min || val > c->max)
			fail(name, "bad_value", 2);
	}

	struct flir_ctx *ctx = flir_open(NULL, cci, FLIR_MODE_RAW16);
	if (!ctx)
		fail(name, "open_failed", 1);

	int rc = -1;
	if (!strcmp(name, "ffc")) {
		rc = flir_cci_ffc_trigger(ctx);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"ffc\"}\n");
	} else if (!strcmp(name, "fpa")) {
		float t;
		rc = flir_cci_get_fpa_temp(ctx, &t);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"fpa\",\"value\":%.1f}\n", t);
	} else if (!strcmp(name, "sn")) {
		uint32_t sn;
		rc = flir_cci_get_serial_number(ctx, &sn);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"sn\",\"value\":%lu}\n",
			       (unsigned long)sn);
	} else if (!strcmp(name, "sensor-sn")) {
		uint32_t sn;
		rc = flir_cci_get_sensor_sn(ctx, &sn);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"sensor-sn\",\"value\":%lu}\n",
			       (unsigned long)sn);
	} else if (!strcmp(name, "pn")) {
		char pn[32] = { 0 };
		rc = flir_cci_get_part_number(ctx, pn, sizeof(pn));
		if (!rc) {
			size_t n = strlen(pn);
			while (n && pn[n - 1] == ' ')
				pn[--n] = '\0';
			printf("{\"ok\":true,\"cmd\":\"pn\",\"value\":");
			json_str(pn);
			printf("}\n");
		}
	} else {
		int v = (int)val;
		if (!strcmp(name, "gain"))
			rc = flir_cci_set_gain_mode(ctx, v);
		else if (!strcmp(name, "lut"))
			rc = flir_cci_set_color_lut(ctx, v);
		else if (!strcmp(name, "agc"))
			rc = flir_cci_set_agc_mode(ctx, v);
		else if (!strcmp(name, "zoom"))
			rc = flir_cci_set_zoom(ctx, v);
		else if (!strcmp(name, "telemetry"))
			rc = flir_cci_enable_telemetry(ctx, v);
		else if (!strcmp(name, "tlinear"))
			rc = flir_cci_tlinear_enable(ctx, v);
		else if (!strcmp(name, "radiometry"))
			rc = flir_cci_radiometry_enable(ctx, v);
		else if (!strcmp(name, "spotmeter"))
			rc = flir_cci_spotmeter_enable(ctx, v);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"%s\",\"value\":%d}\n", name, v);
	}

	flir_close(ctx);
	if (rc)
		fail(name, "cci_failed", 3);
	return 0;
}
