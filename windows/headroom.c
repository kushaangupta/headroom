/* Headroom for Windows -- free disk space, write rate and time-until-full,
 * shown on the taskbar (next to the tray, like the weather) or in a small
 * floating window.  Made for low-end data-acquisition PCs:
 *
 *  - never scans files: one GetDiskFreeSpaceEx per drive per interval reads the
 *    volume's in-memory free-space counter (no disk I/O);
 *  - plain Win32 C with no runtime: between updates the process sleeps in
 *    GetMessage; one coalescable timer; redraws only when a shown value changes;
 *  - below-normal priority + EcoQoS efficiency mode; never takes keyboard focus.
 *
 * Self-contained: declares the few Win32 functions it uses and needs no C
 * runtime.  Build with build.sh (clang + lld-link) or see README.md.
 * With -DHR_TEST only the portable core is compiled (for unit tests on any OS).
 */

typedef unsigned short WCHAR; /* UTF-16 code unit */
typedef long long i64;
typedef unsigned long long u64;

#define MAX_VOLS 32
#define MAX_HIST 512
#define PATHLEN 260

#define MiB 1048576.0
#define GiB 1073741824.0
#define TiB 1099511627776.0
#define PiB 1125899906842624.0
#define IDLE_RATE (256.0 * 1024.0) /* bytes/s; a slower net change counts as "idle" */

enum { LV_NA = -1, LV_OK = 0, LV_WARN = 1, LV_CRIT = 2 };
enum { K_LOCAL, K_REMOVABLE, K_NETWORK, K_FOLDER };

/* ------------------------------------------------------------ strings */

static int ws_len(const WCHAR *s) {
	int n = 0;
	while (s[n]) n++;
	return n;
}

static void ws_cpy(WCHAR *d, const WCHAR *s, int cap) {
	int i = 0;
	for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

static void ws_cat(WCHAR *d, const WCHAR *s, int cap) {
	int n = ws_len(d);
	if (n < cap) ws_cpy(d + n, s, cap - n);
}

static int ws_eq(const WCHAR *a, const WCHAR *b) {
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}

static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ws_ieq(const WCHAR *a, const WCHAR *b) {
	while (*a && lower(*a) == lower(*b)) a++, b++;
	return lower(*a) == lower(*b);
}

static void ws_int(WCHAR *d, i64 v, int cap) {
	WCHAR t[24];
	int n = 0, neg = v < 0;
	u64 u = neg ? (u64)(-v) : (u64)v;
	do t[n++] = (WCHAR)('0' + u % 10), u /= 10; while (u);
	if (neg) t[n++] = '-';
	int len = ws_len(d);
	while (n && len < cap - 1) d[len++] = t[--n];
	d[len] = 0;
}

/* append v with a fixed number of decimals (round half up) */
static void ws_fixed(WCHAR *d, double v, int decimals, int cap) {
	i64 scale = 1;
	for (int i = 0; i < decimals; i++) scale *= 10;
	if (v < 0) { ws_cat(d, u"-", cap); v = -v; }
	i64 x = (i64)(v * (double)scale + 0.5);
	ws_int(d, x / scale, cap);
	if (decimals) {
		WCHAR frac[24];
		frac[0] = '.';
		i64 f = x % scale;
		for (int i = decimals; i > 0; i--) frac[i] = (WCHAR)('0' + f % 10), f /= 10;
		frac[decimals + 1] = 0;
		ws_cat(d, frac, cap);
	}
}

/* ---------------------------------------------------------- formatting */

static void fixed_unit(WCHAR *out, int cap, double v, const WCHAR *unit) {
	out[0] = 0;
	ws_fixed(out, v, v < 9.995 ? 2 : v < 99.95 ? 1 : 0, cap);
	ws_cat(out, u" ", cap);
	ws_cat(out, unit, cap);
}

static void fmt_size(double n, WCHAR *out, int cap) {
	double v = n / MiB;
	const WCHAR *u = u"MB";
	if (v >= 999.5) v = n / GiB, u = u"GB";
	if (v >= 999.5) v = n / TiB, u = u"TB";
	if (v >= 999.5) v = n / PiB, u = u"PB";
	fixed_unit(out, cap, v, u);
}

static void fmt_rate(double r, WCHAR *out, int cap) {
	double v = r / MiB;
	const WCHAR *u = u"MB/s";
	if (v >= 999.5) v = r / GiB, u = u"GB/s";
	out[0] = 0;
	ws_fixed(out, v, v < 99.95 ? 1 : 0, cap);
	ws_cat(out, u" ", cap);
	ws_cat(out, u, cap);
}

static void fmt_eta(double seconds, WCHAR *out, int cap) {
	i64 s = (i64)seconds, m, h, d;
	out[0] = 0;
	if (s < 60) { ws_int(out, s, cap); ws_cat(out, u"s", cap); return; }
	m = s / 60;
	if (m < 10) {
		ws_int(out, m, cap); ws_cat(out, u"m", cap);
		if (s % 60 < 10) ws_cat(out, u"0", cap);
		ws_int(out, s % 60, cap); ws_cat(out, u"s", cap);
		return;
	}
	if (m < 60) { ws_int(out, m, cap); ws_cat(out, u"m", cap); return; }
	h = m / 60;
	if (h < 48) {
		ws_int(out, h, cap); ws_cat(out, u"h", cap);
		if (m % 60 < 10) ws_cat(out, u"0", cap);
		ws_int(out, m % 60, cap); ws_cat(out, u"m", cap);
		return;
	}
	d = h / 24;
	if (d >= 100) { ws_cpy(out, u">99d", cap); return; }
	ws_int(out, d, cap); ws_cat(out, u"d", cap);
	ws_int(out, h % 24, cap); ws_cat(out, u"h", cap);
}

/* "…/last/two" when a path is longer than n characters */
static void short_path(const WCHAR *p, WCHAR *out, int n, int cap) {
	int len = ws_len(p), end = len, slashes = 0, start = 0;
	if (len <= n) { ws_cpy(out, p, cap); return; }
	while (end > 0 && (p[end - 1] == '\\' || p[end - 1] == '/')) end--;
	for (int i = end - 1; i >= 0; i--)
		if (p[i] == '\\' || p[i] == '/') {
			if (++slashes == 2) { start = i + 1; break; }
		}
	WCHAR t[PATHLEN + 4] = u"…/";
	int k = 2;
	for (int i = start; i < end && k < PATHLEN; i++) t[k++] = p[i] == '\\' ? '/' : p[i];
	t[k] = 0;
	if (k <= n) { ws_cpy(out, t, cap); return; }
	out[0] = 0x2026;
	ws_cpy(out + 1, t + k - (n - 1), cap - 1);
}

/* Windows path comparison key: case-insensitive, "D:" == "D:\" */
static void norm_key(const WCHAR *p, WCHAR *out, int cap) {
	while (*p == ' ') p++;
	int n = 0;
	for (; p[n] && n < cap - 2; n++) out[n] = p[n] == '/' ? '\\' : lower(p[n]);
	while (n > 0 && out[n - 1] == ' ') n--;
	out[n] = 0;
	if (n == 2 && out[1] == ':') out[n++] = '\\', out[n] = 0;
	while (n > 3 && out[n - 1] == '\\') out[--n] = 0;
}

/* --------------------------------------------------------- measurement */

typedef struct { double t, f; } Sample;

static int slope(const Sample *p, int n, double *out) {
	double mt = 0, mf = 0, sxx = 0, sxy = 0;
	for (int i = 0; i < n; i++) mt += p[i].t, mf += p[i].f;
	mt /= n, mf /= n;
	for (int i = 0; i < n; i++) {
		sxx += (p[i].t - mt) * (p[i].t - mt);
		sxy += (p[i].t - mt) * (p[i].f - mf);
	}
	if (sxx <= 0) return 0;
	*out = sxy / sxx;
	return 1;
}

/* Sustained space consumption in bytes/s: median of the least-squares trends of
 * the window's three thirds, so a recording shows its true rate while a
 * one-off file copy doesn't look like one. */
static int write_rate(const Sample *h, int n, double *rate) {
	double s[3], t;
	if (n < 5 || h[n - 1].t - h[0].t < 3) return 0;
	int i = n / 3, j = 2 * n / 3;
	if (!slope(h, i + 1, &s[0]) || !slope(h + i, j - i + 1, &s[1]) || !slope(h + j, n - j, &s[2]))
		return 0;
	if (s[0] > s[1]) t = s[0], s[0] = s[1], s[1] = t;
	if (s[1] > s[2]) t = s[1], s[1] = s[2], s[2] = t;
	if (s[0] > s[1]) t = s[0], s[0] = s[1], s[1] = t;
	*rate = -s[1];
	return 1;
}

/* ------------------------------------------------------------- config */

typedef struct {
	int ndrives;
	WCHAR drives[MAX_VOLS][PATHLEN]; /* none = every local drive (auto) */
	int include_network;             /* auto mode: also show network drives */
	double interval_s, warn_gb, crit_gb, warn_min, crit_min, rate_window_s;
	int show_rate, sound;
	double alarm_repeat_s, font_size, opacity;
	int has_x, has_y, has_offset;
	double x, y, taskbar_offset; /* taskbar_offset: px from the taskbar's right end */
	int floating;                /* "mode": 0 = taskbar, 1 = floating window */
	int hide_fullscreen;         /* hide while a full-screen app covers the monitor */
} Config;

static void config_defaults(Config *c) {
	c->ndrives = 0;
	c->include_network = 0;
	c->interval_s = 2, c->warn_gb = 100, c->crit_gb = 20, c->warn_min = 60, c->crit_min = 15;
	c->rate_window_s = 30, c->show_rate = 1, c->sound = 1, c->alarm_repeat_s = 30;
	c->font_size = 11, c->opacity = 0.92;
	c->has_x = c->has_y = c->has_offset = 0;
	c->x = c->y = c->taskbar_offset = 0;
	c->floating = 0, c->hide_fullscreen = 1;
}

static int eta_level_min(double minutes, const Config *c) {
	return minutes < c->crit_min ? LV_CRIT : minutes < c->warn_min ? LV_WARN : LV_OK;
}

static int level_of(double free_, double total, double rate, int has_rate, const Config *c) {
	if (total <= 0) return LV_NA;
	double gb = free_ / GiB;
	int lvl = gb < c->crit_gb ? LV_CRIT : gb < c->warn_gb ? LV_WARN : LV_OK;
	if (has_rate && rate > IDLE_RATE) {
		int e = eta_level_min(free_ / rate / 60, c);
		if (e > lvl) lvl = e;
	}
	return lvl;
}

/* --- tiny JSON reader for the flat settings object (lenient, like headroom.py) */

typedef struct { const unsigned char *s; int n, i; } JP;

static void jws(JP *p) {
	while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
		p->i++;
}

static int jpeek(JP *p) { jws(p); return p->i < p->n ? p->s[p->i] : -1; }

static int jlit(JP *p, const char *w) {
	int k = 0;
	while (w[k] && p->i + k < p->n && p->s[p->i + k] == (unsigned char)w[k]) k++;
	if (w[k]) return 0;
	p->i += k;
	return 1;
}

static int hexval(int c) {
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* JSON string (UTF-8) -> UTF-16; truncates silently at cap */
static int jstr(JP *p, WCHAR *out, int cap) {
	int k = 0;
	if (jpeek(p) != '"') return 0;
	p->i++;
	while (p->i < p->n) {
		unsigned c = p->s[p->i++];
		if (c == '"') { out[k < cap ? k : cap - 1] = 0; return 1; }
		if (c < 0x20) return 0;
		if (c == '\\') {
			if (p->i >= p->n) return 0;
			c = p->s[p->i++];
			switch (c) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = 8; break;
			case 'f': c = 12; break;
			case 'u': {
				if (p->i + 4 > p->n) return 0;
				unsigned v = 0;
				for (int q = 0; q < 4; q++) {
					int h = hexval(p->s[p->i++]);
					if (h < 0) return 0;
					v = v * 16 + (unsigned)h;
				}
				c = v;
				break;
			}
			case '"': case '\\': case '/': break;
			default: return 0;
			}
		} else if (c >= 0x80) { /* UTF-8 sequence */
			int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
			c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : 0x1F;
			for (int q = 0; q < extra && p->i < p->n; q++) c = (c << 6) | (p->s[p->i++] & 0x3F);
			if (c >= 0x10000) { /* surrogate pair */
				c -= 0x10000;
				if (k < cap - 1) out[k++] = (WCHAR)(0xD800 + (c >> 10));
				c = 0xDC00 + (c & 0x3FF);
			}
		}
		if (k < cap - 1) out[k++] = (WCHAR)c;
	}
	return 0;
}

static int jnum(JP *p, double *out) {
	double v = 0, frac = 0.1;
	int neg = 0, digits = 0, ex = 0, exneg = 0;
	jws(p);
	if (p->i < p->n && p->s[p->i] == '-') neg = 1, p->i++;
	while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') v = v * 10 + (p->s[p->i++] - '0'), digits++;
	if (p->i < p->n && p->s[p->i] == '.') {
		p->i++;
		while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') v += (p->s[p->i++] - '0') * frac, frac /= 10, digits++;
	}
	if (!digits) return 0;
	if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
		p->i++;
		if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) exneg = p->s[p->i++] == '-';
		while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9' && ex < 400) ex = ex * 10 + (p->s[p->i++] - '0');
		while (ex--) v = exneg ? v / 10 : v * 10;
	}
	*out = neg ? -v : v;
	return 1;
}

