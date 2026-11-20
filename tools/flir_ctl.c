/*
 * flir_ctl.c — run ONE CCI command on a FLIR Boson, print ONE JSON line.
 *
 * Scalar settings come from flir_params[] (`flir_ctl NAME` reads,
 * `flir_ctl NAME VALUE` writes). Other commands are listed in cmds[].
 * Destructive commands (save, factory-reset, reboot) need --confirm.
 *
 * Opens only the CCI serial port (never the V4L2 node), so it works while
 * another process is streaming video.
 */
#define _GNU_SOURCE
#include "flir_adk.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * form[]: accepted argument patterns, one char per argument ("" = none,
 * NULL = unused slot):
 *   b 0|1   A 0..5   z 0..100   R row 0..511   C column 0..639
 *   S splash screen 0..3   M milliseconds 0..60000   I symbol id 0..255
 *   s int16 -32768..32767   x 0..0xFFFFFFFF (0x.. ok)   a text alignment
 *   t text, 1..127 printable ASCII   p absolute path, 1..127 printable ASCII
 */
struct cmd {
	const char *name;
	const char *form[2];
	int confirm;          /* 1 = needs trailing --confirm (flash / reboot) */
	const char *help;
};

static const struct cmd cmds[] = {
	{ "ffc",             { "", NULL },          0, "run flat-field correction now" },
	{ "fpa",             { "", NULL },          0, "FPA temperature, degC" },
	{ "sn",              { "", NULL },          0, "camera serial number" },
	{ "sensor-sn",       { "", NULL },          0, "sensor serial number" },
	{ "pn",              { "", NULL },          0, "camera part number" },
	{ "sensor-pn",       { "", NULL },          0, "sensor part number" },
	{ "fw",              { "", NULL },          0, "firmware revision" },
	{ "status",          { "", NULL },          0, "every readable value as one JSON object" },
	{ "agc",             { "A", NULL },         0, "N  Boson+ agcSetMode 0..5 (plain Boson: status 0x161, use agc-*)" },
	{ "zoom",            { "z", "zCR" },        0, "N [X Y]  e-zoom 0..100 about column X, row Y (default 320 256)" },
	{ "agc-roi",         { "", "RRCC" },        0, "[R0 R1 C0 C1]  camera AGC region" },
	{ "stats-roi",       { "", "RRCC" },        0, "[R0 R1 C0 C1]  image statistics region" },
	{ "splash-duration", { "S", "SM" },         0, "SCREEN [MS]  splash minimum display time" },
	{ "splash-bg",       { "S", "Sx" },         0, "SCREEN [0xRRGGBB]  splash background" },
	{ "ls",              { "", NULL },          0, "list the camera's current directory (read-only)" },
	{ "file-size",       { "p", NULL },         0, "PATH  size of a camera file (read-only)" },
	{ "sym-text",        { "Isssssaxt", NULL }, 0, "ID X Y W H SIZE ALIGN 0xAARRGGBB TEXT" },
	{ "sym-rect",        { "Issssx", NULL },    0, "ID X Y W H COLOR  filled rectangle" },
	{ "sym-box",         { "Issssx", NULL },    0, "ID X Y W H COLOR  outlined rectangle" },
	{ "sym-line",        { "Issssx", NULL },    0, "ID X1 Y1 X2 Y2 COLOR" },
	{ "sym-show",        { "Ib", NULL },        0, "ID 0|1  draw / hide object" },
	{ "sym-delete",      { "I", NULL },         0, "ID" },
	{ "sym-move",        { "Iss", NULL },       0, "ID DX DY  move by offset" },
	{ "save",            { "", NULL },          1, "--confirm  write current settings to flash (power-up defaults)" },
	{ "factory-reset",   { "", NULL },          1, "--confirm  load factory defaults (then save to keep)" },
	{ "reboot",          { "", NULL },          1, "--confirm  reboot the camera" },
};

