// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): settings: Display, Power, About, Storage, sound. */
#include "ui_m3_priv.h"

/* ------------------------------------------------------------ Display */

static lv_obj_t *bright_val;
static lv_obj_t *bright_slider;
static lv_obj_t *auto_bright_sw;
static lv_obj_t *night_sw;
static lv_obj_t *night_slider;
static lv_obj_t *night_val;

#define CLCD_MIE_PATH	"/sys/devices/platform/display-subsystem/mie"

void display_night_apply(void)
{
	mbx_gpu_set_warmth(conf.night_shift ? conf.night_warmth : 0);
}

void display_mie_apply(void)
{
	FILE *f = fopen(CLCD_MIE_PATH, "w");

	if (f) {
		fprintf(f, "%d\n", conf.mie_mode);
		fclose(f);
	}
}

static void mie_cb(lv_event_t *e)
{
	uint32_t sel = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));

	if (sel > 2 || (int)sel == conf.mie_mode)
		return;
	conf.mie_mode = (int)sel;
	display_mie_apply();
	pf_label_changed(lv_event_get_target(e));
	conf_save();
}

static lv_obj_t *segmented_matrix(lv_obj_t *parent, const char *const *map, uint32_t active,
				    lv_event_cb_t cb)
{
	lv_obj_t *bm = lv_buttonmatrix_create(parent);

	lv_buttonmatrix_set_map(bm, map);
	lv_buttonmatrix_set_one_checked(bm, true);
	lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
	lv_buttonmatrix_set_button_ctrl(bm, active, LV_BUTTONMATRIX_CTRL_CHECKED);
	lv_obj_set_size(bm, lv_pct(100), 52);
	plain(bm);
	lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(bm, 0, 0);
	lv_obj_set_style_radius(bm, LV_RADIUS_CIRCLE, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(bm, M.surface_c_high, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(bm, M.primary, LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_text_color(bm, M.on_surface, LV_PART_ITEMS);
	lv_obj_set_style_text_color(bm, M.on_primary, LV_PART_ITEMS | LV_STATE_CHECKED);
	/* the default theme gives the buttons a shadow (a halo on light) */
	lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
	lv_obj_set_style_outline_width(bm, 0, LV_PART_ITEMS);
	lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return bm;
}

void ui_night_sync_widgets(void)
{
	if (!night_sw)
		return;
	if (conf.night_shift)
		lv_obj_add_state(night_sw, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(night_sw, LV_STATE_CHECKED);
	pf_label_changed(night_sw);
}

static void night_sw_cb(lv_event_t *e)
{
	conf.night_shift = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	display_night_apply();
	qs_night_sync();
	pf_label_changed(lv_event_get_target(e));
	conf_save();
}

static void night_warmth_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	int v = lv_slider_get_value(lv_event_get_target(e));
	bool turned_on = false;
	char buf[16];

	conf.night_warmth = v;
	snprintf(buf, sizeof(buf), "%d%%", v);
	label_set_if_changed(night_val, buf);
	if (!conf.night_shift) {
		conf.night_shift = true;
		turned_on = true;
		ui_night_sync_widgets();
	}
	pf_label_changed(lv_event_get_target(e));
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		qs_night_sync();
		display_night_apply();
		conf_save();
	} else {
		conf_save_later();
		if (turned_on)
			qs_night_sync();
	}
}

void ui_bright_sync_widgets(int pct)
{
	char buf[16];

	if (home_bright) {
		lv_slider_set_value(home_bright, pct, LV_ANIM_OFF);
		shade_changed(home_bright);
	}
	if (bright_slider) {
		lv_slider_set_value(bright_slider, pct, LV_ANIM_OFF);
		pf_label_changed(bright_slider);
	}
	if (bright_val) {
		snprintf(buf, sizeof(buf), "%d%%", pct);
		label_set_if_changed(bright_val, buf);
	}
}

void ui_auto_bright_off(void)
{
	if (!conf.auto_bright)
		return;
	conf.auto_bright = false;
	if (auto_bright_sw) {
		lv_obj_remove_state(auto_bright_sw, LV_STATE_CHECKED);
		pf_label_changed(auto_bright_sw);
	}
	auto_bright_sync();
	conf_save();
}

static void auto_bright_cb(lv_event_t *e)
{
	conf.auto_bright = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	auto_bright_sync();
	pf_label_changed(lv_event_get_target(e));
	conf_save();
}

static void bright_cb(lv_event_t *e)
{
	int v;
	char buf[16];

	if (sd.on)
		return;
	if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
		conf_save_flush();
		return;
	}
	v = lv_slider_get_value(lv_event_get_target(e));
	ui_auto_bright_off();
	backlight_set(v);
	conf.bright = v;
	conf_save_later();
	lv_slider_set_value(home_bright, v, LV_ANIM_OFF);
	shade_changed(home_bright);	/* (the panel's picture is taken again: no jump when pulled) */
	snprintf(buf, sizeof(buf), "%d%%", v);
	label_set_if_changed(bright_val, buf);
	pf_label_changed(lv_event_get_target(e));
}

static void theme_cb(lv_event_t *e)
{
	uint32_t sel = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));

	if (sel > 1 || (bool)(sel == 0) == conf.dark)
		return;
	conf.dark = sel == 0;
	ui_restart("display");
}

static void accent_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	if (i == conf.accent && !conf.wp_accent)
		return;
	conf.accent = i;
	conf.wp_accent = false;
	ui_restart("display");
}

static const int idle_opts[] = { 15, 30, 60, 120, 300, 0 };

static void idle_cb(lv_event_t *e)
{
	uint32_t i = lv_dropdown_get_selected(lv_event_get_target(e));

	if (i < sizeof(idle_opts) / sizeof(idle_opts[0])) {
		conf.idle_s = idle_opts[i];
		idle_kick();
		pf_label_changed(lv_event_get_target(e));
		conf_save();
	}
}

static void plug_cb(lv_event_t *e)
{
	conf.wake_plug = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	pf_label_changed(lv_event_get_target(e));
	conf_save();
}

