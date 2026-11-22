/*
 * The weather (included by ui_m3.c before the lock screen): Open-Meteo
 * (open-meteo.com, free, no key), on the home card's right side and under
 * the lock screen's clock; the place is set in Settings > Weather by name
 * (Open-Meteo's geocoding).
 *
 * Kept cheap: one curl a half hour at most, and only while the screen is
 * on and Wi-Fi is up (an awake timer: nothing runs dark); after a failure
 * 5 minutes before the next try. The last answer is kept in
 * CONF_DIR/weather.cache, so a start shows it at once and asks again only
 * when it is old. The icons are drawn once, small, and kept.
 */

#include <ctype.h>

#define WX_EVERY_S	(30 * 60)	/* a new forecast at most this often */
#define WX_RETRY_S	(5 * 60)
#define WX_CACHE	CONF_DIR "/weather.cache"

enum { WXI_SUN, WXI_MOON, WXI_PART, WXI_PART_N, WXI_CLOUD, WXI_FOG, WXI_RAIN, WXI_SNOW, WXI_STORM, WXI_N };

static struct {
	bool ok;		/* a forecast to show */
	double temp, hi, lo;	/* deg C */
	int code, day;		/* WMO weather code; daytime */
	time_t at;		/* when it was fetched */
	time_t fail_at;
	bool fetching;
	lv_timer_t *timer;
	lv_draw_buf_t *icon[2][WXI_N];	/* [0 card, 1 lock] */
	lv_obj_t *c_icon, *c_temp, *c_text;	/* on the home card */
	lv_obj_t *l_row, *l_icon, *l_text;	/* on the lock screen */
} wx;

/* ----------------------------------------------------------- the codes */

static int wx_kind(int code, bool day)
{
	if (code == 0 || code == 1)
		return day ? WXI_SUN : WXI_MOON;
	if (code == 2)
		return day ? WXI_PART : WXI_PART_N;
	if (code == 3)
		return WXI_CLOUD;
	if (code == 45 || code == 48)
		return WXI_FOG;
	if ((code >= 71 && code <= 77) || code == 85 || code == 86)
		return WXI_SNOW;
	if (code >= 95)
		return WXI_STORM;
	if (code >= 51 && code <= 82)
		return WXI_RAIN;
	return WXI_CLOUD;
}

static const char *wx_words(int code)
{
	switch (code) {
	case 0: return "Clear";
	case 1: return "Mostly clear";
	case 2: return "Partly cloudy";
	case 3: return "Cloudy";
	case 45: case 48: return "Fog";
	case 51: case 53: case 55: return "Drizzle";
	case 56: case 57: return "Freezing drizzle";
	case 61: case 63: return "Rain";
	case 65: return "Heavy rain";
	case 66: case 67: return "Freezing rain";
	case 71: case 73: case 75: case 77: return "Snow";
	case 80: case 81: return "Showers";
	case 82: return "Heavy showers";
	case 85: case 86: return "Snow showers";
	case 95: return "Thunderstorm";
	case 96: case 99: return "Storm, hail";
	}
	return "";
}

static int wx_deg(double c)
{
	return (int)lround(conf.wx_f ? c * 9.0 / 5.0 + 32.0 : c);
}

/* ----------------------------------------------------------- the icons */

static void wx_circle(lv_layer_t *l, int32_t cx, int32_t cy, int32_t r, lv_color_t c)
{
	lv_draw_rect_dsc_t d;
	lv_area_t a = { cx - r, cy - r, cx + r, cy + r };

	lv_draw_rect_dsc_init(&d);
	d.radius = LV_RADIUS_CIRCLE;
	d.bg_color = c;
	d.bg_opa = LV_OPA_COVER;
	lv_draw_rect(l, &d, &a);
}

static void wx_line(lv_layer_t *l, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w, lv_color_t c)
{
	lv_draw_line_dsc_t d;

	lv_draw_line_dsc_init(&d);
	d.color = c;
	d.width = w;
	d.round_start = d.round_end = 1;
	d.p1.x = x1;
	d.p1.y = y1;
	d.p2.x = x2;
	d.p2.y = y2;
	lv_draw_line(l, &d);
}

/* a cloud filling the box (x, y, w, h) */
static void wx_cloud(lv_layer_t *l, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t c)
{
	lv_draw_rect_dsc_t d;
	lv_area_t a = { x, y + h / 2, x + w - 1, y + h - 1 };

	wx_circle(l, x + w * 32 / 100, y + h * 58 / 100, h * 30 / 100, c);
	wx_circle(l, x + w * 58 / 100, y + h * 42 / 100, h * 40 / 100, c);
	wx_circle(l, x + w * 80 / 100, y + h * 64 / 100, h * 26 / 100, c);
	lv_draw_rect_dsc_init(&d);
	d.radius = h / 4;
	d.bg_color = c;
	d.bg_opa = LV_OPA_COVER;
	lv_draw_rect(l, &d, &a);
}