/* Validate one argument of kind k; numeric value goes to *v. */
static int arg_ok(char k, const char *s, long long *v)
{
	if (k == 't' || k == 'p') {
		size_t n = strlen(s);
		if (n < 1 || n > 127)
			return 0;
		for (size_t i = 0; i < n; i++)
			if (s[i] < 0x20 || s[i] > 0x7e)
				return 0;
		return k == 't' || s[0] == '/';
	}
	char *end;
	errno = 0;
	long long x = strtoll(s, &end, k == 'x' ? 0 : 10);
	if (errno || end == s || *end)
		return 0;
	*v = x;
	switch (k) {
	case 'b': return x == 0 || x == 1;
	case 'A': return x >= 0 && x <= 5;
	case 'z': return x >= 0 && x <= 100;
	case 'R': return x >= 0 && x <= 511;
	case 'C': return x >= 0 && x <= 639;
	case 'S': return x >= 0 && x <= 3;
	case 'M': return x >= 0 && x <= 60000;
	case 'I': return x >= 0 && x <= 255;
	case 's': return x >= -32768 && x <= 32767;
	case 'x': return x >= 0 && x <= 0xFFFFFFFFLL;
	case 'a': return x == 17 || x == 18 || x == 19 || x == 33 || x == 34 ||
			 x == 35 || x == 49 || x == 50 || x == 51;
	}
	return 0;
}

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

/* JSON number for a param value; non-finite floats become null. */
static void print_num(char type, double v)
{
	if (type != 'f')
		printf("%lld", (long long)v);
	else if (isfinite(v))
		printf("%.6g", v);
	else
		printf("null");
}

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && s[n - 1] == ' ')
		s[--n] = '\0';
}

static void print_roi_arr(const uint16_t r[4])
{
	printf("[%u,%u,%u,%u]", (unsigned)r[0], (unsigned)r[1],
	       (unsigned)r[2], (unsigned)r[3]);
}

static void usage(void)
{
	printf(
"Usage: flir_ctl [-c CCI_DEV] COMMAND [ARG...] [--confirm]\n"
"Send one CCI command to a FLIR Boson and print one JSON line on stdout.\n"
"  -c CCI_DEV   CCI serial port (default /dev/ttyACM0)\n"
"  -h, --help   show this help\n"
"Commands:\n");
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		printf("  %-16s %s\n", cmds[i].name, cmds[i].help);
	printf("Settings (NAME reads, NAME VALUE writes, [ro] = read-only):\n");
	for (const struct flir_param_info *p = flir_params; p->name; p++)
		printf("  %-20s %s%s\n", p->name, p->set_id ? "" : "[ro] ", p->help);
	printf("Exit codes: 0 ok, 1 open_failed, 2 usage/unknown_command/bad_value/read_only/confirm_required, 3 cci_failed\n");
}

static int run_param(struct flir_ctx *ctx, const struct flir_param_info *p,
		     int nargs, double v)
{
	if (nargs == 0) {
		double got;
		if (flir_cci_param_get(ctx, p, &got))
			return -1;
		v = got;
	} else if (flir_cci_param_set(ctx, p, v)) {
		return -1;
	}
	printf("{\"ok\":true,\"cmd\":\"%s\",\"value\":", p->name);
	print_num(p->type, v);
	printf("}\n");
	return 0;
}

static void print_ok(const char *name)
{
	printf("{\"ok\":true,\"cmd\":\"%s\"}\n", name);
}

static void print_roi_value(const char *name, const uint16_t r[4])
{
	printf("{\"ok\":true,\"cmd\":\"%s\",\"value\":", name);
	print_roi_arr(r);
	printf("}\n");
}

static int run_ls(struct flir_ctx *ctx)
{
	static char names[256][129];
	size_t n = 0;

	while (n < 256) {
		if (flir_cci_fileops_dir(ctx, names[n], sizeof(names[n])))
			return -1;
		if (!names[n][0])
			break;
		n++;
	}
	printf("{\"ok\":true,\"cmd\":\"ls\",\"value\":[");
	for (size_t i = 0; i < n; i++) {
		if (i)
			putchar(',');
		json_str(names[i]);
	}
	printf("]}\n");
	return 0;
}

