/* Unit tests for the portable core of headroom.c (same expectations as the
 * Python and GNOME versions).  Build & run:  cc -std=c11 -DHR_TEST test_core.c -lm && ./a.out */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "headroom.c"

static int fails;
static void check_ws(const WCHAR *got, const char *want, int line) {
	char g[128];
	int i = 0;
	for (; got[i] && i < 127; i++) g[i] = got[i] < 128 ? (char)got[i] : '?';
	g[i] = 0;
	if (strcmp(g, want)) printf("line %d: got \"%s\" want \"%s\"\n", line, g, want), fails++;
}
#define CHECK_WS(got, want) check_ws(got, want, __LINE__)
#define CHECK(cond) do { if (!(cond)) printf("line %d: %s\n", __LINE__, #cond), fails++; } while (0)

static int series(Sample *h, double (*f)(double)) {
	int n = 0;
	for (double t = 0; t < 30; t += 2) h[n].t = t, h[n].f = f(t), n++;
	return n;
}
static double steady(double t) { return 1e12 - 100 * MiB * t; }
static double at_;
static double copy10(double t) { return t >= at_ ? 500 * GiB - 10 * GiB : 500 * GiB; }
static double started(double t) { return 500 * GiB - fmax(0, t - 10) * 100 * MiB; }

