/*
 * ui_m3.c - n72-ui's screens in a Material You (Material 3) style, sized
 * for the iPod touch 2G's 320x480 portrait panel. The interface is this
 * file and the ui_*.c files, a banner section each; what they and main.c
 * share is declared in ui_m3_priv.h and main_priv.h (backlight_*,
 * system_sleep, screen_off, W/H...).
 *
 * Settings, networks and Bluetooth go through the standard tools: wpa_cli
 * (wpa_supplicant's control client) and bluetoothctl (BlueZ), run in the
 * background so the UI never blocks on them.
 *
 * Settings persist in /tmp/n72-ui.conf for this boot (no persistent
 * storage yet).
 */

#include "ui_m3_priv.h"


bool system_sleep(void);
void kbd_toggle_app(void);	/* the system keyboard (kbd.c) */
void kbd_app_gone(void);
void kbd_hide_now(bool anim);
void kbd_turned(void);
void kbd_init(void);

/* turning and the video controls (at the end) */
extern int ui_rot, video_rot;
char *app_cmd;		/* the running application's command */
extern lv_obj_t *home_apps;
lv_obj_t *home_dock;	/* (dock.c) */
bool app_is_video(void);
bool cmd_is_video(const char *cmd);
bool app_is_game(void);
bool cmd_is_game(const char *cmd);
extern int rot_side, game_rot;	/* (defined with the turning code below) */
void open_games(lv_event_t *e);
bool gp_up(void);		/* the games' pause panel (ui_games.c) */
bool gp_home(void);
void gp_gone(void);
void rot_kick(void);
bool ui_turn(int rot);
void vc_start(void);
void vc_stop(void);
/* background playback (with the video controls, at the end) */
bool bg_start(void);
void bg_end(void);
bool bg_busy(void);
void np_build(lv_obj_t *shade, int32_t y);
void np_sync(void);
void np_opened(void);
/* the shade's card for it, and the status bar's note symbol */
struct np np;
lv_obj_t *status_np;
/* the same on the lock screen (np_lock_build) */
struct npl npl;
void np_lock_build(lv_obj_t *scr);
void np_lock_sync(void);

/* ------------------------------------------------------------ settings */

#define CONF_OLD "/tmp/n72-ui.conf"


struct n72_conf conf = { true, 0, 120, 60, true, true, 0, 0, 0, false, false, 50, 0, -1, -1, false, "", false, "", false, "", false, "", 0, 0, false, false };

/*
 * Auto-rotate on: the screen turns with the iPod. Off: it stays as it was
 * held when turned off (rotlock), and the accelerometer powers down.
 */

const struct accents accents[] = {
	{ "Violet", 265, 48 }, { "Blue", 215, 60 }, { "Teal", 178, 55 },
	{ "Green", 135, 40 }, { "Amber", 38, 70 }, { "Rose", 340, 55 },
};

struct wp wp = { .hue = -1 };
void wp_load(void);
bool wp_put(lv_obj_t *scr, int kind);
bool home_wp;		/* home is built over a wallpaper */
lv_draw_buf_t *wp_pic(int kind);
lv_draw_buf_t *wp_upright(int kind);
bool wp_hold(lv_obj_t *img, lv_draw_buf_t *b);
void wp_apply_kick(void);	/* (something in the way of new wallpaper frames ended) */
void dock_carry_stop(void);	/* (dock.c: a carried icon put down) */
bool pg_drag(int phase, int x, int y);
void pg_fb0_dirty(void);
void pg_abort(void);