static void lock_col_style_dot(lv_obj_t *dot, int idx, bool sel)
{
	lv_obj_t *lbl = lv_obj_get_child(dot, 0);
	lv_color_t bg, fg;

	if (idx == 0) {
		bg = sel ? M.primary : M.surface_c_highest;
		fg = sel ? M.on_primary : M.on_surface;
	} else if (idx == 3) {
		bg = M.primary;
		fg = M.on_primary;
	} else {
		bg = lv_color_hex(lock_cols[idx].hex);
		fg = idx == 2 ? lv_color_white() : lv_color_hex(0x18181c);
	}
	lv_obj_set_style_bg_color(dot, bg, 0);
	lv_obj_set_style_border_width(dot, sel ? 3 : 1, 0);
	lv_obj_set_style_border_color(dot, sel ? M.on_surface : M.outline, 0);
	if (lbl) {
		lv_label_set_text(lbl, idx == 0 ? "A" : sel ? LV_SYMBOL_OK : "");
		lv_obj_set_style_text_color(lbl, fg, 0);
	}
}

static void lock_col_cb(lv_event_t *e)
{
	lv_obj_t *dot = lv_event_get_target(e);
	lv_obj_t *row = lv_obj_get_parent(dot);
	lv_obj_t *val = lv_obj_get_user_data(row);
	int idx = (int)(intptr_t)lv_event_get_user_data(e);

	if (idx == conf.lock_col || idx < 0 || idx >= N_LOCK_COLS)
		return;
	conf.lock_col = idx;
	for (int i = 0; i < N_LOCK_COLS; i++) {
		lv_obj_t *d = lv_obj_get_child(row, (int32_t)i);

		if (d)
			lock_col_style_dot(d, i, i == conf.lock_col);
	}
	if (val && lv_obj_is_valid(val))
		label_set_if_changed(val, lock_cols[conf.lock_col].name);
	lock_color_sync();
	pf_label_changed(row);
	conf_save();
}

