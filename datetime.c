/*
 * Settings > Date & Time (included by ui_m3.c before open_settings).
 *
 * The time zone: picked by region, then place, from tz_table.h (the IANA
 * zones with the POSIX rule musl needs: the device has no zoneinfo files).
 * The rule goes into /etc/TZ, which the service's start exports as TZ, and
 * into this process at once (setenv + tzset); ui.conf keeps the name shown.
 *
 * The time itself: set automatically by chronyd from the network (its
 * service started or stopped, and added to or taken from the default
 * runlevel), or by hand: date and time on rollers, set with settimeofday()
 * and written to the RTC (hwclock), which keeps it while powered off.
 */

#include "tz_table.h"

static lv_obj_t *dt_zone_sub;	/* (the Time zone row's line, while its page is up) */

static const char *dt_zone_name(void)
{
	return conf.tz[0] ? conf.tz : "UTC";
}

/* chronyd running: the time is the network's */
static bool dt_auto(void)
{
	return !system("pidof chronyd >/dev/null 2>&1");
}

/* clocks shown everywhere: again now (a zone or the time changed) */
static void dt_refresh_clocks(void)
{
	home_update(NULL);
	if (lock_scr)
		lock_update();
	bp_changed(home_scr);
}

/* @posix (a zone's rule) for this process and the next ones (/etc/TZ) */
static void dt_set_zone(const struct tz_zone *z)
{
	FILE *f = fopen("/etc/TZ.new", "w");

	if (f) {
		fprintf(f, "%s\n", z->posix);
		if (fflush(f) == 0)
			fsync(fileno(f));
		fclose(f);
		rename("/etc/TZ.new", "/etc/TZ");
	}
	setenv("TZ", z->posix, 1);
	tzset();
	snprintf(conf.tz, sizeof(conf.tz), "%s", z->name);
	conf.tz_set = true;
	conf_save_later();
	if (dt_zone_sub)
		lv_label_set_text(dt_zone_sub, z->name);
	dt_refresh_clocks();
}

/* ------------------------------------------------------------ the zones */

static void open_datetime(lv_event_t *e);
static char dt_region[24];	/* the region whose places show */

static void dt_zone_cb(lv_event_t *e)
{
	const struct tz_zone *z = lv_event_get_user_data(e);

	dt_set_zone(z);
	go_back();
	go_back();
}

/* "America/Argentina/Buenos_Aires" -> "Argentina/Buenos Aires" */
static void dt_place(const char *zone, char *out, size_t len)
{
	const char *p = strchr(zone, '/');
	size_t i;

	snprintf(out, len, "%s", p ? p + 1 : zone);
	for (i = 0; out[i]; i++)
		if (out[i] == '_')
			out[i] = ' ';
}