void conf_load(void)
{
	char line[1300];
	FILE *f = fopen(CONF_PATH, "r");
	const char *e = getenv("N72_IDLE_SLEEP");

	if (!f)
		f = fopen(CONF_OLD, "r");

	if (e)
		conf.idle_s = atoi(e);
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		int v;

		if (sscanf(line, "dark=%d", &v) == 1)
			conf.dark = v;
		else if (sscanf(line, "accent=%d", &v) == 1 && v >= 0 && v < N_ACCENTS)
			conf.accent = v;
		else if (sscanf(line, "idle=%d", &v) == 1 && v >= 0)
			conf.idle_s = v;
		else if (sscanf(line, "vol=%d", &v) == 1 && v >= 0 && v <= 100)
			conf.vol = v;
		else if (sscanf(line, "spk=%d", &v) == 1)
			conf.spk = v;
		else if (sscanf(line, "plug=%d", &v) == 1)
			conf.wake_plug = v;
		else if (sscanf(line, "rot=%d", &v) == 1 && v >= 0 && v <= 2) {
			conf.autorot = v == ROT_AUTO;
			if (v == 2)		/* (the old landscape lock) */
				conf.rotlock = 90;
		} else if (sscanf(line, "rotlock=%d", &v) == 1 && (v == 0 || v == 90 || v == 270))
			conf.rotlock = v;
		else if (sscanf(line, "bright=%d", &v) == 1 && v >= 5 && v <= 100)
			conf.bright = v;
		else if (sscanf(line, "autobright=%d", &v) == 1)
			conf.auto_bright = !!v;
		else if (sscanf(line, "night=%d", &v) == 1)
			conf.night_shift = !!v;
		else if (sscanf(line, "warmth=%d", &v) == 1 && v >= 10 && v <= 100)
			conf.night_warmth = v;
		else if (sscanf(line, "mie=%d", &v) == 1 && v >= 0 && v <= 2)
			conf.mie_mode = v;
		else if (sscanf(line, "wifi=%d", &v) == 1)
			conf.wifi = !!v;
		else if (sscanf(line, "bt=%d", &v) == 1)
			conf.bt = !!v;
		else if (sscanf(line, "wpaccent=%d", &v) == 1)
			conf.wp_accent = !!v;
		else if (!strncmp(line, "wx=", 3)) {
			int n = 0;

			if (sscanf(line + 3, "%lf,%lf,%n", &conf.wx_lat, &conf.wx_lon, &n) == 2 && n > 0) {
				snprintf(conf.wx_name, sizeof(conf.wx_name), "%s", line + 3 + n);
				conf.wx_name[strcspn(conf.wx_name, "\r\n")] = 0;
				conf.wx_set = true;
			}
		} else if (sscanf(line, "wxf=%d", &v) == 1)
			conf.wx_f = !!v;
		else if (!strncmp(line, "order=", 6)) {
			snprintf(conf.order, sizeof(conf.order), "%s", line + 6);
			conf.order[strcspn(conf.order, "\r\n")] = 0;
			conf.order_set = true;
		} else if (!strncmp(line, "tz=", 3)) {
			snprintf(conf.tz, sizeof(conf.tz), "%s", line + 3);
			conf.tz[strcspn(conf.tz, "\r\n")] = 0;
			conf.tz_set = true;
		} else if (!strncmp(line, "dock=", 5)) {
			snprintf(conf.dock, sizeof(conf.dock), "%s", line + 5);
			conf.dock[strcspn(conf.dock, "\r\n")] = 0;
			conf.dock_set = true;
		}
	}
	fclose(f);
}

void conf_save(void)
{
	FILE *f;

	mkdir(CONF_DIR, 0755);
	f = fopen(CONF_PATH ".new", "w");
	if (!f)
		return;
	fprintf(f, "dark=%d\naccent=%d\nidle=%d\nvol=%d\nspk=%d\nplug=%d\nrot=%d\nrotlock=%d\n"
		"bright=%d\nautobright=%d\nnight=%d\nwarmth=%d\nmie=%d\nwifi=%d\nbt=%d\nwpaccent=%d\n",
		conf.dark, conf.accent, conf.idle_s, conf.vol, conf.spk, conf.wake_plug,
		conf.autorot, conf.rotlock, conf.bright, conf.auto_bright, conf.night_shift,
		conf.night_warmth, conf.mie_mode, conf.wifi, conf.bt, conf.wp_accent);
	if (conf.dock_set)	/* (only once changed: the file is as it was till then) */
		fprintf(f, "dock=%s\n", conf.dock);
	if (conf.order_set)
		fprintf(f, "order=%s\n", conf.order);
	if (conf.tz_set)
		fprintf(f, "tz=%s\n", conf.tz);
	if (conf.wx_set)
		fprintf(f, "wx=%.4f,%.4f,%s\n", conf.wx_lat, conf.wx_lon, conf.wx_name);
	if (conf.wx_f)
		fprintf(f, "wxf=1\n");
	if (fflush(f) == 0)
		fsync(fileno(f));
	fclose(f);
	rename(CONF_PATH ".new", CONF_PATH);	/* (whole or not at all) */
}