/* Prints nothing unless every step succeeds, except per-field nulls. */
static int run_status(struct flir_ctx *ctx)
{
	uint32_t mj, mn, pt;
	if (flir_cci_get_software_rev(ctx, &mj, &mn, &pt))
		return -1;

	printf("{\"ok\":true,\"cmd\":\"status\",\"fw\":\"%lu.%lu.%lu\"",
	       (unsigned long)mj, (unsigned long)mn, (unsigned long)pt);

	uint32_t u;
	printf(",\"sn\":");
	if (!flir_cci_get_serial_number(ctx, &u))
		printf("%lu", (unsigned long)u);
	else
		printf("null");

	printf(",\"sensor-sn\":");
	if (!flir_cci_get_sensor_sn(ctx, &u))
		printf("%lu", (unsigned long)u);
	else
		printf("null");

	char pn[32] = { 0 };
	printf(",\"pn\":");
	if (!flir_cci_get_part_number(ctx, pn, sizeof(pn))) {
		trim(pn);
		json_str(pn);
	} else {
		printf("null");
	}

	char spn[40] = { 0 };
	printf(",\"sensor-pn\":");
	if (!flir_cci_get_sensor_pn(ctx, spn, sizeof(spn))) {
		trim(spn);
		json_str(spn);
	} else {
		printf("null");
	}

	float t;
	printf(",\"fpa\":");
	if (!flir_cci_get_fpa_temp(ctx, &t) && isfinite(t))
		printf("%.1f", (double)t);
	else
		printf("null");

	for (const struct flir_param_info *p = flir_params; p->name; p++) {
		double v;
		if (!p->get_id)
			continue;
		printf(",\"%s\":", p->name);
		if (!flir_cci_param_get(ctx, p, &v))
			print_num(p->type, v);
		else
			printf("null");
	}

	uint16_t roi[4] = { 0 };
	printf(",\"agc-roi\":");
	if (!flir_cci_agc_get_roi(ctx, roi))
		print_roi_arr(roi);
	else
		printf("null");

	printf(",\"stats-roi\":");
	if (!flir_cci_stats_get_roi(ctx, roi))
		print_roi_arr(roi);
	else
		printf("null");

	printf("}\n");
	return 0;
}