static int jskip(JP *p, int depth) {
	int c = jpeek(p);
	WCHAR dummy[2];
	double d;
	if (depth > 32) return 0;
	if (c == '"') return jstr(p, dummy, 2);
	if (c == '{' || c == '[') {
		int close = c == '{' ? '}' : ']';
		p->i++;
		if (jpeek(p) == close) { p->i++; return 1; }
		for (;;) {
			if (c == '{') {
				if (!jstr(p, dummy, 2) || jpeek(p) != ':') return 0;
				p->i++;
			}
			if (!jskip(p, depth + 1)) return 0;
			int q = jpeek(p);
			p->i++;
			if (q == close) return 1;
			if (q != ',') return 0;
		}
	}
	if (jlit(p, "true") || jlit(p, "false") || jlit(p, "null")) return 1;
	return jnum(p, &d);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static int streq(const char *a, const char *b) {
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}

/* Parse settings; unknown keys and wrong-typed values are ignored.  Returns 0 on a
 * syntax error (c then holds defaults). */
static int config_parse(const unsigned char *s, int n, Config *c) {
	JP p = {s, n, 0};
	char key[32];
	config_defaults(c);
	if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) p.i = 3; /* UTF-8 BOM (Notepad) */
	if (jpeek(&p) != '{') return 0;
	p.i++;
	if (jpeek(&p) == '}') return 1;
	for (;;) {
		WCHAR wkey[32];
		if (!jstr(&p, wkey, 32) || jpeek(&p) != ':') return 0;
		p.i++;
		for (int k = 0; k < 32; k++) key[k] = (char)wkey[k];
		int c0 = jpeek(&p);
		int isnum = c0 == '-' || (c0 >= '0' && c0 <= '9');
		struct { const char *k; double *v; double lo, hi; } nums[] = {
			{"interval_s", &c->interval_s, 0.5, 3600}, {"warn_gb", &c->warn_gb, 0, 1e9},
			{"crit_gb", &c->crit_gb, 0, 1e9}, {"warn_min", &c->warn_min, 0, 1e6},
			{"crit_min", &c->crit_min, 0, 1e6}, {"rate_window_s", &c->rate_window_s, 5, 3600},
			{"alarm_repeat_s", &c->alarm_repeat_s, 2, 86400}, {"font_size", &c->font_size, 6, 72},
			{"opacity", &c->opacity, 0.2, 1}};
		struct { const char *k; int *v; } flags[] = {
			{"include_network", &c->include_network}, {"show_rate", &c->show_rate},
			{"sound", &c->sound}, {"hide_fullscreen", &c->hide_fullscreen}};
		struct { const char *k; int *has; double *v; } opts[] = {
			{"x", &c->has_x, &c->x}, {"y", &c->has_y, &c->y}, {"taskbar_offset", &c->has_offset, &c->taskbar_offset}};
		int done = 0;
#define KEYIS(name) streq(key, name)
		for (unsigned q = 0; !done && q < sizeof nums / sizeof *nums; q++)
			if (KEYIS(nums[q].k) && isnum) {
				double v;
				if (!jnum(&p, &v)) return 0;
				*nums[q].v = clampd(v, nums[q].lo, nums[q].hi);
				done = 1;
			}
		for (unsigned q = 0; !done && q < sizeof flags / sizeof *flags; q++)
			if (KEYIS(flags[q].k) && (c0 == 't' || c0 == 'f')) {
				if (jlit(&p, "true")) *flags[q].v = 1;
				else if (jlit(&p, "false")) *flags[q].v = 0;
				else return 0;
				done = 1;
			}
		for (unsigned q = 0; !done && q < sizeof opts / sizeof *opts; q++)
			if (KEYIS(opts[q].k) && isnum) {
				if (!jnum(&p, opts[q].v)) return 0;
				*opts[q].has = done = 1;
			}
		if (!done && KEYIS("mode") && c0 == '"') {
			WCHAR m[16];
			if (!jstr(&p, m, 16)) return 0;
			if (ws_eq(m, u"taskbar")) c->floating = 0;
			if (ws_eq(m, u"floating")) c->floating = 1;
			done = 1;
		}
		if (!done && KEYIS("drives") && (c0 == '"' || c0 == '[')) {
			static WCHAR tmp[MAX_VOLS][PATHLEN];
			int n_ok = 0, all_str = 1;
			if (c0 == '"') {
				if (!jstr(&p, tmp[0], PATHLEN)) return 0;
				n_ok = 1;
			} else {
				p.i++;
				if (jpeek(&p) == ']') p.i++;
				else for (;;) {
					if (jpeek(&p) == '"') {
						WCHAR *dst = tmp[n_ok < MAX_VOLS ? n_ok : MAX_VOLS - 1];
						if (!jstr(&p, dst, PATHLEN)) return 0;
						if (n_ok < MAX_VOLS) n_ok++;
					} else {
						all_str = 0;
						if (!jskip(&p, 1)) return 0;
					}
					int q = jpeek(&p);
					p.i++;
					if (q == ']') break;
					if (q != ',') return 0;
				}
			}
			if (all_str) {
				c->ndrives = 0;
				for (int q = 0; q < n_ok; q++) {
					WCHAR *src = tmp[q];
					while (*src == ' ') src++;
					int l = ws_len(src);
					while (l && src[l - 1] == ' ') src[--l] = 0;
					if (l) ws_cpy(c->drives[c->ndrives++], src, PATHLEN);
				}
			}
			done = 1;
		}
#undef KEYIS
		if (!done && !jskip(&p, 1)) return 0;
		int q = jpeek(&p);
		p.i++;
		if (q == '}') return 1;
		if (q != ',') return 0;
	}
}

/* --- settings writer (UTF-8 JSON; same keys and order as headroom.py) */

typedef struct { unsigned char *b; int n, cap; } Buf;

static void bput(Buf *o, const char *s) {
	while (*s && o->n < o->cap - 1) o->b[o->n++] = (unsigned char)*s++;
	o->b[o->n] = 0;
}

static void bput_ws(Buf *o, const WCHAR *s) { /* UTF-16 -> escaped UTF-8 JSON string */
	bput(o, "\"");
	for (; *s && o->n < o->cap - 8; s++) {
		unsigned c = *s;
		if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000)
			c = 0x10000 + ((c - 0xD800) << 10) + (*++s - 0xDC00);
		if (c == '"' || c == '\\') o->b[o->n++] = '\\', o->b[o->n++] = (unsigned char)c;
		else if (c < 0x20) {
			const char *hx = "0123456789abcdef";
			o->b[o->n++] = '\\', o->b[o->n++] = 'u', o->b[o->n++] = '0', o->b[o->n++] = '0';
			o->b[o->n++] = (unsigned char)hx[c >> 4], o->b[o->n++] = (unsigned char)hx[c & 15];
		} else if (c < 0x80) o->b[o->n++] = (unsigned char)c;
		else if (c < 0x800) o->b[o->n++] = (unsigned char)(0xC0 | c >> 6), o->b[o->n++] = (unsigned char)(0x80 | (c & 0x3F));
		else if (c < 0x10000) {
			o->b[o->n++] = (unsigned char)(0xE0 | c >> 12), o->b[o->n++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
			o->b[o->n++] = (unsigned char)(0x80 | (c & 0x3F));
		} else {
			o->b[o->n++] = (unsigned char)(0xF0 | c >> 18), o->b[o->n++] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
			o->b[o->n++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)), o->b[o->n++] = (unsigned char)(0x80 | (c & 0x3F));
		}
	}
	o->b[o->n] = 0;
	bput(o, "\"");
}

static void bput_num(Buf *o, double v) {
	WCHAR t[40] = {0};
	char a[40];
	int i = 0;
	if (v == (double)(i64)v && v < 1e15 && v > -1e15) ws_int(t, (i64)v, 40);
	else {
		ws_fixed(t, v, 6, 40);
		int n = ws_len(t);
		while (n > 1 && t[n - 1] == '0') t[--n] = 0;
		if (n && t[n - 1] == '.') t[--n] = 0;
	}
	for (; t[i] && i < 39; i++) a[i] = (char)t[i];
	a[i] = 0;
	bput(o, a);
}

static int config_write(const Config *c, unsigned char *out, int cap) {
	Buf o = {out, 0, cap};
	bput(&o, "{\n  \"drives\": [");
	for (int i = 0; i < c->ndrives; i++) {
		bput(&o, i ? ", " : "");
		bput_ws(&o, c->drives[i]);
	}
	bput(&o, "],\n  \"include_network\": "); bput(&o, c->include_network ? "true" : "false");
	bput(&o, ",\n  \"interval_s\": "); bput_num(&o, c->interval_s);
	bput(&o, ",\n  \"warn_gb\": "); bput_num(&o, c->warn_gb);
	bput(&o, ",\n  \"crit_gb\": "); bput_num(&o, c->crit_gb);
	bput(&o, ",\n  \"warn_min\": "); bput_num(&o, c->warn_min);
	bput(&o, ",\n  \"crit_min\": "); bput_num(&o, c->crit_min);
	bput(&o, ",\n  \"rate_window_s\": "); bput_num(&o, c->rate_window_s);
	bput(&o, ",\n  \"show_rate\": "); bput(&o, c->show_rate ? "true" : "false");
	bput(&o, ",\n  \"sound\": "); bput(&o, c->sound ? "true" : "false");
	bput(&o, ",\n  \"alarm_repeat_s\": "); bput_num(&o, c->alarm_repeat_s);
	bput(&o, ",\n  \"font_size\": "); bput_num(&o, c->font_size);
	bput(&o, ",\n  \"opacity\": "); bput_num(&o, c->opacity);
	bput(&o, ",\n  \"x\": "); if (c->has_x) bput_num(&o, c->x); else bput(&o, "null");
	bput(&o, ",\n  \"y\": "); if (c->has_y) bput_num(&o, c->y); else bput(&o, "null");
	bput(&o, ",\n  \"mode\": "); bput(&o, c->floating ? "\"floating\"" : "\"taskbar\"");
	bput(&o, ",\n  \"taskbar_offset\": "); if (c->has_offset) bput_num(&o, c->taskbar_offset); else bput(&o, "null");
	bput(&o, ",\n  \"hide_fullscreen\": "); bput(&o, c->hide_fullscreen ? "true" : "false");
	bput(&o, "\n}\n");
	return o.n;
}

/* ---------------------------------------------------------- volumes */

typedef struct {
	WCHAR path[PATHLEN], name[48], label[48], err[24];
	int kind, fast, used;
	double total, free_;
	Sample hist[MAX_HIST];
	int nh, has_rate, level, inflight, label_tried;
	double rate, sent;
} Vol;

static void vol_store(Vol *v, double t, double total, double free_, double window) {
	v->err[0] = 0, v->total = total, v->free_ = free_;
	if (v->nh == MAX_HIST) {
		for (int i = 1; i < MAX_HIST; i++) v->hist[i - 1] = v->hist[i];
		v->nh--;
	}
	v->hist[v->nh].t = t, v->hist[v->nh].f = free_, v->nh++;
	int k = 0;
	while (k < v->nh && t - v->hist[k].t > window) k++;
	if (k) {
		for (int i = k; i < v->nh; i++) v->hist[i - k] = v->hist[i];
		v->nh -= k;
	}
	v->has_rate = write_rate(v->hist, v->nh, &v->rate);
}