/* A setting that moves a lot (the brightness slider): saved once it rests. */
static lv_timer_t *conf_save_timer;

static void conf_save_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	conf_save_timer = NULL;
	conf_save();
}

void conf_save_later(void)
{
	if (conf_save_timer)
		lv_timer_reset(conf_save_timer);
	else {
		conf_save_timer = lv_timer_create(conf_save_cb, 2000, NULL);
		lv_timer_set_repeat_count(conf_save_timer, 1);
	}
}

/* --------------------------------------------------- Material You theme */

struct M M;

/* HSL tone (0-100 lightness) of the accent hue, Material 3 style. */
lv_color_t tone(uint16_t h, uint8_t s, uint8_t l)
{
	float hf = h / 60.0f, sf = s / 100.0f, lf = l / 100.0f;
	float c = (1 - fabsf(2 * lf - 1)) * sf;
	float x = c * (1 - fabsf(fmodf(hf, 2) - 1));
	float m = lf - c / 2, r = 0, g = 0, b = 0;

	if (hf < 1)      { r = c; g = x; }
	else if (hf < 2) { r = x; g = c; }
	else if (hf < 3) { g = c; b = x; }
	else if (hf < 4) { g = x; b = c; }
	else if (hf < 5) { r = x; b = c; }
	else             { r = c; b = x; }
	return lv_color_make((r + m) * 255 + 0.5f, (g + m) * 255 + 0.5f,
			     (b + m) * 255 + 0.5f);
}

static void m3_build_palette(void)
{
	bool wa = conf.wp_accent && wp.set[WP_HOME] && wp.hue >= 0;
	uint16_t h = wa ? wp.hue : accents[conf.accent].hue, h2 = (h + 60) % 360;
	uint8_t s = wa ? wp.sat : accents[conf.accent].sat, sn = 8, snv = 14;

	if (conf.dark) {
		M.primary = tone(h, s, 80);	M.on_primary = tone(h, s, 20);
		M.primary_c = tone(h, s, 30);	M.on_primary_c = tone(h, s, 90);
		M.secondary_c = tone(h, s / 3, 30); M.on_secondary_c = tone(h, s / 3, 90);
		M.tertiary_c = tone(h2, s / 2, 30); M.on_tertiary_c = tone(h2, s / 2, 90);
		M.surface = tone(h, sn, 6);	M.surface_c = tone(h, sn, 12);
		M.surface_c_high = tone(h, sn, 17); M.surface_c_highest = tone(h, sn, 22);
		M.on_surface = tone(h, sn, 90);	M.on_surface_v = tone(h, snv, 80);
		M.outline = tone(h, snv, 60);	M.outline_v = tone(h, snv, 30);
		M.error = lv_color_hex(0xffb4ab);
	} else {
		M.primary = tone(h, s, 40);	M.on_primary = lv_color_white();
		M.primary_c = tone(h, s, 90);	M.on_primary_c = tone(h, s, 10);
		M.secondary_c = tone(h, s / 3, 90); M.on_secondary_c = tone(h, s / 3, 10);
		M.tertiary_c = tone(h2, s / 2, 90); M.on_tertiary_c = tone(h2, s / 2, 10);
		M.surface = tone(h, sn, 98);	M.surface_c = tone(h, sn, 94);
		M.surface_c_high = tone(h, sn, 92); M.surface_c_highest = tone(h, sn, 90);
		M.on_surface = tone(h, sn, 10);	M.on_surface_v = tone(h, snv, 30);
		M.outline = tone(h, snv, 50);	M.outline_v = tone(h, snv, 80);
		M.error = lv_color_hex(0xba1a1a);
	}
}