int main(void) {
	WCHAR b[64];
	fmt_size(28 * GiB, b, 64); CHECK_WS(b, "28.0 GB");
	fmt_size(686 * GiB, b, 64); CHECK_WS(b, "686 GB");
	fmt_size(3.65 * TiB, b, 64); CHECK_WS(b, "3.65 TB");
	fmt_size(850 * MiB, b, 64); CHECK_WS(b, "850 MB");
	fmt_size(8.534 * GiB, b, 64); CHECK_WS(b, "8.53 GB");
	fmt_size(999.7 * GiB, b, 64); CHECK_WS(b, "0.98 TB");
	fmt_size(695 * TiB, b, 64); CHECK_WS(b, "695 TB");
	fmt_size(0, b, 64); CHECK_WS(b, "0.00 MB");
	fmt_rate(85.2 * MiB, b, 64); CHECK_WS(b, "85.2 MB/s");
	fmt_rate(512 * MiB, b, 64); CHECK_WS(b, "512 MB/s");
	fmt_rate(1.2 * GiB, b, 64); CHECK_WS(b, "1.2 GB/s");
	fmt_rate(0.3 * MiB, b, 64); CHECK_WS(b, "0.3 MB/s");
	double etas[] = {42, 148, 45 * 60, 2 * 3600 + 13 * 60, 50 * 3600, 200 * 86400};
	const char *ew[] = {"42s", "2m28s", "45m", "2h13m", "2d2h", ">99d"};
	for (int i = 0; i < 6; i++) fmt_eta(etas[i], b, 64), CHECK_WS(b, ew[i]);
	short_path(u"/data", b, 22, 64); CHECK_WS(b, "/data");
	short_path(u"/media/cbsuruizfs1/storage", b, 22, 64); CHECK_WS(b, "?/cbsuruizfs1/storage");
	norm_key(u"d:", b, 64); CHECK_WS(b, "d:\\");
	norm_key(u"D:\\Rec\\", b, 64); CHECK_WS(b, "d:\\rec");
	norm_key(u"C:\\", b, 64); CHECK_WS(b, "c:\\");

	Sample h[64];
	double r;
	int n = series(h, steady);
	CHECK(write_rate(h, n, &r) && fabs(r - 100 * MiB) < 1);
	double ats[] = {0, 8, 14, 20, 28};
	for (int i = 0; i < 5; i++) {
		at_ = ats[i], n = series(h, copy10);
		CHECK(write_rate(h, n, &r) && fabs(r) < 1);
	}
	CHECK(!write_rate(h, 4, &r));
	n = series(h, started);
	CHECK(write_rate(h, n, &r) && fabs(r - 100 * MiB) < 1);

	Config c;
	config_defaults(&c);
#define L(free, rate) level_of(free, 10 * TiB, rate, (rate) > 0, &c)
	CHECK(L(500 * GiB, 0) == LV_OK && L(99 * GiB, 0) == LV_WARN && L(19 * GiB, 0) == LV_CRIT);
	CHECK(L(500 * GiB, 100 * MiB) == LV_OK && L(300 * GiB, 100 * MiB) == LV_WARN && L(80 * GiB, 100 * MiB) == LV_CRIT);
	CHECK(L(500 * GiB, 100 * 1024) == LV_OK && level_of(0, 0, 0, 0, &c) == LV_NA);
	c.warn_min = c.crit_min = 0;
	CHECK(L(150 * GiB, 1 * GiB) == LV_OK);

	const char *bad = "{not json";
	CHECK(!config_parse((const unsigned char *)bad, (int)strlen(bad), &c) && c.interval_s == 2);
	const char *lenient = "\xEF\xBB\xBF{\"drives\": \" D: \", \"interval_s\": 0.01, \"sound\": \"yes\", \"opacity\": 3,"
	                      " \"x\": 10.7, \"y\": null, \"warn_gb\": true, \"mode\": \"floating\", \"bogus\": {\"a\": [1, {\"b\": null}]},"
	                      " \"crit_min\": 1e1}";
	CHECK(config_parse((const unsigned char *)lenient, (int)strlen(lenient), &c));
	CHECK(c.ndrives == 1 && c.interval_s == 0.5 && c.sound == 1 && c.opacity == 1 && c.has_x && c.x == 10.7);
	CHECK(!c.has_y && c.warn_gb == 100 && c.floating == 1 && c.crit_min == 10);
	CHECK_WS(c.drives[0], "D:");
	const char *list = "{\"drives\": [\"C:\", \"E:\\\\Recordings\", \"\\u00e9t\\u00e9\"], \"taskbar_offset\": 564}";
	CHECK(config_parse((const unsigned char *)list, (int)strlen(list), &c) && c.ndrives == 3 && c.has_offset && c.taskbar_offset == 564);
	CHECK_WS(c.drives[1], "E:\\Recordings");
	CHECK(c.drives[2][0] == 0xE9 && c.drives[2][2] == 0xE9);
	const char *mixed = "{\"drives\": [\"C:\", 5]}"; /* not all strings: ignored, like the Python version */
	CHECK(config_parse((const unsigned char *)mixed, (int)strlen(mixed), &c) && c.ndrives == 0);

	/* volume history: window trimming, ETA, failure reset, title */
	static Vol v;
	for (double t = 0; t <= 40; t += 2) vol_store(&v, t, 4 * TiB, 1 * TiB - 100 * MiB * t, 30);
	double eta;
	CHECK(v.nh == 16 && v.hist[0].t == 10);
	CHECK(vol_eta(&v, &eta) && fabs(eta - (1 * TiB - 4000 * MiB) / (100 * MiB)) < 1);
	vol_fail(&v, u"no media");
	CHECK(!v.has_rate && v.nh == 0 && v.err[0]);
	ws_cpy(v.name, u"D:", 48), ws_cpy(v.label, u"A very long volume label", 48);
	vol_title(&v, b, 64); CHECK_WS(b, "D:  A very long vo");
	CHECK(ws_ieq(u"C:\\Data", u"c:\\DATA") && !ws_ieq(u"C:", u"D:"));

	/* round trip through the writer */
	static unsigned char out[8192];
	static Config c2;
	config_parse((const unsigned char *)list, (int)strlen(list), &c);
	c.opacity = 0.92, c.has_x = 1, c.x = -1280, c.sound = 0;
	int len = config_write(&c, out, sizeof out);
	CHECK(config_parse(out, len, &c2));
	CHECK(c2.ndrives == 3 && c2.opacity == 0.92 && c2.has_x && c2.x == -1280 && !c2.sound && c2.has_offset);
	CHECK(c2.drives[2][0] == 0xE9);
	if (fails) printf("%d FAILED\n", fails); else printf("all C core tests passed\n");
	puts((char *)out);
	return fails != 0;
}