static void vol_fail(Vol *v, const WCHAR *msg) {
	ws_cpy(v->err, msg, 24);
	v->has_rate = 0, v->nh = 0;
}

static int vol_eta(const Vol *v, double *eta) {
	if (v->has_rate && v->rate > IDLE_RATE) { *eta = v->free_ / v->rate; return 1; }
	return 0;
}

static void vol_title(const Vol *v, WCHAR *out, int cap) {
	ws_cpy(out, v->name, cap);
	if (v->label[0]) {
		WCHAR l[15];
		ws_cpy(l, v->label, 15);
		ws_cat(out, u"  ", cap);
		ws_cat(out, l, cap);
	}
}

#ifndef HR_TEST
/* =================================================================== Win32 */

#define WINAPI __stdcall
#define IMP __declspec(dllimport)
typedef void *HANDLE, *HWND, *HDC, *HMENU, *HMONITOR, *HKEY, *HINSTANCE, *HGDIOBJ, *HCURSOR, *HWINEVENTHOOK;
typedef unsigned int UINT, DWORD;
typedef int BOOL;
typedef u64 WPARAM;
typedef i64 LPARAM, LRESULT;
typedef LRESULT(WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void(WINAPI *WINEVENTPROC)(HWINEVENTHOOK, DWORD, HWND, int, int, DWORD, DWORD);
typedef DWORD(WINAPI *THREADPROC)(void *);

typedef struct { int x, y; } POINT;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { int cx, cy; } SIZE;
typedef struct { DWORD lo, hi; } FILETIME;
typedef struct { UINT cbSize, style; WNDPROC proc; int clsExtra, wndExtra; HINSTANCE inst; HANDLE icon; HCURSOR cursor; HANDLE bg; const WCHAR *menu, *cls; HANDLE iconSm; } WNDCLASSEXW;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; DWORD priv; } MSG;
typedef struct { DWORD cbSize; RECT monitor, work; DWORD flags; } MONITORINFO;
typedef struct { DWORD size; int width, height; unsigned short planes, bitCount; DWORD compression, sizeImage; int xppm, yppm; DWORD clrUsed, clrImportant; DWORD colors[1]; } BITMAPINFO;
typedef struct { unsigned char op, flags, alpha, format; } BLENDFUNCTION;
typedef struct { DWORD attrs; FILETIME created, accessed, written; DWORD sizeHi, sizeLo; } FILEATTRDATA;

IMP HINSTANCE WINAPI GetModuleHandleW(const WCHAR *);
IMP DWORD WINAPI GetModuleFileNameW(HINSTANCE, WCHAR *, DWORD);
IMP WCHAR *WINAPI GetCommandLineW(void);
IMP void WINAPI ExitProcess(UINT);
IMP HANDLE WINAPI CreateMutexW(void *, BOOL, const WCHAR *);
IMP DWORD WINAPI GetLastError(void);
IMP UINT WINAPI SetErrorMode(UINT);
IMP DWORD WINAPI GetLogicalDrives(void);
IMP DWORD WINAPI GetLogicalDriveStringsW(DWORD, WCHAR *);
IMP UINT WINAPI GetDriveTypeW(const WCHAR *);
IMP BOOL WINAPI GetDiskFreeSpaceExW(const WCHAR *, u64 *, u64 *, u64 *);
IMP BOOL WINAPI GetVolumeInformationW(const WCHAR *, WCHAR *, DWORD, DWORD *, DWORD *, DWORD *, WCHAR *, DWORD);
IMP HANDLE WINAPI GetCurrentProcess(void);
IMP BOOL WINAPI SetPriorityClass(HANDLE, DWORD);
IMP void *WINAPI GetProcAddress(HINSTANCE, const char *);
IMP BOOL WINAPI Beep(DWORD, DWORD);
IMP BOOL WINAPI QueueUserWorkItem(THREADPROC, void *, UINT);
IMP u64 WINAPI GetTickCount64(void);
IMP HANDLE WINAPI CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMP BOOL WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
IMP BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMP BOOL WINAPI CloseHandle(HANDLE);
IMP BOOL WINAPI MoveFileExW(const WCHAR *, const WCHAR *, DWORD);
IMP BOOL WINAPI CreateDirectoryW(const WCHAR *, void *);
IMP DWORD WINAPI GetFileAttributesW(const WCHAR *);
IMP BOOL WINAPI GetFileAttributesExW(const WCHAR *, int, FILEATTRDATA *);
IMP DWORD WINAPI GetEnvironmentVariableW(const WCHAR *, WCHAR *, DWORD);
IMP HANDLE WINAPI GetProcessHeap(void);
IMP void *WINAPI HeapAlloc(HANDLE, DWORD, u64);
IMP BOOL WINAPI HeapFree(HANDLE, DWORD, void *);
IMP BOOL WINAPI IsWindow(HWND);

IMP unsigned short WINAPI RegisterClassExW(const WNDCLASSEXW *);
IMP HWND WINAPI CreateWindowExW(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, void *);
IMP LRESULT WINAPI DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
IMP BOOL WINAPI ShowWindow(HWND, int);
IMP BOOL WINAPI GetMessageW(MSG *, HWND, UINT, UINT);
IMP BOOL WINAPI TranslateMessage(const MSG *);
IMP LRESULT WINAPI DispatchMessageW(const MSG *);
IMP BOOL WINAPI PostMessageW(HWND, UINT, WPARAM, LPARAM);
IMP void WINAPI PostQuitMessage(int);
IMP u64 WINAPI SetTimer(HWND, u64, UINT, void *);
IMP BOOL WINAPI KillTimer(HWND, u64);
IMP BOOL WINAPI SetWindowPos(HWND, HWND, int, int, int, int, UINT);
IMP BOOL WINAPI GetWindowRect(HWND, RECT *);
IMP BOOL WINAPI GetCursorPos(POINT *);
IMP HWND WINAPI SetCapture(HWND);
IMP BOOL WINAPI ReleaseCapture(void);
IMP BOOL WINAPI UpdateLayeredWindow(HWND, HDC, POINT *, SIZE *, HDC, POINT *, DWORD, BLENDFUNCTION *, DWORD);
IMP HMENU WINAPI CreatePopupMenu(void);
IMP BOOL WINAPI AppendMenuW(HMENU, UINT, u64, const WCHAR *);
IMP BOOL WINAPI TrackPopupMenu(HMENU, UINT, int, int, int, HWND, const RECT *);
IMP BOOL WINAPI DestroyMenu(HMENU);
IMP BOOL WINAPI SetForegroundWindow(HWND);
IMP HWND WINAPI GetForegroundWindow(void);
IMP HWND WINAPI GetDesktopWindow(void);
IMP HWND WINAPI GetShellWindow(void);
IMP HWND WINAPI FindWindowW(const WCHAR *, const WCHAR *);
IMP HWND WINAPI FindWindowExW(HWND, HWND, const WCHAR *, const WCHAR *);
IMP int WINAPI GetClassNameW(HWND, WCHAR *, int);
IMP int WINAPI GetWindowLongW(HWND, int);
IMP BOOL WINAPI IsWindowVisible(HWND);
IMP HMONITOR WINAPI MonitorFromRect(const RECT *, DWORD);
IMP HMONITOR WINAPI MonitorFromWindow(HWND, DWORD);
IMP HMONITOR WINAPI MonitorFromPoint(POINT, DWORD);
IMP BOOL WINAPI GetMonitorInfoW(HMONITOR, MONITORINFO *);
IMP BOOL WINAPI SetProcessDPIAware(void);
IMP HWINEVENTHOOK WINAPI SetWinEventHook(DWORD, DWORD, HINSTANCE, WINEVENTPROC, DWORD, DWORD, DWORD);
IMP UINT WINAPI RegisterWindowMessageW(const WCHAR *);
IMP HCURSOR WINAPI LoadCursorW(HINSTANCE, const WCHAR *);
IMP int WINAPI DrawTextW(HDC, const WCHAR *, int, RECT *, UINT);
IMP HDC WINAPI GetDC(HWND);
IMP int WINAPI ReleaseDC(HWND, HDC);
IMP int WINAPI MessageBoxW(HWND, const WCHAR *, const WCHAR *, UINT);