static void lock_col_section(lv_obj_t *body)
{
	lv_obj_t *hdr, *val, *row;
	int cur = (conf.lock_col >= 0 && conf.lock_col < N_LOCK_COLS) ? conf.lock_col : 0;

	hdr = lv_obj_create(body);
	plain(hdr);
	lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_hor(hdr, 8, 0);
	lv_obj_set_style_pad_top(hdr, 8, 0);
	lv_obj_set_style_pad_bottom(hdr, 0, 0);
	lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	m3_label(hdr, "Lock screen text", &lv_font_montserrat_14, M.primary);
	val = m3_label(hdr, lock_cols[cur].name, &lv_font_montserrat_14, M.on_surface_v);

	row = lv_obj_create(body);
	plain(row);
	lv_obj_set_size(row, lv_pct(100), 56);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_user_data(row, val);
	for (int i = 0; i < N_LOCK_COLS; i++) {
		lv_obj_t *dot = lv_obj_create(row);
		lv_obj_t *lbl;

		plain(dot);
		lv_obj_set_size(dot, 40, 40);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_all(dot, 0, 0);
		lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
		lbl = m3_label(dot, "", &lv_font_montserrat_14, M.on_surface);
		lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_center(lbl);
		lock_col_style_dot(dot, i, i == cur);
		lv_obj_add_event_cb(dot, lock_col_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
}

void open_display(lv_event_t *e)
{
	nav_mark(open_display, NULL);
	static const char *const theme_map[] = { "Dark", "Light", "" };
	static const char *const mie_map[] = { "Off", "Video", "Always", "" };
	lv_obj_t *scr, *body, *c, *s, *row, *dd;
	unsigned int i;

	LV_UNUSED(e);
	body = m3_page("Display", &scr);

	m3_section(body, "Brightness");
	c = m3_card(body, M.surface_c);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 12, 0);
	m3_label(c, LV_SYMBOL_IMAGE, &lv_font_montserrat_20, M.on_surface_v);
	s = row_slider(c, false);
	bright_slider = s;
	obj_track(&bright_slider);
	lv_slider_set_range(s, 5, 100);
	lv_slider_set_value(s, backlight_get(), LV_ANIM_OFF);
	lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(s, bright_cb, LV_EVENT_RELEASED, NULL);
	bright_val = slider_value(c);
	obj_track(&bright_val);
	lv_label_set_text_fmt(bright_val, "%d%%", backlight_get());

	if (als_present()) {
		c = m3_card(body, M.surface_c);
		lv_obj_set_height(c, LV_SIZE_CONTENT);
		lv_obj_set_style_min_height(c, 64, 0);
		s = m3_label(c, "Auto-brightness", &lv_font_montserrat_14, M.on_surface);
		lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 0);
		s = ios_switch(lv_switch_create(c));
		auto_bright_sw = s;
		obj_track(&auto_bright_sw);
		lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
		if (conf.auto_bright)
			lv_obj_add_state(s, LV_STATE_CHECKED);
		lv_obj_add_event_cb(s, auto_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}

	if (mbx_gpu_gamma_supported()) {
		m3_section(body, "Night Shift");
		c = m3_card(body, M.surface_c);
		lv_obj_set_height(c, LV_SIZE_CONTENT);
		lv_obj_set_style_min_height(c, 64, 0);
		s = m3_label(c, "Warm color tint", &lv_font_montserrat_14, M.on_surface);
		lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 0);
		s = ios_switch(lv_switch_create(c));
		night_sw = s;
		obj_track(&night_sw);
		lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
		if (conf.night_shift)
			lv_obj_add_state(s, LV_STATE_CHECKED);
		lv_obj_add_event_cb(s, night_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

		c = m3_card(body, M.surface_c);
		lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(c, 12, 0);
		m3_label(c, LV_SYMBOL_TINT, &lv_font_montserrat_20, M.on_surface_v);
		s = row_slider(c, false);
		night_slider = s;
		obj_track(&night_slider);
		lv_slider_set_range(s, 10, 100);
		lv_slider_set_value(s, conf.night_warmth, LV_ANIM_OFF);
		lv_obj_add_event_cb(s, night_warmth_cb, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(s, night_warmth_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(s, night_warmth_cb, LV_EVENT_PRESS_LOST, NULL);
		night_val = slider_value(c);
		obj_track(&night_val);
		lv_label_set_text_fmt(night_val, "%d%%", conf.night_warmth);
	}

	if (access(CLCD_MIE_PATH, W_OK) == 0) {
		m3_section(body, "Adaptive contrast");
		segmented_matrix(body, mie_map, (uint32_t)conf.mie_mode, mie_cb);
		m3_note(body, "Adjusts contrast and backlight for dark scenes. Video enables it only during playback.");
	}

	m3_section(body, "Theme");
	segmented_matrix(body, theme_map, conf.dark ? 0 : 1, theme_cb);

	m3_section(body, "Colour");
	row = lv_obj_create(body);
	plain(row);
	lv_obj_set_size(row, lv_pct(100), 56);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	for (i = 0; i < (unsigned int)N_ACCENTS; i++) {
		lv_obj_t *dot = lv_obj_create(row);

		plain(dot);
		lv_obj_set_size(dot, 40, 40);	/* six in the page's width, gaps between */
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(dot, tone(accents[i].hue, accents[i].sat, conf.dark ? 70 : 45), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		if ((int)i == conf.accent && !conf.wp_accent) {
			lv_obj_set_style_border_width(dot, 3, 0);
			lv_obj_set_style_border_color(dot, M.on_surface, 0);
		}
		lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(dot, accent_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}

	lock_col_section(body);

	m3_section(body, "Screen timeout");
	c = m3_card(body, M.surface_c);
	m3_label(c, "Turn the screen off after", &lv_font_montserrat_14, M.on_surface_v);
	dd = lv_dropdown_create(c);
	lv_dropdown_set_options(dd, "15 seconds\n30 seconds\n1 minute\n2 minutes\n5 minutes\nNever");
	for (i = 0; i < sizeof(idle_opts) / sizeof(idle_opts[0]); i++)
		if (idle_opts[i] == conf.idle_s)
			lv_dropdown_set_selected(dd, i);
	lv_obj_set_width(dd, lv_pct(100));
	lv_obj_align(dd, LV_ALIGN_TOP_LEFT, 0, 26);
	lv_obj_set_height(c, 100);
	lv_obj_add_event_cb(dd, idle_cb, LV_EVENT_VALUE_CHANGED, NULL);
	m3_note(body, "The screen locks when it turns off. On battery or a wall charger the "
		"iPod sleeps; connected to a computer only the screen turns off.");

	c = m3_card(body, M.surface_c);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(c, 64, 0);
	s = m3_label(c, "Wake when the cable is plugged in or out", &lv_font_montserrat_14,
		     M.on_surface);
	lv_obj_set_width(s, lv_pct(72));
	lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
	lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 0);
	s = ios_switch(lv_switch_create(c));
	lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
	if (conf.wake_plug)
		lv_obj_add_state(s, LV_STATE_CHECKED);
	lv_obj_add_event_cb(s, plug_cb, LV_EVENT_VALUE_CHANGED, NULL);
	push(scr);
}

/* -------------------------------------------------------------- Power */

static void power_do(lv_event_t *e)
{
	intptr_t what = (intptr_t)lv_event_get_user_data(e);

	conf_save_flush();
	sync();
	reboot(what == 1 ? RB_POWER_OFF : RB_AUTOBOOT);
}

void open_power(lv_event_t *e)
{
	nav_mark(open_power, NULL);
	lv_obj_t *scr, *body;

	LV_UNUSED(e);
	body = m3_page("Power", &scr);
	body = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(body, 4, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	m3_row(body, LV_SYMBOL_REFRESH, "Restart", "Back to the boot loader",
	       power_do, (void *)2, NULL);
	m3_row(body, LV_SYMBOL_POWER, "Power off", "Hold Power to turn on",
	       power_do, (void *)1, NULL);
	push(scr);
}

/* -------------------------------------------------------------- About */

/* ------------------------------------------------------------ Storage */

/*
 * Storage: the flash's root file system (df) as a bar in parts (system
 * and applications, media, other files, free) and a row for each part;
 * the folders' sizes come from du in the background. The media rows open
 * their folders in Files. Also the RAM file system (/tmp).
 */
static void storage_fmt(char *b, size_t n, uint64_t bytes, const char *tail)
{
	if (bytes >= 1000ULL * 1000 * 1000)
		snprintf(b, n, "%llu.%llu GB%s", (unsigned long long)(bytes / 1000000000ULL),
			 (unsigned long long)(bytes / 100000000ULL % 10), tail);
	else if (bytes >= 1000ULL * 1000)
		snprintf(b, n, "%llu MB%s", (unsigned long long)(bytes / 1000000ULL), tail);
	else
		snprintf(b, n, "%llu kB%s", (unsigned long long)(bytes / 1000ULL), tail);
}

static lv_obj_t *st_page, *st_head, *st_bar, *st_sub[7];

static void st_open_dir(lv_event_t *e)
{
	files_open_dir(lv_event_get_user_data(e));
}

/* df / and /tmp, du of the folders (kB) */
static void st_done(const char *out, void *ud)
{
	unsigned long long tot = 0, used = 0, avail = 0, ttot = 0, tused = 0, k;
	unsigned long long vid = 0, mus = 0, pic = 0, root = 0, usr = 0, var = 0;
	const char *p;
	char path[96], b[48];

	LV_UNUSED(ud);
	if (!st_page || !lv_obj_is_valid(st_page))
		return;
	for (p = out; p && *p; ) {
		char mnt[64];
		unsigned long long t, u, a;

		if (sscanf(p, "%*s %llu %llu %llu %*s %63s", &t, &u, &a, mnt) == 4) {
			if (!strcmp(mnt, "/")) {
				tot = t;
				used = u;
				avail = a;
			} else if (!strcmp(mnt, "/tmp")) {
				ttot = t;
				tused = u;
			}
		} else if (sscanf(p, "%llu %95s", &k, path) == 2) {
			if (!strcmp(path, "/root/Videos"))
				vid = k;
			else if (!strcmp(path, "/root/Music"))
				mus = k;
			else if (!strcmp(path, "/root/Pictures"))
				pic = k;
			else if (!strcmp(path, "/root"))
				root = k;
			else if (!strcmp(path, "/usr"))
				usr = k;
			else if (!strcmp(path, "/var"))
				var = k;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!tot)
		return;
	{
		unsigned long long media = vid + mus + pic;
		unsigned long long home = root > media ? root - media : 0;
		unsigned long long sys = used > root ? used - root : 0;	/* (system, apps, the rest) */
		unsigned long long parts[3] = { sys, media, home };
		int i;

		storage_fmt(b, sizeof(b), used * 1024, " used");
		label_set_if_changed(st_head, b);
		storage_fmt(b, sizeof(b), tot * 1024, "");
		{
			char line[96], fr[32];

			storage_fmt(fr, sizeof(fr), avail * 1024, " free");
			snprintf(line, sizeof(line), "of %s, %s", b, fr);
			label_set_if_changed(st_sub[0], line);
		}
		/* the bar: a part each, as wide as its share (at least a sliver) */
		for (i = 0; i < 3; i++) {
			lv_obj_t *seg = lv_obj_get_child(st_bar, i);
			int32_t w = (int32_t)(parts[i] * 1000 / tot);

			if (seg)
				lv_obj_set_width(seg, lv_pct(LV_MAX(parts[i] ? 1 : 0, w / 10)));
		}
		storage_fmt(b, sizeof(b), sys * 1024, "");
		label_set_if_changed(st_sub[1], b);
		storage_fmt(b, sizeof(b), vid * 1024, "");
		label_set_if_changed(st_sub[2], b);
		storage_fmt(b, sizeof(b), mus * 1024, "");
		label_set_if_changed(st_sub[3], b);
		storage_fmt(b, sizeof(b), pic * 1024, "");
		label_set_if_changed(st_sub[4], b);
		storage_fmt(b, sizeof(b), home * 1024, "");
		label_set_if_changed(st_sub[5], b);
		{
			char t1[32], t2[32], line[80];

			storage_fmt(t1, sizeof(t1), tused * 1024, "");
			storage_fmt(t2, sizeof(t2), ttot * 1024, "");
			snprintf(line, sizeof(line), "%s of %s (memory)", t1, t2);
			label_set_if_changed(st_sub[6], line);
		}
		LV_UNUSED(var);
		LV_UNUSED(usr);
	}
}

static void open_storage(lv_event_t *e)
{
	nav_mark(open_storage, NULL);
	static const struct {
		const char *icon, *title, *dir;
		int col;	/* 0 primary, 1 tertiary, 2 secondary */
	} rows[] = {
		{ LV_SYMBOL_SETTINGS, "System and apps", NULL, 0 },
		{ LV_SYMBOL_VIDEO, "Videos", FILES_ROOT "/Videos", 1 },
		{ LV_SYMBOL_AUDIO, "Music", FILES_ROOT "/Music", 1 },
		{ LV_SYMBOL_IMAGE, "Pictures", FILES_ROOT "/Pictures", 1 },
		{ LV_SYMBOL_DIRECTORY, "Other files", FILES_ROOT, 2 },
		{ LV_SYMBOL_REFRESH, "Temporary files", NULL, 2 },
	};
	lv_obj_t *scr, *body, *c, *seg;
	unsigned int i;

	LV_UNUSED(e);
	body = m3_page("Storage", &scr);
	st_page = scr;
	obj_track(&st_page);

	/* used of total, and the bar in parts */
	c = m3_card(body, M.surface_c);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 6, 0);
	st_head = m3_label(c, "...", &lv_font_montserrat_24, M.on_surface);
	obj_track(&st_head);
	st_sub[0] = m3_label(c, "Internal storage", &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&st_sub[0]);
	st_bar = lv_obj_create(c);
	obj_track(&st_bar);
	plain(st_bar);
	lv_obj_set_size(st_bar, lv_pct(100), 12);
	lv_obj_set_style_radius(st_bar, 6, 0);
	lv_obj_set_style_clip_corner(st_bar, true, 0);
	lv_obj_set_style_bg_color(st_bar, M.surface_c_highest, 0);	/* free */
	lv_obj_set_style_bg_opa(st_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(st_bar, 0, 0);
	lv_obj_set_style_pad_column(st_bar, 2, 0);
	lv_obj_set_flex_flow(st_bar, LV_FLEX_FLOW_ROW);
	lv_obj_remove_flag(st_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	for (i = 0; i < 3; i++) {
		seg = lv_obj_create(st_bar);
		plain(seg);
		lv_obj_set_size(seg, 0, lv_pct(100));
		lv_obj_set_style_radius(seg, 0, 0);
		lv_obj_set_style_bg_color(seg, i == 0 ? M.primary : i == 1 ? M.tertiary_c :
					  M.secondary_c, 0);
		lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
		/* the theme's padding is more than the bar is high: with it the
		 * segment counted as overflowing and drew a scrollbar */
		lv_obj_set_style_pad_all(seg, 0, 0);
		lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	}

	/* the parts */
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		lv_color_t bg = rows[i].col == 0 ? M.primary_c : rows[i].col == 1 ? M.tertiary_c :
				M.secondary_c;
		lv_color_t fg = rows[i].col == 0 ? M.on_primary_c : rows[i].col == 1 ?
				M.on_tertiary_c : M.on_secondary_c;

		m3_row_ex(c, rows[i].icon, bg, fg, rows[i].title, "...",
			  rows[i].dir ? st_open_dir : NULL, (void *)rows[i].dir,
			  &st_sub[i + 1], rows[i].dir != NULL);
		obj_track(&st_sub[i + 1]);
	}
	push(scr);
	job_run("df -k / /tmp; du -sk /root/Videos /root/Music /root/Pictures /root /usr /var "
		"2>/dev/null", scr, st_done, NULL);
}

static lv_obj_t *about_val[8];
static lv_timer_t *about_timer;

static bool usb_is_mtp(void)
{
	char buf[32] = "";
	FILE *f = fopen("/run/n72-usb", "r");

	if (!f)
		f = fopen("/var/lib/n72/usb-mode", "r");
	if (!f)
		return false;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return !strncmp(buf, "mtp", 3);
}

static void about_update(lv_timer_t *t)
{
	char buf[160], kernel[96] = "?", line[128], usb[32], wlan[32];
	long mem_total = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
	struct utsname u;
	struct sysinfo si;
	FILE *f;
	int cap, mv, ilim;
	enum batt_lvl blvl;

	LV_UNUSED(t);
	if (!uname(&u))
		snprintf(kernel, sizeof(kernel), "%s %s", u.release, u.version);
	sysinfo(&si);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %ld", &mem_total);
			sscanf(line, "MemAvailable: %ld", &mem_avail);
			sscanf(line, "SwapTotal: %ld", &swap_total);
			sscanf(line, "SwapFree: %ld", &swap_free);
		}
		fclose(f);
	}
	label_set_if_changed(about_val[0], "iPod touch 2G (N72AP)\nS5L8720, ARM1176 at 532 MHz");
	label_set_if_changed(about_val[1], kernel);
	snprintf(buf, sizeof(buf), "%ld h %02ld min", si.uptime / 3600, si.uptime / 60 % 60);
	label_set_if_changed(about_val[2], buf);
	snprintf(buf, sizeof(buf), "%ld of %ld MiB free\nzram swap: %ld of %ld MiB used",
		 mem_avail / 1024, mem_total / 1024, (swap_total - swap_free) / 1024,
		 swap_total / 1024);
	label_set_if_changed(about_val[3], buf);
	cap = read_sysfs_int("/sys/class/power_supply/battery/capacity", -1);
	mv = read_sysfs_int("/sys/class/power_supply/battery/voltage_now", 0) / 1000;
	ilim = read_sysfs_int("/sys/class/power_supply/usb/input_current_limit", 0) / 1000;
	blvl = battery_cap_level();
	snprintf(buf, sizeof(buf), "%d%%%s, %d.%02d V%s", cap,
		 blvl == BATT_CRITICAL ? " (Critical)" : blvl == BATT_LOW ? " (Low)" : "",
		 mv / 1000, mv % 1000 / 10, usb_powered() ? "" : ", on battery");
	if (usb_powered())
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\nUSB power, %d mA limit", ilim);
	label_set_if_changed(about_val[4], buf);
	if (usb_is_mtp())
		snprintf(usb, sizeof(usb), "MTP");
	else
		ip_of("usb0", usb, sizeof(usb));
	ip_of("wlan0", wlan, sizeof(wlan));
	snprintf(buf, sizeof(buf), "USB: %s\nWi-Fi: %s", usb, wlan);
	label_set_if_changed(about_val[5], buf);
}

static void about_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (about_timer)
		awake_timer_delete(about_timer);
	about_timer = NULL;
}

void open_about(lv_event_t *e)
{
	nav_mark(open_about, NULL);
	static const char *const titles[] = { "Device", "Kernel", "Uptime", "Memory",
		"Battery", "Network" };
	lv_obj_t *scr, *body, *c;
	unsigned int i;

	LV_UNUSED(e);
	body = m3_page("About", &scr);
	for (i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
		c = m3_card(body, M.surface_c);
		lv_obj_set_style_pad_all(c, 12, 0);
		lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
		m3_label(c, titles[i], &lv_font_montserrat_12, M.primary);
		about_val[i] = m3_label(c, "", &lv_font_montserrat_14, M.on_surface);
		obj_track(&about_val[i]);
		lv_obj_set_width(about_val[i], lv_pct(100));
		lv_label_set_long_mode(about_val[i], LV_LABEL_LONG_WRAP);
	}
	about_update(NULL);
	about_timer = awake_timer(about_update, 2000, NULL);
	lv_obj_add_event_cb(scr, about_delete_cb, LV_EVENT_DELETE, NULL);
	push(scr);
}

/* ----------------------------------------------------------- Settings */

/* -------------------------------------------------------------- sound */

/*
 * Volume keys change the sound volume and show a pop-up on the top layer;
 * the jack decides between earphones and speaker (audio.c).
 */
lv_obj_t *vol_hud, *vol_hud_slider, *vol_hud_icon;
lv_timer_t *vol_hud_timer;
static lv_obj_t *snd_page, *snd_slider, *snd_val, *snd_out_lbl, *set_snd_sub;

/*
 * Bluetooth audio: while a speaker or headphones are connected,
 * n72-audio-route (n72-base) makes them ALSA's default output and writes
 * /run/n72-audio (pcm=, addr=, name=). The volume then goes to bluez-alsa
 * (bluealsa-cli, 0..127), and the outputs show the device's name.
 */
#define BT_AUDIO_STATE	"/run/n72-audio"

static struct {
	bool on;
	time_t mtime;
	char pcm[128], name[64];
	bool busy;		/* a volume command is running */
	int sent, want;		/* last sent, wanted (0..100) */
} bta = { .sent = -1 };

static void bta_volume_done(const char *out, void *ud);

static void bta_volume_send(void)
{
	char cmd[200];
	int v;

	if (!bta.on || bta.busy || bta.want == bta.sent)
		return;
	v = (bta.want * 127 + 50) / 100;
	snprintf(cmd, sizeof(cmd), "bluealsa-cli volume %s %d %d", bta.pcm, v, v);
	bta.busy = true;
	bta.sent = bta.want;
	job_run(cmd, NULL, bta_volume_done, NULL);
}

static void bta_volume_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bta.busy = false;
	bta_volume_send();	/* the slider moved on meanwhile */
}

static void bta_volume(int pct)
{
	bta.want = pct;
	bta_volume_send();
}

static void snd_refresh_ui(void);

/* true when Bluetooth audio came or went */
static bool bta_check(void)
{
	struct stat st;
	bool on = stat(BT_AUDIO_STATE, &st) == 0;
	FILE *f;
	char line[192];

	if (on == bta.on && (!on || st.st_mtime == bta.mtime))
		return false;
	bta.on = on;
	bta.sent = -1;
	if (!on)
		return true;
	bta.mtime = st.st_mtime;
	bta.pcm[0] = bta.name[0] = 0;
	f = fopen(BT_AUDIO_STATE, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "pcm=", 4))
				snprintf(bta.pcm, sizeof(bta.pcm), "%s", line + 4);
			else if (!strncmp(line, "name=", 5))
				snprintf(bta.name, sizeof(bta.name), "%s", line + 5);
		}
		fclose(f);
	}
	if (!bta.pcm[0]) {
		bta.on = false;
		return true;
	}
	bta_volume(conf.vol);	/* the volume goes with you */
	return true;
}