static int run_cmd(struct flir_ctx *ctx, const struct cmd *c, char **av,
		   int nargs, const long long *iv)
{
	const char *n = c->name;
	int rc = -1;

	if (!strcmp(n, "ffc")) {
		rc = flir_cci_ffc_trigger(ctx);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "fpa")) {
		float t;
		rc = flir_cci_get_fpa_temp(ctx, &t);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"fpa\",\"value\":%.1f}\n", (double)t);
	} else if (!strcmp(n, "sn")) {
		uint32_t sn;
		rc = flir_cci_get_serial_number(ctx, &sn);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"sn\",\"value\":%lu}\n",
			       (unsigned long)sn);
	} else if (!strcmp(n, "sensor-sn")) {
		uint32_t sn;
		rc = flir_cci_get_sensor_sn(ctx, &sn);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"sensor-sn\",\"value\":%lu}\n",
			       (unsigned long)sn);
	} else if (!strcmp(n, "pn")) {
		char pn[32] = { 0 };
		rc = flir_cci_get_part_number(ctx, pn, sizeof(pn));
		if (!rc) {
			trim(pn);
			printf("{\"ok\":true,\"cmd\":\"pn\",\"value\":");
			json_str(pn);
			printf("}\n");
		}
	} else if (!strcmp(n, "sensor-pn")) {
		char pn[40] = { 0 };
		rc = flir_cci_get_sensor_pn(ctx, pn, sizeof(pn));
		if (!rc) {
			trim(pn);
			printf("{\"ok\":true,\"cmd\":\"sensor-pn\",\"value\":");
			json_str(pn);
			printf("}\n");
		}
	} else if (!strcmp(n, "fw")) {
		uint32_t mj, mn, pt;
		rc = flir_cci_get_software_rev(ctx, &mj, &mn, &pt);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"fw\",\"value\":\"%lu.%lu.%lu\"}\n",
			       (unsigned long)mj, (unsigned long)mn,
			       (unsigned long)pt);
	} else if (!strcmp(n, "status")) {
		rc = run_status(ctx);
	} else if (!strcmp(n, "agc")) {
		rc = flir_cci_set_agc_mode(ctx, (int)iv[0]);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"agc\",\"value\":%lld}\n", iv[0]);
	} else if (!strcmp(n, "zoom")) {
		if (nargs == 1) {
			rc = flir_cci_set_zoom(ctx, (int)iv[0]);
			if (!rc)
				printf("{\"ok\":true,\"cmd\":\"zoom\",\"value\":%lld}\n", iv[0]);
		} else {
			rc = flir_cci_set_zoom_params(ctx, (uint32_t)iv[0],
						      (uint32_t)iv[1], (uint32_t)iv[2]);
			if (!rc)
				printf("{\"ok\":true,\"cmd\":\"zoom\",\"value\":[%lld,%lld,%lld]}\n",
				       iv[0], iv[1], iv[2]);
		}
	} else if (!strcmp(n, "agc-roi") || !strcmp(n, "stats-roi")) {
		int agc = !strcmp(n, "agc-roi");
		uint16_t roi[4] = { 0 };
		if (nargs == 4) {
			for (int i = 0; i < 4; i++)
				roi[i] = (uint16_t)iv[i];
			if (agc)
				rc = flir_cci_agc_set_roi(ctx, roi[0], roi[1], roi[2], roi[3]);
			else
				rc = flir_cci_stats_set_roi(ctx, roi[0], roi[1], roi[2], roi[3]);
		} else {
			if (agc)
				rc = flir_cci_agc_get_roi(ctx, roi);
			else
				rc = flir_cci_stats_get_roi(ctx, roi);
		}
		if (!rc)
			print_roi_value(n, roi);
	} else if (!strcmp(n, "splash-duration")) {
		uint32_t ms = 0;
		uint32_t scr = (uint32_t)iv[0];
		if (nargs == 2) {
			ms = (uint32_t)iv[1];
			rc = flir_cci_splash_set_duration(ctx, scr, ms);
		} else {
			rc = flir_cci_splash_get_duration(ctx, scr, &ms);
		}
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"splash-duration\",\"value\":%lu}\n",
			       (unsigned long)ms);
	} else if (!strcmp(n, "splash-bg")) {
		uint32_t rgb = 0;
		uint32_t scr = (uint32_t)iv[0];
		if (nargs == 2) {
			rgb = (uint32_t)iv[1];
			rc = flir_cci_splash_set_background(ctx, scr, rgb);
		} else {
			rc = flir_cci_splash_get_background(ctx, scr, &rgb);
		}
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"splash-bg\",\"value\":%lu}\n",
			       (unsigned long)rgb);
	} else if (!strcmp(n, "ls")) {
		rc = run_ls(ctx);
	} else if (!strcmp(n, "file-size")) {
		uint32_t sz = 0;
		rc = flir_cci_fileops_file_size(ctx, av[0], &sz);
		if (!rc)
			printf("{\"ok\":true,\"cmd\":\"file-size\",\"value\":%lu}\n",
			       (unsigned long)sz);
	} else if (!strcmp(n, "sym-text")) {
		rc = flir_cci_sym_create_text(ctx, (uint8_t)iv[0], (int16_t)iv[1],
					      (int16_t)iv[2], (int16_t)iv[3],
					      (int16_t)iv[4], (int16_t)iv[5],
					      (int16_t)iv[6], (uint32_t)iv[7], av[8]);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "sym-rect") || !strcmp(n, "sym-box")) {
		rc = flir_cci_sym_create_rect(ctx, (uint8_t)iv[0], (int16_t)iv[1],
					      (int16_t)iv[2], (int16_t)iv[3],
					      (int16_t)iv[4], (uint32_t)iv[5],
					      !strcmp(n, "sym-rect"));
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "sym-line")) {
		rc = flir_cci_sym_create_line(ctx, (uint8_t)iv[0], (int16_t)iv[1],
					      (int16_t)iv[2], (int16_t)iv[3],
					      (int16_t)iv[4], (uint32_t)iv[5]);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "sym-show")) {
		rc = flir_cci_sym_show(ctx, (uint8_t)iv[0], (int)iv[1]);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "sym-delete")) {
		rc = flir_cci_sym_delete(ctx, (uint8_t)iv[0]);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "sym-move")) {
		rc = flir_cci_sym_move_by(ctx, (uint8_t)iv[0], (int16_t)iv[1],
					  (int16_t)iv[2]);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "save")) {
		rc = flir_cci_save_settings(ctx);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "factory-reset")) {
		rc = flir_cci_restore_factory_defaults(ctx);
		if (!rc)
			print_ok(n);
	} else if (!strcmp(n, "reboot")) {
		rc = flir_cci_reboot(ctx);
		if (rc && flir_cci_last_status() == 0xFFFFFFFFu)
			rc = 0; /* camera may reset before replying */
		if (!rc)
			print_ok(n);
	}
	return rc;
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
	while ((opt = getopt(argc, argv, "+c:")) != -1) {
		if (opt == 'c')
			cci = optarg;
		else
			fail("", "usage", 2);
	}

	if (optind >= argc)
		fail("", "usage", 2);

	const char *name = argv[optind];
	char **av = argv + optind + 1;
	int nargs = argc - optind - 1;
	int confirm = 0;
	if (nargs > 0 && !strcmp(av[nargs - 1], "--confirm")) {
		confirm = 1;
		nargs--;
	}

	const struct flir_param_info *p = flir_param_find(name);
	const struct cmd *c = NULL;
	if (!p) {
		for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
			if (!strcmp(cmds[i].name, name))
				c = &cmds[i];
		if (!c)
			fail(name, "unknown_command", 2);
	}

	if (confirm && (!c || !c->confirm))
		fail(name, "usage", 2);
	if (c && c->confirm && !confirm)
		fail(name, "confirm_required", 2);

	double v = 0;
	long long iv[9] = { 0 };
	if (p) {
		if (nargs > 1)
			fail(name, "usage", 2);
		if (nargs == 1) {
			if (!p->set_id)
				fail(name, "read_only", 2);
			char *end;
			errno = 0;
			v = strtod(av[0], &end);
			if (errno || end == av[0] || *end)
				fail(name, "bad_value", 2);
			if (!(v >= p->lo && v <= p->hi) ||
			    (p->type != 'f' && v != (double)(long long)v))
				fail(name, "bad_value", 2);
		}
	} else {
		const char *form = NULL;
		for (int k = 0; k < 2; k++)
			if (c->form[k] && strlen(c->form[k]) == (size_t)nargs) {
				form = c->form[k];
				break;
			}
		if (!form)
			fail(name, "usage", 2);
		for (int i = 0; i < nargs; i++)
			if (!arg_ok(form[i], av[i], &iv[i]))
				fail(name, "bad_value", 2);
		if (!strcmp(form, "RRCC") &&
		    (iv[0] > iv[1] || iv[2] > iv[3]))
			fail(name, "bad_value", 2);
	}

	struct flir_ctx *ctx = flir_open(NULL, cci, FLIR_MODE_RAW16);
	if (!ctx)
		fail(name, "open_failed", 1);

	int rc = p ? run_param(ctx, p, nargs, v) : run_cmd(ctx, c, av, nargs, iv);

	flir_close(ctx);
	if (rc)
		fail(name, "cci_failed", 3);
	return 0;
}