static void wx_sun(lv_layer_t *l, int32_t cx, int32_t cy, int32_t r, lv_color_t c)
{
	int i;

	wx_circle(l, cx, cy, r * 55 / 100, c);
	for (i = 0; i < 8; i++) {
		float a = (float)i * (float)M_PI / 4.0f, s = sinf(a), k = cosf(a);

		wx_line(l, cx + (int32_t)lroundf(k * r * 0.78f), cy + (int32_t)lroundf(s * r * 0.78f),
			cx + (int32_t)lroundf(k * r * 0.98f), cy + (int32_t)lroundf(s * r * 0.98f),
			LV_MAX(2, r / 6), c);
	}
}

static void wx_moon(lv_layer_t *l, int32_t cx, int32_t cy, int32_t r, lv_color_t c)
{
	lv_draw_arc_dsc_t d;

	lv_draw_arc_dsc_init(&d);
	d.center.x = cx;
	d.center.y = cy;
	d.radius = (uint16_t)(r * 70 / 100);
	d.width = r * 32 / 100;
	d.start_angle = 70;
	d.end_angle = 290;
	d.rounded = 1;
	d.color = c;
	lv_draw_arc(l, &d);
}

static lv_draw_buf_t *wx_icon_make(int kind, int32_t s, lv_color_t cloud)
{
	static const lv_color32_t no = { 0 };
	lv_draw_buf_t *b = lv_draw_buf_create(s, s, LV_COLOR_FORMAT_ARGB8888, 0);
	lv_color_t sun = lv_color_hex(0xffc94d), rain = lv_color_hex(0x6ab4ff), bolt = lv_color_hex(0xffd84d);
	lv_obj_t *cv;
	lv_layer_t l;
	int i;

	LV_UNUSED(no);
	if (!b)
		return NULL;
	lv_draw_buf_clear(b, NULL);
	cv = lv_canvas_create(lv_layer_sys());
	lv_canvas_set_draw_buf(cv, b);
	lv_canvas_init_layer(cv, &l);
	switch (kind) {
	case WXI_SUN:
		wx_sun(&l, s / 2, s / 2, s * 45 / 100, sun);
		break;
	case WXI_MOON:
		wx_moon(&l, s / 2, s / 2, s * 45 / 100, sun);
		break;
	case WXI_PART:
	case WXI_PART_N:
		if (kind == WXI_PART)
			wx_sun(&l, s * 38 / 100, s * 36 / 100, s * 32 / 100, sun);
		else
			wx_moon(&l, s * 38 / 100, s * 36 / 100, s * 32 / 100, sun);
		wx_cloud(&l, s * 20 / 100, s * 40 / 100, s * 78 / 100, s * 50 / 100, cloud);
		break;
	case WXI_CLOUD:
		wx_cloud(&l, s * 4 / 100, s * 20 / 100, s * 92 / 100, s * 62 / 100, cloud);
		break;
	case WXI_FOG:
		for (i = 0; i < 3; i++)
			wx_line(&l, s * (14 + 8 * (i & 1)) / 100, s * (30 + 20 * i) / 100,
				s * (86 - 8 * (i & 1)) / 100, s * (30 + 20 * i) / 100, LV_MAX(2, s / 12), cloud);
		break;
	case WXI_RAIN:
	case WXI_SNOW:
	case WXI_STORM:
		wx_cloud(&l, s * 4 / 100, s * 6 / 100, s * 92 / 100, s * 56 / 100, cloud);
		if (kind == WXI_STORM) {
			wx_line(&l, s * 56 / 100, s * 62 / 100, s * 44 / 100, s * 80 / 100, LV_MAX(2, s / 12), bolt);
			wx_line(&l, s * 44 / 100, s * 80 / 100, s * 58 / 100, s * 80 / 100, LV_MAX(2, s / 12), bolt);
			wx_line(&l, s * 58 / 100, s * 80 / 100, s * 46 / 100, s * 96 / 100, LV_MAX(2, s / 12), bolt);
			break;
		}
		for (i = 0; i < 3; i++) {
			int32_t x = s * (28 + 22 * i) / 100;

			if (kind == WXI_RAIN)
				wx_line(&l, x + s / 20, s * 70 / 100, x - s / 20, s * 90 / 100, LV_MAX(2, s / 14), rain);
			else
				wx_circle(&l, x, s * 80 / 100, LV_MAX(2, s / 14), cloud);
		}
		break;
	}
	lv_canvas_finish_layer(cv, &l);
	lv_obj_delete(cv);
	lv_draw_buf_flush_cache(b, NULL);
	return b;
}