IMP HDC WINAPI CreateCompatibleDC(HDC);
IMP HANDLE WINAPI CreateDIBSection(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
IMP HGDIOBJ WINAPI SelectObject(HDC, HGDIOBJ);
IMP BOOL WINAPI DeleteObject(HGDIOBJ);
IMP HANDLE WINAPI CreateFontW(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const WCHAR *);
IMP DWORD WINAPI SetTextColor(HDC, DWORD);
IMP int WINAPI SetBkMode(HDC, int);
IMP BOOL WINAPI GetTextExtentPoint32W(HDC, const WCHAR *, int, SIZE *);
IMP BOOL WINAPI GdiFlush(void);
IMP int WINAPI GetDeviceCaps(HDC, int);

typedef struct { DWORD cb; WCHAR *reserved, *desktop, *title; DWORD x, y, w, h, cx, cy, fill, flags; unsigned short show, r2; void *r3; HANDLE in, out, err; } STARTUPINFOW;
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
IMP BOOL WINAPI CreateProcessW(const WCHAR *, WCHAR *, void *, void *, BOOL, DWORD, void *, const WCHAR *, STARTUPINFOW *, PROCESS_INFORMATION *);

IMP long WINAPI RegOpenKeyExW(HKEY, const WCHAR *, DWORD, DWORD, HKEY *);
IMP long WINAPI RegCreateKeyExW(HKEY, const WCHAR *, DWORD, WCHAR *, DWORD, DWORD, void *, HKEY *, DWORD *);
IMP long WINAPI RegSetValueExW(HKEY, const WCHAR *, DWORD, DWORD, const void *, DWORD);
IMP long WINAPI RegQueryValueExW(HKEY, const WCHAR *, DWORD *, DWORD *, void *, DWORD *);
IMP long WINAPI RegDeleteValueW(HKEY, const WCHAR *);
IMP long WINAPI RegCloseKey(HKEY);

/* No C runtime: the compiler may still emit calls to these. */
int _fltused = 0x9875;
void *memset(void *d, int c, u64 n) {
	void *r = d;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}
void *memcpy(void *d, const void *s, u64 n) {
	void *r = d;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

#define HKCU ((HKEY)(i64)(int)0x80000001)
#define HWND_TOPMOST ((HWND)(i64)-1)
#define WM_APP_PROBE 0x8001
#define T_TICK 1
#define T_FLASH 2
#define RUN_KEY u"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define THEME_KEY u"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define APPNAME u"Headroom"
#define VERSION u"1.0"
#define SWP_FLAGS_MOVE (0x0001 | 0x0004 | 0x0010) /* NOSIZE | NOZORDER | NOACTIVATE */
#define SWP_FLAGS_RAISE (0x0001 | 0x0002 | 0x0010) /* NOSIZE | NOMOVE | NOACTIVATE */
#define DT_FLAGS (0x20 | 0x04 | 0x800)          /* SINGLELINE | VCENTER | NOPREFIX */

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int iround(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }

/* ---------------------------------------------------------- state */

typedef struct {
	unsigned char kind; /* 0 fill, 1 text */
	unsigned char alpha, bold, right;
	RECT r;
	DWORD color; /* 0xRRGGBB */
	WCHAR text[44];
} Op;
#define MAX_OPS 320

typedef struct { DWORD fg, dim, bar_bg; unsigned char bar_alpha; DWORD level[4]; } Palette;
static const Palette dark_pal = {0xE6EDF3, 0x9DA7B3, 0x8B949E, 90, {0x9DA7B3, 0x3FB950, 0xE3B341, 0xFF6B61}};
static const Palette light_pal = {0x1F2328, 0x59636E, 0x6E7781, 80, {0x59636E, 0x1A7F37, 0x9A6700, 0xCF222E}};
#define FLOAT_BG 0x1B1F24
#define FLOAT_BORDER 0x444C56
#define FLOAT_BAR_BG 0x373E47
#define FLASH_FLOAT 0x7A1D18
#define FLASH_TASK 0xFF3B30

typedef struct { WCHAR path[PATHLEN], name[48]; int kind, fast; } Drive;

typedef struct {
	WCHAR path[PATHLEN];
	int want_label, ok;
	u64 total, avail;
	DWORD error;
	WCHAR label[48];
	double t;
} Job;

static struct {
	Config cfg;
	WCHAR cfg_path[PATHLEN * 2], cfg_err[96];
	FILETIME cfg_mtime;
	HWND ctl, wnd;
	Vol pool[MAX_VOLS];
	Vol *vols[MAX_VOLS], *shown[MAX_VOLS];
	int nvols, nshown;
	DWORD drive_mask;
	u64 t0;
	double last_hk;
	UINT dpi;
	HANDLE font, bold;
	int font_px;
	struct { int lineh, digit, space, free_, unit, unit_na, rate, eta, line2, line2_norate; } m;
	HDC mem_dc, mask_dc;
	HANDLE dib, mask_dib;
	HGDIOBJ stock_bmp, stock_mask;
	unsigned *bits, *mask_bits;
	int bw, bh;
	Op ops[MAX_OPS], prev[MAX_OPS];
	int nops, nprev, bg_a;
	int x, y, w, h, visible, in_taskbar;
	int drag, drag_moved;
	POINT drag_from, drag_origin;
	int menu_open;
	Drive menu_drives[MAX_VOLS * 2];
	int n_menu_drives;
	int in_crit, flashing, flash_on;
	double crit_since, last_beep;
	int light, fullscreen, geo_dirty;
	HWND tb_hwnd, tray_hwnd;
	RECT work; /* work area of the floating window's monitor */
	struct { WCHAR s[48]; int bold, w; } widths[64];
	int nwidths;
	HWINEVENTHOOK hook;
	UINT msg_taskbar_created;
	HANDLE heap;
	BOOL(WINAPI *SetProcessDpiAwarenessContext)(HANDLE);
	UINT(WINAPI *GetDpiForWindow)(HWND);
	u64(WINAPI *SetCoalescableTimer)(HWND, u64, UINT, void *, u64);
	BOOL(WINAPI *SetProcessInformation)(HANDLE, int, void *, DWORD);
} G;

static Config g_tmp_cfg, g_old_cfg;      /* large: never on the stack */
static unsigned char g_io[96 * 1024];    /* settings file buffer */

static double now_s(void) { return (double)(GetTickCount64() - G.t0) / 1000.0; }

/* ---------------------------------------------------------- settings file */

static void default_config_path(WCHAR *out, int cap) {
	WCHAR exe[PATHLEN * 2];
	DWORD n = GetModuleFileNameW(0, exe, PATHLEN * 2);
	while (n > 0 && exe[n - 1] != '\\') n--;
	exe[n] = 0;
	ws_cpy(out, exe, cap);
	ws_cat(out, u"headroom.json", cap);
	if (GetFileAttributesW(out) != 0xFFFFFFFF) return; /* portable: settings travel with the exe */
	if (!GetEnvironmentVariableW(u"APPDATA", out, (DWORD)cap - 40)) ws_cpy(out, exe, cap);
	ws_cat(out, u"\\Headroom\\headroom.json", cap);
}

static int file_mtime(FILETIME *ft) {
	FILEATTRDATA a;
	if (!GetFileAttributesExW(G.cfg_path, 0, &a)) return 0;
	*ft = a.written;
	return 1;
}

/* Returns 1 = loaded, 0 = no file (defaults), -1 = error (cfg_err set) */
static int load_config(Config *c) {
	DWORD got = 0;
	config_defaults(c);
	HANDLE f = CreateFileW(G.cfg_path, 0x80000000 /* GENERIC_READ */, 1 | 2, 0, 3 /* OPEN_EXISTING */, 0, 0);
	if (f == (HANDLE)(i64)-1) return 0;
	ReadFile(f, g_io, sizeof g_io - 1, &got, 0);
	CloseHandle(f);
	if (!config_parse(g_io, (int)got, c)) {
		ws_cpy(G.cfg_err, u"headroom.json is not valid JSON - using previous settings", 96);
		return -1;
	}
	G.cfg_err[0] = 0;
	return 1;
}

static void save_config(void) {
	WCHAR dir[PATHLEN * 2], tmp[PATHLEN * 2 + 8];
	int n = config_write(&G.cfg, g_io, sizeof g_io);
	DWORD put = 0;
	ws_cpy(dir, G.cfg_path, PATHLEN * 2);
	int k = ws_len(dir);
	while (k > 0 && dir[k - 1] != '\\') k--;
	if (k > 0) dir[k - 1] = 0, CreateDirectoryW(dir, 0);
	ws_cpy(tmp, G.cfg_path, PATHLEN * 2 + 8);
	ws_cat(tmp, u".tmp", PATHLEN * 2 + 8);
	HANDLE f = CreateFileW(tmp, 0x40000000 /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
	if (f == (HANDLE)(i64)-1) return;
	WriteFile(f, g_io, (DWORD)n, &put, 0);
	CloseHandle(f);
	MoveFileExW(tmp, G.cfg_path, 1 | 8); /* REPLACE_EXISTING | WRITE_THROUGH */
	file_mtime(&G.cfg_mtime);
}

static int reg_value_exists(const WCHAR *path, const WCHAR *name, DWORD *dword_out) {
	HKEY k;
	DWORD v = 0, type = 0, size = 4;
	if (RegOpenKeyExW(HKCU, path, 0, 1 /* KEY_QUERY_VALUE */, &k)) return 0;
	long r = RegQueryValueExW(k, name, 0, &type, dword_out ? (void *)&v : 0, dword_out ? &size : 0);
	RegCloseKey(k);
	if (dword_out) *dword_out = type == 4 ? v : 0;
	return r == 0;
}

static void reg_set_string(const WCHAR *path, const WCHAR *name, const WCHAR *value) {
	HKEY k;
	if (RegCreateKeyExW(HKCU, path, 0, 0, 0, 2 /* KEY_SET_VALUE */, 0, &k, 0)) return;
	if (value) RegSetValueExW(k, name, 0, 1 /* REG_SZ */, value, (DWORD)(ws_len(value) + 1) * 2);
	else RegDeleteValueW(k, name);
	RegCloseKey(k);
}

static int light_taskbar(void) {
	DWORD v = 0;
	return reg_value_exists(THEME_KEY, u"SystemUsesLightTheme", &v) && v;
}

/* ---------------------------------------------------------- drives */

static int discover(Drive *out, int max, int include_net) {
	WCHAR buf[512];
	DWORD n = GetLogicalDriveStringsW(512, buf);
	int count = 0;
	if (n > 512) n = 512;
	for (DWORD i = 0, start = 0; i < n; i++) {
		if (buf[i]) continue;
		if (i > start && count < max) {
			WCHAR *root = buf + start;
			UINT t = GetDriveTypeW(root);
			int kind = t == 2 ? K_REMOVABLE : (t == 3 || t == 6) ? K_LOCAL : t == 4 ? K_NETWORK : -1;
			if (kind >= 0 && (include_net || kind != K_NETWORK)) {
				Drive *d = &out[count++];
				ws_cpy(d->path, root, PATHLEN);
				ws_cpy(d->name, root, 3); /* "C:" */
				d->kind = kind, d->fast = kind == K_LOCAL;
			}
		}
		start = i + 1;
	}
	return count;
}

static int wanted_drives(Drive *out, int max) {
	static Drive known[MAX_VOLS];
	WCHAR a[PATHLEN], b[PATHLEN];
	if (!G.cfg.ndrives) return discover(out, max, G.cfg.include_network);
	int nk = discover(known, MAX_VOLS, 1), n = 0;
	for (int i = 0; i < G.cfg.ndrives && n < max; i++) {
		const WCHAR *p = G.cfg.drives[i];
		int found = 0;
		norm_key(p, a, PATHLEN);
		for (int j = 0; j < nk && !found; j++) {
			norm_key(known[j].path, b, PATHLEN);
			if (ws_eq(a, b)) out[n++] = known[j], found = 1;
		}
		if (!found) {
			Drive *d = &out[n++];
			ws_cpy(d->path, p, PATHLEN);
			if (ws_len(p) == 2 && p[1] == ':') ws_cat(d->path, u"\\", PATHLEN);
			short_path(p, d->name, 22, 48);
			d->kind = K_FOLDER, d->fast = 0;
		}
	}
	return n;
}

static void sync_volumes(void) {
	static Drive want[MAX_VOLS];
	int n = wanted_drives(want, MAX_VOLS), keep[MAX_VOLS] = {0};
	WCHAR a[PATHLEN], b[PATHLEN];
	Vol *vols[MAX_VOLS];
	for (int i = 0; i < n; i++) {
		Vol *v = 0;
		norm_key(want[i].path, a, PATHLEN);
		for (int j = 0; j < MAX_VOLS && !v; j++) {
			if (!G.pool[j].used || keep[j]) continue;
			norm_key(G.pool[j].path, b, PATHLEN);
			if (ws_eq(a, b)) v = &G.pool[j], keep[j] = 1;
		}
		for (int j = 0; j < MAX_VOLS && !v; j++)
			if (!G.pool[j].used) {
				v = &G.pool[j];
				memset(v, 0, sizeof *v);
				v->used = keep[j] = 1, v->level = LV_NA;
			}
		if (!v) break;
		ws_cpy(v->path, want[i].path, PATHLEN);
		ws_cpy(v->name, want[i].name, 48);
		v->kind = want[i].kind, v->fast = want[i].fast;
		vols[i] = v;
	}
	for (int j = 0; j < MAX_VOLS; j++)
		if (!keep[j]) G.pool[j].used = 0;
	G.nvols = n;
	for (int i = 0; i < n; i++) G.vols[i] = vols[i];
}

static void update_shown(void) {
	int autom = !G.cfg.ndrives; /* hide empty card readers etc. in auto mode */
	G.nshown = 0;
	for (int i = 0; i < G.nvols; i++)
		if (!(autom && G.vols[i]->kind == K_REMOVABLE && G.vols[i]->err[0])) G.shown[G.nshown++] = G.vols[i];
}

/* ---------------------------------------------------------- sampling */

static void probe(Job *j) {
	j->ok = GetDiskFreeSpaceExW(j->path, &j->avail, &j->total, 0);
	j->error = j->ok ? 0 : GetLastError();
	j->t = now_s();
	j->label[0] = 0;
	if (j->want_label && !GetVolumeInformationW(j->path, j->label, 48, 0, 0, 0, 0, 0)) j->label[0] = 0;
}

static DWORD WINAPI probe_worker(void *arg) { /* network/USB/folder: never block the UI on it */
	probe((Job *)arg);
	if (!PostMessageW(G.ctl, WM_APP_PROBE, 0, (LPARAM)arg)) HeapFree(G.heap, 0, arg);
	return 0;
}

static void take(Vol *v, const Job *j) {
	if (j->ok) vol_store(v, j->t, (double)j->total, (double)j->avail, G.cfg.rate_window_s);
	else vol_fail(v, j->error == 21 ? u"no media" : (j->error == 2 || j->error == 3 || j->error == 15) ? u"not found"
	                : j->error == 5 ? u"no access" : u"unavailable");
	if (j->want_label && j->label[0]) ws_cpy(v->label, j->label, 48);
}

static int is_root(const WCHAR *p) { return ws_len(p) == 3 && p[1] == ':' && p[2] == '\\'; }

static DWORD WINAPI beep_worker(void *arg) {
	(void)arg;
	Beep(1400, 180), Beep(1000, 180), Beep(1400, 180);
	return 0;
}

static void evaluate(void) {
	double now = now_s();
	int crit = 0;
	for (int i = 0; i < G.nvols; i++) {
		Vol *v = G.vols[i];
		v->level = v->err[0] ? LV_NA : level_of(v->free_, v->total, v->rate, v->has_rate, &G.cfg);
	}
	update_shown();
	for (int i = 0; i < G.nshown; i++) crit |= G.shown[i]->level == LV_CRIT;
	if (!crit) {
		G.in_crit = 0, G.last_beep = 0;
		if (G.flashing) G.flashing = G.flash_on = 0, KillTimer(G.ctl, T_FLASH);
		return;
	}
	if (!G.in_crit) G.in_crit = 1, G.crit_since = now;
	double confirm = 1.5 * G.cfg.interval_s > 3 ? 1.5 * G.cfg.interval_s : 3; /* ignore one-sample blips */
	if (now - G.crit_since >= confirm && G.cfg.sound && (G.last_beep == 0 || now - G.last_beep >= G.cfg.alarm_repeat_s)) {
		G.last_beep = now;
		QueueUserWorkItem(beep_worker, 0, 0x10);
	}
	if (!G.flashing) G.flashing = 1, SetTimer(G.ctl, T_FLASH, 500, 0); /* only while something is red */
}

/* ---------------------------------------------------------- drawing */

static int px(double v) { return iround(v * G.dpi / 96.0); }

static UINT window_dpi(void) {
	if (G.GetDpiForWindow) {
		UINT d = G.GetDpiForWindow(G.wnd);
		if (d) return d;
	}
	HDC dc = GetDC(0);
	int d = GetDeviceCaps(dc, 90); /* LOGPIXELSY */
	ReleaseDC(0, dc);
	return d > 0 ? (UINT)d : 96;
}

static int measure(const WCHAR *s, int bold) { /* cached: GDI calls are not free */
	SIZE sz = {0, 0};
	for (int i = 0; i < G.nwidths; i++)
		if (G.widths[i].bold == bold && ws_eq(G.widths[i].s, s)) return G.widths[i].w;
	SelectObject(G.mask_dc, bold ? G.bold : G.font);
	GetTextExtentPoint32W(G.mask_dc, s, ws_len(s), &sz);
	if (G.nwidths < 64 && ws_len(s) < 48) {
		ws_cpy(G.widths[G.nwidths].s, s, 48);
		G.widths[G.nwidths].bold = bold, G.widths[G.nwidths++].w = sz.cx;
	}
	return sz.cx;
}

static void set_font(int size_px) {
	if (size_px == G.font_px && G.font) return;
	if (G.font) DeleteObject(G.font), DeleteObject(G.bold);
	/* ANTIALIASED_QUALITY: grey-scale AA, which works with per-pixel alpha */
	G.font = CreateFontW(-size_px, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 4, 0, u"Segoe UI");
	G.bold = CreateFontW(-size_px, 0, 0, 0, 700, 0, 0, 0, 1, 0, 0, 4, 0, u"Segoe UI");
	G.font_px = size_px, G.nwidths = 0;
	SIZE sz;
	SelectObject(G.mask_dc, G.bold);
	GetTextExtentPoint32W(G.mask_dc, u"Ag", 2, &sz);
	G.m.lineh = sz.cy;
	G.m.digit = measure(u"0", 0), G.m.space = measure(u" ", 0);
	G.m.free_ = imax(imax(measure(u"8.88 GB", 1), measure(u"88.8 GB", 1)), measure(u"888 MB", 1));
	G.m.unit = measure(u" free", 0), G.m.unit_na = measure(u" unavailable", 0);
	G.m.rate = measure(u"888 MB/s", 0), G.m.eta = measure(u"88h88m left", 0);
	G.m.line2 = measure(u"888 MB/s · 88h88m", 0), G.m.line2_norate = measure(u"88h88m left", 0);
}

static void op_fill(int l, int t, int r, int b, DWORD color, int alpha) {
	if (G.nops >= MAX_OPS) return;
	Op *o = &G.ops[G.nops++];
	memset(o, 0, sizeof *o);
	o->kind = 0, o->r.left = l, o->r.top = t, o->r.right = r, o->r.bottom = b, o->color = color, o->alpha = (unsigned char)alpha;
}

static void op_text(const WCHAR *s, int l, int t, int r, int b, DWORD color, int bold, int right) {
	if (G.nops >= MAX_OPS || !s[0]) return;
	Op *o = &G.ops[G.nops++];
	memset(o, 0, sizeof *o);
	o->kind = 1, o->r.left = l, o->r.top = t, o->r.right = r, o->r.bottom = b;
	o->color = color, o->alpha = 255, o->bold = (unsigned char)bold, o->right = (unsigned char)right;
	ws_cpy(o->text, s, 44);
}

typedef struct { WCHAR free_[24], rate[24], eta[24], status[24]; DWORD color, eta_color; double frac; } VolState;

static void vol_state(const Vol *v, const Palette *p, VolState *s) {
	double eta;
	memset(s, 0, sizeof *s);
	s->color = p->level[0], s->eta_color = p->fg;
	if (v->err[0]) ws_cpy(s->free_, u"—", 24), ws_cpy(s->status, v->err, 24);
	else if (v->total <= 0) ws_cpy(s->free_, u"…", 24);
	else {
		s->color = p->level[v->level + 1];
		fmt_size(v->free_, s->free_, 24);
		s->frac = 1 - v->free_ / v->total;
		s->frac = s->frac < 0 ? 0 : s->frac > 1 ? 1 : s->frac;
		if (vol_eta(v, &eta)) {
			fmt_rate(v->rate, s->rate, 24), fmt_eta(eta, s->eta, 24);
			int el = eta_level_min(eta / 60, &G.cfg);
			if (el > LV_OK) s->eta_color = p->level[el + 1];
		} else ws_cpy(s->rate, u"idle", 24);
	}
	double stale = 3 * G.cfg.interval_s > 6 ? 3 * G.cfg.interval_s : 6;
	if (v->inflight && now_s() - v->sent > stale) { /* hung network share */
		s->color = p->level[0], s->rate[0] = s->eta[0] = 0;
		ws_cpy(s->status, u"no response", 24);
	}
}

static int layout_floating(int *w_out, int *h_out) {
	const Palette *p = &dark_pal;
	WCHAR t[64];
	int padx = px(10), pady = px(5), gap = px(12), lh = G.m.lineh, rh = lh + px(4);
	int bar_h = imax(4, lh * 45 / 100), bar_w = G.m.digit * 10, name_w = measure(u"C:", 1);
	for (int i = 0; i < G.nshown; i++) vol_title(G.shown[i], t, 64), name_w = imax(name_w, measure(t, 1));
	int xbar = padx + name_w + gap, xfree = xbar + bar_w + gap + G.m.free_;
	int unit_w = G.cfg.show_rate ? G.m.unit : G.m.unit_na, xend = xfree + unit_w, xrate = 0, xeta = 0;
	if (G.cfg.show_rate) xrate = xend + gap + G.m.rate, xeta = xrate + gap, xend = xeta + G.m.eta;
	int w = xend + padx, h = 2 * pady + rh * imax(1, G.nshown);
	G.nops = 0;
	if (!G.nshown) {
		const WCHAR *msg = u"No drives - right-click to choose";
		w = imax(w, measure(msg, 0) + 2 * padx);
		op_text(msg, padx, pady, w - padx, pady + rh, p->dim, 0, 0);
	}
	for (int i = 0; i < G.nshown; i++) {
		Vol *v = G.shown[i];
		VolState s;
		int y0 = pady + i * rh, y1 = y0 + rh, by0 = y0 + (rh - bar_h) / 2;
		vol_state(v, p, &s);
		if (v->level == LV_CRIT && G.flash_on) op_fill(1, y0, w - 1, y1, FLASH_FLOAT, 255);
		vol_title(v, t, 64);
		op_text(t, padx, y0, padx + name_w, y1, p->fg, 1, 0);
		op_fill(xbar, by0, xbar + bar_w, by0 + bar_h, FLOAT_BAR_BG, 255);
		int fw = iround(bar_w * s.frac);
		if (fw > 0) op_fill(xbar, by0, xbar + fw, by0 + bar_h, s.color, 255);
		op_text(s.free_, xbar + bar_w + gap, y0, xfree, y1, s.color, 1, 1);
		if (G.cfg.show_rate) {
			DWORD ec = s.eta_color;
			t[0] = 0;
			if (s.status[0]) ws_cpy(t, s.status, 64), ec = p->dim;
			else if (s.eta[0]) ws_cpy(t, s.eta, 64), ws_cat(t, u" left", 64);
			op_text(u" free", xfree, y0, xfree + unit_w, y1, p->dim, 0, 0);
			op_text(s.rate, xfree + unit_w + gap, y0, xrate, y1, p->dim, 0, 1);
			op_text(t, xeta, y0, xend, y1, ec, 0, 0);
		} else {
			ws_cpy(t, u" ", 64);
			ws_cat(t, s.status[0] ? s.status : u"free", 64);
			op_text(t, xfree, y0, xend, y1, p->dim, 0, 0);
		}
	}
	op_fill(0, 0, w, 1, FLOAT_BORDER, 255), op_fill(0, h - 1, w, h, FLOAT_BORDER, 255);
	op_fill(0, 0, 1, h, FLOAT_BORDER, 255), op_fill(w - 1, 0, w, h, FLOAT_BORDER, 255);
	*w_out = w, *h_out = h;
	return FLOAT_BG;
}

/* One compact column per drive, two lines high:
 *     ▌C: 84.2 GB     ▌D: 412 GB
 *     ▌idle           ▌85.0 MB/s · 2h13m                                       */
static void layout_taskbar(int thick, int *w_out, int *h_out) {
	const Palette *p = G.light ? &light_pal : &dark_pal;
	WCHAR line2[64];
	int lh = G.m.lineh, h = thick, padx = px(6), gap = px(16), bar_w = px(4), bar_gap = px(6);
	int two = 2 * lh + px(2) <= thick, x = padx;
	G.nops = 0;
	if (!G.nshown) {
		const WCHAR *msg = u"Headroom: no drives";
		int w = measure(msg, 0);
		op_text(msg, x, 0, x + w, h, p->dim, 0, 0);
		*w_out = x + w + padx, *h_out = h;
		return;
	}
	for (int i = 0; i < G.nshown; i++) {
		Vol *v = G.shown[i];
		VolState s;
		DWORD c2 = p->dim;
		if (i) x += gap;
		vol_state(v, p, &s);
		ws_cpy(line2, s.status[0] ? s.status : s.rate, 64); /* status, "idle" or "" */
		if (!s.status[0] && s.eta[0]) {
			c2 = s.eta_color;
			if (G.cfg.show_rate) ws_cpy(line2, s.rate, 64), ws_cat(line2, u" · ", 64), ws_cat(line2, s.eta, 64);
			else ws_cpy(line2, s.eta, 64), ws_cat(line2, u" left", 64);
		}
		int name_w = measure(v->name, 0), col1 = name_w + G.m.space + G.m.free_;
		int col_w = imax(col1, G.cfg.show_rate ? G.m.line2 : G.m.line2_norate), lines = 2;
		if (!two) lines = 1, col_w = col1 + G.m.space + G.m.eta;
		int top = (h - lines * lh) / 2, b0 = top + px(2), b1 = top + lines * lh - px(2);
		if (v->level == LV_CRIT && G.flash_on)
			op_fill(x - px(4), px(2), x + bar_w + bar_gap + col_w + px(4), h - px(2), FLASH_TASK, 110);
		op_fill(x, b0, x + bar_w, b1, p->bar_bg, p->bar_alpha);
		int fh = iround((b1 - b0) * s.frac); /* usage, filling from the bottom */
		if (fh > 0) op_fill(x, b1 - fh, x + bar_w, b1, s.color, 255);
		int tx = x + bar_w + bar_gap;
		op_text(v->name, tx, top, tx + name_w, top + lh, p->fg, 0, 0);
		op_text(s.free_, tx + name_w + G.m.space, top, tx + col1, top + lh, s.color, 1, 0);
		if (two) op_text(line2, tx, top + lh, tx + col_w, top + 2 * lh, c2, 0, 0);
		else if (s.eta[0]) op_text(s.eta, tx + col1 + G.m.space, top, tx + col_w, top + lh, c2, 0, 0);
		x = tx + col_w;
	}
	*w_out = x + padx, *h_out = h;
}

typedef struct { RECT r, mon; int ok, horizontal, shown, has_tray, tray_left; } Taskbar;
static Taskbar g_tb; /* cached; see refresh_taskbar() */

/* full = look the windows up again (startup, every 5 s, explorer restart, display
 * change); otherwise just re-read the taskbar's rectangle (auto-hide). */
static void refresh_taskbar(int full) {
	MONITORINFO mi;
	Taskbar *tb = &g_tb;
	if (full || !G.tb_hwnd || !IsWindow(G.tb_hwnd)) {
		memset(tb, 0, sizeof *tb);
		G.tb_hwnd = FindWindowW(u"Shell_TrayWnd", 0);
		G.tray_hwnd = G.tb_hwnd ? FindWindowExW(G.tb_hwnd, 0, u"TrayNotifyWnd", 0) : 0;
		mi.cbSize = sizeof mi;
		if (G.tb_hwnd && GetMonitorInfoW(MonitorFromWindow(G.tb_hwnd, 2), &mi)) tb->mon = mi.monitor;
		RECT nr;
		tb->has_tray = 0;
		if (G.tray_hwnd && GetWindowRect(G.tray_hwnd, &nr) && nr.right > nr.left) tb->has_tray = 1, tb->tray_left = nr.left;
	}
	tb->ok = 0;
	if (!G.tb_hwnd || !GetWindowRect(G.tb_hwnd, &tb->r) || tb->r.right <= tb->r.left || tb->r.bottom <= tb->r.top) return;
	int w = tb->r.right - tb->r.left, hh = tb->r.bottom - tb->r.top;
	int vis_w = imin(tb->r.right, tb->mon.right) - imax(tb->r.left, tb->mon.left);
	int vis_h = imin(tb->r.bottom, tb->mon.bottom) - imax(tb->r.top, tb->mon.top);
	tb->ok = 1, tb->horizontal = w >= hh;
	tb->shown = tb->horizontal ? vis_h >= hh / 2 : vis_w >= w / 2; /* auto-hide parks it off-screen */
	if (tb->has_tray && (tb->tray_left <= tb->r.left || tb->tray_left >= tb->r.right)) tb->has_tray = 0;
}

static void ensure_dib(int w, int h) {
	BITMAPINFO bi;
	if (w == G.bw && h == G.bh && G.dib) return;
	if (G.dib) {
		SelectObject(G.mem_dc, G.stock_bmp), SelectObject(G.mask_dc, G.stock_mask);
		DeleteObject(G.dib), DeleteObject(G.mask_dib);
	}
	memset(&bi, 0, sizeof bi);
	bi.size = 40, bi.width = w, bi.height = -h, bi.planes = 1, bi.bitCount = 32;
	G.dib = CreateDIBSection(0, &bi, 0, (void **)&G.bits, 0, 0);
	G.mask_dib = CreateDIBSection(0, &bi, 0, (void **)&G.mask_bits, 0, 0);
	G.stock_bmp = SelectObject(G.mem_dc, G.dib);
	G.stock_mask = SelectObject(G.mask_dc, G.mask_dib);
	G.bw = w, G.bh = h;
}

static unsigned premul(DWORD c, unsigned a) {
	return a << 24 | ((c >> 16 & 255) * a / 255) << 16 | ((c >> 8 & 255) * a / 255) << 8 | (c & 255) * a / 255;
}

/* colour (r,g,b) with coverage a over a premultiplied BGRA pixel */
static unsigned blend(unsigned d, unsigned r, unsigned g, unsigned b, unsigned a) {
	unsigned ia = 255 - a;
	return (a + ((d >> 24) * ia + 127) / 255) << 24 | ((r * a + (d >> 16 & 255) * ia + 127) / 255) << 16 |
	       ((g * a + (d >> 8 & 255) * ia + 127) / 255) << 8 | (b * a + (d & 255) * ia + 127) / 255;
}

/* Render the ops into a per-pixel-alpha bitmap: text goes through a GDI coverage
 * mask, so it can be coloured and blended over the translucent taskbar. */
static void paint(int w, int h, DWORD bg, int bg_a) {
	ensure_dib(w, h);
	if (!G.bits || !G.mask_bits) return;
	int n = w * h;
	unsigned base = premul(bg, (unsigned)bg_a);
	for (int i = 0; i < n; i++) G.bits[i] = base, G.mask_bits[i] = 0;
	SetBkMode(G.mask_dc, 1);
	SetTextColor(G.mask_dc, 0xFFFFFF);
	for (int i = 0; i < G.nops; i++) {
		Op *o = &G.ops[i];
		if (o->kind != 1) continue;
		RECT r = o->r;
		SelectObject(G.mask_dc, o->bold ? G.bold : G.font);
		DrawTextW(G.mask_dc, o->text, -1, &r, DT_FLAGS | (o->right ? 2 : 0));
	}
	GdiFlush();
	for (int i = 0; i < G.nops; i++) {
		Op *o = &G.ops[i];
		int l = imax(o->r.left, 0), t = imax(o->r.top, 0), r = imin(o->r.right, w), b = imin(o->r.bottom, h);
		unsigned cr = o->color >> 16 & 255, cg = o->color >> 8 & 255, cb = o->color & 255;
		for (int y = t; y < b; y++)
			for (int x = l; x < r; x++) {
				int k = y * w + x;
				unsigned a = o->alpha;
				if (o->kind == 1 && !(a = (G.mask_bits[k] & 255) * a / 255)) continue;
				G.bits[k] = blend(G.bits[k], cr, cg, cb, a);
			}
	}
	POINT pt = {G.x, G.y}, src = {0, 0};
	SIZE sz = {w, h};
	BLENDFUNCTION bf = {0, 0, 255, 1}; /* AC_SRC_OVER, per-pixel alpha */
	UpdateLayeredWindow(G.wnd, 0, &pt, &sz, G.mem_dc, &src, 0, &bf, 2 /* ULW_ALPHA */);
	memcpy(G.prev, G.ops, sizeof(Op) * (u64)G.nops);
	G.nprev = G.nops, G.w = w, G.h = h, G.bg_a = bg_a;
}

static int ops_changed(void) {
	if (G.nops != G.nprev) return 1;
	const unsigned char *a = (const unsigned char *)G.ops, *b = (const unsigned char *)G.prev;
	for (u64 i = 0; i < sizeof(Op) * (u64)G.nops; i++)
		if (a[i] != b[i]) return 1;
	return 0;
}

/* ---------------------------------------------------------- window behaviour */

static void raise_window(void) {
	if (G.visible) SetWindowPos(G.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_FLAGS_RAISE);
}

/* e.g. a full-screen stimulus or video on the same monitor: get out of the way,
 * like the taskbar itself does */
static int fullscreen_on_our_monitor(void) {
	WCHAR cls[64];
	RECT r, ours = {G.x, G.y, G.x + G.w, G.y + G.h};
	MONITORINFO mi;
	HWND fg = GetForegroundWindow();
	if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow() || fg == G.wnd || fg == G.ctl || !IsWindowVisible(fg))
		return 0;
	GetClassNameW(fg, cls, 64);
	if (ws_eq(cls, u"Progman") || ws_eq(cls, u"WorkerW") || ws_eq(cls, u"#32769") || ws_eq(cls, u"Shell_TrayWnd") ||
	    ws_eq(cls, u"Shell_SecondaryTrayWnd"))
		return 0;
	HMONITOR hm = MonitorFromWindow(fg, 0);
	mi.cbSize = sizeof mi;
	if (!hm || !GetWindowRect(fg, &r) || !GetMonitorInfoW(hm, &mi)) return 0;
	if (r.left > mi.monitor.left || r.top > mi.monitor.top || r.right < mi.monitor.right || r.bottom < mi.monitor.bottom)
		return 0;
	if ((GetWindowLongW(fg, -16) & 0x00C00000) == 0x00C00000) return 0; /* has a caption: just maximized */
	return MonitorFromRect(&ours, 2) == hm;
}

static void set_visible(int on) {
	if (on == G.visible) return;
	G.visible = on;
	ShowWindow(G.wnd, on ? 4 /* SW_SHOWNOACTIVATE */ : 0);
	raise_window();
}

static int want_visible(void) {
	if (G.in_taskbar && !g_tb.shown) return 0;
	return !(G.cfg.hide_fullscreen && G.fullscreen);
}

static void position(const Taskbar *tb, int w, int h, int *x, int *y) { /* geo_dirty: re-query monitors */
	MONITORINFO mi;
	mi.cbSize = sizeof mi;
	if (G.drag) { *x = G.x, *y = G.y; return; }
	if (G.in_taskbar) {
		int xx = G.cfg.has_offset ? tb->r.right - iround(G.cfg.taskbar_offset) - w
		         : tb->has_tray ? tb->tray_left - w - px(4) : tb->r.right - px(330) - w;
		*x = iclamp(xx, tb->r.left, tb->r.right - w);
		*y = tb->r.top + (tb->r.bottom - tb->r.top - h) / 2;
		return;
	}
	if (!G.cfg.has_x || !G.cfg.has_y) { /* bottom-right of the primary work area */
		if (G.geo_dirty || G.work.right <= G.work.left) {
			POINT o = {0, 0};
			if (GetMonitorInfoW(MonitorFromPoint(o, 1), &mi)) G.work = mi.work;
		}
		*x = G.work.right - w - px(16), *y = G.work.bottom - h - px(16);
		return;
	}
	RECT r = {iround(G.cfg.x), iround(G.cfg.y), 0, 0};
	r.right = r.left + w, r.bottom = r.top + h;
	if (G.geo_dirty || G.work.right <= G.work.left)
		if (GetMonitorInfoW(MonitorFromRect(&r, 2), &mi)) G.work = mi.work;
	*x = r.left, *y = r.top;
	if (G.work.right > G.work.left) { /* never lose it off-screen */
		*x = iclamp(r.left, G.work.left, G.work.right - w);
		*y = iclamp(r.top, G.work.top, G.work.bottom - h);
	}
}

/* Lay out and repaint -- but only if something visible changed.  force = also
 * re-query geometry (DPI, taskbar, monitors). */
static void redraw(int force) {
	int w, h, x, y, bg = 0, bg_a = 1; /* taskbar: invisible but clickable background */
	if (force) G.geo_dirty = 1;
	if (G.geo_dirty || !G.dpi) G.dpi = window_dpi();
	update_shown();
	if (!G.cfg.floating) refresh_taskbar(G.geo_dirty);
	Taskbar tb = g_tb;
	G.in_taskbar = !G.cfg.floating && tb.ok && tb.horizontal; /* vertical (Win10 side) taskbars: floating */
	if (G.in_taskbar) {
		set_font(iclamp(iround((tb.r.bottom - tb.r.top) * 0.27), px(11), px(16)));
		layout_taskbar(tb.r.bottom - tb.r.top, &w, &h);
	} else {
		set_font(iround(G.cfg.font_size * G.dpi / 72.0));
		bg = layout_floating(&w, &h);
		bg_a = iround(G.cfg.opacity * 255);
	}
	position(&tb, w, h, &x, &y);
	int changed = force || w != G.w || h != G.h || bg_a != G.bg_a || ops_changed();
	int moved = x != G.x || y != G.y;
	G.x = x, G.y = y;
	if (changed) paint(w, h, (DWORD)bg, bg_a);
	else if (moved) SetWindowPos(G.wnd, 0, x, y, 0, 0, SWP_FLAGS_MOVE);
	G.geo_dirty = 0;
	set_visible(want_visible());
}

static void refresh_visibility(void) { /* on window activation and every 5 s */
	G.fullscreen = fullscreen_on_our_monitor();
	if (G.in_taskbar) refresh_taskbar(0);
	set_visible(want_visible());
}

static void set_tick_timer(void) {
	UINT ms = (UINT)(G.cfg.interval_s * 1000);
	if (G.SetCoalescableTimer) /* let Windows batch our wakeup with others */
		G.SetCoalescableTimer(G.ctl, T_TICK, ms, 0, ms / 10 > 1000 ? 1000 : ms / 10);
	else SetTimer(G.ctl, T_TICK, ms, 0);
}

static void apply_config(void) { /* G.cfg changed; g_old_cfg holds the previous one */
	int drives_changed = g_old_cfg.ndrives != G.cfg.ndrives || g_old_cfg.include_network != G.cfg.include_network;
	for (int i = 0; i < G.cfg.ndrives && !drives_changed; i++)
		drives_changed = !ws_eq(g_old_cfg.drives[i], G.cfg.drives[i]);
	if (drives_changed) sync_volumes();
	if (g_old_cfg.interval_s != G.cfg.interval_s) set_tick_timer();
	evaluate();
	redraw(1);
}

static void housekeeping(void) {
	FILETIME ft;
	if (file_mtime(&ft) && (ft.lo != G.cfg_mtime.lo || ft.hi != G.cfg_mtime.hi)) { /* edited by hand */
		G.cfg_mtime = ft;
		if (load_config(&g_tmp_cfg) > 0) g_old_cfg = G.cfg, G.cfg = g_tmp_cfg, apply_config();
	}
	DWORD m = GetLogicalDrives();
	if (m != G.drive_mask) G.drive_mask = m, sync_volumes(); /* drive (un)plugged */
	G.geo_dirty = 1; /* tray width, monitors, taskbar position */
	if (!G.menu_open) refresh_visibility(), raise_window();
}

/* the only periodic wakeup: measure, redraw what changed, housekeeping */
static void tick(void) {
	double now = now_s();
	Job job;
	for (int i = 0; i < G.nvols; i++) {
		Vol *v = G.vols[i];
		int want_label = !v->label_tried && is_root(v->path);
		if (v->fast) { /* fixed local disk: microseconds, do it inline */
			v->label_tried = 1;
			ws_cpy(job.path, v->path, PATHLEN);
			job.want_label = want_label;
			probe(&job);
			take(v, &job);
		} else if (!v->inflight) {
			Job *j = (Job *)HeapAlloc(G.heap, 8 /* HEAP_ZERO_MEMORY */, sizeof(Job));
			if (!j) continue;
			ws_cpy(j->path, v->path, PATHLEN);
			j->want_label = want_label;
			v->inflight = v->label_tried = 1, v->sent = now;
			if (!QueueUserWorkItem(probe_worker, j, 0x10 /* WT_EXECUTELONGFUNCTION */))
				v->inflight = 0, HeapFree(G.heap, 0, j);
		}
	}
	if (now - G.last_hk >= 5) G.last_hk = now, housekeeping();
	evaluate();
	redraw(0);
}

/* ---------------------------------------------------------- menu */

enum { C_EXIT = 1, C_AUTO, C_NET, C_SOUND, C_RATE, C_FULLSCREEN, C_TASKBAR, C_FLOATING, C_RESET, C_AUTOSTART, C_EDIT,
       C_DRIVE = 100, C_PRESET = 1000 };

typedef struct { const WCHAR *title; int n; const double *vals; const WCHAR *const *labels; double *ptr; } Preset;
static const double v_warn_gb[] = {25, 50, 100, 250, 500, 1000}, v_crit_gb[] = {5, 10, 20, 50, 100, 250};
static const double v_warn_min[] = {0, 30, 60, 120, 240}, v_crit_min[] = {0, 5, 10, 15, 30, 60};
static const double v_size[] = {9, 11, 14, 20}, v_opacity[] = {1, .9, .8, .7, .5}, v_interval[] = {1, 2, 5, 10};
static const WCHAR *const l_warn_gb[] = {u"25 GB free", u"50 GB free", u"100 GB free", u"250 GB free", u"500 GB free", u"1000 GB free"};
static const WCHAR *const l_crit_gb[] = {u"5 GB free", u"10 GB free", u"20 GB free", u"50 GB free", u"100 GB free", u"250 GB free"};
static const WCHAR *const l_warn_min[] = {u"off", u"30 min", u"60 min", u"120 min", u"240 min"};
static const WCHAR *const l_crit_min[] = {u"off", u"5 min", u"10 min", u"15 min", u"30 min", u"60 min"};
static const WCHAR *const l_size[] = {u"Small", u"Medium", u"Large", u"Huge"};
static const WCHAR *const l_opacity[] = {u"100%", u"90%", u"80%", u"70%", u"50%"};
static const WCHAR *const l_interval[] = {u"1 s", u"2 s", u"5 s", u"10 s"};

static Preset preset(int i) {
	Preset all[] = {
		{u"Amber below", 6, v_warn_gb, l_warn_gb, &G.cfg.warn_gb},
		{u"Red + alarm below", 6, v_crit_gb, l_crit_gb, &G.cfg.crit_gb},
		{u"Amber when full within", 5, v_warn_min, l_warn_min, &G.cfg.warn_min},
		{u"Red + alarm when full within", 6, v_crit_min, l_crit_min, &G.cfg.crit_min},
		{u"Size", 4, v_size, l_size, &G.cfg.font_size},
		{u"Opacity", 5, v_opacity, l_opacity, &G.cfg.opacity},
		{u"Update every", 4, v_interval, l_interval, &G.cfg.interval_s},
	};
	return all[i];
}

static void add_item(HMENU m, UINT flags, u64 id, const WCHAR *label) {
	WCHAR t[200];
	int k = 0;
	for (int i = 0; label[i] && k < 196; i++) { /* '&' would become a mnemonic */
		if (label[i] == '&') t[k++] = '&';
		t[k++] = label[i];
	}
	t[k] = 0;
	AppendMenuW(m, flags, id, t);
}

static UINT checked(int on) { return on ? 8 /* MF_CHECKED */ : 0; }

static HMENU preset_menu(int i) {
	Preset p = preset(i);
	HMENU m = CreatePopupMenu();
	int found = 0;
	for (int j = 0; j < p.n; j++) {
		double d = *p.ptr - p.vals[j];
		int on = d < 1e-9 && d > -1e-9;
		found |= on;
		add_item(m, checked(on), (u64)(C_PRESET + i * 100 + j), p.labels[j]);
	}
	if (!found) {
		WCHAR t[48] = u"Custom (";
		ws_fixed(t, *p.ptr, *p.ptr == (double)(i64)*p.ptr ? 0 : 2, 48);
		ws_cat(t, u")", 48);
		add_item(m, 8 | 1 /* CHECKED | GRAYED */, 0, t);
	}
	return m;
}

static void detail_line(const Vol *v, WCHAR *out, int cap) {
	WCHAR a[24], b[24];
	double eta;
	vol_title(v, out, cap);
	ws_cat(out, u" — ", cap);
	if (v->err[0]) { ws_cat(out, v->err, cap); return; }
	if (v->total <= 0) { ws_cat(out, u"measuring…", cap); return; }
	fmt_size(v->free_, a, 24), fmt_size(v->total, b, 24);
	ws_cat(out, a, cap), ws_cat(out, u" free of ", cap), ws_cat(out, b, cap), ws_cat(out, u" (", cap);
	ws_int(out, (i64)(100 * v->free_ / v->total + 0.5), cap);
	ws_cat(out, u"%)", cap);
	if (vol_eta(v, &eta)) {
		fmt_rate(v->rate, a, 24), fmt_eta(eta, b, 24);
		ws_cat(out, u", writing ", cap), ws_cat(out, a, cap), ws_cat(out, u", full in ", cap), ws_cat(out, b, cap);
	}
}

static void toggle_drive(const WCHAR *path) {
	WCHAR a[PATHLEN], b[PATHLEN];
	int found = 0, n = 0;
	norm_key(path, a, PATHLEN);
	for (int i = 0; i < G.nshown; i++) {
		norm_key(G.shown[i]->path, b, PATHLEN);
		if (ws_eq(a, b)) found = 1;
		else if (n < MAX_VOLS) ws_cpy(G.cfg.drives[n++], G.shown[i]->path, PATHLEN);
	}
	if (!found && n < MAX_VOLS) ws_cpy(G.cfg.drives[n++], path, PATHLEN);
	G.cfg.ndrives = n; /* none left = auto */
}

static void toggle_autostart(void) {
	static WCHAR cmd[PATHLEN * 5], exe[PATHLEN * 2], def[PATHLEN * 2];
	if (reg_value_exists(RUN_KEY, APPNAME, 0)) { reg_set_string(RUN_KEY, APPNAME, 0); return; }
	GetModuleFileNameW(0, exe, PATHLEN * 2);
	ws_cpy(cmd, u"\"", PATHLEN * 5), ws_cat(cmd, exe, PATHLEN * 5), ws_cat(cmd, u"\"", PATHLEN * 5);
	default_config_path(def, PATHLEN * 2);
	if (!ws_ieq(def, G.cfg_path))
		ws_cat(cmd, u" -config \"", PATHLEN * 5), ws_cat(cmd, G.cfg_path, PATHLEN * 5), ws_cat(cmd, u"\"", PATHLEN * 5);
	reg_set_string(RUN_KEY, APPNAME, cmd);
}

static void command(int id) {
	g_old_cfg = G.cfg;
	Config *c = &G.cfg;
	if (id == C_EXIT) ExitProcess(0);
	else if (id == C_AUTO) {
		if (c->ndrives) c->ndrives = 0;
		else for (int i = 0; i < G.nshown && i < MAX_VOLS; i++) ws_cpy(c->drives[c->ndrives++], G.shown[i]->path, PATHLEN);
	} else if (id == C_NET) c->include_network ^= 1;
	else if (id == C_SOUND) c->sound ^= 1;
	else if (id == C_RATE) c->show_rate ^= 1;
	else if (id == C_FULLSCREEN) c->hide_fullscreen ^= 1;
	else if (id == C_TASKBAR) c->floating = 0;
	else if (id == C_FLOATING) c->floating = 1;
	else if (id == C_RESET) c->has_x = c->has_y = c->has_offset = 0;
	else if (id == C_AUTOSTART) { toggle_autostart(); return; }
	else if (id == C_EDIT) {
		static WCHAR cmd[PATHLEN * 2 + 16];
		STARTUPINFOW si;
		PROCESS_INFORMATION pi;
		save_config();
		ws_cpy(cmd, u"notepad.exe \"", PATHLEN * 2 + 16), ws_cat(cmd, G.cfg_path, PATHLEN * 2 + 16), ws_cat(cmd, u"\"", PATHLEN * 2 + 16);
		memset(&si, 0, sizeof si);
		si.cb = sizeof si;
		if (CreateProcessW(0, cmd, 0, 0, 0, 0, 0, 0, &si, &pi)) CloseHandle(pi.process), CloseHandle(pi.thread);
		return;
	} else if (id >= C_PRESET) {
		int i = (id - C_PRESET) / 100, j = (id - C_PRESET) % 100;
		if (i < 7) {
			Preset p = preset(i);
			if (j < p.n) *p.ptr = p.vals[j];
		}
	} else if (id >= C_DRIVE && id - C_DRIVE < G.n_menu_drives) toggle_drive(G.menu_drives[id - C_DRIVE].path);
	save_config();
	apply_config();
}

static void show_menu(void) {
	static const WCHAR *const kinds[] = {u"local", u"removable", u"network", u"folder"};
	WCHAR t[200], a[PATHLEN], b[PATHLEN];
	HMENU m = CreatePopupMenu(), dm = CreatePopupMenu(), am = CreatePopupMenu(), vm = CreatePopupMenu();
	add_item(m, 1, 0, APPNAME u" " VERSION);
	for (int i = 0; i < G.nshown; i++) detail_line(G.shown[i], t, 200), add_item(m, 1, 0, t);
	if (G.cfg_err[0]) add_item(m, 1, 0, G.cfg_err);
	AppendMenuW(m, 0x800, 0, 0);

	add_item(dm, checked(!G.cfg.ndrives), C_AUTO, u"All local drives (auto)");
	add_item(dm, checked(G.cfg.include_network), C_NET, u"Include network drives in auto");
	AppendMenuW(dm, 0x800, 0, 0);
	static Drive cand[MAX_VOLS * 2];
	int nc = discover(cand, MAX_VOLS, 1);
	nc += wanted_drives(cand + nc, MAX_VOLS);
	G.n_menu_drives = 0;
	for (int i = 0; i < nc; i++) {
		int dup = 0, on = 0;
		norm_key(cand[i].path, a, PATHLEN);
		for (int j = 0; j < G.n_menu_drives && !dup; j++) norm_key(G.menu_drives[j].path, b, PATHLEN), dup = ws_eq(a, b);
		if (dup) continue;
		for (int j = 0; j < G.nshown && !on; j++) norm_key(G.shown[j]->path, b, PATHLEN), on = ws_eq(a, b);
		ws_cpy(t, cand[i].name, 200), ws_cat(t, u"\t", 200), ws_cat(t, kinds[cand[i].kind], 200);
		add_item(dm, checked(on), (u64)(C_DRIVE + G.n_menu_drives), t);
		G.menu_drives[G.n_menu_drives++] = cand[i];
	}
	AppendMenuW(m, 0x10 /* MF_POPUP */, (u64)dm, u"Drives");

	for (int i = 0; i < 4; i++) AppendMenuW(am, 0x10, (u64)preset_menu(i), preset(i).title);
	AppendMenuW(am, 0x800, 0, 0);
	add_item(am, checked(G.cfg.sound), C_SOUND, u"Sound alarm");
	AppendMenuW(m, 0x10, (u64)am, u"Alerts");

	add_item(vm, checked(!G.cfg.floating), C_TASKBAR, u"On the taskbar");
	add_item(vm, checked(G.cfg.floating), C_FLOATING, u"Floating window");
	AppendMenuW(vm, 0x800, 0, 0);
	add_item(vm, checked(G.cfg.show_rate), C_RATE, u"Show write rate and time left");
	add_item(vm, checked(G.cfg.hide_fullscreen), C_FULLSCREEN, u"Hide over full-screen apps");
	if (G.cfg.floating) {
		AppendMenuW(vm, 0x10, (u64)preset_menu(4), u"Size");
		AppendMenuW(vm, 0x10, (u64)preset_menu(5), u"Opacity");
	}
	add_item(vm, 0, C_RESET, u"Move to default position");
	AppendMenuW(m, 0x10, (u64)vm, u"Display");
	AppendMenuW(m, 0x10, (u64)preset_menu(6), u"Update every");
	AppendMenuW(m, 0x800, 0, 0);
	add_item(m, checked(reg_value_exists(RUN_KEY, APPNAME, 0)), C_AUTOSTART, u"Start at login");
	add_item(m, 0, C_EDIT, u"Edit settings file…");
	AppendMenuW(m, 0x800, 0, 0);
	add_item(m, 0, C_EXIT, u"Exit");

	HWND prev = GetForegroundWindow();
	POINT pt;
	GetCursorPos(&pt);
	G.menu_open = 1;
	SetForegroundWindow(G.ctl); /* required for the menu to close when clicking elsewhere */
	int cmd = TrackPopupMenu(m, 0x100 | 0x2 | 0x80 /* RETURNCMD | RIGHTBUTTON | NONOTIFY */, pt.x, pt.y, 0, G.ctl, 0);
	PostMessageW(G.ctl, 0, 0, 0);
	G.menu_open = 0;
	DestroyMenu(m);
	if (prev && cmd != C_EDIT) SetForegroundWindow(prev); /* keyboard back to the acquisition software */
	if (cmd) command(cmd);
}

/* ---------------------------------------------------------- input */

static void drag_start(void) {
	GetCursorPos(&G.drag_from);
	G.drag = 1, G.drag_moved = 0, G.drag_origin.x = G.x, G.drag_origin.y = G.y;
	SetCapture(G.wnd);
}

static void drag_move(void) {
	POINT c;
	GetCursorPos(&c);
	int dx = c.x - G.drag_from.x, dy = c.y - G.drag_from.y;
	if (!G.drag_moved && (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) < px(4)) return;
	G.drag_moved = 1;
	int x = G.drag_origin.x + dx, y = G.drag_origin.y + dy;
	if (G.in_taskbar) { /* slide along the taskbar only */
		y = G.y;
		if (g_tb.ok) x = iclamp(x, g_tb.r.left, g_tb.r.right - G.w);
	}
	G.x = x, G.y = y;
	SetWindowPos(G.wnd, 0, x, y, 0, 0, SWP_FLAGS_MOVE);
}

static void drag_end(void) {
	if (!G.drag) return;
	G.drag = 0;
	ReleaseCapture();
	if (!G.drag_moved) return;
	if (G.in_taskbar) {
		if (g_tb.ok) G.cfg.has_offset = 1, G.cfg.taskbar_offset = g_tb.r.right - G.x - G.w;
	} else {
		G.cfg.has_x = G.cfg.has_y = 1, G.cfg.x = G.x, G.cfg.y = G.y;
		redraw(0); /* snaps back fully on-screen; remember where it actually is */
		G.cfg.x = G.x, G.cfg.y = G.y;
	}
	save_config();
	redraw(0);
}

/* ---------------------------------------------------------- windows */

static LRESULT WINAPI wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (!G.wnd || (hwnd != G.ctl && hwnd != G.wnd)) return DefWindowProcW(hwnd, msg, wp, lp);
	switch (msg) {
	case 0x0113: /* WM_TIMER */
		if (wp == T_TICK) tick();
		else if (wp == T_FLASH) G.flash_on ^= 1, redraw(0);
		return 0;
	case WM_APP_PROBE: {
		Job *j = (Job *)lp;
		for (int i = 0; i < G.nvols; i++)
			if (G.vols[i]->inflight && ws_eq(G.vols[i]->path, j->path)) {
				G.vols[i]->inflight = 0;
				take(G.vols[i], j);
				break;
			}
		HeapFree(G.heap, 0, j);
		evaluate();
		redraw(0);
		return 0;
	}
	case 0x0021: return 3; /* WM_MOUSEACTIVATE: MA_NOACTIVATE -- never steal focus */
	case 0x0201: if (hwnd == G.wnd) { drag_start(); return 0; } break; /* WM_LBUTTONDOWN */
	case 0x0200: if (G.drag) { drag_move(); return 0; } break;         /* WM_MOUSEMOVE */
	case 0x0202: if (G.drag) { drag_end(); return 0; } break;          /* WM_LBUTTONUP */
	case 0x0215: if (G.drag) drag_end(); return 0;                     /* WM_CAPTURECHANGED */
	case 0x0205: if (hwnd == G.wnd) { show_menu(); return 0; } break;  /* WM_RBUTTONUP */
	case 0x02E0: if (hwnd == G.wnd) { redraw(1); return 0; } break;    /* WM_DPICHANGED */
	case 0x001A: case 0x007E: /* WM_SETTINGCHANGE, WM_DISPLAYCHANGE */
		if (hwnd == G.ctl) G.light = light_taskbar(), redraw(1);
		break;
	case 0x0010: ExitProcess(0); return 0;       /* WM_CLOSE (e.g. taskkill): exit, don't just vanish */
	case 0x0011: return 1;                       /* WM_QUERYENDSESSION */
	case 0x0016: if (wp) ExitProcess(0); return 0; /* WM_ENDSESSION */
	default:
		if (G.msg_taskbar_created && msg == G.msg_taskbar_created) { redraw(1); return 0; } /* explorer restarted */
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void WINAPI on_foreground(HWINEVENTHOOK h, DWORD ev, HWND w, int o, int c, DWORD t, DWORD ms) {
	(void)h, (void)ev, (void)w, (void)o, (void)c, (void)t, (void)ms;
	if (!G.menu_open) refresh_visibility(), raise_window(); /* stay above the taskbar */
}

/* split the command line (quotes group words; enough for paths) */
static int split_args(WCHAR **argv, int max) {
	static WCHAR buf[2048];
	const WCHAR *s = GetCommandLineW();
	int argc = 0, k = 0;
	while (*s && argc < max) {
		while (*s == ' ' || *s == '\t') s++;
		if (!*s) break;
		argv[argc++] = buf + k;
		for (int quoted = 0; *s && (quoted || (*s != ' ' && *s != '\t')); s++)
			if (*s == '"') quoted = !quoted;
			else if (k < 2046) buf[k++] = *s;
		buf[k++] = 0;
		if (k >= 2046) break;
	}
	return argc;
}

static void args(int *dirty) {
	WCHAR *argv[32];
	int argc = split_args(argv, 32);
	G.cfg_path[0] = 0;
	for (int i = 1; i < argc; i++) {
		WCHAR *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : 0;
		while (*a == '-') a++;
		if (ws_eq(a, u"config") && val) ws_cpy(G.cfg_path, val, PATHLEN * 2), i++;
		else if (ws_eq(a, u"version")) {
			MessageBoxW(0, APPNAME u" " VERSION, APPNAME, 0x40);
			ExitProcess(0);
		}
	}
	if (!G.cfg_path[0]) default_config_path(G.cfg_path, PATHLEN * 2);
	if (load_config(&G.cfg) == 0) *dirty = 1; /* first run: write the settings file */
	for (int i = 1; i < argc; i++) {
		WCHAR *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : 0;
		while (*a == '-') a++;
		if (ws_eq(a, u"drives") && val) { /* "C:,D:" or "all" */
			G.cfg.ndrives = 0;
			if (!ws_ieq(val, u"all"))
				for (WCHAR *s = val; *s && G.cfg.ndrives < MAX_VOLS;) {
					WCHAR *e = s;
					while (*e && *e != ',') e++;
					int len = (int)(e - s);
					while (len && *s == ' ') s++, len--;
					while (len && s[len - 1] == ' ') len--;
					if (len > 0 && len < PATHLEN) {
						for (int k = 0; k < len; k++) G.cfg.drives[G.cfg.ndrives][k] = s[k];
						G.cfg.drives[G.cfg.ndrives++][len] = 0;
					}
					s = *e ? e + 1 : e;
				}
			*dirty = 1, i++;
		} else if (ws_eq(a, u"mode") && val) {
			if (ws_eq(val, u"taskbar")) G.cfg.floating = 0, *dirty = 1;
			if (ws_eq(val, u"floating")) G.cfg.floating = 1, *dirty = 1;
			i++;
		} else if (ws_eq(a, u"reset-position")) G.cfg.has_x = G.cfg.has_y = G.cfg.has_offset = 0, *dirty = 1;
	}
}

static void lower_priority(void) { /* never compete with the acquisition software */
	HANDLE proc = GetCurrentProcess();
	SetPriorityClass(proc, 0x4000); /* BELOW_NORMAL_PRIORITY_CLASS */
	if (G.SetProcessInformation) {
		DWORD eco[3] = {1, 1, 1}; /* PROCESS_POWER_THROTTLING_STATE: EcoQoS "efficiency mode" */
		DWORD low = 2;            /* MEMORY_PRIORITY_LOW: our pages are trimmed first */
		G.SetProcessInformation(proc, 4, eco, sizeof eco);
		G.SetProcessInformation(proc, 0, &low, sizeof low);
	}
}

void WINAPI WinMainCRTStartup(void) {
	int dirty = 0;
	MSG msg;
	WCHAR mutex_name[64] = u"Local\\Headroom-";
	HINSTANCE inst = GetModuleHandleW(0), u32 = GetModuleHandleW(u"user32.dll"), k32 = GetModuleHandleW(u"kernel32.dll");
	G.heap = GetProcessHeap();
	G.t0 = GetTickCount64();
	*(void **)&G.SetProcessDpiAwarenessContext = GetProcAddress(u32, "SetProcessDpiAwarenessContext");
	*(void **)&G.GetDpiForWindow = GetProcAddress(u32, "GetDpiForWindow");
	*(void **)&G.SetCoalescableTimer = GetProcAddress(u32, "SetCoalescableTimer");
	*(void **)&G.SetProcessInformation = GetProcAddress(k32, "SetProcessInformation");
	args(&dirty);

	unsigned hsh = 2166136261u; /* one instance per settings file */
	for (WCHAR *s = G.cfg_path; *s; s++) hsh = (hsh ^ lower(*s)) * 16777619u;
	for (int i = 0; i < 8; i++) {
		WCHAR d[2] = {(WCHAR) "0123456789abcdef"[(hsh >> (28 - 4 * i)) & 15], 0};
		ws_cat(mutex_name, d, 64);
	}
	if (CreateMutexW(0, 0, mutex_name) && GetLastError() == 183 /* ERROR_ALREADY_EXISTS */) ExitProcess(0);
	SetErrorMode(0x0001 | 0x8000); /* never show "insert a disk" dialogs while probing */
	if (!G.SetProcessDpiAwarenessContext || !G.SetProcessDpiAwarenessContext((HANDLE)(i64)-4)) SetProcessDPIAware();
	lower_priority();

	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc, wc.proc = wndproc, wc.inst = inst, wc.cls = u"HeadroomWnd";
	wc.cursor = LoadCursorW(0, (const WCHAR *)(i64)32512); /* IDC_ARROW */
	RegisterClassExW(&wc);
	G.ctl = CreateWindowExW(0x80, u"HeadroomWnd", APPNAME, 0x80000000, 0, 0, 1, 1, 0, 0, inst, 0);
	G.wnd = CreateWindowExW(0x8 | 0x80 | 0x80000 | 0x08000000 /* TOPMOST|TOOLWINDOW|LAYERED|NOACTIVATE */,
	                        u"HeadroomWnd", APPNAME, 0x80000000 /* WS_POPUP */, 0, 0, 1, 1, 0, 0, inst, 0);
	G.msg_taskbar_created = RegisterWindowMessageW(u"TaskbarCreated");
	G.mem_dc = CreateCompatibleDC(0), G.mask_dc = CreateCompatibleDC(0);
	G.light = light_taskbar();
	G.drive_mask = GetLogicalDrives();
	sync_volumes();
	if (dirty) save_config();
	else file_mtime(&G.cfg_mtime);
	/* re-assert our place above the taskbar whenever a window is activated (event driven) */
	G.hook = SetWinEventHook(3, 3, 0, on_foreground, 0, 0, 2 /* OUTOFCONTEXT | SKIPOWNPROCESS */);
	tick();
	set_tick_timer();
	while (GetMessageW(&msg, 0, 0, 0) > 0) TranslateMessage(&msg), DispatchMessageW(&msg);
	ExitProcess(0);
}
#endif /* HR_TEST */