static const char *snd_output_name(void)
{
	if (bta.on)
		return bta.name[0] ? bta.name : "Bluetooth";
	if (!snd_present)
		return "No sound card";
	if (jack_in)
		return "Earphones";
	return conf.spk ? "Speaker" : "Earphones (speaker off)";
}

static void snd_summary(char *buf, size_t len)
{
	if (!snd_present && !bta.on)
		snprintf(buf, len, "No sound card");
	else if (!conf.vol)
		snprintf(buf, len, "Muted, %s", snd_output_name());
	else
		snprintf(buf, len, "%d%%, %s", conf.vol, snd_output_name());
}

#define VOL_HUD_Y	36
#define VOL_HUD_HIDE_Y	(-84)

static void vol_hud_sig(char *buf, size_t len)
{
	snprintf(buf, len, "%d:%d", conf.vol, (int)W);
}

bool vol_hud_visible(void)
{
	return vol_hud && (!lv_obj_has_flag(vol_hud, LV_OBJ_FLAG_HIDDEN) || psl_active(vol_hud));
}

static void vol_hud_hide(lv_timer_t *t)
{
	LV_UNUSED(t);
	vol_hud_timer = NULL;
	if (vol_hud && lv_obj_has_state(vol_hud_slider, LV_STATE_PRESSED)) {
		vol_hud_timer = lv_timer_create(vol_hud_hide, 1500, NULL);	/* still held */
		lv_timer_set_repeat_count(vol_hud_timer, 1);
		return;
	}
	if (vol_hud && !psl_start_sig(vol_hud, VOL_HUD_HIDE_Y, 160, ios_path_ease_in,
				      true, vol_hud_sig)) {
		lv_obj_add_flag(vol_hud, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_y(vol_hud, VOL_HUD_HIDE_Y);
		app_dormant_sync();
	}
}

static void snd_refresh_ui(void)
{
	char buf[64];

	if (vol_hud) {
		if (!lv_obj_has_state(vol_hud_slider, LV_STATE_PRESSED))
			lv_slider_set_value(vol_hud_slider, conf.vol, LV_ANIM_OFF);
		label_set_if_changed(vol_hud_icon, !conf.vol ? LV_SYMBOL_MUTE :
				     conf.vol < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
		psl_check(vol_hud);
	}
	if (snd_page && lv_obj_is_valid(snd_page)) {
		lv_slider_set_value(snd_slider, conf.vol, LV_ANIM_OFF);
		snprintf(buf, sizeof(buf), conf.vol ? "%d%%" : "Mute", conf.vol);
		label_set_if_changed(snd_val, buf);
		label_set_if_changed(snd_out_lbl, snd_output_name());
	}
	if (set_snd_sub && lv_obj_is_valid(set_snd_sub)) {
		snd_summary(buf, sizeof(buf));
		label_set_if_changed(set_snd_sub, buf);
	}
}

static void volume_set(int pct, bool hud);
void vol_hud_show(void);

static void vol_hud_slider_cb(lv_event_t *e)
{
	volume_set(lv_slider_get_value(lv_event_get_target(e)), false);
	vol_hud_show();		/* keeps it up while dragged */
}

/*
 * The volume pop-up: the brightness control's look (a full-width pill in
 * surface container highest, the icon, a slider), raised over the page by
 * a shadow, sliding down below the status bar via psl. The slider can be
 * dragged too.
 */
void vol_hud_show(void)
{
	if (app_dormant)
		app_dormant_wake(false);
	mbx_gpu_hw_slide_abort(vol_hud);
	if (!vol_hud) {
		vol_hud = lv_obj_create(lv_layer_top());
		plain(vol_hud);
		lv_obj_set_size(vol_hud, W - 2 * STATUS_PAD, 56);
		lv_obj_set_pos(vol_hud, STATUS_PAD, VOL_HUD_HIDE_Y);
		lv_obj_set_style_radius(vol_hud, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(vol_hud, M.surface_c_highest, 0);
		lv_obj_set_style_bg_opa(vol_hud, LV_OPA_COVER, 0);
		lv_obj_set_style_shadow_width(vol_hud, 16, 0);
		lv_obj_set_style_shadow_offset_y(vol_hud, 4, 0);
		lv_obj_set_style_shadow_opa(vol_hud, LV_OPA_30, 0);
		lv_obj_set_style_pad_hor(vol_hud, 18, 0);
		lv_obj_remove_flag(vol_hud, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(vol_hud, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(vol_hud, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(vol_hud, 14, 0);
		vol_hud_icon = m3_label(vol_hud, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_16,
					M.on_surface_v);
		lv_obj_set_width(vol_hud_icon, 20);
		vol_hud_slider = row_slider(vol_hud, true);
		lv_slider_set_range(vol_hud_slider, 0, 100);
		lv_obj_add_event_cb(vol_hud_slider, vol_hud_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_remove_flag(vol_hud_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
		lv_obj_add_flag(vol_hud, LV_OBJ_FLAG_HIDDEN);
	}
	snd_refresh_ui();
	lv_obj_move_foreground(vol_hud);
	if (!vol_hud_visible() || psl_sliding(vol_hud)) {
		if (!psl_start_sig(vol_hud, VOL_HUD_Y, 180, ios_path_ease_out,
				   false, vol_hud_sig)) {
			lv_obj_set_y(vol_hud, VOL_HUD_Y);
			lv_obj_remove_flag(vol_hud, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (vol_hud_timer)
		lv_timer_reset(vol_hud_timer);
	else {
		vol_hud_timer = lv_timer_create(vol_hud_hide, 1500, NULL);
		lv_timer_set_repeat_count(vol_hud_timer, 1);
	}
}

static void volume_set(int pct, bool hud)
{
	conf.vol = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	if (bta.on)
		bta_volume(conf.vol);
	else
		snd_apply_volume(conf.vol);
	conf_save_later();	/* not a flash write per step of a drag */
	if (hud)
		vol_hud_show();
	else
		snd_refresh_ui();
}

/* volume keys: 16 steps of about 4 dB at the top, finer near mute */
void volume_step(int dir)
{
	int v = conf.vol;

	if (!snd_present && !bta.on)
		return;
	v += dir * ((dir > 0 ? v : v - 1) >= 30 ? 6 : 3);
	volume_set(v, true);
}

static void jack_check(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (bta_check()) {
		snd_refresh_ui();
		net_ui_sync();	/* the Bluetooth tile and row: the device's name or "On" */
	}
	if (!jack_poll())
		return;
	snd_apply_output(conf.spk);
	vol_hud_show();
}

static void snd_slider_cb(lv_event_t *e)
{
	volume_set(lv_slider_get_value(lv_event_get_target(e)), false);
	pf_label_changed(lv_event_get_target(e));
}

static void snd_spk_cb(lv_event_t *e)
{
	conf.spk = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	snd_apply_output(conf.spk);
	pf_label_changed(lv_event_get_target(e));
	conf_save();
	snd_refresh_ui();
}

static void snd_test_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!have_tool("n72tone"))
		return;
	/* through ALSA's default: mixed with what plays (dmix), or the
	 * Bluetooth device while one is connected */
	job_run("/usr/bin/n72tone -w -f 660 -a -12 -t 0.6 -r 44100 | "
		"aplay -q -D default -f S16_LE -c 2 -r 44100", NULL, NULL, NULL);
}

void open_sound(lv_event_t *e)
{
	nav_mark(open_sound, NULL);
	lv_obj_t *scr, *body, *c, *row, *sw;

	LV_UNUSED(e);
	body = m3_page("Sound", &scr);
	snd_page = scr;
	obj_track(&snd_page);

	m3_section(body, "Volume");
	c = m3_card(body, M.surface_c);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 12, 0);
	m3_label(c, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_20, M.on_surface_v);
	snd_slider = row_slider(c, false);
	obj_track(&snd_slider);
	lv_slider_set_range(snd_slider, 0, 100);
	lv_slider_set_value(snd_slider, conf.vol, LV_ANIM_OFF);
	lv_obj_add_event_cb(snd_slider, snd_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
	snd_val = slider_value(c);
	obj_track(&snd_val);

	m3_section(body, "Output");
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_AUDIO, "Playing on", snd_output_name(), NULL, NULL, &snd_out_lbl);
	obj_track(&snd_out_lbl);
	row = m3_row(c, LV_SYMBOL_VOLUME_MAX, "Speaker", "When no earphones are plugged in",
		     NULL, NULL, NULL);
	sw = ios_switch(lv_switch_create(row));
	if (conf.spk)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, snd_spk_cb, LV_EVENT_VALUE_CHANGED, NULL);
	m3_note(body, "The volume keys change the volume everywhere.");
	if (have_tool("n72tone"))
		m3_button(body, "Test sound", false, snd_test_cb, NULL);
	snd_refresh_ui();
	push(scr);
}

/* The jack's switch events (its input device): earphones in or out. */
static void jack_ev(int fd)
{
	LV_UNUSED(fd);
	if (!jack_poll())
		return;
	snd_apply_output(conf.spk);
	vol_hud_show();
}

/* /run changed: n72-audio-route (Bluetooth sound) or n72-usb (USB gadget mode). */
static void run_ev(int fd)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool ours = false, usb_ev = false;
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;

		while (p < buf + n) {
			struct inotify_event *ev = (struct inotify_event *)p;

			if (ev->len && !strcmp(ev->name, "n72-audio"))
				ours = true;
			else if (ev->len && !strcmp(ev->name, "n72-usb"))
				usb_ev = true;
			p += sizeof(*ev) + ev->len;
		}
	}
	if (ours && bta_check()) {
		snd_refresh_ui();
		net_ui_sync();	/* the Bluetooth tile and row: the device's name or "On" */
	}
	if (usb_ev)
		usb_refresh_ui();
}

/*
 * Events, not looking every 200 ms: the jack's input device, and an
 * inotify watch on /run (n72-audio-route writes /run/n72-audio by a
 * rename and removes it), both in the main loop's poll().
 */
void sound_ui_init(void)
{
	int ino;

	pf_init();
	glide_init();
	snd_init(conf.vol, conf.spk);
	if (jack_fd >= 0)
		ui_watch(jack_fd, jack_ev);
	ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino >= 0 && inotify_add_watch(ino, "/run", IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
					  IN_MOVED_TO | IN_MOVED_FROM) >= 0)
		ui_watch(ino, run_ev);
	else
		lv_timer_create(jack_check, 200, NULL);	/* (no inotify: look) */
	if (bta_check())
		snd_refresh_ui();
}

#include "apps.c"	/* Settings > App manager and Files > Install .ipa */
#include "wallpaper.c"	/* wallpapers: conversion, frames, the Settings page */
#include "datetime.c"	/* Settings > Date & Time: the zone, network or set time */

/* ------------------------------------------------------------------ USB */

#define USB_UDC_STATE "/sys/class/udc/38400000.usb/state"

static lv_obj_t *set_usb_sub, *usb_page, *usb_mode_mat, *usb_state_lbl, *usb_detail_lbl;
static int usb_udc_fd = -1;
static bool usb_busy;

static void usb_summary(char *buf, size_t len)
{
	snprintf(buf, len, "%s", usb_is_mtp() ? "File transfer (MTP)" : "Networking");
}

static void usb_udc_state(char *buf, size_t len)
{
	buf[0] = 0;
	if (!len)
		return;
	if (usb_udc_fd >= 0) {
		ssize_t n = pread(usb_udc_fd, buf, len - 1, 0);

		if (n > 0) {
			buf[n] = 0;
			buf[strcspn(buf, "\r\n")] = 0;
		}
		return;
	}
	{
		FILE *f = fopen(USB_UDC_STATE, "r");

		if (!f)
			return;
		if (!fgets(buf, (int)len, f))
			buf[0] = 0;
		fclose(f);
		buf[strcspn(buf, "\r\n")] = 0;
	}
}

void usb_refresh_ui(void)
{
	char buf[96], detail[96];
	bool mtp = usb_is_mtp();

	if (set_usb_sub) {
		usb_summary(buf, sizeof(buf));
		label_set_if_changed(set_usb_sub, buf);
	}
	if (!usb_page)
		return;
	if (usb_mode_mat && !usb_busy &&
	    !lv_buttonmatrix_has_button_ctrl(usb_mode_mat, mtp ? 1 : 0,
					     LV_BUTTONMATRIX_CTRL_CHECKED))
		lv_buttonmatrix_set_button_ctrl(usb_mode_mat, mtp ? 1 : 0,
						LV_BUTTONMATRIX_CTRL_CHECKED);
	if (usb_state_lbl) {
		const char *conn = usb_sdp() ? "Computer (USB)" :
				   usb_powered() ? "Wall charger" : "Not connected";
		label_set_if_changed(usb_state_lbl, conn);
	}
	if (usb_detail_lbl) {
		if (usb_busy) {
			snprintf(detail, sizeof(detail), "Switching USB mode...");
		} else if (mtp) {
			usb_udc_state(buf, sizeof(buf));
			snprintf(detail, sizeof(detail), "Exposing / over MTP%s%s%s",
				 buf[0] ? " (" : "", buf, buf[0] ? ")" : "");
		} else {
			ip_of("usb0", buf, sizeof(buf));
			snprintf(detail, sizeof(detail), "usb0: %s", buf);
		}
		label_set_if_changed(usb_detail_lbl, detail);
	}
}

static void usb_switch_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	usb_busy = false;
	usb_refresh_ui();
}

static void usb_mode_cb(lv_event_t *e)
{
	uint32_t sel = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
	bool want_mtp, cur = usb_is_mtp();

	if (sel > 1 || usb_busy) {
		if (usb_mode_mat &&
		    !lv_buttonmatrix_has_button_ctrl(usb_mode_mat, cur ? 1 : 0,
						     LV_BUTTONMATRIX_CTRL_CHECKED))
			lv_buttonmatrix_set_button_ctrl(usb_mode_mat, cur ? 1 : 0,
							LV_BUTTONMATRIX_CTRL_CHECKED);
		return;
	}
	want_mtp = sel == 1;
	if (want_mtp == cur)
		return;
	usb_busy = true;
	usb_refresh_ui();
	if (job_run(want_mtp ? "/usr/sbin/n72-usb set mtp" : "/usr/sbin/n72-usb set net",
		    NULL, usb_switch_done, NULL) < 0) {
		usb_busy = false;
		usb_refresh_ui();
	}
}

static void usb_udc_ev(int fd)
{
	char buf[32];

	if (pread(fd, buf, sizeof(buf), 0) < 0)
		return;
	if (!screen_off)
		usb_refresh_ui();
}

static void usb_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (usb_udc_fd >= 0) {
		ui_unwatch(usb_udc_fd);
		close(usb_udc_fd);
		usb_udc_fd = -1;
	}
}

static void open_usb(lv_event_t *e)
{
	static const char *const usb_mode_map[] = { "Network", "MTP", "" };
	lv_obj_t *scr, *body, *c;

	LV_UNUSED(e);
	nav_mark(open_usb, NULL);
	body = m3_page("USB", &scr);
	usb_page = scr;
	obj_track(&usb_page);

	m3_section(body, "Mode");
	usb_mode_mat = segmented_matrix(body, usb_mode_map, usb_is_mtp() ? 1 : 0, usb_mode_cb);
	obj_track(&usb_mode_mat);

	m3_section(body, "Status");
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 12, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 4, 0);
	m3_label(c, "Connection", &lv_font_montserrat_12, M.primary);
	usb_state_lbl = m3_label(c, "", &lv_font_montserrat_14, M.on_surface);
	obj_track(&usb_state_lbl);
	m3_label(c, "Active interface", &lv_font_montserrat_12, M.primary);
	usb_detail_lbl = m3_label(c, "", &lv_font_montserrat_14, M.on_surface);
	obj_track(&usb_detail_lbl);
	lv_obj_set_width(usb_detail_lbl, lv_pct(100));
	lv_label_set_long_mode(usb_detail_lbl, LV_LABEL_LONG_WRAP);

	m3_note(body, "Network provides USB Ethernet (SSH at 192.168.7.2). MTP exposes the "
		"full filesystem (/) for file transfers. Switching reconnects USB.");

	if (usb_udc_fd >= 0) {
		ui_unwatch(usb_udc_fd);
		close(usb_udc_fd);
		usb_udc_fd = -1;
	}
	usb_udc_fd = open(USB_UDC_STATE, O_RDONLY | O_CLOEXEC);
	if (usb_udc_fd >= 0) {
		char dummy[32];

		(void)pread(usb_udc_fd, dummy, sizeof(dummy), 0);
		ui_watch_ev(usb_udc_fd, usb_udc_ev, POLLPRI);
	}
	usb_refresh_ui();
	lv_obj_add_event_cb(scr, usb_delete_cb, LV_EVENT_DELETE, NULL);
	push(scr);
}

void open_settings(lv_event_t *e)
{
	nav_mark(open_settings, NULL);
	lv_obj_t *scr, *body, *c;
	char sub[80];

	LV_UNUSED(e);
	body = m3_page("Settings", &scr);

	/* Connectivity */
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_WIFI, "Wi-Fi", wifi_up ? wifi_ssid : "Not connected",
	       open_wifi, NULL, &set_wifi_sub);
	obj_track(&set_wifi_sub);
	m3_row(c, LV_SYMBOL_BLUETOOTH, "Bluetooth",
	       !have_tool("bluetoothctl") ? "Not installed" : bt_powered ? "On" : "Off",
	       open_bt, NULL, &set_bt_sub);
	obj_track(&set_bt_sub);
	if (access("/usr/sbin/n72-usb", X_OK) == 0) {
		usb_summary(sub, sizeof(sub));
		m3_row(c, LV_SYMBOL_USB, "USB", sub, open_usb, NULL, &set_usb_sub);
		obj_track(&set_usb_sub);
	}

	/* Appearance & Sound */
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	snprintf(sub, sizeof(sub), "%s, %s, %d%%", conf.dark ? "Dark" : "Light",
		 conf.wp_accent ? "wallpaper colours" : accents[conf.accent].name, backlight_get());
	m3_row(c, LV_SYMBOL_IMAGE, "Display", sub, open_display, NULL, NULL);
	m3_row(c, LV_SYMBOL_HOME, "Wallpaper", wp.set[WP_HOME] || wp.set[WP_LOCK] ?
	       (wp.set[WP_HOME] && wp.set[WP_LOCK] ? "Home and lock screens" : wp.set[WP_HOME] ?
		"Home screen" : "Lock screen") : "None", open_wallpaper, NULL, NULL);
	snd_summary(sub, sizeof(sub));
	m3_row(c, LV_SYMBOL_VOLUME_MAX, "Sound", sub, open_sound, NULL, &set_snd_sub);
	obj_track(&set_snd_sub);

	/* General, Storage & Apps */
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_BELL, "Date & Time", dt_zone_name(), open_datetime, NULL, NULL);
	m3_row(c, LV_SYMBOL_GPS, "Weather", conf.wx_set ? conf.wx_name : "Off", open_weather, NULL, NULL);
	{
		struct statvfs v;

		if (!statvfs("/", &v))
			storage_fmt(sub, sizeof(sub), (uint64_t)v.f_bavail * v.f_frsize, " free");
		else
			snprintf(sub, sizeof(sub), "Space used and free");
		m3_row(c, LV_SYMBOL_SD_CARD, "Storage", sub, open_storage, NULL, NULL);
	}
	if (!access(IOS_TILES, X_OK) || !access(touchhle_dir(), F_OK)) {
		apps_summary(sub, sizeof(sub));
		m3_row(c, LV_SYMBOL_LIST, "App manager", sub, open_apps, NULL, &set_apps_sub);
		obj_track(&set_apps_sub);
	}

	/* System & Power */
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_POWER, "Power", "Restart, power off", open_power, NULL, NULL);
	m3_row(c, LV_SYMBOL_EYE_OPEN, "About", "Device, memory, battery",
	       open_about, NULL, NULL);
	m3_row(c, LV_SYMBOL_FILE, "Logs", "Kernel messages", open_logs, NULL, NULL);
	push(scr);
}