static lv_draw_buf_t *wx_icon(int which, int kind)
{
	if (!wx.icon[which][kind])
		wx.icon[which][kind] = wx_icon_make(kind, which ? 32 : 44,
						   which ? lv_color_hex(0xf0f0f4) : M.on_surface_v);
	return wx.icon[which][kind];
}

/* --------------------------------------------------------- the showing */

static void wx_show(void)
{
	char t[48];
	bool on = conf.wx_set && wx.ok;
	int kind = wx_kind(wx.code, wx.day);

	if (wx.c_temp && lv_obj_is_valid(wx.c_temp)) {
		if (on) {
			snprintf(t, sizeof(t), "%d\xc2\xb0", wx_deg(wx.temp));
			label_set_if_changed(wx.c_temp, t);
			snprintf(t, sizeof(t), "%s", wx_words(wx.code));
			label_set_if_changed(wx.c_text, t);
			lv_image_set_src(wx.c_icon, wx_icon(0, kind));
			lv_obj_remove_flag(wx.c_icon, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wx.c_temp, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(wx.c_text, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(wx.c_icon, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(wx.c_temp, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(wx.c_text, LV_OBJ_FLAG_HIDDEN);
		}
		bp_changed(home_scr);
	}
	if (wx.l_row && lv_obj_is_valid(wx.l_row)) {
		if (on) {
			snprintf(t, sizeof(t), "%d\xc2\xb0  %s", wx_deg(wx.temp), wx_words(wx.code));
			label_set_if_changed(wx.l_text, t);
			lv_image_set_src(wx.l_icon, wx_icon(1, kind));
			lv_obj_remove_flag(wx.l_row, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(wx.l_row, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

/* the right side of the home card */
static void wx_card_build(lv_obj_t *card)
{
	wx.c_icon = lv_image_create(card);
	lv_obj_align(wx.c_icon, LV_ALIGN_TOP_RIGHT, -16, 12);
	wx.c_temp = m3_label(card, "", &lv_font_montserrat_28, M.on_surface);
	lv_obj_align(wx.c_temp, LV_ALIGN_TOP_RIGHT, -66, 18);
	wx.c_text = m3_label(card, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(wx.c_text, LV_ALIGN_BOTTOM_RIGHT, -18, -14);
	obj_track(&wx.c_icon);
	obj_track(&wx.c_temp);
	obj_track(&wx.c_text);
	wx_show();
}

/* a line under the lock screen's clock */
static void wx_lock_build(lv_obj_t *scr, int32_t y)
{
	lv_obj_t *r = wx.l_row = lv_obj_create(scr);

	plain(r);
	lv_obj_set_size(r, LV_SIZE_CONTENT, 36);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 8, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	wx.l_icon = lv_image_create(r);
	wx.l_text = m3_label(r, "", &lv_font_montserrat_16, lv_color_hex(0xe8e0f0));
	lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
	obj_track(&wx.l_row);
	obj_track(&wx.l_icon);
	obj_track(&wx.l_text);
	wx_show();
}

/* ---------------------------------------------------------- the asking */

/* the number after "@key": in @s (false: none) */
static bool wx_num(const char *s, const char *key, double *v)
{
	char k[48];
	const char *p;
	char *end;

	snprintf(k, sizeof(k), "\"%s\":", key);
	if (!s || !(p = strstr(s, k)))
		return false;
	p += strlen(k);
	if (*p == '[')
		p++;
	*v = strtod(p, &end);
	return end != p;
}

static bool wx_str(const char *s, const char *key, char *out, size_t len)
{
	char k[48];
	const char *p, *q;

	snprintf(k, sizeof(k), "\"%s\":\"", key);
	if (!s || !(p = strstr(s, k)))
		return false;
	p += strlen(k);
	if (!(q = strchr(p, '"')))
		return false;
	snprintf(out, len, "%.*s", (int)LV_MIN((size_t)(q - p), len - 1), p);
	return true;
}

static void wx_cache_write(void)
{
	FILE *f = fopen(WX_CACHE ".new", "w");

	if (!f)
		return;
	fprintf(f, "%ld %.4f %.4f %.1f %.1f %.1f %d %d\n", (long)wx.at, conf.wx_lat, conf.wx_lon,
		wx.temp, wx.hi, wx.lo, wx.code, wx.day);
	fclose(f);
	rename(WX_CACHE ".new", WX_CACHE);
}

static void wx_cache_read(void)
{
	FILE *f = fopen(WX_CACHE, "r");
	long at;
	double lat, lon;

	if (!f)
		return;
	if (fscanf(f, "%ld %lf %lf %lf %lf %lf %d %d", &at, &lat, &lon, &wx.temp, &wx.hi, &wx.lo,
		   &wx.code, &wx.day) == 8 && fabs(lat - conf.wx_lat) < 1e-3 && fabs(lon - conf.wx_lon) < 1e-3) {
		wx.at = (time_t)at;
		wx.ok = true;
	}
	fclose(f);
}

static void wx_done(const char *out, void *ud)
{
	const char *cur = strstr(out, "\"current\":{"), *daily = strstr(out, "\"daily\":{");
	double t, c, d, hi, lo;

	LV_UNUSED(ud);
	wx.fetching = false;
	if (!cur || !wx_num(cur, "temperature_2m", &t) || !wx_num(cur, "weather_code", &c) ||
	    !wx_num(cur, "is_day", &d)) {
		wx.fail_at = time(NULL);
		if (getenv("N72_FPS"))
			fprintf(stderr, "weather: no forecast (%.60s)\n", out);
		return;
	}
	wx.temp = t;
	wx.code = (int)c;
	wx.day = d != 0;
	wx.hi = daily && wx_num(daily, "temperature_2m_max", &hi) ? hi : t;
	wx.lo = daily && wx_num(daily, "temperature_2m_min", &lo) ? lo : t;
	wx.at = time(NULL);
	wx.ok = true;
	wx_cache_write();
	wx_show();
}

static void wx_fetch(void)
{
	char cmd[400];

	if (!conf.wx_set || wx.fetching)
		return;
	snprintf(cmd, sizeof(cmd),
		 "curl -s -m 20 'https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
		 "&current=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min"
		 "&forecast_days=1&timezone=auto' 2>/dev/null", conf.wx_lat, conf.wx_lon);
	if (!job_run(cmd, NULL, wx_done, NULL))
		wx.fetching = true;
}

/* every minute while lit: a new forecast when the last is old */
static void wx_tick(lv_timer_t *t)
{
	time_t now = time(NULL);

	LV_UNUSED(t);
	if (!conf.wx_set || !wifi_up || wx.fetching || screen_off)
		return;
	if (wx.ok && now - wx.at < WX_EVERY_S && now >= wx.at)
		return;
	if (wx.fail_at && now - wx.fail_at < WX_RETRY_S && now >= wx.fail_at)
		return;
	wx_fetch();
}

/* at the start (once) */
static void wx_init(void)
{
	if (wx.timer)
		return;
	wx_cache_read();
	wx.timer = awake_timer(wx_tick, 60000, NULL);
	lv_timer_ready(wx.timer);
}

/* ----------------------------------------------------- Settings > Weather */

static lv_obj_t *wx_find_ta, *wx_find_list, *wx_find_state;
static struct wx_place { char name[96]; double lat, lon; } wx_found[8];
static int wx_nfound;

static void wx_place_cb(lv_event_t *e)
{
	struct wx_place *p = lv_event_get_user_data(e);

	snprintf(conf.wx_name, sizeof(conf.wx_name), "%s", p->name);
	conf.wx_lat = p->lat;
	conf.wx_lon = p->lon;
	conf.wx_set = true;
	conf_save_later();
	wx.ok = false;
	wx.fail_at = 0;
	wx_show();
	wx_fetch();
	go_back();
	go_back();
}

static void wx_found_cb(const char *out, void *ud)
{
	const char *p = out;
	int i;

	LV_UNUSED(ud);
	wx_nfound = 0;
	while (wx_nfound < 8 && (p = strstr(p, "{\"id\":"))) {
		const char *next = strstr(p + 1, "{\"id\":");
		char seg[1024], nm[48] = "", a1[48] = "", co[48] = "";
		size_t n = next ? (size_t)(next - p) : strlen(p);
		struct wx_place *w = &wx_found[wx_nfound];

		snprintf(seg, sizeof(seg), "%.*s", (int)LV_MIN(n, sizeof(seg) - 1), p);
		p += 1;
		if (!wx_str(seg, "name", nm, sizeof(nm)) || !wx_num(seg, "latitude", &w->lat) ||
		    !wx_num(seg, "longitude", &w->lon))
			continue;
		wx_str(seg, "admin1", a1, sizeof(a1));
		wx_str(seg, "country", co, sizeof(co));
		snprintf(w->name, sizeof(w->name), "%s%s%s%s%s", nm, a1[0] ? ", " : "", a1,
			 co[0] ? ", " : "", co);
		wx_nfound++;
	}
	if (!wx_find_list || !lv_obj_is_valid(wx_find_list))
		return;
	lv_obj_clean(wx_find_list);
	label_set_if_changed(wx_find_state, wx_nfound ? "" : "No place of that name (or no network)");
	for (i = 0; i < wx_nfound; i++)
		m3_row(wx_find_list, LV_SYMBOL_GPS, wx_found[i].name, NULL, wx_place_cb, &wx_found[i], NULL);
}

static void wx_find_cb(lv_event_t *e)
{
	char q[64], enc[200], cmd[400];
	const char *s;
	size_t n = 0;

	if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
		go_back();
		return;
	}
	snprintf(q, sizeof(q), "%s", lv_textarea_get_text(wx_find_ta));
	for (s = q; *s && n + 4 < sizeof(enc); s++) {
		unsigned char ch = (unsigned char)*s;

		if (isalnum(ch) || ch == '-' || ch == '.')
			enc[n++] = (char)ch;
		else
			n += snprintf(enc + n, sizeof(enc) - n, "%%%02X", ch);
	}
	enc[n] = 0;
	if (!n)
		return;
	label_set_if_changed(wx_find_state, "Looking...");
	snprintf(cmd, sizeof(cmd),
		 "curl -s -m 15 'https://geocoding-api.open-meteo.com/v1/search?name=%s&count=8&language=en&format=json'"
		 " 2>/dev/null", enc);
	job_run(cmd, wx_find_list, wx_found_cb, NULL);
}

static void open_wx_find(lv_event_t *e)
{
	lv_obj_t *scr, *body;

	LV_UNUSED(e);
	nav_mark(NULL, NULL);	/* (a field: not reopened by a turn) */
	body = m3_page("Location", &scr);
	m3_label(body, "A city or town", &lv_font_montserrat_14, M.on_surface_v);
	wx_find_ta = lv_textarea_create(body);
	obj_track(&wx_find_ta);
	lv_textarea_set_one_line(wx_find_ta, true);
	lv_obj_set_width(wx_find_ta, lv_pct(100));
	lv_obj_set_style_radius(wx_find_ta, 12, 0);
	lv_obj_set_style_anim_duration(wx_find_ta, 0, LV_PART_CURSOR);
	lv_obj_add_event_cb(wx_find_ta, wx_find_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(wx_find_ta, wx_find_cb, LV_EVENT_CANCEL, NULL);
	wx_find_state = m3_label(body, "", &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&wx_find_state);
	wx_find_list = m3_card(body, M.surface_c);
	obj_track(&wx_find_list);
	lv_obj_set_style_pad_all(wx_find_list, 4, 0);
	lv_obj_set_flex_flow(wx_find_list, LV_FLEX_FLOW_COLUMN);
	push(scr);
	kbd_attach(wx_find_ta, NULL, true);
}

static void wx_unit_cb(lv_event_t *e)
{
	conf.wx_f = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	conf_save_later();
	wx_show();
}

static void wx_off_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	conf.wx_set = false;
	conf.wx_name[0] = 0;
	conf_save_later();
	wx.ok = false;
	unlink(WX_CACHE);
	wx_show();
	go_back();
}

static void open_weather(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *r, *sw;
	char sub[64];

	LV_UNUSED(e);
	nav_mark(open_weather, NULL);
	body = m3_page("Weather", &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_GPS, "Location", conf.wx_set ? conf.wx_name : "Not set: no weather",
	       open_wx_find, NULL, NULL);
	r = m3_row(c, LV_SYMBOL_SETTINGS, "Fahrenheit", "Else Celsius", NULL, NULL, NULL);
	sw = ios_switch(lv_switch_create(r));
	if (conf.wx_f)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, wx_unit_cb, LV_EVENT_VALUE_CHANGED, NULL);
	if (conf.wx_set) {
		if (wx.ok) {
			long m = (long)(time(NULL) - wx.at) / 60;

			if (m < 1)
				snprintf(sub, sizeof(sub), "Updated just now");
			else
				snprintf(sub, sizeof(sub), "Updated %ld min ago", m);
		} else {
			snprintf(sub, sizeof(sub), "Not updated yet");
		}
		m3_row(c, LV_SYMBOL_REFRESH, "Open-Meteo", sub, NULL, NULL, NULL);
		m3_row(c, LV_SYMBOL_CLOSE, "No weather", "Remove the location", wx_off_cb, NULL, NULL);
	}
	push(scr);
}