void m3_apply_theme(void)
{
	lv_display_t *d = lv_display_get_default();
	lv_theme_t *th;

	m3_build_palette();
	th = lv_theme_default_init(d, M.primary, M.tertiary_c, conf.dark,
				   &lv_font_montserrat_16);
	lv_display_set_theme(d, th);
}

/* ------------------------------------------------------------ widgets */

void m3_screen(lv_obj_t *scr)
{
	lv_obj_set_style_bg_color(scr, M.surface, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, M.on_surface, 0);
	lv_obj_set_style_text_font(scr, &lv_font_montserrat_16, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

/*
 * A global that points at an object of a page: cleared when the object
 * is deleted (the page closed, or rebuilt by a turn). lv_obj_is_valid()
 * alone is not enough: a new object may take the freed one's address, and
 * the old pointer then "is valid" but points at something else.
 */
static void obj_untrack_cb(lv_event_t *e)
{
	lv_obj_t **pp = lv_event_get_user_data(e);

	if (*pp == lv_event_get_target(e))
		*pp = NULL;
}

void obj_track(lv_obj_t **pp)
{
	if (*pp)
		lv_obj_add_event_cb(*pp, obj_untrack_cb, LV_EVENT_DELETE, pp);
}

void plain(lv_obj_t *o)
{
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_shadow_width(o, 0, 0);
	lv_obj_set_style_outline_width(o, 0, 0);
}

lv_obj_t *m3_card(lv_obj_t *parent, lv_color_t bg)
{
	lv_obj_t *c = lv_obj_create(parent);

	plain(c);
	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(c, bg, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(c, 20, 0);
	lv_obj_set_style_pad_all(c, 16, 0);
	lv_obj_set_style_text_color(c, M.on_surface, 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	return c;
}

lv_obj_t *m3_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
			  lv_color_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, color, 0);
	return l;
}

/*
 * A slider in a row (after the row's gap is set): its knob reaches half its
 * size past the track's ends, into the gaps beside it, so those grow by as
 * much (and the row's end padding too when the slider is last). The knob
 * then keeps the row's spacing to what is beside it, at 0 as at the maximum.
 */
lv_obj_t *row_slider(lv_obj_t *parent, bool last)
{
	lv_obj_t *s = lv_slider_create(parent);
	int32_t over = lv_obj_get_style_height(s, LV_PART_MAIN) / 2 +
		       lv_obj_get_style_pad_right(s, LV_PART_KNOB);

	lv_obj_set_style_pad_column(parent, lv_obj_get_style_pad_column(parent, 0) + over, 0);
	if (last)
		lv_obj_set_style_pad_right(parent, lv_obj_get_style_pad_right(parent, 0) + over, 0);
	lv_obj_set_flex_grow(s, 1);
	return s;
}

/*
 * The value beside a slider ("60%", "Mute"): as wide as its widest text,
 * right-aligned, so the slider keeps its length while it is dragged (a
 * label that grew from "99%" to "100%" shortened it under the finger, and
 * the knob jumped and the end padding changed).
 */
lv_obj_t *slider_value(lv_obj_t *parent)
{
	const lv_font_t *f = &lv_font_montserrat_14;
	lv_obj_t *l = m3_label(parent, "", f, M.on_surface_v);
	int32_t w = LV_MAX(lv_text_get_width("100%", 4, f, 0), lv_text_get_width("Mute", 4, f, 0));

	lv_obj_set_width(l, w);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
	return l;
}

/*
 * Symbols of our own beside LVGL's (n72_icons.c, tools/svg2glyph.py): a
 * badge's label takes their font when its text is one of them.
 */
#include "n72_icons.c"

const lv_font_t *icon_font(const char *icon, bool big)
{
	if (icon && (unsigned char)icon[0] == 0xEE && (unsigned char)icon[1] == 0x80)
		return big ? &n72_icon_24 : &n72_icon_20;
	return big ? &lv_font_montserrat_24 : &lv_font_montserrat_20;
}

/* A round icon "badge" in a tonal container, as Material 3 list items use. */
lv_obj_t *m3_badge(lv_obj_t *parent, const char *icon, lv_color_t bg,
			  lv_color_t fg, int size)
{
	lv_obj_t *b = lv_obj_create(parent), *l;

	plain(b);
	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(b, bg, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	l = m3_label(b, icon, icon_font(icon, size >= 48), fg);
	lv_obj_center(l);
	return b;
}

/* A switch as iOS's: the thumb goes over in 250 ms, ease-in-out (UISwitch). */
static void ios_switch_cb(lv_event_t *e)
{
	lv_anim_t *a = lv_anim_get(lv_event_get_current_target(e), NULL);

	if (a)	/* (LVGL's own, started just before: linear) */
		lv_anim_set_path_cb(a, ios_path_ease_in_out);
}

lv_obj_t *ios_switch(lv_obj_t *sw)
{
	lv_obj_set_style_anim_duration(sw, 250, LV_PART_MAIN);
	lv_obj_add_event_cb(sw, ios_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
	return sw;
}

/*
 * A row's press, as iOS's: the highlight shows for at least 120 ms however
 * short the tap (UIButton/UITableViewCell), and goes in a 500 ms fade
 * (deselect, ease-in-out). Only the tint's opacity changes: the row is drawn
 * again for those frames (the tint is translucent, so a picture would not
 * spare that), comes up at once.
 */
#define ROW_HOLD_MS	120
#define ROW_FADE_MS	500

static lv_obj_t *row_held;
static uint32_t row_press_ms;
static lv_timer_t *row_hold_timer;

static void row_hold_end(lv_timer_t *t)
{
	LV_UNUSED(t);
	row_hold_timer = NULL;
	if (row_held && lv_obj_is_valid(row_held))
		lv_obj_remove_state(row_held, LV_STATE_PRESSED);
	row_held = NULL;
}

static void row_press_cb(lv_event_t *e)
{
	lv_obj_t *r = lv_event_get_current_target(e);
	uint32_t held;

	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		if (row_hold_timer) {	/* (the last tap's highlight goes now) */
			lv_timer_delete(row_hold_timer);
			row_hold_end(NULL);
		}
		row_press_ms = lv_tick_get();
		return;
	}
	held = lv_tick_elaps(row_press_ms);	/* (released) */
	/* (a flick that started on the row is a scroll, not a tap: no highlight) */
	if (held >= ROW_HOLD_MS || !lv_obj_is_valid(r) ||
	    (lv_indev_active() && lv_indev_get_scroll_obj(lv_indev_active())))
		return;
	lv_obj_add_state(r, LV_STATE_PRESSED);	/* (LVGL took it off already) */
	row_held = r;
	row_hold_timer = lv_timer_create(row_hold_end, ROW_HOLD_MS - held, NULL);
	lv_timer_set_repeat_count(row_hold_timer, 1);
}

static void row_deleted_cb(lv_event_t *e)
{
	if (row_held == lv_event_get_current_target(e))
		row_held = NULL;
}

static void row_press_style(lv_obj_t *r)
{
	static const lv_style_prop_t props[] = { LV_STYLE_BG_OPA, 0 };
	static lv_style_transition_dsc_t fade, none;
	static bool init;

	if (!init) {
		lv_style_transition_dsc_init(&fade, props, ios_path_ease_in_out, ROW_FADE_MS, 0, NULL);
		lv_style_transition_dsc_init(&none, props, lv_anim_path_linear, 0, 0, NULL);
		init = true;
	}
	lv_obj_set_style_transition(r, &fade, 0);		/* into the plain state */
	lv_obj_set_style_transition(r, &none, LV_STATE_PRESSED);	/* pressed: at once */
	lv_obj_add_event_cb(r, row_press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(r, row_press_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(r, row_deleted_cb, LV_EVENT_DELETE, NULL);
}

/*
 * A 64 px list row: badge, title and supporting text, optional trailing
 * widget area (returned in *trail). Clicking runs cb.
 */
/*
 * A two-line list item (Material 3): leading icon in a tonal circle
 * (@bg/@fg), headline and supporting text, a trailing chevron for an item
 * that navigates.
 */
lv_obj_t *m3_row_ex(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_color_t fg,
			   const char *title, const char *sub, lv_event_cb_t cb, void *ud,
			   lv_obj_t **sub_lbl, bool chevron)
{
	lv_obj_t *r = lv_obj_create(parent), *t, *col;

	plain(r);
	lv_obj_set_size(r, lv_pct(100), 64);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(r, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
	row_press_style(r);
	lv_obj_set_style_radius(r, 16, 0);
	lv_obj_set_style_pad_hor(r, 8, 0);
	lv_obj_set_style_pad_ver(r, 0, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 14, 0);
	if (icon)
		m3_badge(r, icon, bg, fg, 40);
	col = lv_obj_create(r);
	plain(col);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	t = m3_label(col, title, &lv_font_montserrat_16, M.on_surface);
	lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
	lv_obj_set_size(t, lv_pct(100), 20);
	t = m3_label(col, sub ? sub : "", &lv_font_montserrat_14, M.on_surface_v);
	lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
	lv_obj_set_size(t, lv_pct(100), 18);
	if (!sub)
		lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
	if (sub_lbl)
		*sub_lbl = t;
	if (cb)
		lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
	if (chevron)
		m3_label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, M.on_surface_v);
	return r;
}

/* A row marked as the current one (the network or device in use):
 * Material's primary container, its text and badge to match. */
void m3_row_current(lv_obj_t *r)
{
	lv_obj_t *badge = lv_obj_get_child(r, 0), *col = lv_obj_get_child(r, 1);
	uint32_t i;

	lv_obj_set_style_bg_color(r, M.primary_c, 0);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
	if (badge && lv_obj_get_child_count(badge)) {
		lv_obj_set_style_bg_color(badge, M.primary, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(badge, 0), M.on_primary, 0);
	}
	for (i = 0; col && i < lv_obj_get_child_count(col); i++)
		lv_obj_set_style_text_color(lv_obj_get_child(col, i), M.on_primary_c, 0);
	if (lv_obj_get_child_count(r) > 2)	/* the chevron */
		lv_obj_set_style_text_color(lv_obj_get_child(r, 2), M.on_primary_c, 0);
}

lv_obj_t *m3_row(lv_obj_t *parent, const char *icon, const char *title,
			const char *sub, lv_event_cb_t cb, void *ud, lv_obj_t **sub_lbl)
{
	return m3_row_ex(parent, icon, M.secondary_c, M.on_secondary_c, title, sub, cb, ud,
			 sub_lbl, cb != NULL);
}

void m3_section(lv_obj_t *parent, const char *title)
{
	lv_obj_t *l = m3_label(parent, title, &lv_font_montserrat_14, M.primary);

	lv_obj_set_style_pad_left(l, 8, 0);
	lv_obj_set_style_pad_top(l, 8, 0);
}

/* Wrapped supporting text. */
void m3_note(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = m3_label(parent, text, &lv_font_montserrat_14, M.on_surface_v);

	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_pad_hor(l, 8, 0);
}

lv_obj_t *m3_button(lv_obj_t *parent, const char *text, bool filled,
			   lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	plain(b);
	lv_obj_set_height(b, 44);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(b, 22, 0);
	lv_obj_set_style_bg_color(b, filled ? M.primary : M.secondary_c, 0);
	l = m3_label(b, text, &lv_font_montserrat_16, filled ? M.on_primary : M.on_secondary_c);
	lv_obj_center(l);
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}