static void open_dt_places(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c;
	char title[32], place[64];
	int i;

	LV_UNUSED(e);
	nav_mark(open_dt_places, NULL);
	snprintf(title, sizeof(title), "%s", dt_region);
	body = m3_page(title, &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	for (i = 0; i < TZ_ZONES; i++) {
		size_t n = strlen(dt_region);
		const struct tz_zone *z = &tz_zones[i];

		if (strncmp(z->name, dt_region, n) || (z->name[n] != '/' && z->name[n]))
			continue;
		dt_place(z->name, place, sizeof(place));
		m3_row(c, LV_SYMBOL_GPS, place, z->posix, dt_zone_cb, (void *)z, NULL);
	}
	push(scr);
}

static void dt_region_cb(lv_event_t *e)
{
	snprintf(dt_region, sizeof(dt_region), "%s", (const char *)lv_event_get_user_data(e));
	open_dt_places(NULL);
}

static void open_dt_regions(lv_event_t *e)
{
	static char regions[16][24];
	lv_obj_t *scr, *body, *c;
	int n = 0, i, k;

	LV_UNUSED(e);
	nav_mark(open_dt_regions, NULL);
	body = m3_page("Time zone", &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	for (i = 0; i < TZ_ZONES && n < 16; i++) {
		char r[24];
		const char *s = strchr(tz_zones[i].name, '/');
		size_t l = s ? (size_t)(s - tz_zones[i].name) : strlen(tz_zones[i].name);

		snprintf(r, sizeof(r), "%.*s", (int)LV_MIN(l, sizeof(r) - 1), tz_zones[i].name);
		for (k = 0; k < n && strcmp(regions[k], r); k++)
			;
		if (k == n)
			snprintf(regions[n++], sizeof(regions[0]), "%s", r);
	}
	for (i = 0; i < n; i++)
		m3_row(c, LV_SYMBOL_GPS, regions[i], NULL, dt_region_cb, regions[i], NULL);
	push(scr);
}

/* --------------------------------------------------- the time by hand */

static lv_obj_t *dt_roll[5];	/* day, month, year; hour, minute */

static lv_obj_t *dt_roller(lv_obj_t *parent, const char *opts, int sel, int32_t w)
{
	lv_obj_t *r = lv_roller_create(parent);

	lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
	lv_roller_set_visible_row_count(r, 3);
	lv_roller_set_selected(r, (uint32_t)sel, LV_ANIM_OFF);
	lv_obj_set_width(r, w);
	lv_obj_set_style_bg_color(r, M.surface_c_high, 0);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_radius(r, 16, 0);
	lv_obj_set_style_text_color(r, M.on_surface_v, 0);
	lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
	lv_obj_set_style_bg_color(r, M.primary_c, LV_PART_SELECTED);
	lv_obj_set_style_text_color(r, M.on_primary_c, LV_PART_SELECTED);
	return r;
}

static void dt_set_cb(lv_event_t *e)
{
	struct tm tm = { 0 };
	struct timeval tv = { 0 };
	int k;

	LV_UNUSED(e);
	for (k = 0; k < 5; k++)
		if (!dt_roll[k] || !lv_obj_is_valid(dt_roll[k]))
			return;
	tm.tm_mday = (int)lv_roller_get_selected(dt_roll[0]) + 1;
	tm.tm_mon = (int)lv_roller_get_selected(dt_roll[1]);
	tm.tm_year = 2024 + (int)lv_roller_get_selected(dt_roll[2]) - 1900;
	tm.tm_hour = (int)lv_roller_get_selected(dt_roll[3]);
	tm.tm_min = (int)lv_roller_get_selected(dt_roll[4]);
	tm.tm_isdst = -1;
	tv.tv_sec = mktime(&tm);	/* (in the zone set) */
	if (tv.tv_sec == (time_t)-1 || settimeofday(&tv, NULL)) {
		fprintf(stderr, "n72-ui: setting the time failed: %s\n", strerror(errno));
		return;
	}
	job_run("hwclock -w -u 2>&1", NULL, NULL, NULL);	/* the RTC keeps it (UTC) */
	dt_refresh_clocks();
	go_back();
}

static void open_dt_manual(lv_event_t *e)
{
	static char days[31 * 3 + 1], years[16 * 5 + 1], hours[24 * 3 + 1], mins[60 * 3 + 1];
	static const char months[] = "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec";
	time_t now = time(NULL);
	struct tm tm;
	lv_obj_t *scr, *body, *row;
	int i;
	size_t n;

	LV_UNUSED(e);
	nav_mark(open_dt_manual, NULL);
	localtime_r(&now, &tm);
	for (i = 1, n = 0; i <= 31; i++)
		n += snprintf(days + n, sizeof(days) - n, i > 1 ? "\n%d" : "%d", i);
	for (i = 0, n = 0; i < 16; i++)
		n += snprintf(years + n, sizeof(years) - n, i ? "\n%d" : "%d", 2024 + i);
	for (i = 0, n = 0; i < 24; i++)
		n += snprintf(hours + n, sizeof(hours) - n, i ? "\n%02d" : "%02d", i);
	for (i = 0, n = 0; i < 60; i++)
		n += snprintf(mins + n, sizeof(mins) - n, i ? "\n%02d" : "%02d", i);
	body = m3_page("Set the time", &scr);
	m3_label(body, "Date", &lv_font_montserrat_14, M.on_surface_v);
	row = lv_obj_create(body);
	plain(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 8, 0);
	dt_roll[0] = dt_roller(row, days, tm.tm_mday - 1, 64);
	dt_roll[1] = dt_roller(row, months, tm.tm_mon, 80);
	dt_roll[2] = dt_roller(row, years, LV_CLAMP(0, tm.tm_year + 1900 - 2024, 15), 96);
	m3_label(body, "Time", &lv_font_montserrat_14, M.on_surface_v);
	row = lv_obj_create(body);
	plain(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 8, 0);
	dt_roll[3] = dt_roller(row, hours, tm.tm_hour, 72);
	dt_roll[4] = dt_roller(row, mins, tm.tm_min, 72);
	for (i = 0; i < 5; i++)
		obj_track(&dt_roll[i]);
	m3_button(body, "Set", true, dt_set_cb, NULL);
	push(scr);
}

/* -------------------------------------------------------------- the page */

static lv_obj_t *dt_manual_row;

static void dt_auto_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	dt_refresh_clocks();
}

static void dt_auto_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

	/* (the service, now and at every start) */
	job_run(on ? "rc-update add chronyd default >/dev/null 2>&1; rc-service chronyd start 2>&1" :
		     "rc-service chronyd stop 2>&1; rc-update del chronyd default >/dev/null 2>&1",
		NULL, dt_auto_done, NULL);
	if (dt_manual_row && lv_obj_is_valid(dt_manual_row)) {
		if (on)
			lv_obj_add_flag(dt_manual_row, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(dt_manual_row, LV_OBJ_FLAG_HIDDEN);
	}
}

static void open_datetime(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *r, *sw;
	bool on = dt_auto();

	LV_UNUSED(e);
	nav_mark(open_datetime, NULL);
	body = m3_page("Date & Time", &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	r = m3_row(c, LV_SYMBOL_REFRESH, "Set automatically", "The time from the network", NULL, NULL, NULL);
	sw = ios_switch(lv_switch_create(r));
	if (on)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, dt_auto_cb, LV_EVENT_VALUE_CHANGED, NULL);
	dt_manual_row = m3_row(c, LV_SYMBOL_EDIT, "Date and time", "Set by hand", open_dt_manual, NULL, NULL);
	obj_track(&dt_manual_row);
	if (on)
		lv_obj_add_flag(dt_manual_row, LV_OBJ_FLAG_HIDDEN);
	m3_row(c, LV_SYMBOL_GPS, "Time zone", dt_zone_name(), open_dt_regions, NULL, &dt_zone_sub);
	obj_track(&dt_zone_sub);
	push(scr);
}
