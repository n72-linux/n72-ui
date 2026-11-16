/*
 * ui_m3.c - n72-ui's screens in a Material You (Material 3) style, sized
 * for the iPod touch 2G's 320x480 portrait panel. #included by main.c in
 * place of the old UI block, so it shares main.c's static helpers
 * (backlight_*, system_sleep, screen_off, W/H...).
 *
 * Settings, networks and Bluetooth go through the standard tools: wpa_cli
 * (wpa_supplicant's control client) and bluetoothctl (BlueZ), run in the
 * background so the UI never blocks on them.
 *
 * Settings persist in /tmp/n72-ui.conf for this boot (no persistent
 * storage yet).
 */

#include <math.h>
#include <spawn.h>
#include <strings.h>
#include <linux/rtnetlink.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include "lvgl/src/indev/lv_indev_private.h"	/* the throw velocity */
#include "lvgl/src/core/lv_obj_scroll_private.h"	/* lv_obj_scroll_by_raw() */
#include "lvgl/src/core/lv_obj_draw_private.h"	/* lv_obj_get_ext_draw_size() */
#include "lvgl/src/misc/lv_area_private.h"	/* lv_area_intersect() */
#include "lvgl/src/draw/lv_draw_private.h"	/* snap_area(): a layer of its own */
#include "lvgl/src/core/lv_refr_private.h"
#include "lvgl/src/misc/lv_anim_private.h"	/* the animation list (turning) */
#include "lvgl/src/core/lv_global.h"
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/statvfs.h>
#include <sys/inotify.h>

extern char **environ;

static bool system_sleep(void);
static void kbd_toggle_app(void);	/* the system keyboard (kbd.c) */
static void kbd_app_gone(void);
static void kbd_hide_now(bool anim);
static void kbd_turned(void);
static void kbd_init(void);

/* turning and the video controls (at the end) */
static int ui_rot, video_rot;
static char *app_cmd;		/* the running application's command */
static lv_obj_t *home_apps;
static lv_obj_t *home_dock;	/* (dock.c) */
#define QS_COLS (W > H ? 3 : 2)	/* quick-settings tiles a row */
static bool app_is_video(void);
static bool cmd_is_video(const char *cmd);
static bool app_is_game(void);
static bool cmd_is_game(const char *cmd);
static int rot_side, game_rot;	/* (defined with the turning code below) */
static void open_games(lv_event_t *e);
static void rot_kick(void);
static bool ui_turn(int rot);
static void vc_start(void);
static void vc_stop(void);

/* ------------------------------------------------------------ settings */

/* on the flash, so that a reboot keeps them (/tmp: the old place, read once) */
#define CONF_DIR "/var/lib/n72"
#define CONF_PATH CONF_DIR "/ui.conf"
#define CONF_OLD "/tmp/n72-ui.conf"

/* layout: every screen's side margin (status bar, pages, shade, home) */
#define STATUS_PAD	16
#define STATUS_GAP	10	/* between the status bar's icons */

static struct {
	bool dark;
	int accent;		/* index into accents[] */
	int idle_s;		/* screen idle timeout, 0 = never */
	int vol;		/* sound volume, 0..100 % (0 = mute) */
	bool spk;		/* speaker when no earphones are plugged */
	bool wake_plug;		/* the screen comes on when the cable goes in or out */
	int autorot;		/* Auto-rotate (quick settings): ROT_AUTO or ROT_LOCKED */
	int rotlock;		/* with it off: the turn it was left in, 0, 90 or 270 */
	int bright;		/* brightness %, 0 = as it is */
	int wifi, bt;		/* left on (1) or off (0); -1 not set */
	bool wp_accent;		/* the colours from the home wallpaper (wallpaper.c) */
	char dock[200];		/* the dock's icons, names joined by '|' (dock.c) */
	bool dock_set;		/* (no "dock=" line: the default) */
	char order[1200];	/* the grid's order, names joined by '|' (dock.c) */
	bool order_set;
	char tz[48];		/* the time zone's name (datetime.c; its rule is /etc/TZ) */
	bool tz_set;
	char wx_name[96];	/* the weather's place (weather.c) */
	double wx_lat, wx_lon;
	bool wx_set, wx_f;	/* a place set; Fahrenheit */
} conf = { true, 0, 120, 60, true, true, 0, 0, 0, -1, -1, false, "", false, "", false, "", false, "", 0, 0, false, false };

/*
 * Auto-rotate on: the screen turns with the iPod. Off: it stays as it was
 * held when turned off (rotlock), and the accelerometer powers down.
 */
#define ROT_LOCKED	0
#define ROT_AUTO	1

static const struct { const char *name; uint16_t hue; uint8_t sat; } accents[] = {
	{ "Violet", 265, 48 }, { "Blue", 215, 60 }, { "Teal", 178, 55 },
	{ "Green", 135, 40 }, { "Amber", 38, 70 }, { "Rose", 340, 55 },
};
#define N_ACCENTS ((int)(sizeof(accents) / sizeof(accents[0])))

/* Wallpapers (wallpaper.c) */
enum { WP_HOME, WP_LOCK, WP_KINDS };
static struct {
	lv_draw_buf_t *pic[WP_KINDS][2];	/* [kind][0 upright, 1 turned] */
	bool set[WP_KINDS];
	int luma[WP_KINDS];			/* mean luma under the app grid / clock */
	int hue, sat;				/* the picture's own accent; hue -1: none */
	int turn[WP_KINDS];			/* the turn pic[kind][1] was made for */
	bool busy;				/* a conversion is running */
	bool loaded;				/* what is set has been read (wp_load) */
} wp = { .hue = -1 };
static void wp_load(void);
static bool wp_put(lv_obj_t *scr, int kind);
static bool home_wp;		/* home is built over a wallpaper */
static lv_draw_buf_t *wp_pic(int kind);
static lv_draw_buf_t *wp_upright(int kind);
static bool wp_hold(lv_obj_t *img, lv_draw_buf_t *b);
static void wp_apply_kick(void);	/* (something in the way of new wallpaper frames ended) */
static void dock_carry_stop(void);	/* (dock.c: a carried icon put down) */
static bool pg_drag(int phase, int x, int y);
static void pg_fb0_dirty(void);
static void pg_abort(void);
/* text straight on a wallpaper: white (the picture is darkened under it) */
#define WP_TEXT lv_color_white()

static void conf_load(void)
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

static void conf_save(void)
{
	FILE *f;

	mkdir(CONF_DIR, 0755);
	f = fopen(CONF_PATH ".new", "w");
	if (!f)
		return;
	fprintf(f, "dark=%d\naccent=%d\nidle=%d\nvol=%d\nspk=%d\nplug=%d\nrot=%d\nrotlock=%d\n"
		"bright=%d\nwifi=%d\nbt=%d\nwpaccent=%d\n", conf.dark, conf.accent, conf.idle_s, conf.vol,
		conf.spk, conf.wake_plug, conf.autorot, conf.rotlock, conf.bright, conf.wifi, conf.bt,
		conf.wp_accent);
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

static void conf_save_later(void)
{
	if (conf_save_timer)
		lv_timer_reset(conf_save_timer);
	else {
		conf_save_timer = lv_timer_create(conf_save_cb, 2000, NULL);
		lv_timer_set_repeat_count(conf_save_timer, 1);
	}
}

/* --------------------------------------------------- Material You theme */

static struct {
	lv_color_t primary, on_primary, primary_c, on_primary_c;
	lv_color_t secondary_c, on_secondary_c, tertiary_c, on_tertiary_c;
	lv_color_t surface, surface_c, surface_c_high, surface_c_highest;
	lv_color_t on_surface, on_surface_v, outline, outline_v, error;
} M;

/* HSL tone (0-100 lightness) of the accent hue, Material 3 style. */
static lv_color_t tone(uint16_t h, uint8_t s, uint8_t l)
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

static void m3_apply_theme(void)
{
	lv_display_t *d = lv_display_get_default();
	lv_theme_t *th;

	m3_build_palette();
	th = lv_theme_default_init(d, M.primary, M.tertiary_c, conf.dark,
				   &lv_font_montserrat_16);
	lv_display_set_theme(d, th);
}

/* ------------------------------------------------------------ widgets */

static void m3_screen(lv_obj_t *scr)
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

static void obj_track(lv_obj_t **pp)
{
	if (*pp)
		lv_obj_add_event_cb(*pp, obj_untrack_cb, LV_EVENT_DELETE, pp);
}

static void plain(lv_obj_t *o)
{
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_shadow_width(o, 0, 0);
	lv_obj_set_style_outline_width(o, 0, 0);
}

static lv_obj_t *m3_card(lv_obj_t *parent, lv_color_t bg)
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

static lv_obj_t *m3_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
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
static lv_obj_t *row_slider(lv_obj_t *parent, bool last)
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
static lv_obj_t *slider_value(lv_obj_t *parent)
{
	const lv_font_t *f = &lv_font_montserrat_14;
	lv_obj_t *l = m3_label(parent, "", f, M.on_surface_v);
	int32_t w = LV_MAX(lv_text_get_width("100%", 4, f, 0), lv_text_get_width("Mute", 4, f, 0));

	lv_obj_set_width(l, w);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
	return l;
}

/* A round icon "badge" in a tonal container, as Material 3 list items use. */
static lv_obj_t *m3_badge(lv_obj_t *parent, const char *icon, lv_color_t bg,
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
	l = m3_label(b, icon, size >= 48 ? &lv_font_montserrat_24 : &lv_font_montserrat_20, fg);
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

static lv_obj_t *ios_switch(lv_obj_t *sw)
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
static lv_obj_t *m3_row_ex(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_color_t fg,
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
static void m3_row_current(lv_obj_t *r)
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

static lv_obj_t *m3_row(lv_obj_t *parent, const char *icon, const char *title,
			const char *sub, lv_event_cb_t cb, void *ud, lv_obj_t **sub_lbl)
{
	return m3_row_ex(parent, icon, M.secondary_c, M.on_secondary_c, title, sub, cb, ud,
			 sub_lbl, cb != NULL);
}

static void m3_section(lv_obj_t *parent, const char *title)
{
	lv_obj_t *l = m3_label(parent, title, &lv_font_montserrat_14, M.primary);

	lv_obj_set_style_pad_left(l, 8, 0);
	lv_obj_set_style_pad_top(l, 8, 0);
}

/* Wrapped supporting text. */
static void m3_note(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = m3_label(parent, text, &lv_font_montserrat_14, M.on_surface_v);

	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_pad_hor(l, 8, 0);
}

static lv_obj_t *m3_button(lv_obj_t *parent, const char *text, bool filled,
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

/* ------------------------------------------------------------ navigation */

static bool in_demo;		/* (no demos any more; kept for main.c) */

static void back_cb(lv_event_t *e);

/*
 * Transitions. LVGL's own screen animations redraw both screens, text and
 * all, every frame (15-30 ms on this CPU: choppy). Instead each picture is
 * rendered once with lv_snapshot_take() into GPU memory, and the frames
 * only move those pictures, which the GPU copies (a few ms per frame).
 */
#define TR_MS 350	/* iOS's navigation push/pop: 350 ms ease-in-out */

static struct {
	lv_obj_t *scr, *bg, *fg;	/* transition screen and its two pictures */
	lv_draw_buf_t *sbg, *sfg;
	lv_obj_t *keep_for;		/* forward: keep the old page's picture for Back */
	lv_obj_t *to, *from;		/* screen to load at the end, and to delete */
	int kind;			/* 0 slide left, 1 slide right, 2 shade */
	void (*done)(void);
} tr;

static void tr_exec(void *var, int32_t v)
{
	LV_UNUSED(var);
	if (tr.kind == 2) {
		lv_obj_set_y(tr.fg, v);
		return;
	}
	lv_obj_set_x(tr.bg, v);
	lv_obj_set_x(tr.fg, v + (tr.kind == 0 ? W : -W));
}

static void tr_free(lv_draw_buf_t **b)
{
	if (*b) {
		lv_image_cache_drop(*b);
		lv_draw_buf_destroy(*b);
		*b = NULL;
	}
}

/*
 * Back slides: the picture of a page taken when it was left (forward) is
 * the picture to slide back to, so going back renders nothing. Two pages
 * at most (each is a full screen of GPU memory). A picture is dropped when
 * the page goes, when one of its labels changes (label_set_if_changed:
 * status texts), or when the minute changes (the clock).
 */
#define BP_MAX 2
static struct {
	lv_obj_t *scr;
	lv_draw_buf_t *buf;
	long minute;
} bp[BP_MAX];

static long bp_minute(void)
{
	return (long)(time(NULL) / 60);
}

static void bp_drop(int i)
{
	tr_free(&bp[i].buf);
	bp[i].scr = NULL;
}

static void bp_scr_deleted(lv_event_t *e)
{
	lv_obj_t *scr = lv_event_get_current_target(e);

	for (int i = 0; i < BP_MAX; i++)
		if (bp[i].scr == scr)
			bp_drop(i);
}

/* @scr changed (a label of it): its picture no longer matches. */
static unsigned long home_ver;	/* bumped when anything on home changes */

static void bp_changed(lv_obj_t *scr)
{
	if (scr && scr == home_scr)
		home_ver++;
	for (int i = 0; i < BP_MAX; i++)
		if (bp[i].scr && bp[i].scr == scr)
			bp_drop(i);
}

static void bp_put(lv_obj_t *scr, lv_draw_buf_t *buf)
{
	int i, slot = 0;

	bp_changed(scr);
	for (i = 0; i < BP_MAX; i++)
		if (!bp[i].scr) {
			slot = i;
			break;
		}
	if (i == BP_MAX) {		/* full: drop the older one */
		slot = 0;
		bp_drop(0);
	}
	bp[slot].scr = scr;
	bp[slot].buf = buf;
	bp[slot].minute = bp_minute();
	lv_obj_remove_event_cb(scr, bp_scr_deleted);
	lv_obj_add_event_cb(scr, bp_scr_deleted, LV_EVENT_DELETE, NULL);
}

/* @scr's picture, if still current (the caller owns it then). */
static lv_draw_buf_t *bp_take(lv_obj_t *scr)
{
	lv_draw_buf_t *b;

	for (int i = 0; i < BP_MAX; i++) {
		if (bp[i].scr != scr)
			continue;
		if (bp[i].minute != bp_minute()) {
			bp_drop(i);
			return NULL;
		}
		b = bp[i].buf;
		bp[i].buf = NULL;
		bp[i].scr = NULL;
		return b;
	}
	return NULL;
}

static void tr_finish(void)
{
	if (tr.scr && getenv("N72_FPS")) {
		extern uint64_t n72_mark_us;

		fprintf(stderr, "slide end at %.0f ms after the mark\n", (mono_us() - n72_mark_us) / 1000.0);
	}
	lv_obj_t *scr = tr.scr, *from = tr.from, *keep = tr.keep_for;
	void (*done)(void) = tr.done;

	if (!scr)
		return;
	tr.scr = NULL;
	tr.from = NULL;
	tr.done = NULL;
	tr.keep_for = NULL;
	wp_apply_kick();
	lv_anim_delete(&tr, tr_exec);
	lv_screen_load(tr.to);
	lv_obj_delete(scr);
	if (keep && tr.sbg && lv_obj_is_valid(keep)) {
		lv_image_cache_drop(tr.sbg);	/* (its image object is gone) */
		bp_put(keep, tr.sbg);
		tr.sbg = NULL;
	}
	tr_free(&tr.sbg);
	tr_free(&tr.sfg);
	if (from)
		lv_obj_delete(from);
	if (done)
		done();
}

static void tr_done_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	tr_finish();
}

/*
 * Looks changed for a picture and changed back (hidden, a colour): no area
 * marked, nothing drawn again on screen for it. (Nested: a counter.)
 */
static void quiet_begin(void)	/* (the layout brought up to date first, outside) */
{
	lv_display_enable_invalidation(NULL, false);
}

static void quiet_end(void)
{
	lv_display_enable_invalidation(NULL, true);
}

static lv_draw_buf_t *snap(lv_obj_t *obj, lv_color_format_t cf)
{
	lv_draw_buf_t *b;

	lv_obj_update_layout(obj);
	b = lv_snapshot_take(obj, cf);
	if (b)
		lv_draw_buf_flush_cache(b, NULL);	/* the GPU reads it */
	return b;
}

/*
 * Picture slides: an object that slides in or out (the keyboard, the bar
 * over applications) moves as a picture of itself taken once at the start,
 * not as itself: a moved object is drawn again every frame (over an
 * application on the CPU, in the transparent mode, then premultiplied),
 * its picture is one exact GPU copy (mbx_image_premul). The object itself
 * is back, at its place, once the slide ends. iOS's longer timings (300 to
 * 350 ms) then cost nothing more per frame.
 */
#define PSL_MAX 2
#define PSL_SIG 160

/*
 * @sig (optional): a short text that changes whenever the object's look
 * does. With it the picture is kept between slides (taken again only when
 * the text changed, ahead of time by psl_warm), and after a slide in the
 * picture stays on display in the object's place: the object itself comes
 * back (one redraw) only when psl_wake() is called, for a touch or a
 * change. A show and hide then draw the object not at all.
 */
typedef void (*psl_sig_fn)(char *buf, size_t len);

struct psl {
	lv_obj_t *live, *img;
	lv_draw_buf_t *pic;
	int32_t ext;		/* the picture's margin (shadows) */
	int32_t to;
	bool hide;		/* hidden at the end (a slide out) */
	bool resting;		/* the picture shows in the object's place */
	psl_sig_fn sig;
	char pic_sig[PSL_SIG];	/* what the picture shows */
};

static struct psl psl[PSL_MAX];

static void psl_y(void *var, int32_t v)
{
	struct psl *s = var;

	lv_obj_set_y(s->img, v - s->ext);
}

static void psl_img_free(struct psl *s)
{
	if (s->img) {
		lv_obj_delete(s->img);
		s->img = NULL;
	}
}

static void psl_finish(struct psl *s)
{
	if (!s->live)
		return;
	lv_anim_delete(s, psl_y);
	lv_obj_set_y(s->live, s->to);
	if (!s->hide && s->sig) {	/* the picture stays in its place */
		s->resting = true;
		return;
	}
	if (!s->hide)
		lv_obj_remove_flag(s->live, LV_OBJ_FLAG_HIDDEN);
	psl_img_free(s);
	s->resting = false;
	if (!s->sig) {
		tr_free(&s->pic);
		s->live = NULL;
	}
}

static void psl_done(lv_anim_t *a)
{
	psl_finish(a->var);
}

static struct psl *psl_of(lv_obj_t *live)
{
	int i;

	for (i = 0; i < PSL_MAX; i++)
		if (psl[i].live == live)
			return &psl[i];
	return NULL;
}

/* Is @live sliding? Then (x, y) on its place at the end is still its own. */
static bool psl_covers(lv_obj_t *live, int x, int y)
{
	struct psl *s = psl_of(live);
	lv_area_t a;

	if (!s || !s->img || s->hide)
		return false;
	lv_obj_get_coords(live, &a);	/* (already at the end, hidden) */
	return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

/* A new picture of @live into @s (true: there is one). */
static bool psl_take(struct psl *s, lv_obj_t *live)
{
	bool hidden = lv_obj_has_flag(live, LV_OBJ_FLAG_HIDDEN);
	char sig[PSL_SIG] = "";

	if (s->sig)
		s->sig(sig, sizeof(sig));
	if (s->pic && s->sig && !strcmp(sig, s->pic_sig))
		return true;		/* (still what it shows) */
	tr_free(&s->pic);
	lv_obj_update_layout(live);	/* (what moved for real is marked: outside the quiet) */
	quiet_begin();		/* (over an application, a redraw of its place) */
	lv_obj_remove_flag(live, LV_OBJ_FLAG_HIDDEN);	/* (a hidden object draws nothing) */
	s->pic = snap(live, LV_COLOR_FORMAT_ARGB8888);
	if (hidden)
		lv_obj_add_flag(live, LV_OBJ_FLAG_HIDDEN);
	quiet_end();
	if (!s->pic)
		return false;
	mbx_gpu_premultiply_buf(s->pic);	/* exact copy on the transparent display */
	s->ext = lv_obj_get_ext_draw_size(live);
	snprintf(s->pic_sig, sizeof(s->pic_sig), "%s", sig);
	return true;
}

static struct psl *psl_slot(lv_obj_t *live, psl_sig_fn sig)
{
	struct psl *s = psl_of(live);
	int i;

	if (s)
		return s;
	for (i = 0; i < PSL_MAX && psl[i].live; i++)
		;
	if (i == PSL_MAX)
		return NULL;
	s = &psl[i];
	memset(s, 0, sizeof(*s));
	s->live = live;
	s->sig = sig;
	return s;
}

/*
 * Slide @live (a child of a layer or screen, positioned by its y) to @to over
 * @ms on @path; @hide: hidden once there. False if there was no memory for
 * the picture: the caller moves the object itself.
 */
static bool psl_start_sig(lv_obj_t *live, int32_t to, uint32_t ms, lv_anim_path_cb_t path,
			  bool hide, psl_sig_fn sig)
{
	struct psl *s = psl_slot(live, sig);
	int32_t from;
	lv_anim_t a;

	if (!s)
		return false;
	if (s->img) {
		/* sliding, or resting as its picture: from where that is now */
		from = lv_obj_get_style_y(s->img, LV_PART_MAIN) + s->ext;
		lv_anim_delete(s, psl_y);
		if (s->resting) {	/* (the picture may be old: the object again) */
			char now[PSL_SIG] = "";

			s->sig(now, sizeof(now));
			if (strcmp(now, s->pic_sig))
				psl_img_free(s);
		}
	} else {
		from = lv_obj_get_style_y(live, LV_PART_MAIN);
	}
	s->resting = false;
	if (!s->img) {
		lv_obj_set_y(live, from);
		if (!psl_take(s, live)) {
			if (!s->sig)
				s->live = NULL;
			return false;
		}
		s->img = lv_image_create(lv_obj_get_parent(live));
		lv_image_set_src(s->img, s->pic);
		lv_obj_remove_flag(s->img, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_move_to_index(s->img, lv_obj_get_index(live) + 1);
		lv_obj_set_pos(s->img, lv_obj_get_x(live) - s->ext, from - s->ext);
	}
	s->to = to;
	s->hide = hide;
	lv_obj_add_flag(live, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_y(live, to);		/* (touches go by its final place) */
	lv_anim_init(&a);
	lv_anim_set_var(&a, s);
	lv_anim_set_exec_cb(&a, psl_y);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, path);
	lv_anim_set_completed_cb(&a, psl_done);
	lv_anim_start(&a);
	return true;
}

static bool psl_start(lv_obj_t *live, int32_t to, uint32_t ms, lv_anim_path_cb_t path, bool hide)
{
	return psl_start_sig(live, to, ms, path, hide, NULL);
}

/* @live itself again in place of its resting picture (a touch, a change). */
static void psl_wake(lv_obj_t *live)
{
	struct psl *s = psl_of(live);

	if (!s || !s->resting)
		return;
	s->resting = false;
	psl_img_free(s);
	lv_obj_remove_flag(live, LV_OBJ_FLAG_HIDDEN);
}

/*
 * A touch on @live's place while it slides in: the slide ends there and
 * then, and the object itself takes the touch (hidden, it would get
 * nothing: the touch was lost). A resting picture: the object again.
 */
static void psl_touch(lv_obj_t *live)
{
	struct psl *s = psl_of(live);

	if (!s || !s->img || (s->hide && !s->resting))
		return;
	if (!s->resting)
		psl_finish(s);
	psl_wake(live);
}

/* Resting, and its look changed since the picture: the object again. */
static void psl_check(lv_obj_t *live)
{
	struct psl *s = psl_of(live);
	char now[PSL_SIG] = "";

	if (!s || !s->resting)
		return;
	s->sig(now, sizeof(now));
	if (strcmp(now, s->pic_sig))
		psl_wake(live);
}

/* Hidden and still: its picture ready for the next slide (with a @sig). */
static void psl_warm(lv_obj_t *live, psl_sig_fn sig)
{
	struct psl *s;

	if (!lv_obj_has_flag(live, LV_OBJ_FLAG_HIDDEN) || !(s = psl_slot(live, sig)) || s->img)
		return;
	psl_take(s, live);
}

/* Stop @live's slide where it ends, and forget it (a rebuild, a jump). */
static void psl_stop(lv_obj_t *live)
{
	struct psl *s = psl_of(live);

	if (!s)
		return;
	if (s->img && !s->resting)
		psl_finish(s);
	if (s->resting) {
		psl_img_free(s);
		lv_obj_remove_flag(live, LV_OBJ_FLAG_HIDDEN);
	}
	tr_free(&s->pic);
	memset(s, 0, sizeof(*s));
}

/* The screen on display: copied from the display buffer when it is current. */
static uint32_t gs_refr_ms, gs_grab_ms;	/* (statistics) */
static uint32_t ui_release_ms;		/* (statistics) the last finger lift */

static void ui_release_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_release_ms = lv_tick_get();
}
static bool gs_grabbed;

static lv_draw_buf_t *grab_or_snap(lv_obj_t *scr)
{
	lv_draw_buf_t *b = NULL;
	uint32_t t0 = lv_tick_get(), t1 = t0;

	if (scr == lv_screen_active()) {
		lv_refr_now(NULL);	/* pending changes (a pressed row) first */
		t1 = lv_tick_get();
		b = mbx_gpu_grab_screen();
	}
	gs_refr_ms = t1 - t0;
	gs_grab_ms = lv_tick_get() - t1;
	gs_grabbed = b != NULL;
	return b ? b : snap(scr, LV_COLOR_FORMAT_XRGB8888);
}

static lv_obj_t *tr_image(lv_draw_buf_t *b)
{
	lv_obj_t *i = lv_image_create(tr.scr);

	lv_image_set_src(i, b);
	lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
	return i;
}

/* Start a transition with pictures @bg and @fg; false: no memory for them. */
static bool tr_start(lv_draw_buf_t *bg, lv_draw_buf_t *fg, int kind, lv_obj_t *to,
		     lv_obj_t *del, int32_t from_v, int32_t to_v)
{
	lv_anim_t a;

	if (!bg || !fg) {
		tr_free(&bg);
		tr_free(&fg);
		return false;
	}
	tr.sbg = bg;
	tr.sfg = fg;
	tr.kind = kind;
	tr.to = to;
	tr.from = del;
	tr.scr = lv_obj_create(NULL);
	m3_screen(tr.scr);
	tr.bg = tr_image(bg);
	tr.fg = tr_image(fg);
	tr_exec(NULL, from_v);
	lv_screen_load(tr.scr);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &tr);
	lv_anim_set_exec_cb(&a, tr_exec);
	lv_anim_set_values(&a, from_v, to_v);
	lv_anim_set_duration(&a, TR_MS);
	lv_anim_set_path_cb(&a, ios_path_ease_in_out);
	lv_anim_set_completed_cb(&a, tr_done_cb);
	lv_anim_start(&a);
	return true;
}

/* Slide to @to: left for forward, right for back; @del_from: delete the old screen. */
static void app_bar_set(bool show);
static pid_t app_pid;

static void slide(lv_obj_t *to, bool left, bool del_from)
{
	lv_obj_t *from;

	lv_draw_buf_t *bg, *fg;
	uint32_t t0, t1, t2;
	bool reused;

	tr_finish();
	from = lv_screen_active();
	if (from == to)
		return;
	if (to == home_scr && app_pid <= 0)
		app_bar_set(false);	/* home has its own status bar */
	{
		extern uint64_t n72_mark_us;

		n72_mark_us = mono_us();
	}
	t0 = lv_tick_get();
	bg = grab_or_snap(from);
	t1 = lv_tick_get();
	fg = bp_take(to);
	reused = fg != NULL;
	if (!fg)
		fg = snap(to, LV_COLOR_FORMAT_XRGB8888);
	t2 = lv_tick_get();
	if (getenv("N72_FPS"))
		fprintf(stderr, "slide %s: %u ms after the lift; old page %u ms (refresh %u, %s %u), new page %u ms%s\n",
			left ? "forward" : "back", (unsigned)(t0 - ui_release_ms), (unsigned)(t1 - t0),
			(unsigned)gs_refr_ms,
			gs_grabbed ? "grab" : "grab failed, snapshot", (unsigned)gs_grab_ms,
			(unsigned)(t2 - t1), reused ? " (kept picture)" : "");
	if (!tr_start(bg, fg, left ? 0 : 1, to, del_from ? from : NULL, 0, left ? -W : W)) {
		lv_screen_load_anim(to, left ? LV_SCR_LOAD_ANIM_MOVE_LEFT :
				    LV_SCR_LOAD_ANIM_MOVE_RIGHT, TR_MS, 0, del_from);
		return;
	}
	if (!del_from)
		tr.keep_for = from;	/* the page stays: Back can slide to this picture */
}

static void show(lv_obj_t *scr)
{
	slide(scr, true, false);
}

/* Back goes to the previous page; pages are a small stack. */
#define NAV_MAX 16	/* (Files: a page a folder) */
static lv_obj_t *nav[NAV_MAX];
static int nav_n;
/* where an application was launched from (home, or a page such as Files):
 * the picture behind its card, and the screen it ends on */
static lv_obj_t *launch_from;
static void app_bar_set(bool show);
static bool locked;

/* A page (Settings and the pages under it, Paint) is on the screen. */
static bool on_page(void)
{
	return !locked && nav_n > 0 && lv_screen_active() == nav[nav_n - 1];
}

static void shade_set(bool open, bool anim);

static bool app_end(void);
static lv_obj_t *app_scr;	/* the empty screen over a running application */
static bool locked;		/* the lock screen is up */
static void lock_wake(void);
static void za_finish(void);
static pid_t app_pid;
static void app_shade_slide(bool open);
static void app_bar_update(void);
static lv_obj_t *vol_hud;

static void ui_settle_all(void);
static bool za_page_close(void);
static bool page_back_to_app(void);
static bool page_home_over_app(void);

static void home_fresh(void);
static void page_fresh(int i);

static void go_home(void)
{
	if (locked)
		return;		/* Home does not unlock */
	home_fresh();		/* (a turn on a page left home for later) */
	/*
	 * A shade drag, its settling or an app animation has a screen of its
	 * own on display, which it deletes itself: end those first, or the
	 * slide below would delete that screen as well.
	 */
	ui_settle_all();
	if (page_home_over_app())
		return;
	launch_from = NULL;		/* Home: the application ends on home, not its page */
	if (app_end())
		return;
	if (za_page_close())
		return;
	tr_finish();
	shade_set(false, false);
	while (nav_n > 0) {
		lv_obj_t *s = nav[--nav_n];

		if (s != lv_screen_active())
			lv_obj_delete_async(s);
	}
	if (lv_screen_active() != home_scr)
		slide(home_scr, false, true);
}

static void go_back(void)
{
	lv_obj_t *prev;

	if (nav_n <= 1) {
		if (page_back_to_app())
			return;	/* Settings over an application: back to it */
		go_home();	/* (with the card, for a page opened from a tile) */
		return;
	}
	page_fresh(nav_n - 2);	/* (a turn left it for later) */
	nav_n--;
	prev = nav[nav_n - 1];
	slide(prev, false, true);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	go_back();
}

/*
 * Scrolling from snapshots: when a page body starts to scroll, each of its
 * children (cards, rows) is rendered once into GPU memory and shown as a
 * picture in its place; the live child is skipped while drawing (layered
 * opacity 0). Scroll frames are then a few GPU copies instead of CPU text
 * and shapes. When scrolling ends the live children come back.
 */
#define SC_MAX		64
#define SC_BUDGET	(3 * 1024 * 1024)	/* bytes of snapshots at most */
#define SC_KEEP_MS	1500			/* pictures stay this long after the page stops */

static struct {
	lv_obj_t *owner;
	lv_obj_t *live[SC_MAX], *pic[SC_MAX];
	lv_draw_buf_t *buf[SC_MAX];
	long size[SC_MAX];
	long bytes;
	int n;
	lv_timer_t *timer;
	uint32_t last_move;
} sc;

static void pf_give(lv_obj_t *owner, lv_obj_t *c, lv_draw_buf_t *b);

static void sc_restore(bool delete_pics)
{
	int i;

	for (i = 0; i < sc.n; i++) {
		if (delete_pics && lv_obj_is_valid(sc.pic[i]))
			lv_obj_delete(sc.pic[i]);
		if (lv_obj_is_valid(sc.live[i]))
			lv_obj_set_style_opa_layered(sc.live[i], LV_OPA_COVER, 0);
		pf_give(delete_pics ? sc.owner : NULL, sc.live[i], sc.buf[i]);
	}
	sc.n = 0;
	sc.bytes = 0;
	sc.owner = NULL;
	wp_apply_kick();
}

static void sc_settle(lv_timer_t *t)
{
	lv_obj_t *body = lv_timer_get_user_data(t);

	/*
	 * Keep the pictures for a while after the page stops: another flick
	 * soon after reuses them instead of taking them all again at its
	 * start (a 60-140 ms hitch on Logs). Taps still reach the live rows,
	 * which stay in place under the pictures.
	 */
	if (sc.owner == body && lv_tick_elaps(sc.last_move) < SC_KEEP_MS)
		return;			/* still moving, or recently: look again */
	lv_timer_delete(t);
	sc.timer = NULL;
	if (getenv("N72_SC_KEEP"))
		return;			/* test: pictures stay (compare with N72_NO_SCROLL_CACHE) */
	if (sc.owner == body)
		sc_restore(true);
}

/*
 * Prefetch: pictures of the rows a scrollable page shows, taken one at a
 * time while it sits idle, so that the first frame of a scroll does not
 * render them (a 35-57 ms hitch on Settings). Kept out of sight; a row that
 * is redrawn meanwhile loses its picture (see pf_invalidated).
 */
#define PREF_MAX		40
#define PREF_BUDGET	(2 * 1024 * 1024)	/* bytes */

static struct {
	lv_obj_t *owner;
	lv_obj_t *child[PREF_MAX];
	lv_draw_buf_t *buf[PREF_MAX];
	long bytes;
	int n;
} pf;

static void pf_child_deleted(lv_event_t *e);

static long pf_size(const lv_draw_buf_t *b)
{
	return (long)b->header.stride * b->header.h;
}

/*
 * Flick and glide, as iOS 4.2.1's UIScrollView (n72-ramboot/ui/IOS-PAGING.md
 * part 2), in closed form of the time since the lift (iOS steps it per
 * frame; the same positions within 0.16 px, no loop):
 *
 * - glide: v(t) = v0 * F^t, F = 0.998 per ms (time constant 499.5 ms);
 *   distance v0 * F (1 - F^t) / (1 - F); stops below 0.01 px/ms. A lift
 *   slower than 0.25 px/ms does not glide at all.
 * - an end reached while gliding: the content goes on past it, the velocity
 *   is not turned round, and the overshoot is pulled back by 0.99 per ms:
 *   edge + 0.99^u * vh * S(u) (u since the hit, vh the speed then).
 * - let go past an end (pulled out): the same pull back, with the speed of
 *   the lift, instead of LVGL's spring.
 * - fast scrolling: after three flicks of at least 0.6 px/ms in a row, each
 *   next touch-down within 1 s of the last lift, a drag's glide goes
 *   1 + min(drag/240, 0.9) * (1 + 0.5 * (flicks - 3)) times as far (cap 16);
 *   a slow drag (< 0.13 px/ms), a turn, an end reached or a glide that runs
 *   out starts again at 1.
 *
 * The release velocity is still a least-squares slope over the last 80 ms
 * of touch samples (the touch controller's jitter; iOS smooths 0.75/0.25).
 * Every step scrolls with lv_obj_scroll_by_raw(), so LV_EVENT_SCROLL fires
 * on each one (the Files page rebinds its rows from it).
 */
#define GLIDE_F		0.998
#define GLIDE_LN_F	-0.0020020026706730793	/* ln 0.998 */
#define BOUNCE_LN_A	-0.01005033585350145	/* ln 0.99 */
#define GLIDE_MIN_V	250.0f	/* px/s: a slower lift does not glide */
#define GLIDE_STOP_V	0.01	/* px/ms */
#define FLICK_WINDOW_US	80000	/* release velocity from this much of the drag */
#define FAST_V		600.0f	/* px/s: a flick that counts towards fast scrolling */
#define FAST_SLOW_V	130.0f	/* px/s: a drag slower than this starts again */
#define FAST_GAP_MS	1000
#define FAST_MAX	16.0

#define TS_N	16
static struct {
	uint64_t t[TS_N];
	int32_t x[TS_N], y[TS_N];
	int n, head;
} ts;			/* recent touch samples while pressed */

static struct {
	lv_obj_t *obj;
	bool hor;
	int sign;	/* +1: content follows a finger moving down/right */
	double v0;	/* px/ms at the start, >= 0 along sign */
	double mult;	/* fast scrolling: distance x this */
	double e0;	/* px already past the end at the start (pulled out) */
	double th;	/* ms when the end is reached; < 0: never */
	double vh;	/* px/ms then */
	int32_t room;	/* px to the end at the start */
	int32_t done;	/* px moved so far (along sign) */
	bool lv_anim_killed;
	bool on;
	lv_obj_t *caught;	/* stopped past an end by a press: back to it on the lift */
	bool caught_hor;
} gl;

static struct {
	int count;		/* fast flicks in a row */
	double mult, start_mult;
	uint32_t end_ms;	/* the last fast flick's lift */
	int sign;
	int32_t drag_start;	/* this drag's first sample */
	bool slow;		/* this drag went slow or turned */
} fs = { .mult = 1, .start_mult = 1 };

static void fs_reset(void)
{
	fs.count = 0;
	fs.mult = fs.start_mult = 1;
}

static void glide_exec(void *var, int32_t ms);
static void sb_opa(void *obj, int32_t v);

static void glide_end(const char *why)
{
	lv_obj_t *obj = gl.obj;

	if (!gl.on)
		return;
	if (getenv("N72_FPS"))
		fprintf(stderr, "glide: %d px, %s\n", (int)LV_ABS(gl.done), why);
	gl.on = false;
	gl.obj = NULL;
	if (obj && lv_obj_is_valid(obj))
		lv_obj_send_event(obj, LV_EVENT_SCROLL_END, NULL);
}

static void glide_stop(void)
{
	glide_end("caught");
	lv_anim_delete(&gl, glide_exec);
}

static void glide_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	fs_reset();		/* (iOS: _stopScrollingNotify) */
	glide_end("slowed down");
}

/* sum of F^i, i = 1..t: the distance of a unit speed after t ms */
static inline double glide_s(double t)
{
	return GLIDE_F * (1.0 - exp(GLIDE_LN_F * t)) / (1.0 - GLIDE_F);
}

/*
 * px moved along gl.sign @t ms after the start. Gliding: past room means past
 * the end. Pulled out (e0 > 0, gl.sign outwards): the overshoot is
 * 0.99^t (e0 + v0 S(t)), so the content ends at -e0.
 */
static double glide_pos(double t)
{
	double u;

	if (gl.e0 > 0)
		return exp(BOUNCE_LN_A * t) * (gl.e0 + gl.v0 * glide_s(t)) - gl.e0;
	if (gl.th < 0 || t <= gl.th)
		return gl.v0 * glide_s(t) * gl.mult;
	u = t - gl.th;
	return gl.room + exp(BOUNCE_LN_A * u) * gl.vh * glide_s(u);
}

/* LVGL's own scroll animation on @obj (its spring back), not ours */
static void glide_kill_lv_scroll(lv_obj_t *obj)
{
	lv_anim_t *a, *next;

	for (a = lv_ll_get_head(&LV_GLOBAL_DEFAULT()->anim_state.anim_ll); a; a = next) {
		next = lv_ll_get_next(&LV_GLOBAL_DEFAULT()->anim_state.anim_ll, a);
		if (a->var == obj && a->exec_cb != (lv_anim_exec_xcb_t)sb_opa) {
			lv_anim_delete(obj, a->exec_cb);
			next = lv_ll_get_head(&LV_GLOBAL_DEFAULT()->anim_state.anim_ll);	/* (the list changed) */
		}
	}
}

/*
 * One step per frame: an LVGL animation (stepped at the start of every
 * refresh) whose value is the time since the lift.
 */
static void glide_exec(void *var, int32_t ms)
{
	int32_t target, step;

	LV_UNUSED(var);
	if (!gl.on || !gl.obj || !lv_obj_is_valid(gl.obj)) {
		gl.on = false;
		gl.obj = NULL;
		return;
	}
	if (!gl.lv_anim_killed) {	/* (LVGL starts its spring back after the throw) */
		glide_kill_lv_scroll(gl.obj);
		gl.lv_anim_killed = true;
	}
	if (gl.th >= 0 && ms >= gl.th && fs.mult != 1)
		fs_reset();	/* an end reached: fast scrolling starts again */
	target = (int32_t)lround(glide_pos(ms));
	step = target - gl.done;
	if (!step)
		return;
	gl.done += step;
	/* raw: lv_obj_scroll_by() wraps each step in SCROLL_BEGIN/END */
	if (gl.hor)
		lv_obj_scroll_by_raw(gl.obj, step * gl.sign, 0);
	else
		lv_obj_scroll_by_raw(gl.obj, 0, step * gl.sign);
}

/* the last frame: exactly at the end if it went past one */
static void glide_last(lv_anim_t *a)
{
	if (gl.on && gl.obj && lv_obj_is_valid(gl.obj) && gl.done != gl.room &&
	    (gl.e0 > 0 || gl.th >= 0)) {
		int32_t step = gl.room - gl.done;

		gl.done = gl.room;
		if (gl.hor)
			lv_obj_scroll_by_raw(gl.obj, step * gl.sign, 0);
		else
			lv_obj_scroll_by_raw(gl.obj, 0, step * gl.sign);
	}
	glide_done(a);
}

static void glide_run(lv_obj_t *obj, double dur)
{
	lv_anim_t a;

	if (dur < 1) {
		gl.obj = NULL;
		return;
	}
	gl.on = true;
	n72_trace("glide>", 0);
	lv_obj_send_event(obj, LV_EVENT_SCROLL_BEGIN, NULL);
	n72_trace("glide<", 0);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &gl);
	lv_anim_set_exec_cb(&a, glide_exec);
	lv_anim_set_values(&a, 0, (int32_t)dur);
	lv_anim_set_duration(&a, (uint32_t)dur);
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_set_completed_cb(&a, glide_last);
	lv_anim_start(&a);
}

static void glide_init_state(lv_obj_t *obj, bool hor, int sign)
{
	glide_stop();
	gl.obj = obj;
	gl.hor = hor;
	gl.sign = sign;
	gl.done = 0;
	gl.lv_anim_killed = false;
	gl.th = -1;
	gl.e0 = 0;
	gl.mult = 1;
}

/* A glide: @v px/s signed as the finger moved, @room px to the end that way. */
static void glide_start(lv_obj_t *obj, bool hor, float v, int32_t room)
{
	double v0 = fabs(v) / 1000.0, dur, arg;

	glide_init_state(obj, hor, v > 0 ? 1 : -1);
	gl.v0 = v0;
	gl.mult = fs.mult;
	gl.room = room;
	/* the end reached when v0 * mult * S(th) = room */
	arg = 1.0 - room * (1.0 - GLIDE_F) / (v0 * gl.mult * GLIDE_F);
	if (arg > 0)
		gl.th = log(arg) / GLIDE_LN_F;
	dur = log(GLIDE_STOP_V / v0) / GLIDE_LN_F;
	if (gl.th >= 0) {
		gl.vh = v0 * exp(GLIDE_LN_F * gl.th);	/* (the boost scales distance only) */
		for (dur = gl.th + 16; dur < gl.th + 3000; dur += 16)
			if (fabs(glide_pos(dur) - gl.room) < 0.5 &&
			    gl.vh * exp((GLIDE_LN_F + BOUNCE_LN_A) * (dur - gl.th)) < GLIDE_STOP_V)
				break;
	}
	glide_run(obj, dur);
}

/*
 * Let go @e0 px past an end; @out +1 if that end is the top/left (content
 * moved down/right); @v px/s as the finger moved (kept, either way).
 */
static void glide_back(lv_obj_t *obj, bool hor, int out, int32_t e0, float v)
{
	double dur;

	glide_init_state(obj, hor, out);
	gl.e0 = e0;
	gl.v0 = v * out / 1000.0;	/* outwards +, inwards - */
	gl.room = -e0;			/* the end */
	for (dur = 16; dur < 3000; dur += 16)
		if (fabs(glide_pos(dur) - gl.room) < 0.5)
			break;
	glide_run(obj, dur);
}

/* a press anywhere stops a glide; fast scrolling only goes on from a
 * glide caught within FAST_GAP_MS of its lift */
static int32_t glide_past(lv_obj_t *obj, bool hor, int *out);

static void ts_press_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!gl.on || lv_tick_elaps(fs.end_ms) > FAST_GAP_MS)
		fs_reset();
	fs.start_mult = fs.mult;
	fs.slow = false;
	gl.caught = NULL;
	if (gl.on && gl.obj && lv_obj_is_valid(gl.obj) && glide_past(gl.obj, gl.hor, NULL) > 0) {
		/* held past an end: a drag goes on from there, a lift goes back (iOS) */
		gl.caught = gl.obj;
		gl.caught_hor = gl.hor;
	}
	glide_stop();
}

/* the lift after a press that caught a list past its end, without a drag */
static void ts_release_cb(lv_event_t *e)
{
	lv_obj_t *obj = gl.caught;
	int32_t e0;
	int out;

	LV_UNUSED(e);
	gl.caught = NULL;
	if (!obj || gl.on || !lv_obj_is_valid(obj) || (e0 = glide_past(obj, gl.caught_hor, &out)) <= 0)
		return;
	glide_back(obj, gl.caught_hor, out, e0, 0);
}

/*
 * The touch path, sampled at each scroll step the finger makes (LVGL sends
 * no PRESSING to input devices); a glide's own steps have no active input.
 */
static void ts_cb(lv_event_t *e)
{
	lv_indev_t *in = lv_indev_active();
	lv_point_t p;
	int prev;

	if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN)
		ts.n = 0;
	if (!in || lv_indev_get_state(in) != LV_INDEV_STATE_PRESSED)
		return;
	lv_indev_get_point(in, &p);
	prev = ts.head;
	ts.head = (ts.head + 1) % TS_N;
	ts.t[ts.head] = mono_us();
	ts.x[ts.head] = p.x;
	ts.y[ts.head] = p.y;
	if (ts.n < TS_N)
		ts.n++;
	if (ts.n == 1) {
		fs.drag_start = in->pointer.scroll_dir == LV_DIR_HOR ? p.x : p.y;
	} else {
		/* a slow stretch or a turn ends fast scrolling (iOS: per pan step) */
		bool hor = in->pointer.scroll_dir == LV_DIR_HOR;
		int32_t d = hor ? ts.x[ts.head] - ts.x[prev] : ts.y[ts.head] - ts.y[prev];
		uint64_t dt = ts.t[ts.head] - ts.t[prev];

		/*
		 * (no movement: LVGL sends SCROLL twice for some steps, 0.1-0.5
		 * ms apart at the same point; counted as a slow stretch, every
		 * drag ended fast scrolling)
		 */
		if (d && dt > 0 && (fabsf(d * 1e6f / dt) < FAST_SLOW_V ||
			       (fs.sign && d && (d > 0 ? 1 : -1) != fs.sign))) {
			if (getenv("N72_FPS") && !fs.slow)
				fprintf(stderr, "fast scroll off: step %d px in %.1f ms (sign %d)\n", (int)d, dt / 1000.0, fs.sign);
			fs.slow = true;
		}
	}
}

/* finger velocity at the lift, px/s, over the last FLICK_WINDOW_US */
static float ts_velocity(bool hor)
{
	int k, i = ts.head, j = ts.head;
	uint64_t now = mono_us();
	float dt;

	if (ts.n < 2 || now - ts.t[ts.head] > FLICK_WINDOW_US)
		return 0;		/* held still before the lift */
	for (k = 1; k < ts.n; k++) {
		int m = (ts.head - k + TS_N) % TS_N;

		if (ts.t[ts.head] - ts.t[m] > FLICK_WINDOW_US)
			break;
		j = m;
	}
	if (j == i)
		return 0;
	dt = (ts.t[i] - ts.t[j]) / 1e6f;
	if (dt <= 0)
		return 0;
	{
		/*
		 * The slope of a least-squares line through the samples in the
		 * window: two end points alone follow the touch controller's
		 * jitter (reports every ~16 ms, a pixel or two off).
		 */
		double st = 0, sp = 0, stt = 0, stp = 0, n = 0;
		int m = j;

		for (;;) {
			double t = (double)(ts.t[m] - ts.t[j]) / 1e6;
			double pos = hor ? ts.x[m] : ts.y[m];

			st += t;
			sp += pos;
			stt += t * t;
			stp += t * pos;
			n++;
			if (m == i)
				break;
			m = (m + 1) % TS_N;
		}
		if (n >= 3 && n * stt - st * st > 1e-9)
			return (float)((n * stp - st * sp) / (n * stt - st * st));
	}
	return (hor ? ts.x[i] - ts.x[j] : ts.y[i] - ts.y[j]) / dt;
}

/* px @obj is past an end (0: within them); @out: +1 past the top/left, -1 the other */
static int32_t glide_past(lv_obj_t *obj, bool hor, int *out)
{
	int32_t a = hor ? lv_obj_get_scroll_left(obj) : lv_obj_get_scroll_top(obj);
	int32_t b = hor ? lv_obj_get_scroll_right(obj) : lv_obj_get_scroll_bottom(obj);

	if (out)
		*out = a < 0 ? 1 : -1;
	return a < 0 ? -a : b < 0 ? -b : 0;
}

static void fling_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_indev_t *in = lv_event_get_param(e);
	lv_point_t p;
	bool hor;
	float v;
	int sign;
	int32_t room, back;

	if (!in)
		return;
	hor = in->pointer.scroll_dir == LV_DIR_HOR;
	gl.caught = NULL;	/* (a drag after the catch: this lift decides) */
	/*
	 * LVGL's throw off, and its scroll ended here: the glide below
	 * replaces both. (Left to it, its throw handler, an animation that
	 * starts after this, springs a list let go past an end back itself:
	 * with the glide's own way back, two springs at once, the list
	 * coming to rest inside its end.)
	 */
	in->pointer.scroll_throw_vect.x = in->pointer.scroll_throw_vect.y = 0;
	in->pointer.scroll_throw_vect_ori = in->pointer.scroll_throw_vect;
	in->pointer.scroll_obj = NULL;
	in->pointer.scroll_dir = LV_DIR_NONE;
	lv_obj_send_event(obj, LV_EVENT_SCROLL_END, in);	/* (what LVGL would send) */
	if (!lv_obj_is_valid(obj))
		return;
	v = ts_velocity(hor);
	sign = v > 0 ? 1 : -1;
	/* past an end (pulled out): back to it, with the lift's speed if outwards */
	{
		int out;

		back = glide_past(obj, hor, &out);
		/*
		 * Thrown inwards hard enough to cross the end: an ordinary
		 * glide from where it is (iOS), not a spring back to the end
		 * after carrying past it.
		 */
		if (back > 0 && !(sign != out && fabsf(v) >= GLIDE_MIN_V &&
				  fabsf(v) / 1000.0 * GLIDE_F / (1.0 - GLIDE_F) > back)) {
			fs_reset();
			glide_back(obj, hor, out, back, v);
			return;
		}
	}
	if (fs.slow || (fs.sign && sign != fs.sign))
		fs_reset();
	if (fabsf(v) >= FAST_V) {
		/* the boost this drag earned (iOS computes it during the drag) */
		if (fs.count >= 3) {
			lv_indev_get_point(in, &p);
			fs.mult = LV_MIN(fs.start_mult + LV_MIN(LV_ABS((hor ? p.x : p.y) - fs.drag_start) / 240.0,
								 0.9) * (1 + 0.5 * (fs.count - 3)), FAST_MAX);
		}
		fs.count++;
		fs.end_ms = lv_tick_get();
		fs.sign = sign;
	} else if (fabsf(v) >= GLIDE_MIN_V) {
		fs_reset();
		fs.sign = sign;
	}
	n72_trace("flick", 0);
	if (getenv("N72_FPS"))
		fprintf(stderr, "flick: %.0f px/s (fast %d, x%.2f)\n", v, fs.count, fs.mult);
	if (fabsf(v) < GLIDE_MIN_V)
		return;		/* iOS: a stop, no glide */
	room = v > 0 ? (hor ? lv_obj_get_scroll_left(obj) : lv_obj_get_scroll_top(obj))
		     : (hor ? lv_obj_get_scroll_right(obj) : lv_obj_get_scroll_bottom(obj));
	glide_start(obj, hor, v, LV_MAX(room, 0));
}

static void fling_accel(lv_obj_t *obj)
{
	lv_obj_add_event_cb(obj, ts_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(obj, ts_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(obj, fling_cb, LV_EVENT_SCROLL_THROW_BEGIN, NULL);
}

/* the touch path of every pointer device (after drags_init) */
static void glide_init(void)
{
	for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
		if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
			lv_indev_add_event_cb(in, ts_press_cb, LV_EVENT_PRESSED, NULL);
			lv_indev_add_event_cb(in, ts_release_cb, LV_EVENT_RELEASED, NULL);
		}
}

/* scroll-cached page bodies, to find the one on the active screen */
#define PF_BODIES	16
static lv_obj_t *pf_bodies[PF_BODIES];
static int pf_nbodies;

static int pf_had;		/* (statistics) */

static void pf_drop(int i)
{
	if (lv_obj_is_valid(pf.child[i]))
		lv_obj_remove_event_cb(pf.child[i], pf_child_deleted);
	pf.bytes -= pf_size(pf.buf[i]);
	lv_draw_buf_destroy(pf.buf[i]);
	pf.n--;
	pf.child[i] = pf.child[pf.n];
	pf.buf[i] = pf.buf[pf.n];
}

static void pf_clear(void)
{
	while (pf.n)
		pf_drop(pf.n - 1);
	pf.owner = NULL;
}

/* Take @c's prefetched picture, if there is one. */
static lv_draw_buf_t *pf_take(lv_obj_t *c)
{
	lv_draw_buf_t *b;
	int i;

	for (i = 0; i < pf.n; i++) {
		if (pf.child[i] != c)
			continue;
		b = pf.buf[i];
		lv_obj_remove_event_cb(c, pf_child_deleted);
		pf.bytes -= pf_size(b);
		pf.n--;
		pf.child[i] = pf.child[pf.n];
		pf.buf[i] = pf.buf[pf.n];
		return b;
	}
	return NULL;
}

/* The MBX copies at most 1024 lines (a taller picture came out blank below). */
#define SC_MAX_H	1024

/*
 * The units a page scrolls as pictures: its children, except those taller
 * than the screen (a long list card), which are drawn live. Measured on
 * Wi-Fi (24 rows): live 54-58 fps; one picture of the whole card came out
 * blank below 1024 lines and took ~100 ms; pictures of each row cost more
 * than they saved.
 */
#define SC_UNITS	32

static int sc_units(lv_obj_t *body, lv_obj_t **u)
{
	uint32_t i, cnt;
	int n = 0;

	cnt = lv_obj_get_child_count(body);
	for (i = 0; i < cnt && n < SC_UNITS; i++) {
		lv_obj_t *c = lv_obj_get_child(body, i);

		if (!c || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) ||
		    lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1) || lv_obj_get_height(c) > H)
			continue;
		u[n++] = c;
	}
	return n;
}

/* The colour a unit's picture is flattened onto: its parent's. */
static lv_color_t sc_bg_of(lv_obj_t *c)
{
	lv_obj_t *p = lv_obj_get_parent(c);

	if (p && lv_obj_get_style_bg_opa(p, 0) >= LV_OPA_COVER)
		return lv_obj_get_style_bg_color(p, 0);
	return M.surface;
}

/* A prefetched row went (a list rebuilt): its picture with it. */
static void pf_child_deleted(lv_event_t *e)
{
	lv_obj_t *c = lv_event_get_current_target(e);

	for (int i = 0; i < pf.n; i++)
		if (pf.child[i] == c) {
			lv_draw_buf_destroy(pf.buf[i]);
			pf.n--;
			pf.child[i] = pf.child[pf.n];
			pf.buf[i] = pf.buf[pf.n];
			break;
		}
	pf.bytes = 0;
	for (int i = 0; i < pf.n; i++)
		pf.bytes += pf_size(pf.buf[i]);
}

/* A scroll picture no longer shown: keep it for the next scroll if it fits. */
static void pf_give(lv_obj_t *owner, lv_obj_t *c, lv_draw_buf_t *b)
{
	lv_image_cache_drop(b);
	if (owner && pf.owner == owner && lv_obj_is_valid(c) && pf.n < PREF_MAX &&
	    pf.bytes + pf_size(b) <= PREF_BUDGET) {
		pf.child[pf.n] = c;
		pf.buf[pf.n++] = b;
		pf.bytes += pf_size(b);
		lv_obj_add_event_cb(c, pf_child_deleted, LV_EVENT_DELETE, NULL);
		return;
	}
	lv_draw_buf_destroy(b);
}

/* A label changed: the prefetched picture of the row holding it is stale. */
static void pf_label_changed(lv_obj_t *lbl)
{
	for (int i = pf.n - 1; i >= 0; i--)
		for (lv_obj_t *o = lbl; o; o = lv_obj_get_parent(o))
			if (o == pf.child[i]) {
				pf_drop(i);
				break;
			}
}

/* A scroll picture of @c: rendered, in GPU memory, flattened onto what is behind. */
static lv_draw_buf_t *sc_picture(lv_obj_t *c)
{
	lv_draw_buf_t *b, *flat;

	if (lv_obj_get_height(c) + 2 * lv_obj_get_ext_draw_size(c) > SC_MAX_H)
		return NULL;
	b = lv_snapshot_take(c, LV_COLOR_FORMAT_ARGB8888);
	if (!b)
		return NULL;
	lv_draw_buf_flush_cache(b, NULL);	/* the GPU reads it */
	/*
	 * Flatten it onto the page colour once: frames then copy opaque
	 * pictures instead of blending them (a third of the memory traffic,
	 * and the GPU is memory bound).
	 */
	flat = mbx_gpu_flatten(b, sc_bg_of(c));
	if (flat) {
		lv_draw_buf_destroy(b);
		b = flat;
	}
	return b;
}

/* Picture of child @c in place of the live child; false if not possible now. */
static bool sc_cache_child(lv_obj_t *body, lv_obj_t *c)
{
	/*
	 * The snapshot covers the child's extra draw area too (labels: a
	 * quarter of the line height on every side, for overhanging glyphs),
	 * so the picture goes that much up and left of the child's position.
	 */
	int32_t ext = lv_obj_get_ext_draw_size(c);
	long sz = (long)(lv_obj_get_width(c) + 2 * ext) * (lv_obj_get_height(c) + 2 * ext) * 4;
	lv_draw_buf_t *b;
	lv_obj_t *p;

	uint32_t t0 = lv_tick_get();

	if (sc.n >= SC_MAX || sz <= 0 || sc.bytes + sz > SC_BUDGET)
		return false;
	b = pf_take(c);
	if (!b)
		b = sc_picture(c);
	if (!b)
		return false;
	/*
	 * Always a child of @body, even for a row of a list card: a child
	 * added to the card would make it lay out all its rows again (their
	 * labels too: 50-90 ms on Wi-Fi). Placed from the row's screen
	 * position, in @body's scrolled content coordinates.
	 */
	p = lv_image_create(body);
	lv_image_set_src(p, b);
	lv_obj_add_flag(p, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_USER_1);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
	if (lv_obj_get_parent(c) == body) {
		lv_obj_set_pos(p, lv_obj_get_x(c) - ext, lv_obj_get_y(c) - ext);
	} else {
		lv_area_t bc, cc;
		int32_t bw = lv_obj_get_style_border_width(body, 0);

		lv_obj_get_coords(body, &bc);
		lv_obj_get_coords(c, &cc);
		lv_obj_set_pos(p, cc.x1 - ext - bc.x1 - bw - lv_obj_get_style_pad_left(body, 0) +
				  lv_obj_get_scroll_x(body),
			       cc.y1 - ext - bc.y1 - bw - lv_obj_get_style_pad_top(body, 0) +
				  lv_obj_get_scroll_y(body));
	}
	lv_obj_set_style_opa_layered(c, LV_OPA_TRANSP, 0);
	sc.live[sc.n] = c;
	sc.pic[sc.n] = p;
	sc.buf[sc.n] = b;
	sc.size[sc.n] = sz;
	sc.bytes += sz;
	sc.n++;
	if (lv_tick_elaps(t0) > 10 && getenv("N72_FPS"))
		fprintf(stderr, "scroll cache: picture %dx%d took %u ms\n", (int)lv_obj_get_width(c),
			(int)lv_obj_get_height(c), (unsigned)lv_tick_elaps(t0));
	return true;
}

static void sc_uncache(int i)
{
	if (lv_obj_is_valid(sc.pic[i]))
		lv_obj_delete(sc.pic[i]);
	if (lv_obj_is_valid(sc.live[i]))
		lv_obj_set_style_opa_layered(sc.live[i], LV_OPA_COVER, 0);
	pf_give(sc.owner, sc.live[i], sc.buf[i]);
	sc.bytes -= sc.size[i];
	sc.n--;
	sc.live[i] = sc.live[sc.n];
	sc.pic[i] = sc.pic[sc.n];
	sc.buf[i] = sc.buf[sc.n];
	sc.size[i] = sc.size[sc.n];
}

/*
 * Keep pictures of the children on screen or within half a screen of it:
 * drop those further away, add those coming in (at most @max new ones per
 * call, so a fling does not stall on a burst of snapshots).
 */
static void sc_fill(lv_obj_t *body, int max, bool visible_only)
{
	lv_obj_t *units[SC_UNITS];
	uint32_t i, cnt;
	lv_area_t bc, cc;
	int32_t lo, hi, k;

	lv_obj_get_coords(body, &bc);
	lo = bc.y1 - (visible_only ? 0 : H / 2);
	hi = bc.y2 + (visible_only ? 0 : H / 2);
	for (k = sc.n - 1; k >= 0; k--) {
		if (!lv_obj_is_valid(sc.pic[k])) {
			sc_uncache(k);
			continue;
		}
		lv_obj_get_coords(sc.pic[k], &cc);
		if (cc.y2 < lo - H || cc.y1 > hi + H)
			sc_uncache(k);
	}
	/* listed after the drops above (pictures are children too, flagged USER_1) */
	cnt = sc_units(body, units);
	for (i = 0; i < cnt && max > 0; i++) {
		lv_obj_t *c = units[i];

		lv_obj_get_coords(c, &cc);
		if (cc.y2 < lo || cc.y1 > hi)
			continue;
		for (k = 0; k < sc.n; k++)
			if (sc.live[k] == c)
				break;
		if (k < sc.n)
			continue;
		if (!sc_cache_child(body, c))
			continue;	/* (too tall, or no memory: drawn live) */
		max--;
	}
}

static void sc_begin(lv_obj_t *body)
{
	if (sc.owner)
		sc_restore(true);
	if (sc.timer) {
		lv_timer_delete(sc.timer);
		sc.timer = NULL;
	}
	pf_had = pf.owner == body ? pf.n : -1;
	sc.owner = body;
	/*
	 * Only what is on screen now (prefetched pictures, mostly); the rows
	 * just outside come one per frame as they near (LV_EVENT_SCROLL).
	 */
	sc_fill(body, SC_MAX, true);
	if (getenv("N72_FPS"))
		fprintf(stderr, "scroll cache: %d pictures, %d prefetched before\n", sc.n, pf_had);
}

static lv_obj_t *pf_body_on(lv_obj_t *scr)
{
	for (int i = 0; i < pf_nbodies; i++)
		if (lv_obj_get_screen(pf_bodies[i]) == scr)
			return pf_bodies[i];
	return NULL;
}

/*
 * One picture per tick, while nothing moves and nobody touches. With
 * nothing left to take (all near rows taken, the budget spent, a screen
 * without a list, or the screen dark) the timer pauses: anything that
 * could give it work again redraws something, and pf_invalidated()
 * resumes it.
 */
static lv_timer_t *pf_timer;
#define PF_QUIET_MS	150		/* nothing redrawn for this long: the screen is still */
static uint32_t pf_moved_ms;		/* when something was last redrawn */

static void pf_tick(lv_timer_t *t)
{
	lv_obj_t *body, *units[SC_UNITS], *best = NULL;
	lv_area_t bc, cc;
	uint32_t i, cnt;
	int32_t best_d = INT32_MAX, d;
	lv_draw_buf_t *b;
	int k;

	if (screen_off) {
		lv_timer_pause(t);
		return;
	}
	/*
	 * Not while the screen is changing either (a glide after the lift,
	 * an animation, text being updated): a picture taken then is dropped
	 * by the next frame's redraw and taken again, 20-30 ms each time in
	 * the middle of the movement.
	 */
	if (tr.scr || sc.owner || gl.on || lv_display_get_inactive_time(NULL) < 300 ||
	    lv_tick_elaps(pf_moved_ms) < PF_QUIET_MS)
		return;
	body = pf_body_on(lv_screen_active());
	if (body != pf.owner) {
		pf_clear();
		pf.owner = body;
	}
	/* (a body that does not scroll never shows pictures: Logs scrolls a box inside it) */
	if (!body || !lv_obj_has_flag(body, LV_OBJ_FLAG_SCROLLABLE) ||
	    pf.n >= PREF_MAX || pf.bytes >= PREF_BUDGET) {
		lv_timer_pause(t);
		return;
	}
	lv_obj_update_layout(body);
	lv_obj_get_coords(body, &bc);
	cnt = sc_units(body, units);
	/* the row nearest the view that has no picture yet: on screen first */
	for (i = 0; i < cnt; i++) {
		lv_obj_t *c = units[i];

		lv_obj_get_coords(c, &cc);
		d = cc.y2 < bc.y1 ? bc.y1 - cc.y2 : cc.y1 > bc.y2 ? cc.y1 - bc.y2 : 0;
		if (d >= best_d || d > 2 * H)
			continue;
		for (k = 0; k < pf.n; k++)
			if (pf.child[k] == c)
				break;
		if (k < pf.n || lv_obj_get_height(c) + 2 * lv_obj_get_ext_draw_size(c) > SC_MAX_H)
			continue;
		best = c;
		best_d = d;
	}
	if (!best) {
		lv_timer_pause(t);
		return;
	}
	if (!(b = sc_picture(best)))
		return;
	pf.child[pf.n] = best;
	pf.buf[pf.n++] = b;
	pf.bytes += pf_size(b);
	lv_obj_add_event_cb(best, pf_child_deleted, LV_EVENT_DELETE, NULL);
}

/*
 * Something on screen is being redrawn: a prefetched row there no longer
 * matches its picture. Not while a finger is down: that is a pressed
 * highlight, which goes again before the scroll takes the picture.
 */
static void pf_invalidated(lv_event_t *e)
{
	const lv_area_t *a = lv_event_get_param(e);
	lv_area_t cc, x;
	int i;

	lv_indev_t *in;

	pf_moved_ms = lv_tick_get();
	if (pf_timer && pf_timer->paused && !screen_off)
		lv_timer_resume(pf_timer);
	if (!pf.n || !a)
		return;
	for (in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
		if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER &&
		    lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED)
			return;
	for (i = pf.n - 1; i >= 0; i--) {
		if (!lv_obj_is_valid(pf.child[i])) {
			pf_drop(i);
			continue;
		}
		lv_obj_get_coords(pf.child[i], &cc);
		if (_lv_area_intersect(&x, &cc, a))
			pf_drop(i);
	}
}

static void pf_init(void)
{
	if (getenv("N72_FPS"))
		for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
			lv_indev_add_event_cb(in, ui_release_cb, LV_EVENT_RELEASED, NULL);
	if (getenv("N72_NO_SCROLL_CACHE") || getenv("N72_NO_PREFETCH"))
		return;
	lv_display_add_event_cb(lv_display_get_default(), pf_invalidated,
				LV_EVENT_INVALIDATE_AREA, NULL);
	pf_timer = lv_timer_create(pf_tick, 80, NULL);
}

static void sc_event(lv_event_t *e)
{
	lv_obj_t *body = lv_event_get_current_target(e);
	static int off = -1;

	if (off < 0)
		off = getenv("N72_NO_SCROLL_CACHE") != NULL;	/* (for measurements) */
	if (off)
		return;

	switch (lv_event_get_code(e)) {
	case LV_EVENT_SCROLL:
		sc.last_move = lv_tick_get();
		if (sc.owner == body)
			sc_fill(body, 1, false);	/* children coming into view: one picture a frame */
		break;
	case LV_EVENT_SCROLL_BEGIN:
		sc.last_move = lv_tick_get();
		if (sc.owner != body)
			sc_begin(body);
		else if (sc.timer)		/* moving again before it settled */
			lv_timer_reset(sc.timer);
		break;
	case LV_EVENT_SCROLL_END:
		/*
		 * Sent when the finger lifts too, but the throw and the elastic
		 * bounce still move it: restore once it is really still.
		 */
		if (sc.owner == body) {
			if (!sc.timer)
				sc.timer = lv_timer_create(sc_settle, 100, body);
			else
				lv_timer_reset(sc.timer);
		}
		break;
	case LV_EVENT_DELETE:	/* the pictures go with the page */
		if (sc.owner == body)
			sc_restore(false);
		if (pf.owner == body)
			pf_clear();
		for (int i = 0; i < pf_nbodies; i++)
			if (pf_bodies[i] == body)
				pf_bodies[i] = pf_bodies[--pf_nbodies];
		if (sc.timer) {
			lv_timer_delete(sc.timer);
			sc.timer = NULL;
		}
		break;
	default:
		break;
	}
}

/*
 * The scroll bar, as iOS's indicator: on at once as a page starts to move,
 * and a 500 ms fade (ease-in-out) as soon as it stops (UIScrollView
 * _hideScrollIndicators); never shorter than 36 px (lv_conf.h). LVGL's
 * "active" mode switched it on and off with the scroll, a flash.
 */
#define SB_IN_MS	0
#define SB_HOLD_MS	0
#define SB_OUT_MS	500

static void sb_opa(void *obj, int32_t v)
{
	lv_obj_set_style_opa(obj, (lv_opa_t)v, LV_PART_SCROLLBAR);
}

static void sb_fade(lv_obj_t *obj, bool in)
{
	lv_anim_t a;
	int32_t now = lv_obj_get_style_opa(obj, LV_PART_SCROLLBAR);

	lv_anim_delete(obj, sb_opa);
	if (in && now == LV_OPA_COVER)
		return;
	if (in && !SB_IN_MS) {
		sb_opa(obj, LV_OPA_COVER);
		return;
	}
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, sb_opa);
	lv_anim_set_values(&a, now, in ? LV_OPA_COVER : LV_OPA_TRANSP);
	lv_anim_set_duration(&a, in ? SB_IN_MS : SB_OUT_MS);
	lv_anim_set_delay(&a, in ? 0 : SB_HOLD_MS);
	lv_anim_set_path_cb(&a, in ? ios_path_ease_out : ios_path_ease_in_out);
	lv_anim_start(&a);
}

static void sb_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target(e);

	switch (lv_event_get_code(e)) {
	case LV_EVENT_SCROLL_BEGIN:
		sb_fade(obj, true);
		break;
	case LV_EVENT_SCROLL_END:
		/* a glide sends its own begin and end: fade only when all is still */
		if (!(gl.on && gl.obj == obj))
			sb_fade(obj, false);
		break;
	case LV_EVENT_DELETE:
		lv_anim_delete(obj, sb_opa);
		break;
	default:
		break;
	}
}

static void scroll_cached(lv_obj_t *body)
{
	/* the bar always drawn, its opacity animated (sb_cb) */
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ON);
	lv_obj_set_style_opa(body, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
	lv_obj_add_event_cb(body, sb_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(body, sb_cb, LV_EVENT_SCROLL_END, NULL);
	lv_obj_add_event_cb(body, sb_cb, LV_EVENT_DELETE, NULL);
	fling_accel(body);
	if (pf_nbodies < PF_BODIES)
		pf_bodies[pf_nbodies++] = body;
	lv_obj_add_event_cb(body, sc_event, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(body, sc_event, LV_EVENT_SCROLL_END, NULL);
	lv_obj_add_event_cb(body, sc_event, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(body, sc_event, LV_EVENT_DELETE, NULL);
}

/*
 * A page with a top app bar (back arrow and title) and a scrolling column
 * body with 12 px margins. Returns the body.
 */
static lv_obj_t *m3_page(const char *title, lv_obj_t **scr_out)
{
	lv_obj_t *scr = lv_obj_create(NULL), *bar, *btn, *body;

	m3_screen(scr);
	bar = lv_obj_create(scr);
	plain(bar);
	lv_obj_set_size(bar, W, 56);
	lv_obj_set_style_bg_color(bar, M.surface, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 4, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	btn = lv_button_create(bar);
	plain(btn);
	lv_obj_set_size(btn, 48, 48);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(btn, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(m3_label(btn, LV_SYMBOL_LEFT, &lv_font_montserrat_20, M.on_surface));
	lv_obj_align(m3_label(bar, title, &lv_font_montserrat_24, M.on_surface),
		     LV_ALIGN_LEFT_MID, 56, 0);

	body = lv_obj_create(scr);
	plain(body);
	lv_obj_set_size(body, W, H - 56);
	lv_obj_set_pos(body, 0, 56);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_pad_hor(body, STATUS_PAD, 0);	/* the screen's side margin */
	lv_obj_set_style_pad_top(body, 4, 0);
	/* the last card ends as far above the bottom as the first starts
	 * below the title (the bar's own margin included) */
	lv_obj_set_style_pad_bottom(body, 20, 0);
	lv_obj_set_style_pad_row(body, 8, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	scroll_cached(body);
	*scr_out = scr;
	return body;
}

/*
 * An object back to its resting look at once, not through its style
 * transition (adding a style ends an object's transitions): for pictures
 * taken right after a tap.
 */
static void obj_rest(lv_obj_t *t)
{
	static lv_style_t none;
	uint32_t i;

	if (!t)
		return;
	lv_style_init(&none);
	lv_obj_remove_state(t, LV_STATE_PRESSED);
	for (i = 0; i <= lv_obj_get_child_count(t); i++) {
		lv_obj_t *o = i ? lv_obj_get_child(t, i - 1) : t;

		lv_obj_add_style(o, &none, 0);
		lv_obj_remove_style(o, &none, 0);
	}
}

static lv_obj_t *push_quiet_scr;	/* set: push() only records the page */
static bool push_quiet;

/*
 * How each page on the stack was opened, so that a turn of the screen can
 * build them all again at the new size (ui_turn): the opener, or the
 * folder of a Files page. Each opener marks itself; push() files it.
 * A page with neither (the Wi-Fi password) keeps the screen as it is.
 */
static struct nav_re {
	void (*open)(lv_event_t *);
	char *dir;
} nav_re[NAV_MAX], nav_cur;

static void nav_mark(void (*open)(lv_event_t *), const char *dir)
{
	free(nav_cur.dir);
	nav_cur.open = open;
	nav_cur.dir = dir ? strdup(dir) : NULL;
}

static void push(lv_obj_t *scr)
{
	if (nav_n < NAV_MAX) {
		free(nav_re[nav_n].dir);
		nav_re[nav_n] = nav_cur;
		nav[nav_n++] = scr;
	} else {
		free(nav_cur.dir);
	}
	nav_cur.open = NULL;
	nav_cur.dir = NULL;
	if (push_quiet) {
		push_quiet_scr = scr;	/* the card animation shows it */
		return;
	}
	show(scr);
}

/* ------------------------------------------------- background commands */

/*
 * Run a shell command in the background; when it ends, done() gets its
 * output (NUL-terminated, truncated at 8 KiB) unless the object @owner
 * was deleted meanwhile (the page was closed).
 */
struct job {
	pid_t pid;
	int fd;
	int pidfd;		/* -1: watched by the 150 ms timer instead */
	char buf[8192];
	size_t len;
	void (*done)(const char *out, void *ud);
	void *ud;
	lv_obj_t *owner;
};

/*
 * A job ends when its shell does (a command it started in the background
 * may keep the output pipe open): the shell's pidfd turns readable then.
 * The pipe is watched too, read as output comes (a job writing more than
 * the pipe holds would otherwise wait for us). Without a pidfd, or with
 * the main loop's watch table full, a 150 ms timer looks instead.
 */
#define JOB_WATCHED 8
static struct job *job_w[JOB_WATCHED];

/* Read what the job wrote; true at the end of its output (EOF). */
static bool job_drain(struct job *j)
{
	ssize_t n = -1;

	while (j->len < sizeof(j->buf) - 1 &&
	       (n = read(j->fd, j->buf + j->len, sizeof(j->buf) - 1 - j->len)) > 0)
		j->len += n;
	if (j->len >= sizeof(j->buf) - 1) {
		char sink[512];	/* (beyond 8 KiB: dropped, so the job is not held up) */

		while ((n = read(j->fd, sink, sizeof(sink))) > 0)
			;
	}
	return n == 0;
}

static void job_end(struct job *j)
{
	job_drain(j);
	j->buf[j->len] = 0;
	close(j->fd);
	if (j->done && (!j->owner || lv_obj_is_valid(j->owner)))
		j->done(j->buf, j->ud);
	free(j);
}

static struct job *job_by_fd(int fd, int *slot)
{
	for (int i = 0; i < JOB_WATCHED; i++)
		if (job_w[i] && (job_w[i]->fd == fd || job_w[i]->pidfd == fd)) {
			*slot = i;
			return job_w[i];
		}
	return NULL;
}

static void job_ev(int fd)
{
	int slot, st;
	struct job *j = job_by_fd(fd, &slot);

	if (!j) {
		ui_unwatch(fd);
		return;
	}
	if (fd == j->fd) {
		/* at EOF the pipe stays readable: watch only the shell from here */
		if (job_drain(j))
			ui_unwatch(j->fd);
		return;
	}
	/* the shell ended */
	if (waitpid(j->pid, &st, WNOHANG) == 0)
		return;		/* (not yet reaped: the next readiness) */
	ui_unwatch(j->fd);
	ui_unwatch(j->pidfd);
	close(j->pidfd);
	job_w[slot] = NULL;
	job_end(j);
}

static bool job_watch(struct job *j)
{
	int slot = -1;

	for (int i = 0; i < JOB_WATCHED; i++)
		if (!job_w[i]) {
			slot = i;
			break;
		}
	if (slot < 0 || nwatch > NWATCH - 2)
		return false;
	j->pidfd = syscall(SYS_pidfd_open, j->pid, 0);
	if (j->pidfd < 0)
		return false;
	fcntl(j->pidfd, F_SETFD, FD_CLOEXEC);
	job_w[slot] = j;
	ui_watch(j->fd, job_ev);
	ui_watch(j->pidfd, job_ev);
	return true;
}

static void job_poll(lv_timer_t *t)
{
	struct job *j = lv_timer_get_user_data(t);
	int st;

	job_drain(j);
	if (waitpid(j->pid, &st, WNOHANG) == 0)
		return;
	lv_timer_delete(t);
	job_end(j);
}

static int job_run(const char *cmd, lv_obj_t *owner,
		   void (*done)(const char *out, void *ud), void *ud);

/* A job asked for during a page slide, started once the slide is over. */
struct job_later {
	char *cmd;
	lv_obj_t *owner;
	void (*done)(const char *out, void *ud);
	void *ud;
};

static void job_later_cb(lv_timer_t *t)
{
	struct job_later *l = lv_timer_get_user_data(t);

	if (tr.scr)
		return;			/* still sliding (a second one): look again */
	lv_timer_delete(t);
	if (!l->owner || lv_obj_is_valid(l->owner))
		job_run(l->cmd, l->owner, l->done, l->ud);
	free(l->cmd);
	free(l);
}

/* 0: started (or to start after the slide); -1: it could not be (@done is not called) */
static int job_run(const char *cmd, lv_obj_t *owner,
		   void (*done)(const char *out, void *ud), void *ud)
{
	struct job *j;
	posix_spawn_file_actions_t fa;
	char *argv[] = { "sh", "-c", (char *)cmd, NULL };
	int p[2];

	/*
	 * Not during a slide: on one core the command (bluetoothctl alone is
	 * ~160 ms of CPU) takes the slide's frames. Its result lands a moment
	 * later instead.
	 */
	if (tr.scr) {
		struct job_later *l = calloc(1, sizeof(*l));

		if (l && (l->cmd = strdup(cmd))) {
			l->owner = owner;
			l->done = done;
			l->ud = ud;
			lv_timer_create(job_later_cb, 50, l);
			return 0;
		}
		free(l);
	}
	j = calloc(1, sizeof(*j));
	if (!j || pipe(p)) {
		free(j);
		return -1;
	}
	/*
	 * posix_spawn, not fork: glibc's is a vfork-style clone, so the page
	 * tables of this process (GPU pool, LVGL) are not copied for a
	 * command that only execs a shell.
	 */
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_adddup2(&fa, p[1], 2);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	if (posix_spawn(&j->pid, "/bin/sh", &fa, NULL, argv, environ))
		j->pid = -1;
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (j->pid < 0) {
		close(p[0]);
		free(j);
		return -1;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	j->fd = p[0];
	j->done = done;
	j->ud = ud;
	j->owner = owner;
	j->pidfd = -1;
	if (!job_watch(j))
		lv_timer_create(job_poll, 150, j);
	return 0;
}

/* Shell-quote @s into @out ('...' with ' escaped). */
static void shq(const char *s, char *out, size_t len)
{
	size_t o = 0;

	if (len < 3)
		return;
	out[o++] = '\'';
	for (; *s && o + 5 < len; s++) {
		if (*s == '\'') {
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			out[o++] = *s;
		}
	}
	out[o++] = '\'';
	out[o] = 0;
}

/* A program is installed: a path as given, a bare name looked up on PATH
 * (the distribution decides between /sbin, /usr/sbin, /usr/bin, ...). */
static bool have_tool(const char *name)
{
	const char *path, *p;
	char buf[256];

	if (strchr(name, '/'))
		return access(name, X_OK) == 0;
	path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	for (p = path; *p; ) {
		size_t n = strcspn(p, ":");
		if (n && n + strlen(name) + 2 <= sizeof(buf)) {
			memcpy(buf, p, n);
			buf[n] = '/';
			strcpy(buf + n + 1, name);
			if (access(buf, X_OK) == 0)
				return true;
		}
		p += n;
		if (*p == ':')
			p++;
	}
	return false;
}

/* -------------------------------------------------------- system state */

static int read_sysfs_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int v = def;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

static bool usb_powered(void)
{
	return read_sysfs_int("/sys/class/power_supply/usb/online", 0) == 1;
}

/*
 * Set a label's text only when it changes: lv_label_set_text() always
 * invalidates the label, and a 1 s timer rewriting unchanged text was a
 * redraw and a GPU flush every second.
 */
static void shade_changed(lv_obj_t *o);

static void label_set_if_changed(lv_obj_t *lbl, const char *text)
{
	const char *cur;

	if (!lbl)
		return;
	cur = lv_label_get_text(lbl);

	if (!cur || strcmp(cur, text)) {
		lv_label_set_text(lbl, text);
		shade_changed(lbl);
		bp_changed(lv_obj_get_screen(lbl));
		pf_label_changed(lbl);
	}
}

static void ip_of(const char *ifname, char *out, size_t len)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	snprintf(out, len, "-");
	if (fd < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (!ioctl(fd, SIOCGIFADDR, &ifr))
		snprintf(out, len, "%s",
			 inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	close(fd);
}

/* wpa_cli status: connected SSID (or "") from a "key=value" listing */
static void kv_get(const char *text, const char *key, char *out, size_t len)
{
	size_t kl = strlen(key);
	const char *p = text;

	out[0] = 0;
	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t n = strcspn(p + kl + 1, "\n");

			if (n >= len)
				n = len - 1;
			memcpy(out, p + kl + 1, n);
			out[n] = 0;
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

static char wifi_ssid[64];	/* connected network, "" if none */

/* The radio is on: the card is there and not switched off (n72-wifi). */
static bool wifi_powered(void)
{
	return access("/sys/class/net/wlan0", F_OK) == 0 &&
	       access("/var/run/wifi.off", F_OK) != 0;
}
static bool wifi_up;
static bool bt_powered;
static void bt_auto_kick(void);

static void net_ui_sync(void);

static void wifi_status_done(const char *out, void *ud)
{
	char st[32] = "";

	LV_UNUSED(ud);
	kv_get(out, "wpa_state", st, sizeof(st));
	wifi_up = !strcmp(st, "COMPLETED");
	kv_get(out, "ssid", wifi_ssid, sizeof(wifi_ssid));
	if (!wifi_up)
		wifi_ssid[0] = 0;
	net_ui_sync();
}

static void bt_show_done(const char *out, void *ud)
{
	static bool was;

	LV_UNUSED(ud);
	bt_powered = strstr(out, "Powered: yes") != NULL;
	if (bt_powered && !was)
		bt_auto_kick();		/* (switched on: look for the devices now) */
	was = bt_powered;
	net_ui_sync();
}

/*
 * wpa_cli is a small static binary (~10 ms); bluetoothctl starts glib and
 * D-Bus (~160 ms), so Bluetooth is only asked at start-up: after that its
 * state changes only through the Bluetooth page, which reads it back.
 */
static void status_refresh(bool with_bt)
{
	if (have_tool("wpa_cli"))
		job_run("wpa_cli -i wlan0 status", NULL, wifi_status_done, NULL);
	if (with_bt && have_tool("bluetoothctl"))
		job_run("bluetoothctl show", NULL, bt_show_done, NULL);
}

/* ------------------------------------------------------------- home */

static lv_obj_t *clock_lbl, *date_lbl, *batt_lbl, *status_time;
static lv_obj_t *home_clock, *home_date, *home_batt, *home_net;	/* the widget */
static lv_obj_t *qs_wifi_sub, *qs_bt_sub, *qs_wifi, *qs_bt;
static lv_obj_t *set_wifi_sub, *set_bt_sub;	/* on the Settings page */

static void battery_text(char *buf, size_t len)
{
	char st[16] = "";
	const char *icon;
	FILE *f;
	int cap = read_sysfs_int("/sys/class/power_supply/battery/capacity", -1);

	if (cap < 0) {
		buf[0] = 0;
		return;
	}
	f = fopen("/sys/class/power_supply/battery/status", "r");
	if (f) {
		if (!fgets(st, sizeof(st), f))
			st[0] = 0;
		fclose(f);
	}
	icon = cap > 87 ? LV_SYMBOL_BATTERY_FULL : cap > 62 ? LV_SYMBOL_BATTERY_3 :
	       cap > 37 ? LV_SYMBOL_BATTERY_2 : cap > 12 ? LV_SYMBOL_BATTERY_1 :
	       LV_SYMBOL_BATTERY_EMPTY;
	/*
	 * The bolt while on external power, charging or not: plugged in and
	 * topped up the battery reads "Full" (or "Not charging"), and the bar
	 * looked as on battery.
	 */
	snprintf(buf, len, "%s%d%%  %s",
		 !strncmp(st, "Charging", 8) ||
		 read_sysfs_int("/sys/class/power_supply/usb/online", 0) == 1 ? LV_SYMBOL_CHARGE " " : "",
		 cap, icon);
}

static lv_timer_t *home_timer;

/*
 * A status bar's contents, the same on home and in the bar that comes down
 * over applications and pages: the time on the left; Wi-Fi, Bluetooth and
 * the battery on the right, in a row with fixed gaps (an icon that is not
 * shown takes its gap with it).
 */

static void status_row(lv_obj_t *bar, lv_obj_t **time, lv_obj_t **wifi, lv_obj_t **bt,
		       lv_obj_t **batt)
{
	lv_obj_t *sp;

	lv_obj_set_style_pad_hor(bar, STATUS_PAD, 0);
	lv_obj_set_style_pad_ver(bar, 0, 0);
	lv_obj_set_style_pad_column(bar, STATUS_GAP, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	*time = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface);
	sp = lv_obj_create(bar);		/* pushes the rest to the right */
	lv_obj_remove_style_all(sp);
	lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(sp, 0, 1);
	lv_obj_set_flex_grow(sp, 1);
	*wifi = m3_label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, M.on_surface_v);
	*bt = m3_label(bar, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_14, M.on_surface_v);
	*batt = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_add_flag(*wifi, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(*bt, LV_OBJ_FLAG_HIDDEN);
}

static void qs_style(lv_obj_t *tile, bool on)
{
	lv_color_t c = on ? M.primary_c : M.surface_c_high;

	if (!tile)
		return;

	if (!lv_color_eq(lv_obj_get_style_bg_color(tile, 0), c)) {
		lv_obj_set_style_bg_color(tile, c, 0);
		shade_changed(tile);
	}
}

/*
 * Wi-Fi and Bluetooth as every view shows them (the shade's tiles, the
 * Settings rows, the Wi-Fi and Bluetooth pages, the status bar), all from
 * one state and all updated together (net_ui_sync). While a switch is
 * being carried out (*_want >= 0) the views show where it is going, and
 * status reads that land meanwhile cannot flip them back.
 */
static int wifi_want = -1, bt_want = -1;	/* -1: no switch in progress */
static lv_obj_t *wifi_switch, *bt_switch, *bt_state_lbl;
static lv_obj_t *status_wifi, *status_bt, *app_bar_wifi, *app_bar_bt;

static bool wifi_shown_on(void)
{
	return wifi_want >= 0 ? wifi_want : wifi_powered();
}

static bool bt_shown_on(void)
{
	return bt_want >= 0 ? bt_want : bt_powered;
}

static void switch_show(lv_obj_t *sw, bool on)
{
	if (!sw || !lv_obj_is_valid(sw) || lv_obj_has_state(sw, LV_STATE_CHECKED) == on)
		return;
	if (on)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

static void label_show(lv_obj_t *l, const char *text)
{
	if (l && lv_obj_is_valid(l))
		label_set_if_changed(l, text);
}

static void icon_show(lv_obj_t *l, bool on)
{
	if (!l || lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN) == !on)
		return;
	if (on)
		lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
	shade_changed(l);
	bp_changed(lv_obj_get_screen(l));	/* (pictures of home: the status icons) */
}

/* The sound device connected (n72-audio-route's /run/n72-audio), if any. */
static bool bt_audio_name(char *name, size_t n)
{
	char line[96];
	FILE *f = fopen("/run/n72-audio", "r");
	bool found = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "name=", 5)) {
			snprintf(name, n, "%s", line + 5);
			name[strcspn(name, "\n")] = 0;
			found = name[0] != 0;
		}
	fclose(f);
	return found;
}

static void net_ui_sync(void)
{
	bool w = wifi_shown_on(), b = bt_shown_on(), bt_tool = have_tool("bluetoothctl");
	const char *wsub = wifi_want == 1 && !wifi_powered() ? "Turning on..." :
			   !w ? "Off" : wifi_up ? wifi_ssid : "Not connected";
	char bt_dev[64];
	const char *bsub = !bt_tool ? "Not installed" :
			   bt_want == 1 && !bt_powered ? "Turning on..." :
			   bt_want == 0 && bt_powered ? "Turning off..." :
			   b && bt_audio_name(bt_dev, sizeof(bt_dev)) ? bt_dev : b ? "On" : "Off";

	qs_style(qs_wifi, w);
	qs_style(qs_bt, b);
	label_show(qs_wifi_sub, wsub);
	label_show(qs_bt_sub, bt_tool ? bsub : "Unavailable");
	label_show(set_wifi_sub, wsub);
	label_show(set_bt_sub, bsub);
	switch_show(wifi_switch, w);
	switch_show(bt_switch, b);
	label_show(bt_state_lbl, bsub);
	icon_show(status_wifi, w && wifi_up && wifi_want < 0);
	icon_show(status_bt, b && bt_want < 0);
	icon_show(app_bar_wifi, w && wifi_up && wifi_want < 0);
	icon_show(app_bar_bt, b && bt_want < 0);
	if (home_net)
		label_set_if_changed(home_net, wifi_up ? wifi_ssid : "Wi-Fi off");
	app_bar_update();
}

/*
 * The status bar follows events, not a poll: the kernel's power_supply
 * uevents (USB in or out, the battery gauge's new level: main.c's
 * uevent_ev) and rtnetlink's link and address messages for wlan0 (Wi-Fi
 * associated, lost, switched off), each read only when it comes; Bluetooth
 * through BlueZ's signals (bz_ev). An event marks what is stale and one
 * short timer reads it, so a burst (an association sends several) costs
 * one sysfs read or one wpa_cli. While the screen is dark only the marks
 * are kept: the wake path's home_update reads what they name.
 */
static bool status_batt_dirty, status_net_dirty;
static lv_timer_t *status_ev_timer;
static int rtnl_fd = -1;

static void status_batt_update(void)
{
	char buf[64];

	battery_text(buf, sizeof(buf));
	label_set_if_changed(batt_lbl, buf);
	if (home_batt)
		label_set_if_changed(home_batt, buf);
	app_bar_update();
}

static void status_ev_run(lv_timer_t *t)
{
	LV_UNUSED(t);
	status_ev_timer = NULL;
	if (getenv("N72_STATUS_LOG"))
		fprintf(stderr, "n72-ui: status event: battery %d, Wi-Fi %d%s\n", status_batt_dirty,
			status_net_dirty, screen_off ? " (dark: on wake)" : "");
	if (screen_off)
		return;
	if (status_batt_dirty) {
		status_batt_dirty = false;
		status_batt_update();
	}
	if (status_net_dirty) {
		status_net_dirty = false;
		status_refresh(false);	/* (wifi_status_done: net_ui_sync) */
	}
}

static void status_kick(bool batt, bool net)
{
	status_batt_dirty |= batt;
	status_net_dirty |= net;
	if (screen_off || status_ev_timer)
		return;
	status_ev_timer = lv_timer_create(status_ev_run, net ? 300 : 100, NULL);
	lv_timer_set_repeat_count(status_ev_timer, 1);
}

/* Is this rtnetlink message about wlan0 (by name for links, by index for addresses)? */
static bool rtnl_is_wlan(struct nlmsghdr *h, unsigned int wl_idx)
{
	if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK) {
		struct ifinfomsg *ifi = NLMSG_DATA(h);
		int len = (int)IFLA_PAYLOAD(h);

		for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, len); a = RTA_NEXT(a, len))
			if (a->rta_type == IFLA_IFNAME)
				return !strcmp(RTA_DATA(a), "wlan0");
		return wl_idx && (unsigned int)ifi->ifi_index == wl_idx;
	}
	if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR) {
		struct ifaddrmsg *ifa = NLMSG_DATA(h);

		return wl_idx && ifa->ifa_index == wl_idx;
	}
	return false;
}

static void rtnl_ev(int fd)
{
	char buf[4096] __attribute__((aligned(4)));
	unsigned int wl_idx = if_nametoindex("wlan0");
	bool wl = false;
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		int len = (int)n;

		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, len);
		     h = NLMSG_NEXT(h, len))
			wl |= rtnl_is_wlan(h, wl_idx);
	}
	if (wl)
		status_kick(false, true);
}

static void rtnl_init(void)
{
	struct sockaddr_nl a = { .nl_family = AF_NETLINK,
				 .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR };
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);

	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		fd = -1;
	}
	rtnl_fd = fd;
	if (fd >= 0)
		ui_watch(fd, rtnl_ev);
}

static void home_update(lv_timer_t *t)
{
	static const char *const days[] = { "Sunday", "Monday", "Tuesday",
		"Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September",
		"October", "November", "December" };
	static time_t last_status;
	time_t now = time(NULL);
	char buf[64];
	struct tm tm;

	if (screen_off)
		return;		/* (the wake path calls this again) */
	/* (Wi-Fi: rtnetlink's events mark it; a poll only without them) */
	if (rtnl_fd >= 0 ? status_net_dirty : now - last_status >= 30 || now < last_status) {
		status_net_dirty = false;
		status_refresh(false);	/* lands in a moment: wifi_status_done */
		last_status = now;
	}
	status_batt_dirty = false;	/* (read below) */
	localtime_r(&now, &tm);
	/* No seconds on screen: run again just after the next minute starts. */
	if (t)
		lv_timer_set_period(t, (60 - tm.tm_sec) * 1000 + 50);
	snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	label_set_if_changed(clock_lbl, buf);
	label_set_if_changed(status_time, buf);
	if (tm.tm_year + 1900 < 2024)
		snprintf(buf, sizeof(buf), "Clock not set");
	else
		snprintf(buf, sizeof(buf), "%s, %d %s", days[tm.tm_wday],
			 tm.tm_mday, months[tm.tm_mon]);
	label_set_if_changed(date_lbl, buf);
	if (home_date) {
		char d[32];

		if (tm.tm_year + 1900 < 2024)
			snprintf(d, sizeof(d), "Clock not set");
		else
			snprintf(d, sizeof(d), "%.3s %d %.3s", days[tm.tm_wday], tm.tm_mday,
				 months[tm.tm_mon]);
		label_set_if_changed(home_date, d);
	}
	if (home_clock) {
		snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
		label_set_if_changed(home_clock, buf);
	}
	battery_text(buf, sizeof(buf));
	label_set_if_changed(batt_lbl, buf);
	if (home_batt)
		label_set_if_changed(home_batt, buf);
	net_ui_sync();
}

static void open_settings(lv_event_t *e);
static void open_wallpaper(lv_event_t *e);
static void open_logs(lv_event_t *e);
static void open_wifi(lv_event_t *e);
static void open_bt(lv_event_t *e);
static void ui_reopen(void);

/* A quick-settings tile: rounded rectangle, icon, title and state line. */
static lv_obj_t *qs_tile(lv_obj_t *parent, const char *icon, const char *title,
			 lv_obj_t **sub, lv_event_cb_t cb)
{
	lv_obj_t *t = lv_obj_create(parent), *col;

	plain(t);
	int32_t tw = (W - 2 * STATUS_PAD - 8 * (QS_COLS - 1)) / QS_COLS;	/* 8 apart */

	lv_obj_set_size(t, tw, 64);
	lv_obj_set_style_radius(t, 20, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(t, 14, 0);
	lv_obj_set_style_pad_ver(t, 0, 0);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(t, 10, 0);
	m3_label(t, icon, &lv_font_montserrat_20, M.on_surface);
	col = lv_obj_create(t);
	plain(col);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_size(col, tw - 28 - 20 - 10, LV_SIZE_CONTENT);	/* (padding, icon, gap) */
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	m3_label(col, title, &lv_font_montserrat_14, M.on_surface);
	*sub = m3_label(col, "", &lv_font_montserrat_12, M.on_surface_v);
	lv_label_set_long_mode(*sub, LV_LABEL_LONG_DOT);
	lv_obj_set_size(*sub, tw - 28 - 20 - 10, 16);
	lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
	return t;
}

/* An app tile on the home screen. */
/*
 * An application's own picture (a manifest's image=): .n72i, "N72I", u16
 * width, u16 height (little endian), then width x height pixels as B, G, R,
 * A bytes (LVGL's ARGB8888, straight alpha). Loaded once per path; the
 * tiles' reload (apps_reload) lets them all go.
 */
struct app_icon {
	char path[160];
	lv_image_dsc_t dsc;
	lv_color_t avg;		/* its colour, for the card of the open animation */
};
#define APP_ICONS 48
static struct app_icon app_icons[APP_ICONS];
static int app_nicons;

static struct app_icon *app_icon_load(const char *path)
{
	struct app_icon *ic;
	uint8_t hd[8], *px;
	uint32_t w, h, n, i, r = 0, g = 0, b = 0, a = 0;
	FILE *f;

	for (i = 0; i < (uint32_t)app_nicons; i++)
		if (!strcmp(app_icons[i].path, path))
			return &app_icons[i];
	if (app_nicons >= APP_ICONS || !(f = fopen(path, "rb")))
		return NULL;
	if (fread(hd, 1, 8, f) != 8 || memcmp(hd, "N72I", 4)) {
		fclose(f);
		return NULL;
	}
	w = hd[4] | hd[5] << 8;
	h = hd[6] | hd[7] << 8;
	n = w * h * 4;
	if (!w || !h || w > 256 || h > 256 || !(px = malloc(n))) {
		fclose(f);
		return NULL;
	}
	if (fread(px, 1, n, f) != n) {
		fclose(f);
		free(px);
		return NULL;
	}
	fclose(f);
	for (i = 0; i < n; i += 4) {	/* (weighted by alpha: the corners do not count) */
		b += px[i] * px[i + 3];
		g += px[i + 1] * px[i + 3];
		r += px[i + 2] * px[i + 3];
		a += px[i + 3];
	}
	ic = &app_icons[app_nicons++];
	memset(ic, 0, sizeof(*ic));
	snprintf(ic->path, sizeof(ic->path), "%s", path);
	ic->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	ic->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
	ic->dsc.header.w = w;
	ic->dsc.header.h = h;
	ic->dsc.header.stride = w * 4;
	ic->dsc.data_size = n;
	ic->dsc.data = px;
	ic->avg = a ? lv_color_make(r / a, g / a, b / a) : lv_color_black();
	return ic;
}

static void app_icons_free(void)
{
	for (int i = 0; i < app_nicons; i++) {
		lv_image_cache_drop(&app_icons[i].dsc);
		free((void *)app_icons[i].dsc.data);
	}
	app_nicons = 0;
}

static void app_tile(lv_obj_t *parent, const char *icon, const char *name,
		     lv_color_t bg, lv_color_t fg, lv_event_cb_t cb);

/*
 * A tile with the application's picture where the symbol badge is: a clear
 * 60 px box (its background colour the picture's, for the open animation's
 * card) with the picture in it, scaled down only if larger.
 */
static void app_tile_img(lv_obj_t *parent, struct app_icon *ic, const char *name,
			 lv_event_cb_t cb)
{
	lv_obj_t *t, *box, *im;
	uint32_t before = lv_obj_get_child_count(parent);
	int32_t side = LV_MAX(ic->dsc.header.w, ic->dsc.header.h);

	app_tile(parent, "", name, ic->avg, lv_color_white(), cb);
	if (lv_obj_get_child_count(parent) <= before)
		return;
	t = lv_obj_get_child(parent, before);
	box = lv_obj_get_child(t, 0);
	lv_obj_clean(box);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	im = lv_image_create(box);
	lv_image_set_src(im, &ic->dsc);
	if (side > 60)
		lv_image_set_scale(im, 256 * 60 / side);
	lv_obj_center(im);
}

/*
 * Icon shadows, over a wallpaper only (Material 3 shows depth by tone on a
 * plain background): one soft shadow of the 60 px badge (radius 20), made
 * once by blurring its shape (three box passes, ~ a gaussian of 5 px),
 * 3 px down. Each tile draws it under its badge from its own draw event,
 * so it costs a small image blend when a page is drawn, nothing a frame
 * while pages slide (it is in their pictures). Darker over a light picture,
 * lighter over a dark one, where it would not show anyway (mean luma under
 * the grid, wallpaper.meta).
 */
#define SHADOW_R	10	/* blur reach around the badge */
#define SHADOW_DY	3

static lv_draw_buf_t *icon_shadow;

static void blur_box(uint8_t *a, uint8_t *tmp, int w, int h, int r)
{
	int x, y, k;

	for (y = 0; y < h; y++) {		/* rows */
		int sum = 0;

		for (k = -r; k <= r; k++)
			sum += a[y * w + LV_CLAMP(0, k, w - 1)];
		for (x = 0; x < w; x++) {
			tmp[y * w + x] = (uint8_t)(sum / (2 * r + 1));
			sum += a[y * w + LV_MIN(x + r + 1, w - 1)] - a[y * w + LV_MAX(x - r, 0)];
		}
	}
	for (x = 0; x < w; x++) {		/* columns */
		int sum = 0;

		for (k = -r; k <= r; k++)
			sum += tmp[LV_CLAMP(0, k, h - 1) * w + x];
		for (y = 0; y < h; y++) {
			a[y * w + x] = (uint8_t)(sum / (2 * r + 1));
			sum += tmp[LV_MIN(y + r + 1, h - 1) * w + x] - tmp[LV_MAX(y - r, 0) * w + x];
		}
	}
}

static lv_draw_buf_t *icon_shadow_make(void)
{
	const int side = 60, rad = 20, n = side + 2 * SHADOW_R;
	uint8_t *a = calloc(n * n, 1), *tmp = malloc(n * n);
	int x, y, i, peak = 40 + 70 * LV_CLAMP(0, wp.luma[WP_HOME], 255) / 255;	/* 16..43 % */
	lv_draw_buf_t *b = NULL;

	if (!a || !tmp)
		goto out;
	for (y = 0; y < side; y++)		/* the badge's shape (anti-aliased corners) */
		for (x = 0; x < side; x++) {
			float dx = LV_MAX(0, LV_MAX(rad - x - 0.5f, x + 0.5f - (side - rad))),
			      dy = LV_MAX(0, LV_MAX(rad - y - 0.5f, y + 0.5f - (side - rad)));
			float d = sqrtf(dx * dx + dy * dy) - rad;

			a[(y + SHADOW_R) * n + x + SHADOW_R] = d <= -0.5f ? 255 : d >= 0.5f ? 0 :
				(uint8_t)(255 * (0.5f - d));
		}
	for (i = 0; i < 3; i++)
		blur_box(a, tmp, n, n, 3);
	b = lv_draw_buf_create(n, n, LV_COLOR_FORMAT_ARGB8888, 0);
	if (!b)
		goto out;
	for (y = 0; y < n; y++) {
		uint32_t *row = (uint32_t *)(b->data + (size_t)y * b->header.stride);

		for (x = 0; x < n; x++)
			row[x] = (uint32_t)(a[y * n + x] * peak / 255) << 24;	/* black */
	}
	lv_draw_buf_flush_cache(b, NULL);
out:
	free(a);
	free(tmp);
	return b;
}

static void tile_shadow_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_current_target(e), *badge;
	lv_draw_image_dsc_t d;
	lv_area_t a;

	if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
		lv_event_set_ext_draw_size(e, SHADOW_R);
		return;
	}
	if (!icon_shadow || !(badge = lv_obj_get_child(t, 0)))
		return;
	lv_obj_get_coords(badge, &a);
	lv_area_set(&a, a.x1 - SHADOW_R, a.y1 - SHADOW_R + SHADOW_DY,
		    a.x1 - SHADOW_R + icon_shadow->header.w - 1,
		    a.y1 - SHADOW_R + SHADOW_DY + icon_shadow->header.h - 1);
	lv_draw_image_dsc_init(&d);
	d.src = icon_shadow;
	lv_draw_image(lv_event_get_layer(e), &d, &a);
}

static void tile_name_free(lv_event_t *e)
{
	free(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

static void app_tile(lv_obj_t *parent, const char *icon, const char *name,
		     lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
	lv_obj_t *t = lv_obj_create(parent);

	plain(t);
	lv_obj_set_size(t, 76, 96);
	lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(t, 0, 0);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(t, 8, 0);
	lv_obj_t *b = m3_badge(t, icon, bg, fg, 60);

	lv_obj_set_style_radius(b, 20, 0);	/* squircle-ish */
	lv_obj_set_style_transform_scale(t, 240, LV_STATE_PRESSED);
	{
		/*
		 * The press shows after 150 ms, as iOS hands a touch to the
		 * icons only then (UIScrollView delaysContentTouches): a swipe
		 * of the pages (10 px) takes it first and no icon flinches.
		 */
		static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X,
							  LV_STYLE_TRANSFORM_SCALE_Y, 0 };
		static lv_style_transition_dsc_t late;
		static bool init;

		if (!init) {
			lv_style_transition_dsc_init(&late, props, lv_anim_path_step, 1, 150, NULL);
			init = true;
		}
		lv_obj_set_style_transition(t, &late, LV_STATE_PRESSED);
	}
	if (home_wp) {
		if (!icon_shadow)
			icon_shadow = icon_shadow_make();
		lv_obj_add_event_cb(t, tile_shadow_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
		lv_obj_add_event_cb(t, tile_shadow_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
		lv_obj_refresh_ext_draw_size(t);
	}
	{
		/* a long name ends in an ellipsis, as on iOS */
		lv_obj_t *l = m3_label(t, name, &lv_font_montserrat_14, home_wp ? WP_TEXT : M.on_surface);

		lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
		lv_obj_set_width(l, 74);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	}
	lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
	/* its name, kept (the label's own text may be cut to "..." at any time) */
	lv_obj_set_user_data(t, strdup(name));
	lv_obj_add_event_cb(t, tile_name_free, LV_EVENT_DELETE, NULL);
}

static lv_obj_t *home_bright, *shade, *scrim, *qs_settings_btn;
static bool shade_open;

/* Screen back on: bring the clock up to date now, not at the next run. */
/*
 * The clock set (Date & Time, chrony's first step after the network came
 * up): the shown time follows at once, not at the minute timer's next run.
 * A timerfd armed years ahead with TFD_TIMER_CANCEL_ON_SET turns readable
 * only then: nothing wakes in between.
 */
static int clockset_fd = -1;

static void clockset_arm(void)
{
	struct itimerspec it = { .it_value = { .tv_sec = time(NULL) + 10L * 365 * 86400 } };

	if (timerfd_settime(clockset_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, NULL) &&
	    errno != ECANCELED)
		fprintf(stderr, "n72-ui: clock-set timer: %s\n", strerror(errno));
}

static void clockset_ev(int fd)
{
	uint64_t n;

	if (read(fd, &n, sizeof(n)) < 0 && errno != ECANCELED && errno != EAGAIN)
		return;
	clockset_arm();
	if (getenv("N72_STATUS_LOG"))
		fprintf(stderr, "n72-ui: status event: clock set\n");
	if (home_timer)
		lv_timer_ready(home_timer);	/* (dark: paused, the wake runs it) */
}

static void clockset_init(void)
{
	clockset_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	if (clockset_fd < 0)
		return;
	clockset_arm();
	ui_watch(clockset_fd, clockset_ev);
}

static void home_wake(void)
{
	if (home_timer)
		lv_timer_ready(home_timer);
	bt_auto_kick();
	lock_wake();
}

static void home_bright_cb(lv_event_t *e)
{
	backlight_set(lv_slider_get_value(lv_event_get_target(e)));
	conf.bright = lv_slider_get_value(lv_event_get_target(e));
	conf_save_later();
	shade_changed(home_bright);
}

/* ------------------------------------------------- quick settings shade */

/* 312 upright (two tiles a row, two rows), 240 turned (three in one row) */
static int32_t shade_h = 312;
#define SHADE_H shade_h

static void shade_closed(void)
{
	lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	/* opened over a page it was on the top layer: home's again */
	if (app_pid <= 0 && lv_obj_get_parent(shade) != home_scr) {
		lv_obj_set_parent(scrim, home_scr);
		lv_obj_set_parent(shade, home_scr);
	}
}

/* Over a page the shade comes down on the top layer, as over applications. */
static void shade_to_top(void)
{
	if (lv_obj_get_parent(shade) == lv_layer_top())
		return;
	lv_obj_set_parent(scrim, lv_layer_top());
	lv_obj_set_parent(shade, lv_layer_top());
}

/*
 * Open or close the shade. Animated: a picture of home (with the scrim,
 * without the shade) and a picture of the shade, moved by the GPU.
 */
static bool sd_animate(bool open);

static void shade_set(bool open, bool anim)
{
	lv_draw_buf_t *bg, *fg;

	if (!shade || open == shade_open)
		return;
	tr_finish();
	if (open && on_page())
		shade_to_top();
	shade_open = open;
	if (open) {
		lv_slider_set_value(home_bright, backlight_get(), LV_ANIM_OFF);
		home_wake();
	}
	lv_obj_remove_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	if (anim && sd_animate(open))
		return;
	if (anim && app_scr && lv_screen_active() == app_scr) {
		app_shade_slide(open);	/* no pictures to move: the application shows */
		return;
	}
	if (anim && lv_screen_active() == home_scr) {
		lv_obj_set_y(shade, -SHADE_H);
		lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
		bg = snap(home_scr, LV_COLOR_FORMAT_XRGB8888);
		lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_y(shade, 0);
		fg = snap(shade, LV_COLOR_FORMAT_ARGB8888);
		lv_obj_set_y(shade, open ? 0 : -SHADE_H);
		if (tr_start(bg, fg, 2, home_scr, NULL, open ? -SHADE_H : 0, open ? 0 : -SHADE_H)) {
			if (!open)
				tr.done = shade_closed;
			return;
		}
	}
	lv_obj_set_y(shade, open ? 0 : -SHADE_H);
	if (!open)
		shade_closed();
}

/* Swipe down anywhere on home opens the shade, swipe up closes it. */
static void home_gesture_cb(lv_event_t *e)
{
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());

	LV_UNUSED(e);
	if (d == LV_DIR_BOTTOM)
		shade_set(true, true);
	else if (d == LV_DIR_TOP)
		shade_set(false, true);
}

static void shade_tap_cb(lv_event_t *e)
{
	shade_set(lv_event_get_user_data(e) != NULL, true);
}

/*
 * The tiles are switches: Wi-Fi disconnects or reconnects (wpa_supplicant
 * keeps the network; the radio stays up), Bluetooth powers the controller
 * off or on as its page does. The tile changes at once; the real state
 * follows from the status read a moment later.
 */
static void bt_set(bool on);

static void wifi_page_refresh(void);

static void wifi_set_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	wifi_want = -1;		/* the card is there or gone now (sysfs) */
	status_refresh(false);
	net_ui_sync();
	wifi_page_refresh();
}

/* Wi-Fi on or off from any switch; a second one waits for the first. */
static void wifi_set(bool on)
{
	if (wifi_want >= 0 || !have_tool("n72-wifi")) {
		net_ui_sync();		/* (puts a switch that moved back) */
		return;
	}
	if (conf.wifi != on) {
		conf.wifi = on;
		conf_save();
	}
	if (on == wifi_powered())
		return;
	wifi_want = on;
	if (!on)
		wifi_up = false;
	net_ui_sync();
	/* (on: the card's power-up, firmware and join take a few seconds) */
	job_run(on ? "n72-wifi on; sleep 4" : "n72-wifi off", NULL, wifi_set_done, NULL);
}

static void qs_open_wifi(lv_event_t *e)
{
	LV_UNUSED(e);
	wifi_set(!wifi_shown_on());
}

static void qs_open_bt(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!have_tool("bluetoothctl"))
		return;
	bt_set(!bt_shown_on());
}

static void page_open_from(lv_obj_t *tile, void (*open)(lv_event_t *));
static lv_obj_t *settings_tile;

static void qs_to_page(void (*open)(lv_event_t *));

/* The gear at the top right of the shade: the shade goes up off Settings. */
static void qs_settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	qs_to_page(open_settings);
}

static lv_obj_t *qs_rot, *qs_rot_sub;
static void rot_conf_changed(void);

static int ui_rot;			/* the screen's turn now */

/* The Auto-rotate tile: lit while on; below, the way the screen is now. */
static void qs_rot_sync(void)
{
	if (!qs_rot)
		return;
	qs_style(qs_rot, conf.autorot == ROT_AUTO);
	label_set_if_changed(qs_rot_sub, ui_rot ? "Landscape" : "Portrait");
	shade_changed(qs_rot);
}

/* A tap: on, or off held the way it is now. */
static void qs_rot_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	conf.autorot = conf.autorot == ROT_AUTO ? ROT_LOCKED : ROT_AUTO;
	if (conf.autorot == ROT_LOCKED)
		conf.rotlock = ui_rot;
	conf_save();
	qs_rot_sync();
	rot_conf_changed();
}

static void build_shade(void)
{
	lv_obj_t *row, *h;

	scrim = lv_obj_create(home_scr);
	plain(scrim);
	lv_obj_set_size(scrim, W, H);
	lv_obj_set_style_radius(scrim, 0, 0);
	lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
	lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(scrim, shade_tap_cb, LV_EVENT_CLICKED, NULL);

	shade = lv_obj_create(home_scr);
	plain(shade);
	lv_obj_set_size(shade, W, SHADE_H);
	lv_obj_set_pos(shade, 0, -SHADE_H);
	lv_obj_set_style_bg_color(shade, M.surface_c, 0);
	lv_obj_set_style_bg_opa(shade, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(shade, 28, 0);
	lv_obj_set_style_pad_all(shade, 0, 0);
	lv_obj_remove_flag(shade, LV_OBJ_FLAG_SCROLLABLE);
	/* square top corners: a strip over the rounded ones */
	h = lv_obj_create(shade);
	plain(h);
	lv_obj_set_size(h, W, 30);
	lv_obj_set_style_radius(h, 0, 0);
	lv_obj_set_style_bg_color(h, M.surface_c, 0);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	clock_lbl = m3_label(shade, "", &lv_font_montserrat_48, M.on_surface);
	lv_obj_align(clock_lbl, LV_ALIGN_TOP_LEFT, STATUS_PAD + 2, 24);	/* (+ the glyphs' side bearing) */
	{
		/* Settings: a round tonal button at the top right */
		lv_obj_t *b = lv_button_create(shade);

		plain(b);
		lv_obj_set_size(b, 48, 48);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, M.surface_c_highest, 0);
		lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -16, 30);
		lv_obj_add_event_cb(b, qs_settings_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_center(m3_label(b, LV_SYMBOL_SETTINGS, &lv_font_montserrat_20, M.on_surface));
		qs_settings_btn = b;
	}
	date_lbl = m3_label(shade, "", &lv_font_montserrat_16, M.primary);
	lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, STATUS_PAD + 4, 84);

	/* the tiles: a grid, QS_COLS a row, 8 apart */
	{
		int rows = (3 + QS_COLS - 1) / QS_COLS;
		int32_t th = rows * 64 + (rows - 1) * 8;

		row = lv_obj_create(shade);
		plain(row);
		lv_obj_set_size(row, W - 2 * STATUS_PAD, th);
		lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 128);
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_set_style_pad_all(row, 0, 0);
		lv_obj_set_style_pad_column(row, 8, 0);
		lv_obj_set_style_pad_row(row, 8, 0);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		qs_wifi = qs_tile(row, LV_SYMBOL_WIFI, "Wi-Fi", &qs_wifi_sub, qs_open_wifi);
		qs_bt = qs_tile(row, LV_SYMBOL_BLUETOOTH, "Bluetooth", &qs_bt_sub, qs_open_bt);
		qs_rot = qs_tile(row, LV_SYMBOL_REFRESH, "Auto-rotate", &qs_rot_sub, qs_rot_cb);
		qs_rot_sync();
		/* the slider 12 below; the handle 52 below it */
		shade_h = 128 + th + 12 + 56 + 52;
		lv_obj_set_height(shade, shade_h);
		lv_obj_set_y(shade, -shade_h);
	}

	row = lv_obj_create(shade);
	plain(row);
	lv_obj_set_size(row, W - 2 * STATUS_PAD, 56);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, shade_h - 52 - 56);
	lv_obj_set_style_bg_color(row, M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(row, 18, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);
	m3_label(row, LV_SYMBOL_IMAGE, &lv_font_montserrat_16, M.on_surface_v);
	home_bright = row_slider(row, true);
	lv_slider_set_range(home_bright, 5, 100);
	lv_slider_set_value(home_bright, backlight_get(), LV_ANIM_OFF);
	lv_obj_add_event_cb(home_bright, home_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	/* a drag on the slider is not a swipe */
	lv_obj_remove_flag(home_bright, LV_OBJ_FLAG_GESTURE_BUBBLE);

	/* drag handle */
	h = lv_obj_create(shade);
	plain(h);
	lv_obj_set_size(h, 36, 4);
	lv_obj_set_style_radius(h, 2, 0);
	lv_obj_set_style_bg_color(h, M.outline, 0);
	lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -14);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
}

/* ------------------------------------------------------- applications */

/*
 * An application is a program of its own that draws on the display's
 * layer 0: as the master of KMS device n72-clcd, or through the frame
 * buffer. The launcher has layer 1 above it (mbx_gpu.c, kms_open) and,
 * while the application runs, renders with alpha (mbx_gpu_set_transparent)
 * a screen with nothing on it. So the application never needs the screen
 * handed to it, and whatever the launcher shows then, the volume pop-up
 * for one, lies over it with its own edges and shadow.
 *
 * Home ends the application; so does its own exit. The launcher holds
 * the touch screen and passes the application the touches that are not
 * its own through a virtual one (main.c, touch router), named to it in
 * N72_TOUCH.
 *
 * Manifests: APPS_DIR/<x>.app with the lines "name=<title>" and
 * "exec=<shell command>"; each becomes a tile on the home screen. Also
 * icon=<symbol name>, color=#rrggbb, or image=<path of a .n72i picture>
 * (app_icon_load). The tiles follow the folder (apps_watch).
 */
/* (tests: N72_APPS_DIR, another folder of manifests) */
#define APPS_DIR (getenv("N72_APPS_DIR") ? getenv("N72_APPS_DIR") : "/usr/share/n72-ui/apps")

static lv_timer_t *app_timer;

/*
 * Over an application the launcher keeps the top edge. A pull down from it
 * slides in a status bar (time, Wi-Fi, Bluetooth, battery) that goes away
 * again by itself after APP_BAR_MS; a second pull, or a tap on the bar,
 * brings the quick settings shade down over the application. A push up
 * closes the shade, and a push up on the bar hides the bar.
 *
 * The shade and its scrim move to the top layer while an application runs
 * (they live on the home screen otherwise). The touch router (main.c) asks
 * ui_wants_touch() where each touch begins and gives the launcher those,
 * the application all the others.
 */
#define APP_BAR_H	40
#define APP_EDGE_H	24	/* the strip at the top that pulls the bar */
#define APP_BAR_MS	3000
#define APP_BAR_SLIDE_MS 350	/* iOS's status bar: 350 ms ease-in-out, as a picture */

static lv_obj_t *app_bar, *app_bar_time, *app_bar_batt;
static bool app_bar_shown;
static lv_timer_t *app_bar_timer;

static void app_y(void *obj, int32_t v)
{
	lv_obj_set_y(obj, v);
}

static void app_hide_done(lv_anim_t *a)
{
	lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void app_shade_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	shade_closed();
}

static void app_slide(lv_obj_t *obj, int32_t to, lv_anim_completed_cb_t done)
{
	lv_anim_t a;

	lv_anim_delete(obj, app_y);
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, app_y);
	lv_anim_set_values(&a, lv_obj_get_style_y(obj, LV_PART_MAIN), to);
	lv_anim_set_duration(&a, 200);
	lv_anim_set_path_cb(&a, ios_path_ease_out);
	if (done)
		lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

static void app_shade_slide(bool open)
{
	app_slide(shade, open ? 0 : -SHADE_H, open ? NULL : app_shade_done);
}

static void app_bar_set(bool show);

/*
 * What the bar shows, for its kept picture (psl_start_sig): it is taken
 * again only when this changes (the minute, the battery, the icons).
 */
static void app_bar_sig(char *buf, size_t len)
{
	snprintf(buf, len, "%s|%s|%d%d|%d", app_bar_time ? lv_label_get_text(app_bar_time) : "",
		 app_bar_batt ? lv_label_get_text(app_bar_batt) : "",
		 app_bar_wifi && !lv_obj_has_flag(app_bar_wifi, LV_OBJ_FLAG_HIDDEN),
		 app_bar_bt && !lv_obj_has_flag(app_bar_bt, LV_OBJ_FLAG_HIDDEN), (int)W);
}

static void app_bar_expire(lv_timer_t *t)
{
	LV_UNUSED(t);
	app_bar_timer = NULL;
	if (shade_open) {
		app_bar_set(true);	/* again once the shade is closed */
		return;
	}
	app_bar_set(false);
}

static void app_bar_set(bool show)
{
	if (!app_bar)
		return;
	if (app_bar_timer) {
		lv_timer_delete(app_bar_timer);
		app_bar_timer = NULL;
	}
	if (show) {
		app_bar_timer = lv_timer_create(app_bar_expire, APP_BAR_MS, NULL);
		lv_timer_set_repeat_count(app_bar_timer, 1);
		if (app_bar_shown)
			return;
		home_update(NULL);
		if (!psl_start_sig(app_bar, 0, APP_BAR_SLIDE_MS, ios_path_ease_in_out, false, app_bar_sig)) {
			lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
			app_slide(app_bar, 0, NULL);
		}
	} else {
		if (!app_bar_shown)
			return;
		if (!psl_start_sig(app_bar, -APP_BAR_H, APP_BAR_SLIDE_MS, ios_path_ease_in_out, true, app_bar_sig))
			app_slide(app_bar, -APP_BAR_H, app_hide_done);
	}
	app_bar_shown = show;
}

/*
 * The bar is only 40 px high, less than a gesture (50 px), so it follows
 * the finger itself: up 12 px hides it, down 20 px brings the shade, and a
 * touch that went neither way is a tap (the shade too).
 */
static int32_t app_bar_y0;
static bool app_bar_dragged;

static void app_bar_touch_cb(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	lv_indev_t *in = lv_indev_active();
	lv_point_t p;

	if (!in)
		return;
	lv_indev_get_point(in, &p);
	if (c == LV_EVENT_PRESSED) {
		app_bar_y0 = p.y;
		app_bar_dragged = false;
		app_bar_set(true);	/* touched: the timeout starts again */
	} else if (c == LV_EVENT_PRESSING && !app_bar_dragged) {
		if (p.y - app_bar_y0 <= -12) {
			app_bar_dragged = true;
			app_bar_set(false);
		} else if (p.y - app_bar_y0 >= 20) {
			app_bar_dragged = true;
			shade_set(true, true);
		}
	} else if (c == LV_EVENT_CLICKED && !app_bar_dragged) {
		shade_set(true, true);
	}
}

/* Pulls and pushes over an application (on its screen and the top layer). */
static void app_gesture_cb(lv_event_t *e)
{
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());

	LV_UNUSED(e);
	if (app_pid <= 0 && !on_page())
		return;
	if (d == LV_DIR_BOTTOM) {
		if (!app_bar_shown)
			app_bar_set(true);
		else
			shade_set(true, true);
	} else if (d == LV_DIR_TOP) {
		if (shade_open) {
			shade_set(false, true);
			app_bar_set(true);	/* the timeout starts again */
		} else {
			app_bar_set(false);
		}
	}
}

static void app_bar_kbd_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	app_bar_set(false);
	kbd_toggle_app();
}

static void app_bar_build(void)
{
	if (app_bar)
		return;
	app_bar = lv_obj_create(lv_layer_top());
	plain(app_bar);
	lv_obj_set_size(app_bar, W, APP_BAR_H);
	lv_obj_set_pos(app_bar, 0, -APP_BAR_H);
	lv_obj_set_style_bg_color(app_bar, M.surface_c, 0);
	lv_obj_set_style_bg_opa(app_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(app_bar, 0, 0);
	lv_obj_set_style_shadow_width(app_bar, 12, 0);
	lv_obj_set_style_shadow_offset_y(app_bar, 2, 0);
	lv_obj_set_style_shadow_opa(app_bar, LV_OPA_30, 0);
	lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* its own */
	status_row(app_bar, &app_bar_time, &app_bar_wifi, &app_bar_bt, &app_bar_batt);
	{	/* the keyboard, for an application that does not ask for it */
		lv_obj_t *b = lv_obj_create(app_bar);

		plain(b);
		lv_obj_set_size(b, 40, 32);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, M.surface_c_highest, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(b, M.secondary_c, LV_STATE_PRESSED);
		lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
		lv_obj_center(m3_label(b, LV_SYMBOL_KEYBOARD, &lv_font_montserrat_16, M.on_surface));
		lv_obj_add_event_cb(b, app_bar_kbd_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_move_to_index(b, 2);	/* after the time and the spacer */
	}
	net_ui_sync();
	lv_obj_add_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
	/* gestures on the bar, the shade and the scrim bubble up to here */
	lv_obj_add_event_cb(lv_layer_top(), app_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* The bar shows what the home screen's bar shows (home_update). */
static void app_bar_update(void)
{
	if (!app_bar)
		return;
	label_set_if_changed(app_bar_time, lv_label_get_text(status_time));
	label_set_if_changed(app_bar_batt, lv_label_get_text(batt_lbl));
	/* showing as its picture: the bar itself if that is old now; hidden:
	 * the picture for the next slide, while nothing moves */
	psl_check(app_bar);
	/* (only over the application: elsewhere the bar is not needed) */
	if (!app_bar_shown && lv_screen_active() == app_scr && !lv_anim_count_running() && !(touch_indev &&
	    lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		psl_warm(app_bar, app_bar_sig);
}

static bool in_obj(lv_obj_t *o, int x, int y)
{
	lv_area_t a;

	if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
		return false;
	lv_obj_get_coords(o, &a);
	return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

#include "kbd.c"		/* the system keyboard */
#include "weather.c"	/* the weather: Open-Meteo, on the home card and the lock screen */

/* Does a touch that begins at (x, y) belong to the launcher? */
static bool ui_wants_touch(int x, int y)
{
	if (locked || app_pid <= 0 || lv_screen_active() != app_scr)
		return true;
	if (kbd_wants(x, y))
		return true;
	if (app_is_video())
		return true;		/* the player has no touch input: its controls */
	if (shade_open || y < APP_EDGE_H)
		return true;
	if (app_bar_shown && y < APP_BAR_H)
		return true;
	return in_obj(vol_hud, x, y);
}

/*
 * The shade follows the finger. The touch router (main.c) offers every
 * launcher touch to ui_drag(); once a vertical drag passes SD_SLOP in a
 * place where the shade can move, the drag is the launcher's: LVGL loses
 * the press, and the shade sits under the finger until the lift, then
 * settles open or closed by where it is and how fast it was moving.
 *
 * On the home screen the frames move two pictures (home, and the shade)
 * with a scrim between them, as the shade's animation does: GPU copies.
 * Over an application the shade and scrim themselves move (the launcher
 * draws with the CPU there anyway).
 */
#define SD_SLOP 12

static struct {
	int8_t cand;		/* this touch may start a drag: +1 down (open), -1 up */
	bool on, pics;
	int px, py;		/* where the finger came down */
	int32_t base, y;	/* the shade's y at the start, and now */
	int last_y;
	uint32_t last_t;
	float v;		/* px per ms, smoothed */
	lv_obj_t *scr, *bgclip, *bg, *scrim, *fg, *back;
	bool fg_cached;		/* sd.sfg is shade_pic (not freed here) */
	lv_draw_buf_t *home_pic;	/* home without the shade, kept while it is open */
	unsigned long home_pic_ver;
	long home_pic_min;
	bool to_page;		/* closing onto a page (Settings from the shade) */
	lv_draw_buf_t *sbg, *sfg;
} sd;

static void sd_free_pics(void)
{
	if (sd.scr) {
		lv_obj_delete(sd.scr);
		sd.scr = NULL;
	}
	tr_free(&sd.sbg);
	if (sd.fg_cached)
		sd.sfg = NULL;		/* (shade_pic's) */
	else
		tr_free(&sd.sfg);
	sd.fg_cached = false;
}

/*
 * The shade's picture, kept: taking it (rendering every widget of the
 * shade) cost ~40-80 ms at each drag's start. Retaken when something on
 * the shade changes (shade_changed: labels, tiles, the slider) or the
 * minute does; when idle, before anyone needs it (shade_pic_warm).
 */
static lv_draw_buf_t *shade_pic;
static bool shade_pic_dirty = true;
static long shade_pic_min;

static lv_timer_t *shade_warm_timer;

static void shade_changed(lv_obj_t *o)
{
	for (; o; o = lv_obj_get_parent(o))
		if (o == shade) {
			shade_pic_dirty = true;
			/* the warmer, resting till the next minute, looks again soon */
			if (shade_warm_timer && shade_warm_timer->period != 1000) {
				lv_timer_set_period(shade_warm_timer, 1000);
				lv_timer_reset(shade_warm_timer);
			}
			return;
		}
}

static lv_draw_buf_t *shade_pic_get(void)
{
	int32_t y;

	if (shade_pic && !shade_pic_dirty && shade_pic_min == bp_minute())
		return shade_pic;
	if (shade_pic && sd.sfg == shade_pic)
		return shade_pic;	/* (in use right now) */
	tr_free(&shade_pic);
	/*
	 * Where it was set to be, not where it was last laid out: on a screen
	 * that is not showing (home, while a page is up) nothing is laid out,
	 * and the coordinates still say "open" after the shade was closed.
	 * Putting it back there left it open for good.
	 */
	y = lv_obj_get_style_y(shade, LV_PART_MAIN);
	lv_obj_set_y(shade, 0);
	shade_pic = snap(shade, LV_COLOR_FORMAT_ARGB8888);
	lv_obj_set_y(shade, y);
	shade_pic_dirty = false;
	shade_pic_min = bp_minute();
	return shade_pic;
}

static lv_opa_t sd_scrim_opa(int32_t y)
{
	return (lv_opa_t)(LV_OPA_50 * (y + SHADE_H) / SHADE_H);
}

static void sd_set(int32_t y)
{
	sd.y = LV_CLAMP(-SHADE_H, y, 0);
	if (sd.pics) {
		/* the scrim only where the shade does not cover (its corners
		 * have the radius): less to blend, and to redraw */
		int32_t top = LV_MAX(0, sd.y + SHADE_H - 28);

		lv_obj_set_y(sd.fg, sd.y);
		/* above top the shade hides home: draw home only below it */
		if (sd.bgclip) {
			lv_obj_set_pos(sd.bgclip, 0, top);
			lv_obj_set_height(sd.bgclip, H - top);
			lv_obj_set_y(sd.bg, -top);
		}
		lv_obj_set_pos(sd.scrim, 0, top);
		lv_obj_set_height(sd.scrim, H - top);
		lv_obj_set_style_bg_opa(sd.scrim, sd_scrim_opa(sd.y), 0);
	} else {
		/*
		 * Over an application (the launcher draws with alpha, on the
		 * CPU): the scrim at one opacity, so that a frame redraws only
		 * the strip the shade moves through. Following the shade, it
		 * changed the whole screen every frame: drawn and premultiplied
		 * whole, 18-23 ms a frame (31-38 fps) over a video.
		 */
		lv_obj_set_y(shade, sd.y);
		if (lv_obj_get_style_bg_opa(scrim, 0) != LV_OPA_50)
			lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);	/* (as settled) */
	}
}

static void sd_anim_exec(void *var, int32_t v)
{
	LV_UNUSED(var);
	sd_set(v);
}

/* The shade has settled: the real objects take over from the pictures. */
static void sd_end(bool open)
{
	if (sd.pics) {
		lv_screen_load(sd.back);
		if (open && sd.back == home_scr && sd.sbg) {
			/* home under the open shade: the picture for its closing */
			tr_free(&sd.home_pic);
			sd.home_pic = sd.sbg;
			sd.home_pic_ver = home_ver;
			sd.home_pic_min = bp_minute();
			sd.sbg = NULL;
		}
		sd_free_pics();
		sd.pics = false;
		lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
		if (app_bar && app_bar_shown)
			lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
	shade_open = open;
	lv_obj_set_y(shade, open ? 0 : -SHADE_H);
	if (open) {
		lv_obj_remove_flag(scrim, LV_OBJ_FLAG_HIDDEN);
		lv_slider_set_value(home_bright, backlight_get(), LV_ANIM_OFF);
		home_wake();
	} else {
		shade_closed();
		if ((app_pid > 0 || on_page()) && !sd.to_page)
			app_bar_set(true);	/* the bar's timeout starts again */
	}
	sd.to_page = false;
}

static void sd_anim_done(lv_anim_t *a)
{
	sd_end(a->end_value == 0);
}

/*
 * The shade opened or closed by a tap or gesture, as a drag that lets go:
 * the pictures move (GPU on home, one picture over an application), not
 * the widgets of the shade. False: not here (the caller moves the shade).
 */
static bool sd_begin(bool touch);

static bool sd_animate(bool open)
{
	lv_anim_t a;

	if (sd.on || sd.scr ||
	    !(lv_screen_active() == home_scr || (app_scr && lv_screen_active() == app_scr) ||
	      on_page()))
		return false;
	shade_open = !open;	/* (sd_begin starts from where it is) */
	sd_begin(false);
	sd.on = false;
	if (!sd.scr) {
		shade_open = open;
		return false;
	}
	lv_anim_init(&a);
	lv_anim_set_var(&a, &sd);
	lv_anim_set_exec_cb(&a, sd_anim_exec);
	lv_anim_set_values(&a, sd.y, open ? 0 : -SHADE_H);
	lv_anim_set_duration(&a, 240);
	lv_anim_set_path_cb(&a, ios_path_ease_out);
	lv_anim_set_completed_cb(&a, sd_anim_done);
	lv_anim_start(&a);
	return true;
}

/* A settling shade is finished at once when a new touch comes. */
static void sd_settle_now(void)
{
	lv_anim_t *a = lv_anim_get(&sd, sd_anim_exec);

	if (a) {
		int32_t end = a->end_value;

		lv_anim_delete(&sd, sd_anim_exec);
		sd_end(end == 0);
	}
}

/* LVGL's press ends here without a click (the launcher's drag has it). */
static void ui_touch_lost(void)
{
	lv_obj_t *a = touch_indev ? touch_indev->pointer.act_obj : NULL;

	if (a) {
		/* no transition back: the picture taken next must show it plain */
		static const lv_style_prop_t none[] = { 0 };
		static lv_style_transition_dsc_t no_tr;

		lv_style_transition_dsc_init(&no_tr, none, NULL, 0, 0, NULL);
		lv_obj_set_style_transition(a, &no_tr, 0);
		lv_obj_remove_state(a, LV_STATE_PRESSED);
		lv_obj_remove_local_style_prop(a, LV_STYLE_TRANSITION, 0);
		lv_obj_send_event(a, LV_EVENT_PRESS_LOST, touch_indev);
	}
	lv_indev_reset(touch_indev, NULL);
}

static bool sd_begin(bool touch)
{
	bool home = lv_screen_active() == home_scr, page = on_page();
	lv_draw_buf_t *bg = NULL, *fg;

	if (touch)
		ui_touch_lost();
	tr_finish();
	if (page) {
		/* the page as it is, under the shade (which is on the top layer) */
		shade_to_top();
		bg = snap(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
	}
	sd.base = shade_open ? 0 : -SHADE_H;
	sd.pics = false;
	/*
	 * Pictures: the shade by itself, and (on home) home without shade or
	 * scrim. Over an application there is no picture below: the screen is
	 * transparent, and moving one picture is much less for the CPU than
	 * drawing the shade's widgets every frame.
	 */
	if (home) {
		if (!shade_open) {
			bg = grab_or_snap(home_scr);
		} else if (sd.home_pic && sd.home_pic_ver == home_ver &&
			   sd.home_pic_min == bp_minute()) {
			bg = sd.home_pic;	/* kept from the opening: home unchanged */
			sd.home_pic = NULL;
		} else {
			lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
			bg = snap(home_scr, LV_COLOR_FORMAT_XRGB8888);
			lv_obj_remove_flag(scrim, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (!page)
		tr_free(&sd.home_pic);
	fg = shade_pic_get();
	sd.fg_cached = fg != NULL;
	if (fg && !home && !page) {
		/*
		 * Over an application: a premultiplied copy, which the GPU
		 * copies into the transparent frame as it is (the straight
		 * one cost a CPU blend and a premultiply of its whole area
		 * every frame: 17-23 ms, 30-47 fps over a video).
		 */
		lv_draw_buf_t *pm = lv_draw_buf_dup(fg);

		if (pm) {
			mbx_gpu_premultiply_buf(pm);
			fg = pm;
			sd.fg_cached = false;	/* (freed with the drag) */
		}
	}
	if (fg && (bg || !home)) {
		sd.sbg = bg;
		sd.sfg = fg;
		sd.back = lv_screen_active();
		sd.scr = lv_obj_create(NULL);
		/* no background: the pictures cover everything (or nothing is
		 * wanted: the application shows) */
		lv_obj_remove_style_all(sd.scr);
		lv_obj_remove_flag(sd.scr, LV_OBJ_FLAG_SCROLLABLE);
		sd.bgclip = NULL;
		if (bg) {
			sd.bgclip = lv_obj_create(sd.scr);	/* clips the picture */
			lv_obj_remove_style_all(sd.bgclip);
			lv_obj_remove_flag(sd.bgclip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_width(sd.bgclip, W);
			sd.bg = lv_image_create(sd.bgclip);
			lv_image_set_src(sd.bg, bg);
		}
		sd.scrim = lv_obj_create(sd.scr);
		plain(sd.scrim);
		lv_obj_set_size(sd.scrim, W, H);
		lv_obj_set_style_radius(sd.scrim, 0, 0);
		lv_obj_set_style_bg_color(sd.scrim, lv_color_black(), 0);
		/* over an application the dimming waits for the shade to settle:
		 * blending it with the CPU every frame would halve the rate */
		if (!bg)
			lv_obj_add_flag(sd.scrim, LV_OBJ_FLAG_HIDDEN);
		if (app_bar)
			lv_obj_add_flag(app_bar, LV_OBJ_FLAG_HIDDEN);	/* not over the shade */
		sd.fg = lv_image_create(sd.scr);
		lv_image_set_src(sd.fg, fg);
		/* the real ones (on the top layer over an application) hide */
		lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
		lv_screen_load(sd.scr);
		sd.pics = true;
	} else {
		tr_free(&bg);
		sd.fg_cached = false;	/* (the cached one stays cached) */
	}
	if (!sd.pics)
		lv_obj_remove_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	sd.on = true;
	sd.v = 0;
	sd_set(sd.base);
	return true;
}

static void sd_release(void)
{
	bool open;
	int32_t to, dist;
	lv_anim_t a;

	sd.on = false;
	if (sd.v > 0.4f)
		open = true;
	else if (sd.v < -0.4f)
		open = false;
	else
		open = sd.y > -SHADE_H / 2;
	to = open ? 0 : -SHADE_H;
	dist = LV_ABS(to - sd.y);
	if (!dist) {
		sd_end(open);
		return;
	}
	lv_anim_init(&a);
	lv_anim_set_var(&a, &sd);
	lv_anim_set_exec_cb(&a, sd_anim_exec);
	lv_anim_set_values(&a, sd.y, to);
	lv_anim_set_duration(&a, 60 + 180 * dist / SHADE_H);
	lv_anim_set_path_cb(&a, ios_path_ease_out);
	lv_anim_set_completed_cb(&a, sd_anim_done);
	lv_anim_start(&a);
}

/* ------------------------------------------------------------ lock */

/*
 * The lock screen: date and a large clock, a hint and a home indicator.
 * It goes up whenever the screen goes off (screen_set, screen_off_or_sleep),
 * so waking shows it at once. A swipe up unlocks: the lock screen's picture
 * follows the finger up, uncovering a picture of where you were (taken at
 * wake, while nobody can touch yet) under a scrim that clears as it goes.
 * Only the part not covered is drawn, so a frame is about one screen of
 * GPU copies. Over an application the launcher draws with the CPU and
 * alpha (nothing of it under the picture), as it does there anyway.
 */
#include "clock_font.h"

static lv_obj_t *lock_scr, *lock_time, *lock_date, *lock_batt;
static lv_obj_t *lock_from;		/* the screen the lock went over */
static lv_font_t *clock_font;

static struct {
	bool on;
	int py, last_y;
	uint32_t last_t;
	float v;
	int32_t y;			/* the lock picture's y, -H..0 */
	lv_obj_t *scr, *clip, *bg, *scrim, *fg;
	lv_draw_buf_t *sbg, *sfg;	/* where you were; the lock screen */
} ul;

static void lock_update(void)
{
	time_t now = time(NULL);
	struct tm tm;
	char buf[48];
	static const char *const days[] = { "Sunday", "Monday", "Tuesday",
		"Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September",
		"October", "November", "December" };

	if (!lock_scr)
		return;
	localtime_r(&now, &tm);
	snprintf(buf, sizeof(buf), "%d:%02d", tm.tm_hour, tm.tm_min);
	label_set_if_changed(lock_time, buf);
	if (tm.tm_year + 1900 < 2024)
		snprintf(buf, sizeof(buf), "Clock not set");
	else
		snprintf(buf, sizeof(buf), "%s, %d %s", days[tm.tm_wday], tm.tm_mday,
			 months[tm.tm_mon]);
	label_set_if_changed(lock_date, buf);
	battery_text(buf, sizeof(buf));
	label_set_if_changed(lock_batt, strstr(buf, LV_SYMBOL_CHARGE) ? buf : "");
}

static void lock_build(void)
{
	lv_obj_t *o;

	if (!clock_font)
		clock_font = lv_tiny_ttf_create_data(clock_ttf, clock_ttf_len, 88);
	lock_scr = lv_obj_create(NULL);
	m3_screen(lock_scr);
	/* a dark wash of the accent colour, top to bottom */
	lv_obj_set_style_bg_color(lock_scr, lv_color_mix(M.primary_c, lv_color_black(), 150), 0);
	lv_obj_set_style_bg_grad_color(lock_scr, lv_color_black(), 0);
	lv_obj_set_style_bg_grad_dir(lock_scr, LV_GRAD_DIR_VER, 0);
	wp_put(lock_scr, WP_LOCK);	/* (a picture, if one is set, over the gradient) */

	lock_date = m3_label(lock_scr, "", &lv_font_montserrat_20, lv_color_hex(0xe8e0f0));
	lv_obj_align(lock_date, LV_ALIGN_TOP_MID, 0, 64);
	lock_time = m3_label(lock_scr, "", clock_font ? clock_font : &lv_font_montserrat_48,
			     lv_color_white());
	lv_obj_align(lock_time, LV_ALIGN_TOP_MID, 0, 90);
	wx_lock_build(lock_scr, 196);	/* (hidden while there is none) */
	lock_batt = m3_label(lock_scr, "", &lv_font_montserrat_14, lv_color_hex(0xc8c0d0));
	lv_obj_align(lock_batt, LV_ALIGN_BOTTOM_MID, 0, -74);
	o = m3_label(lock_scr, "Swipe up to unlock", &lv_font_montserrat_14, lv_color_hex(0xb0a8b8));
	lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -36);
	o = lv_obj_create(lock_scr);	/* home indicator */
	plain(o);
	lv_obj_set_size(o, 120, 5);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_bg_color(o, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void ul_free(void)
{
	if (ul.scr) {
		lv_obj_delete(ul.scr);
		ul.scr = NULL;
	}
	tr_free(&ul.sbg);
	tr_free(&ul.sfg);
}

/* The screen goes off: the lock screen goes up (nothing is animated). */
static void ui_lock(void)
{
	lv_obj_t *cur;

	if (locked || !home_scr)
		return;
	kbd_hide_now(false);	/* (the lock screen has no field) */
	za_finish();
	sd_settle_now();
	tr_finish();
	dock_carry_stop();
	pg_abort();
	if (shade_open)
		sd_end(false);
	app_bar_set(false);
	if (!lock_scr)
		lock_build();
	cur = lv_screen_active();
	lock_from = cur == lock_scr ? home_scr : cur;
	lock_update();
	if (app_pid > 0)
		mbx_gpu_set_transparent(false);
	lv_screen_load(lock_scr);
	locked = true;
	lv_refr_now(NULL);
	{
		/* on the panel before the system sleeps: otherwise the old
		 * frame shows for a moment on waking */
		uint32_t stride;

		(void)mbx_gpu_front(&stride);
	}
}

/* Screen on: a fresh picture of where you were, for the swipe. */
static void lock_wake(void)
{
	if (!locked)
		return;
	lock_update();
	ul_free();
	if (lock_from && lock_from != app_scr && lv_obj_is_valid(lock_from))
		ul.sbg = snap(lock_from, LV_COLOR_FORMAT_XRGB8888);
}

static void ul_set(int32_t y)
{
	int32_t top;

	ul.y = LV_CLAMP(-H, y, 0);
	top = ul.y + H;
	lv_obj_set_y(ul.fg, ul.y);
	if (ul.clip) {
		/* where you were shows only below the lock screen's picture */
		lv_obj_set_pos(ul.clip, 0, top);
		lv_obj_set_height(ul.clip, H - top);
		lv_obj_set_y(ul.bg, -top);
		lv_obj_set_pos(ul.scrim, 0, top);
		lv_obj_set_height(ul.scrim, H - top);
		lv_obj_set_style_bg_opa(ul.scrim, (lv_opa_t)(LV_OPA_60 * top / H), 0);
	}
}

static void ul_exec(void *var, int32_t v)
{
	LV_UNUSED(var);
	ul_set(v);
}

static void ul_finish(bool open)
{
	if (open) {
		locked = false;
		rot_kick();	/* (unlocked: a turn may be due) */
		wp_apply_kick();
		lv_screen_load(lock_from && lv_obj_is_valid(lock_from) ? lock_from : home_scr);
		ul_free();
		if (lock_from == app_scr && app_pid > 0)
			mbx_gpu_set_transparent(true);
		else
			mbx_gpu_set_transparent(false);
		home_update(NULL);
	} else {
		lv_screen_load(lock_scr);
		lv_obj_delete(ul.scr);
		ul.scr = NULL;
		ul.clip = NULL;
		tr_free(&ul.sfg);	/* (where you were stays for the next try) */
		mbx_gpu_set_transparent(false);
	}
}

static void ul_done(lv_anim_t *a)
{
	ul_finish(a->end_value == -H);
}

static void ul_begin(void)
{
	bool over_app = lock_from == app_scr && app_pid > 0;

	ui_touch_lost();
	ul.sfg = grab_or_snap(lock_scr);
	if (!ul.sbg && !over_app && lock_from && lv_obj_is_valid(lock_from))
		ul.sbg = snap(lock_from, LV_COLOR_FORMAT_XRGB8888);
	ul.scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(ul.scr);
	lv_obj_remove_flag(ul.scr, LV_OBJ_FLAG_SCROLLABLE);
	ul.clip = NULL;
	if (!over_app && ul.sbg) {
		ul.clip = lv_obj_create(ul.scr);
		lv_obj_remove_style_all(ul.clip);
		lv_obj_remove_flag(ul.clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_width(ul.clip, W);
		ul.bg = lv_image_create(ul.clip);
		lv_image_set_src(ul.bg, ul.sbg);
		ul.scrim = lv_obj_create(ul.scr);
		plain(ul.scrim);
		lv_obj_set_width(ul.scrim, W);
		lv_obj_set_style_radius(ul.scrim, 0, 0);
		lv_obj_set_style_bg_color(ul.scrim, lv_color_black(), 0);
	}
	ul.fg = lv_image_create(ul.scr);
	if (ul.sfg)
		lv_image_set_src(ul.fg, ul.sfg);
	if (over_app)
		mbx_gpu_set_transparent(true);	/* the application shows below */
	lv_screen_load(ul.scr);
	ul.on = true;
	ul.v = 0;
	ul_set(0);
}

static void ul_release(void)
{
	bool open = ul.v < -0.5f || (ul.v < 0.3f && ul.y < -H / 4);
	int32_t to = open ? -H : 0, dist = LV_ABS(to - ul.y);
	lv_anim_t a;

	ul.on = false;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &ul);
	lv_anim_set_exec_cb(&a, ul_exec);
	lv_anim_set_values(&a, ul.y, to);
	lv_anim_set_duration(&a, LV_MAX(80, 300 * dist / H));
	/* back down: iOS's slide-to-unlock snap back (300 ms x the way, ease-in-out) */
	lv_anim_set_path_cb(&a, open ? ios_path_ease_out : ios_path_ease_in_out);
	lv_anim_set_completed_cb(&a, ul_done);
	lv_anim_start(&a);
}

/* A touch on the lock screen (from ui_drag). */
static bool ul_drag(int phase, int y)
{
	uint32_t now = lv_tick_get();

	if (phase == 0) {
		lv_anim_t *a = lv_anim_get(&ul, ul_exec);

		if (a) {		/* still settling: finish it */
			int32_t end = a->end_value;

			lv_anim_delete(&ul, ul_exec);
			ul_set(end);
			ul_finish(end == -H);
		}
		ul.py = ul.last_y = y;
		ul.last_t = now;
		return false;
	}
	if (!ul.on) {
		if (phase == 2 || ul.py - y <= SD_SLOP || lv_screen_active() != lock_scr)
			return false;
		ul_begin();
		ul.py -= SD_SLOP;
	}
	if (now != ul.last_t) {
		float v = (float)(y - ul.last_y) / (float)(now - ul.last_t);

		ul.v = ul.v * 0.5f + v * 0.5f;
		ul.last_y = y;
		ul.last_t = now;
	}
	if (phase == 2) {
		if (now - ul.last_t > 80)
			ul.v = 0;
		ul_release();
	} else {
		ul_set(y - ul.py);
	}
	return true;
}

/*
 * A launcher touch, from the router: phase 0 down, 1 move, 2 lift.
 * True once the drag is the shade's (the router then keeps it from LVGL).
 */
static bool dock_carry_touch(int phase, int x, int y);

static bool ui_drag(int phase, int x, int y)
{
	uint32_t now = lv_tick_get();

	if (locked || ul.scr)
		return ul_drag(phase, y);
	if (dock_carry_touch(phase, x, y))	/* an icon carried (dock.c): its touch */
		return true;
	if (phase == 0 && app_bar && y < APP_BAR_H)
		psl_touch(app_bar);	/* (the bar itself takes its touches) */
	if (phase == 0 && kbd.obj && kbd_wants(x, y))
		psl_touch(kbd.obj);
	if (pg_drag(phase, x, y))	/* sideways on home: the pages */
		return true;
	if (phase == 0) {
		bool home = lv_screen_active() == home_scr && !tr.scr;
		bool app = app_pid > 0 && lv_screen_active() == app_scr;
		bool page = on_page() && !tr.scr;

		sd_settle_now();
		sd.cand = 0;
		sd.px = x;
		sd.py = y;
		sd.last_y = y;
		sd.last_t = now;
		if (!shade || screen_off || (!home && !app && !page))
			return false;
		if (shade_open && !in_obj(home_bright, x, y))
			sd.cand = -1;
		else if (!shade_open && (home || (app_bar_shown && y < APP_BAR_H)))
			sd.cand = 1;
		else if (page && !shade_open && y < APP_EDGE_H)
			sd.cand = 2;	/* the page's top edge: brings the bar */
		return false;
	}
	if (!sd.on) {
		int dy = y - sd.py, dx = x - sd.px;

		if (!sd.cand || phase == 2)
			return false;
		if (sd.cand == 2) {
			if (LV_ABS(dx) > SD_SLOP && LV_ABS(dx) > LV_ABS(dy)) {
				sd.cand = 0;
				return false;
			}
			if (dy <= SD_SLOP)
				return false;
			sd.cand = 0;
			ui_touch_lost();	/* (not the page's scroll or tap) */
			app_bar_set(true);
			return true;
		}
		if (LV_ABS(dx) > SD_SLOP && LV_ABS(dx) > LV_ABS(dy)) {
			sd.cand = 0;	/* sideways: not the shade's */
			return false;
		}
		if (sd.cand * dy <= SD_SLOP)
			return false;
		sd_begin(true);
		sd.py += sd.cand * SD_SLOP;	/* no jump by the slop */
	}
	if (now != sd.last_t) {
		float v = (float)(y - sd.last_y) / (float)(now - sd.last_t);

		sd.v = sd.v * 0.5f + v * 0.5f;
		sd.last_y = y;
		sd.last_t = now;
	}
	if (phase == 2) {
		if (now - sd.last_t > 80)
			sd.v = 0;	/* held still before the lift */
		sd_release();
	} else {
		sd_set(sd.base + (y - sd.py));
	}
	return true;
}

/* The application is gone (or is being ended): back to the home screen. */
/*
 * Opening and closing animations. A card in the tile's colour, with its
 * symbol, grows from the icon to the whole screen over a picture of home
 * (GPU: the picture, a rounded fill) while the application starts below,
 * and clears to show it. Closing runs it backwards: the card comes up over
 * the application, then goes into its icon over home, and home's icon
 * shows through as it ends. The card over the application is drawn in the
 * launcher's transparent mode (one GPU fill a frame); the moves are XRGB,
 * on the GPU.
 *
 * The motion is a spring's, not a timed curve's (za_path_spring,
 * za_path_glide):
 * - Opening, the card is pulled to the screen by a critically damped
 *   spring: from rest, fastest after ~55 ms, 90 % of the way by ~210 ms,
 *   then a long soft landing. On the way its symbol dissolves (a glyph of
 *   fixed size on a growing card looks stiff) and its corners, a little
 *   rounder at first, square off as they near the screen's. It clears as
 *   soon as it has landed, fast first: the landing is the pause, there is
 *   no hold. (The application gets as long as before to start: the grow
 *   is longer by the hold that went.)
 * - Closing is shorter, as leaving should be. The card comes up at an
 *   even rate and leaves gently (a spring following a smoothed pull: no
 *   speed in the first frame, fastest after ~60 ms), its symbol back on
 *   the way; near the icon it melts into home's.
 * Nothing is drawn that was not before: a frame of a move is home's
 * picture, one rounded fill (its corners never larger than they were) and
 * the symbol, for less of the way.
 */
#define ZA_GROW_MS	380	/* spring, ZA_SPRING_K time constants long */
#define ZA_REVEAL_MS	180	/* the card clears: the application, the page */
#define ZA_COVER_MS	100	/* the card comes up over it */
#define ZA_SHRINK_MS	280	/* glide, ZA_GLIDE_K time constants long */
#define ZA_SPRING_K	7.0f
#define ZA_GLIDE_K	9.0f
#define ZA_SYM_FROM	384	/* the symbol dissolves between these (of 1024) */
#define ZA_SYM_TO	896
#define ZA_MELT		160	/* closing: the card melts into the icon below this */

static struct {
	int phase;		/* 0 idle, 1 grow, 3 clear, 4 cover, 5 shrink; a page's: 6 clear, 7 cover */
	lv_obj_t *scr, *bg, *card, *sym;
	lv_draw_buf_t *home;	/* home, as it was at the launch */
	long home_min;
	lv_obj_t *tile;		/* the tile (its badge gives place and colour) */
	lv_area_t icon;
	lv_color_t col;
	lv_timer_t *kill_timer;
	const char *cmd;	/* the application's command, until it starts */
	bool page;		/* the launcher's own page (Settings, Paint) */
	void (*page_open)(lv_event_t *);
	bool home_stale;	/* retake za.home once the card covers the screen */
	lv_obj_t *page_scr, *page_tile;
	lv_draw_buf_t *page_pic;
} za;

static void za_next(void);
static void app_home_ui(void);

/*
 * Layer 0 shows the console's frame buffer until the application puts up
 * its first frame, and the fade does not wait for that: the console, or
 * the last picture of an application that drew into it, flashed through.
 * Filled with the card's colour, a slow start just looks like the card.
 */
static void za_fill_layer0(lv_color_t c)
{
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	size_t len = (size_t)W * H * 4;
	uint32_t *p, v = 0xff000000u | lv_color_to_u32(c);

	pg_fb0_dirty();		/* (the home wallpaper is no longer there) */
	if (fd < 0)
		return;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p != MAP_FAILED) {
		for (size_t i = 0; i < len / 4; i++)
			p[i] = v;
		munmap(p, len);
	}
	close(fd);
}

/*
 * The paths. Both are the step response of a damped spring, cut where it
 * has all but arrived (@k time constants) and scaled to end on the value:
 * time runs on the animation's clock, so a late frame shows where the
 * card is by then, not the next step.
 */
static int32_t za_path(const lv_anim_t *a, float k, bool glide)
{
	float x, fx, fk;

	if (a->act_time <= 0)
		return a->start_value;
	if (a->act_time >= a->duration)
		return a->end_value;
	x = k * (float)a->act_time / (float)a->duration;
	if (glide) {	/* the pull itself comes on smoothly: no speed, no force at 0 */
		fx = 1.0f - (1.0f + x + 0.5f * x * x) * expf(-x);
		fk = 1.0f - (1.0f + k + 0.5f * k * k) * expf(-k);
	} else {	/* critically damped: no speed at 0, no overshoot */
		fx = 1.0f - (1.0f + x) * expf(-x);
		fk = 1.0f - (1.0f + k) * expf(-k);
	}
	return a->start_value + (int32_t)lroundf((float)(a->end_value - a->start_value) * fx / fk);
}

static int32_t za_path_spring(const lv_anim_t *a)
{
	return za_path(a, ZA_SPRING_K, false);
}

static int32_t za_path_glide(const lv_anim_t *a)
{
	return za_path(a, ZA_GLIDE_K, true);
}

static void za_rect(int32_t v)	/* 0 = the icon, 1024 = the screen */
{
	int32_t x1 = za.icon.x1 + (0 - za.icon.x1) * v / 1024;
	int32_t y1 = za.icon.y1 + (0 - za.icon.y1) * v / 1024;
	int32_t x2 = za.icon.x2 + (W - 1 - za.icon.x2) * v / 1024;
	int32_t y2 = za.icon.y2 + (H - 1 - za.icon.y2) * v / 1024;

	lv_obj_set_pos(za.card, x1, y1);
	lv_obj_set_size(za.card, x2 - x1 + 1, y2 - y1 + 1);
	/* the icon's 20, a card's 26 half way, the screen's square corners */
	lv_obj_set_style_radius(za.card, v < 512 ? 20 + 6 * v / 512 : 26 * (1024 - v) / 512, 0);
}

/* The symbol's opacity on a card @v of the way: gone well before the screen. */
static lv_opa_t za_sym_opa(int32_t v)
{
	if (v <= ZA_SYM_FROM)
		return LV_OPA_COVER;
	if (v >= ZA_SYM_TO)
		return LV_OPA_TRANSP;
	return (lv_opa_t)(255 * (ZA_SYM_TO - v) / (ZA_SYM_TO - ZA_SYM_FROM));
}

/*
 * The card's opacity as its fill's and its symbol's own, not the object's:
 * object opacity renders the card into a layer of its own and blends it,
 * all on the CPU in the launcher's transparent mode; a plain translucent
 * fill is one GPU fill (mbx_gpu.c, mbx_fill_straight).
 */
static void za_look(lv_opa_t card, lv_opa_t sym)
{
	if (lv_obj_get_style_bg_opa(za.card, 0) != card)
		lv_obj_set_style_bg_opa(za.card, card, 0);
	if (lv_obj_check_type(za.sym, &lv_image_class)) {
		if (lv_obj_get_style_image_opa(za.sym, 0) != sym)
			lv_obj_set_style_image_opa(za.sym, sym, 0);
	} else if (lv_obj_get_style_text_opa(za.sym, 0) != sym) {
		lv_obj_set_style_text_opa(za.sym, sym, 0);
	}
}

static void za_exec(void *var, int32_t v)
{
	lv_opa_t o;

	LV_UNUSED(var);
	switch (za.phase) {
	case 1:
		za_rect(v);
		za_look(LV_OPA_COVER, za_sym_opa(v));
		break;
	case 5:
		za_rect(v);
		/* home's icon shows through as the card settles on it */
		o = v >= ZA_MELT ? LV_OPA_COVER : (lv_opa_t)(255 * v / ZA_MELT);
		za_look(o, LV_MIN(o, za_sym_opa(v)));
		break;
	case 3:
	case 4:
	case 6:
	case 7:
		za_look((lv_opa_t)v, LV_OPA_TRANSP);	/* (the symbol went on the way) */
		break;
	}
}

static void za_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	za_next();
}

static void za_run(int phase, int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path)
{
	lv_anim_t a;

	za.phase = phase;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &za);
	lv_anim_set_exec_cb(&a, za_exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, path);
	lv_anim_set_completed_cb(&a, za_done);
	za_exec(NULL, from);
	lv_anim_start(&a);
}

static void za_free(void)
{
	if (za.scr) {
		lv_obj_delete(za.scr);
		za.scr = NULL;
	}
}

/* The animation screen: home's picture (hidden in the fades) and the card. */
static void za_build(void)
{
	lv_obj_t *badge = lv_obj_get_child(za.tile, 0), *l;

	lv_obj_get_coords(badge, &za.icon);
	za.col = lv_obj_get_style_bg_color(badge, 0);
	/* a video opens on black: the card, and layer 0 around the picture
	 * (a Play button is no badge to take a colour from either) */
	if (app_cmd && cmd_is_video(app_cmd) && app_pid != 0 && !za.page)
		za.col = lv_color_black();
	za.scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(za.scr);
	lv_obj_remove_flag(za.scr, LV_OBJ_FLAG_SCROLLABLE);
	za.bg = lv_image_create(za.scr);
	if (za.home)
		lv_image_set_src(za.bg, za.home);
	za.card = lv_obj_create(za.scr);
	plain(za.card);
	lv_obj_remove_flag(za.card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(za.card, za.col, 0);
	lv_obj_set_style_bg_opa(za.card, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(za.card, 0, 0);
	/* the badge's own symbol, or the application's picture: the card
	 * starts, and ends, as the icon */
	l = lv_obj_get_child(badge, 0);
	if (l && lv_obj_check_type(l, &lv_image_class)) {
		za.sym = lv_image_create(za.card);
		lv_image_set_src(za.sym, lv_image_get_src(l));
		lv_image_set_scale(za.sym, lv_image_get_scale(l));
	} else {
		za.sym = m3_label(za.card, l ? lv_label_get_text(l) : "",
				  l ? lv_obj_get_style_text_font(l, 0) : &lv_font_montserrat_24,
				  l ? lv_obj_get_style_text_color(l, 0) : lv_color_white());
	}
	lv_obj_center(za.sym);
}

/* Home's picture, retaken if it went stale (the card covers the screen). */
static void za_home_fresh(void)
{
	if (!za.home_stale && za.home)
		return;
	lv_image_set_src(za.bg, NULL);
	tr_free(&za.home);
	za.home = snap(launch_from && lv_obj_is_valid(launch_from) ? launch_from : home_scr,
		       LV_COLOR_FORMAT_XRGB8888);
	za.home_min = bp_minute();
	za.home_stale = false;
}

/* One phase ends: the next begins. */
static void za_next(void)
{
	if (getenv("N72_FPS"))
		fprintf(stderr, "za: phase %d ends at %u ms\n", za.phase, (unsigned)lv_tick_get());
	if (za.page) {
		switch (za.phase) {
		case 1:		/* grown: the card fades off the page's picture */
			if (!za.page_scr && za.page_open) {
				/* built now, unseen under the card */
				push_quiet = true;
				push_quiet_scr = NULL;
				za.page_open(NULL);
				push_quiet = false;
				za.page_scr = push_quiet_scr;
				if (za.page_scr)
					za.page_pic = snap(za.page_scr, LV_COLOR_FORMAT_XRGB8888);
			}
			if (!za.page_scr || !za.page_pic) {
				/* no page, or no memory for its picture: straight to it */
				za.phase = 0;
				za.page = false;
				lv_screen_load(za.page_scr ? za.page_scr : home_scr);
				za_free();
				return;
			}
			lv_image_set_src(za.bg, za.page_pic);
			za_run(6, LV_OPA_COVER, LV_OPA_TRANSP, ZA_REVEAL_MS, ios_path_ease_out);
			return;
		case 6:
			za.phase = 0;
			za.page = false;
			lv_screen_load(za.page_scr);
			za_free();
			tr_free(&za.page_pic);
			return;
		case 7:		/* covered: into the icon over home */
			za_home_fresh();
			lv_image_set_src(za.bg, za.home);
			za_run(5, 1024, 0, ZA_SHRINK_MS, za_path_glide);
			return;
		case 5:
			za.phase = 0;
			za.page = false;
			lv_screen_load(home_scr);
			while (nav_n > 0)
				lv_obj_delete_async(nav[--nav_n]);
			za_free();
			tr_free(&za.page_pic);
			home_update(NULL);
			return;
		}
	}
	switch (za.phase) {
	case 1:			/* grown: the card clears, the application shows */
		lv_obj_add_flag(za.bg, LV_OBJ_FLAG_HIDDEN);
		/* the card alone: one GPU fill a frame (mbx_fill_straight) */
		lv_obj_add_flag(za.sym, LV_OBJ_FLAG_HIDDEN);
		mbx_gpu_set_transparent(true);
		za_run(3, LV_OPA_COVER, LV_OPA_TRANSP, ZA_REVEAL_MS, ios_path_ease_out);
		break;
	case 3:
		za.phase = 0;
		lv_screen_load(app_pid > 0 ? app_scr : home_scr);
		/* a game turns the launcher's layer to its side now that it is in
		 * front (rot_wanted; during the grow it was not) */
		if (app_is_game())
			rot_kick();
		za_free();
		if (app_pid <= 0)
			app_home_ui();
		break;
	case 4:			/* covered: shrink into the icon over home */
		lv_obj_remove_flag(za.sym, LV_OBJ_FLAG_HIDDEN);
		mbx_gpu_set_transparent(false);
		za_home_fresh();
		lv_image_set_src(za.bg, za.home);
		lv_obj_remove_flag(za.bg, LV_OBJ_FLAG_HIDDEN);
		za_run(5, 1024, 0, ZA_SHRINK_MS, za_path_glide);
		break;
	case 5:
		za.phase = 0;
		app_home_ui();
		za_free();
		break;
	}
}

/* Ending, the animation: finish whatever runs now, at once. */
static void za_finish(void)
{
	while (za.phase) {
		lv_anim_delete(&za, za_exec);
		za_next();
	}
}

/* Every animation of the launcher's own ends now (drags included). */
static void ui_settle_all(void)
{
	if (sd.on) {			/* the finger is still on the shade */
		sd.on = false;
		sd.cand = 0;
		sd_end(sd.y > -SHADE_H / 2);
	}
	sd_settle_now();
	za_finish();
	tr_finish();
	pg_abort();
}

/*
 * The launcher's own pages open the same way: the page is built (quietly:
 * push() only records it) and pictured first, the card grows from the
 * tile, then fades off the page's picture, and the page takes over. Home,
 * or Back from the first page, runs it backwards (za_page_close).
 */
static void page_open_from(lv_obj_t *tile, void (*open)(lv_event_t *))
{
	if (za.phase || app_pid > 0 || lv_screen_active() != home_scr)
		return;
	tr_finish();
	shade_set(false, false);
	/*
	 * The page is built and pictured once the card covers the screen
	 * (za_next, page case 1): doing it first held the tap ~100 ms.
	 */
	za.page_scr = NULL;
	za.page_open = open;
	tr_free(&za.page_pic);
	za.tile = za.page_tile = tile;
	tr_free(&za.home);
	za.home = grab_or_snap(home_scr);
	za.home_min = bp_minute();
	if (!za.home) {
		open(NULL);	/* no memory for pictures: no animation */
		return;
	}
	za.page = true;
	za_build();
	lv_screen_load(za.scr);
	za_run(1, 0, 1024, ZA_GROW_MS, za_path_spring);
}

static void page_launch_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_current_target(e);

	obj_rest(t);	/* (its transition would be pictured) */
	page_open_from(t, (void (*)(lv_event_t *))lv_event_get_user_data(e));
}

/* The tile just added to @parent opens @open's page with the card. */
static lv_obj_t *page_tile_anim(lv_obj_t *parent, void (*open)(lv_event_t *))
{
	lv_obj_t *t = lv_obj_get_child(parent, lv_obj_get_child_count(parent) - 1);

	lv_obj_remove_event_cb(t, open);
	lv_obj_add_event_cb(t, page_launch_cb, LV_EVENT_CLICKED, (void *)open);
	return t;
}

/* Home (or Back from the first page) on a page opened from a tile. */
static bool za_page_close(void)
{
	lv_obj_t *cur = lv_screen_active();

	if (za.phase || !za.page_tile || nav_n < 1 || cur == home_scr || locked)
		return false;
	tr_finish();
	cur = lv_screen_active();
	tr_free(&za.page_pic);
	za.page_pic = grab_or_snap(cur);
	/* a stale home picture is retaken under the card (case 7) */
	za.home_stale = !za.home || za.home_min != bp_minute();
	if (!za.page_pic)
		return false;
	za.tile = za.page_tile;
	za.page = true;
	za_build();
	lv_image_set_src(za.bg, za.page_pic);
	za_rect(1024);
	lv_screen_load(za.scr);
	za_run(7, LV_OPA_TRANSP, LV_OPA_COVER, ZA_COVER_MS, lv_anim_path_linear);
	return true;
}

/*
 * Settings from the shade's gear: the page is built and pictured, and the
 * shade goes up off it, the scrim clearing as it goes (the shade drag's
 * pictures, with the page below instead of home). Over an application the
 * launcher stops being transparent: the page covers the application, which
 * runs on underneath. Back from that page slides it down off the
 * application (page_back_to_app); Home ends the application, the card
 * covering the page and going into the application's icon.
 */
static bool page_over_app;

static void qs_to_page(void (*open)(lv_event_t *))
{
	lv_draw_buf_t *bg, *fg;
	bool over_app = app_pid > 0 && lv_screen_active() == app_scr;

	if (!shade_open || sd.on || sd.scr || za.phase || locked || !settings_tile ||
	    (!over_app && lv_screen_active() != home_scr))
		return;
	push_quiet = true;
	push_quiet_scr = NULL;
	open(NULL);
	push_quiet = false;
	if (!push_quiet_scr)
		return;
	bg = snap(push_quiet_scr, LV_COLOR_FORMAT_XRGB8888);
	fg = shade_pic_get();
	if (over_app) {
		mbx_gpu_set_transparent(false);	/* the page covers it */
		app_bar_set(false);
		if (app_bar) {
			lv_anim_delete(app_bar, app_y);
			lv_obj_add_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
		}
		page_over_app = true;
	}
	za.page_tile = settings_tile;
	if (!bg || !fg) {
		tr_free(&bg);
		shade_set(false, false);
		lv_screen_load(push_quiet_scr);
		return;
	}
	tr_free(&sd.home_pic);
	sd.sbg = bg;
	sd.sfg = fg;
	sd.fg_cached = true;
	sd.back = push_quiet_scr;
	sd.to_page = true;
	sd.scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(sd.scr);
	lv_obj_remove_flag(sd.scr, LV_OBJ_FLAG_SCROLLABLE);
	sd.bgclip = lv_obj_create(sd.scr);
	lv_obj_remove_style_all(sd.bgclip);
	lv_obj_remove_flag(sd.bgclip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_width(sd.bgclip, W);
	sd.bg = lv_image_create(sd.bgclip);
	lv_image_set_src(sd.bg, bg);
	sd.scrim = lv_obj_create(sd.scr);
	plain(sd.scrim);
	lv_obj_set_width(sd.scrim, W);
	lv_obj_set_style_radius(sd.scrim, 0, 0);
	lv_obj_set_style_bg_color(sd.scrim, lv_color_black(), 0);
	sd.fg = lv_image_create(sd.scr);
	lv_image_set_src(sd.fg, fg);
	lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	lv_screen_load(sd.scr);
	sd.pics = true;
	sd.y = 0;
	sd_set(0);
	{
		lv_anim_t a;

		lv_anim_init(&a);
		lv_anim_set_var(&a, &sd);
		lv_anim_set_exec_cb(&a, sd_anim_exec);
		lv_anim_set_values(&a, 0, -SHADE_H);
		lv_anim_set_duration(&a, 300);
		lv_anim_set_path_cb(&a, ios_path_ease_in_out);
		lv_anim_set_completed_cb(&a, sd_anim_done);
		lv_anim_start(&a);
	}
}

/* Back from Settings over an application: the page slides down off it. */
static struct {
	lv_obj_t *scr, *img;
	lv_draw_buf_t *pic;
} pb;

static void pb_exec(void *var, int32_t v)
{
	LV_UNUSED(var);
	lv_obj_set_y(pb.img, v);
}

static void pb_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	lv_screen_load(app_pid > 0 ? app_scr : home_scr);
	while (nav_n > 0)
		lv_obj_delete_async(nav[--nav_n]);
	lv_obj_delete(pb.scr);
	pb.scr = NULL;
	tr_free(&pb.pic);
	if (app_pid <= 0)
		app_home_ui();
}

static bool page_back_to_app(void)
{
	lv_anim_t a;

	if (!page_over_app || app_pid <= 0 || pb.scr)
		return false;
	tr_finish();
	obj_rest(lv_indev_get_active_obj());	/* the Back button, unpressed */
	pb.pic = grab_or_snap(lv_screen_active());
	if (!pb.pic)
		return false;
	page_over_app = false;
	pb.scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(pb.scr);
	lv_obj_remove_flag(pb.scr, LV_OBJ_FLAG_SCROLLABLE);
	pb.img = lv_image_create(pb.scr);
	lv_image_set_src(pb.img, pb.pic);
	mbx_gpu_set_transparent(true);	/* the application shows as it goes */
	lv_screen_load(pb.scr);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &pb);
	lv_anim_set_exec_cb(&a, pb_exec);
	lv_anim_set_values(&a, 0, H);
	lv_anim_set_duration(&a, 280);
	lv_anim_set_path_cb(&a, ios_path_ease_in);
	lv_anim_set_completed_cb(&a, pb_done);
	lv_anim_start(&a);
	return true;
}

static void app_kill_cb(lv_timer_t *t);

/* Home on Settings over an application: the application ends into its icon. */
static bool page_home_over_app(void)
{
	if (!page_over_app || app_pid <= 0)
		return false;
	page_over_app = false;
	kill(-app_pid, SIGTERM);
	if (!za.kill_timer) {
		za.kill_timer = lv_timer_create(app_kill_cb, 600, NULL);
		lv_timer_set_repeat_count(za.kill_timer, 1);
	}
	za.page_tile = za.tile;		/* the application's tile */
	if (!za_page_close()) {
		while (nav_n > 0)
			lv_obj_delete_async(nav[--nav_n]);
		lv_screen_load(home_scr);
	}
	return true;
}

/* Back to home (the application is gone, or going). */
static void ui_rebuild(bool load);
static bool layout_stale;

static bool apps_stale;
static void apps_reload(void);

static void app_home_ui(void)
{
	wp_apply_kick();	/* (a restart for the palette waits for no application) */
	vc_stop();
	if (apps_stale)
		apps_reload();	/* (the folder changed while it ran) */
	if (layout_stale)
		ui_rebuild(false);	/* (turned over a video: home and pages now) */
	else
		home_fresh();
	shade_set(false, false);
	app_bar_set(false);
	if (app_bar) {
		lv_anim_delete(app_bar, app_y);
		lv_obj_set_y(app_bar, -APP_BAR_H);
		lv_obj_add_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
	}
	lv_anim_delete(shade, app_y);
	lv_obj_set_parent(scrim, home_scr);
	lv_obj_set_parent(shade, home_scr);
	mbx_gpu_set_transparent(false);
	if (page_over_app) {
		/* the application went from under Settings: Settings stays,
		 * a page like any other now (Home puts it into its tile) */
		page_over_app = false;
		za.page_tile = settings_tile;
	} else {
		/* back where the application was started: a page (Files) still
		 * on the stack, else home (Home was pressed: the pages go) */
		lv_obj_t *to = launch_from && launch_from != home_scr && lv_obj_is_valid(launch_from) &&
			       nav_n > 0 && nav[nav_n - 1] == launch_from ? launch_from : home_scr;

		if (to == home_scr)
			while (nav_n > 0)
				lv_obj_delete_async(nav[--nav_n]);
		if (lv_screen_active() != to && !locked)
			lv_screen_load(to);
		if (locked)
			lock_from = to;
	}
	lv_obj_invalidate(lv_layer_top());
	home_update(NULL);
}

/* Close: the card covers the application, then goes into its icon. */
static void za_close(void)
{
	if (za.phase || locked || lv_screen_active() != app_scr || !za.tile) {
		app_home_ui();
		return;
	}
	/* a stale home picture is retaken under the card (case 4) */
	za.home_stale = !za.home || za.home_min != bp_minute();
	shade_set(false, false);
	app_bar_set(false);
	za_build();
	lv_obj_add_flag(za.bg, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(za.sym, LV_OBJ_FLAG_HIDDEN);	/* (see case 2) */
	za_rect(1024);
	lv_screen_load(za.scr);
	za_run(4, LV_OPA_TRANSP, LV_OPA_COVER, ZA_COVER_MS, lv_anim_path_linear);
}

/* The application has gone (reaped). */
static void app_pidfd_close(void);

static void app_closed(void)
{
	bool was_ending = za.kill_timer != NULL;

	/* a shade drag or settle over the application ends first: it would
	 * otherwise reload the application's screen later (review finding) */
	if (sd.on) {
		sd.on = false;
		sd.cand = 0;
		sd_end(false);
	}
	sd_settle_now();

	app_pid = 0;
	app_pidfd_close();
	kbd_app_gone();
	rot_kick();		/* (a player gone: no reads needed) */
	vc_stop();
	if (app_timer) {
		lv_timer_delete(app_timer);
		app_timer = NULL;
	}
	if (za.kill_timer) {
		lv_timer_delete(za.kill_timer);
		za.kill_timer = NULL;
	}
	if (za.phase)
		return;			/* the animation ends on home */
	if (!was_ending && lv_screen_active() == app_scr)
		za_close();		/* it quit by itself */
	else
		app_home_ui();
}

static void app_poll(lv_timer_t *t)
{
	int st;

	LV_UNUSED(t);
	if (app_pid > 0 && waitpid(app_pid, &st, WNOHANG) == app_pid) {
		if (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st)))
			fprintf(stderr, "n72-ui: application ended: %s %d\n",
				WIFSIGNALED(st) ? "signal" : "exit status",
				WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		app_closed();
	}
}

/* The application's pidfd turned readable: it has ended (no 200 ms look). */
static int app_pidfd = -1;

static void app_pidfd_ev(int fd)
{
	LV_UNUSED(fd);
	app_poll(NULL);
}

static void app_pidfd_close(void)
{
	if (app_pidfd < 0)
		return;
	ui_unwatch(app_pidfd);
	close(app_pidfd);
	app_pidfd = -1;
}

static void app_kill_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	za.kill_timer = NULL;
	if (app_pid > 0)
		kill(-app_pid, SIGKILL);	/* app_poll reaps it */
}

/* Home: end the application, if one runs (without waiting for it). */
static bool app_end(void)
{
	if (app_pid <= 0)
		return false;
	if (za.kill_timer || (za.phase && za.phase >= 4))
		return true;		/* already ending */
	za_finish();
	kill(-app_pid, SIGTERM);
	za.kill_timer = lv_timer_create(app_kill_cb, 600, NULL);
	lv_timer_set_repeat_count(za.kill_timer, 1);
	za_close();
	return true;
}

/*
 * The application starts once the grow's first frame is out: layer 0
 * filled with the card's colour first (za_fill_layer0, ~5 ms), then the
 * program, by posix_spawn (no copy-on-write of the launcher's heap, whose
 * faults stalled the next frames after fork), in its own process group.
 * (Lowering its priority during the grow did not take here, by any route
 * tried; with the fades on the GPU it matters little.)
 */
extern char **environ;

static void za_spawn_cb(lv_timer_t *t)
{
	posix_spawnattr_t at;
	char touch[96], **envp;
	char *const argv[] = { "sh", "-c", (char *)za.cmd, NULL };
	int n = 0, i;
	pid_t pid;

	LV_UNUSED(t);
	if (app_pid != -1)
		return;		/* (the launch was given up meanwhile) */
	za_fill_layer0(za.col);
	while (environ[n])
		n++;
	envp = calloc(n + 3, sizeof(*envp));	/* (N72_TOUCH, N72_KBD, the end) */
	if (!envp) {
		app_pid = 0;
		return;
	}
	for (i = 0; i < n; i++)
		envp[i] = environ[i];
	if (rt_node[0]) {	/* its touch screen: the router's */
		snprintf(touch, sizeof(touch), "N72_TOUCH=%s", rt_node);
		for (i = 0; i < n && strncmp(envp[i], "N72_TOUCH=", 10); i++)
			;
		if (i == n)
			n++;
		envp[i] = touch;
	}
	{	/* where it asks for the keyboard */
		for (i = 0; i < n && strncmp(envp[i], "N72_KBD=", 8); i++)
			;
		if (i == n)
			n++;
		envp[i] = "N72_KBD=" KBD_FIFO;
	}
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&at, 0);	/* Home ends everything it starts */
	if (posix_spawn(&pid, "/bin/sh", NULL, &at, argv, envp))
		pid = 0;
	if (getenv("N72_FPS"))
		fprintf(stderr, "spawn: pid %d at %u ms, phase %d\n", (int)pid,
			(unsigned)lv_tick_get(), za.phase);
	posix_spawnattr_destroy(&at);
	free(envp);
	app_pid = pid;
	rot_kick();		/* (a player turns with the iPod) */
	if (pid > 0) {
		/* its end as an event: a pidfd in the main loop's poll() */
		app_pidfd_close();
		app_pidfd = syscall(SYS_pidfd_open, pid, 0);
		if (app_pidfd >= 0) {
			fcntl(app_pidfd, F_SETFD, FD_CLOEXEC);
			ui_watch(app_pidfd, app_pidfd_ev);
		} else if (!app_timer) {
			app_timer = lv_timer_create(app_poll, 200, NULL);	/* (no pidfd: look) */
		}
	}
	/* idle while the card grows (the attribute alone did not take here);
	 * za_next gives it full priority when the grow ends */
}

/* Start @cmd as the application, growing out of @tile (a tile, or a row). */
static void app_launch(const char *cmd, lv_obj_t *tile)
{
	if (app_pid != 0 || za.phase || !mbx_gpu_on_shell_layer())
		return;
	free(app_cmd);
	app_cmd = strdup(cmd ? cmd : "");
	if (!app_cmd)
		return;
	cmd = app_cmd;		/* (the row's copy goes with its page in a turn) */
	if (ui_rot && !cmd_is_video(cmd) && !cmd_is_game(cmd)) {
		/* applications draw for an upright panel: turn first, and
		 * find the tile again in the new home */
		lv_obj_t *par = lv_obj_get_parent(tile);
		bool docked = home_dock && par == home_dock;	/* (the dock is rebuilt in its order) */
		int idx = par == home_apps || docked ? (int)lv_obj_get_index(tile) : -1;

		if (idx < 0 || !ui_turn(0))
			return;
		tile = lv_obj_get_child(docked ? home_dock : home_apps, idx);
		if (!tile)
			return;
	}
	if (cmd_is_video(cmd))
		vc_start();
	if (!app_scr) {
		/* nothing on it: transparent, the application shows */
		app_scr = lv_obj_create(NULL);
		lv_obj_remove_style_all(app_scr);
		lv_obj_remove_flag(app_scr, LV_OBJ_FLAG_SCROLLABLE);
		/* LVGL draws its bottom layer under a screen that is not
		 * opaque, and the theme has given that one a background */
		lv_obj_remove_style_all(lv_layer_bottom());
		lv_obj_add_event_cb(app_scr, app_gesture_cb, LV_EVENT_GESTURE, NULL);
		app_bar_build();
	}
	tr_finish();
	shade_set(false, false);

	/* home as it is now, for the grow (and the close) */
	za.tile = tile;
	{
		/* the tile back to rest at once, not by its transition: adding
		 * a style ends an object's transitions */
		static lv_style_t none;
		uint32_t i;

		if (!none.values_and_props)
			lv_style_init(&none);
		lv_obj_remove_state(za.tile, LV_STATE_PRESSED);
		for (i = 0; i <= lv_obj_get_child_count(za.tile); i++) {
			lv_obj_t *o = i ? lv_obj_get_child(za.tile, i - 1) : za.tile;

			lv_obj_add_style(o, &none, 0);
			lv_obj_remove_style(o, &none, 0);
		}
	}
	tr_free(&za.home);
	launch_from = lv_screen_active();	/* home, or a page (Files) */
	za.home = grab_or_snap(launch_from);
	za.home_min = bp_minute();

	za.cmd = cmd;
	/* the shade comes down over the application from the top layer */
	lv_obj_set_parent(scrim, lv_layer_top());
	lv_obj_set_parent(shade, lv_layer_top());
	{
		/* started once the first frame of the grow is out (za_spawn_cb) */
		/* (a video player: once the grow has landed; its start-up,
		 * GStreamer's plugins and the stream opening, made the grow
		 * jitter, and it shows nothing for a second anyway) */
		lv_timer_t *st = lv_timer_create(za_spawn_cb,
						  cmd_is_video(cmd) ? ZA_GROW_MS + 20 : 20, NULL);

		lv_timer_set_repeat_count(st, 1);
	}
	app_pid = -1;		/* (starting: no second launch meanwhile) */

	za_build();
	lv_screen_load(za.scr);
	za_run(1, 0, 1024, ZA_GROW_MS, za_path_spring);
}

static void app_launch_cb(lv_event_t *e)
{
	app_launch(lv_event_get_user_data(e), lv_event_get_current_target(e));
}

/* One tile for every manifest in APPS_DIR. */
/* A tile that starts @cmd: icon=<name> picks a symbol, @rgb the badge. */
static void app_cmd_free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void app_tile_cmd(lv_obj_t *parent, const char *name, const char *cmd,
			 const char *icon, const char *image, bool have_rgb, unsigned rgb)
{
	static const struct { const char *n, *sym; } syms[] = {
		{ "video", LV_SYMBOL_VIDEO }, { "image", LV_SYMBOL_IMAGE },
		{ "music", LV_SYMBOL_AUDIO }, { "gears", LV_SYMBOL_SETTINGS },
		{ "shuffle", LV_SYMBOL_SHUFFLE }, { "eye", LV_SYMBOL_EYE_OPEN },
		{ "file", LV_SYMBOL_FILE }, { "list", LV_SYMBOL_LIST },
		{ "terminal", LV_SYMBOL_KEYBOARD }, { "gps", LV_SYMBOL_GPS },
		{ "charge", LV_SYMBOL_CHARGE }, { "home", LV_SYMBOL_HOME },
		{ "folder", LV_SYMBOL_DIRECTORY }, { "game", LV_SYMBOL_PLAY },
		{ "web", LV_SYMBOL_WIFI },
	};
	uint32_t before = lv_obj_get_child_count(parent);
	const char *sym = LV_SYMBOL_PLAY;
	size_t k;

	struct app_icon *ic = image && image[0] ? app_icon_load(image) : NULL;

	for (k = 0; k < sizeof(syms) / sizeof(syms[0]); k++)
		if (!strcmp(icon, syms[k].n))
			sym = syms[k].sym;
	if (ic)
		app_tile_img(parent, ic, name, app_launch_cb);
	else
		app_tile(parent, sym, name,
			 have_rgb ? lv_color_hex(rgb) : M.secondary_c,
			 have_rgb ? lv_color_white() : M.on_secondary_c, app_launch_cb);
	/* the command rides on the tile's click */
	if (lv_obj_get_child_count(parent) > before) {
		lv_obj_t *t = lv_obj_get_child(parent, before);
		char *c = strdup(cmd);

		lv_obj_remove_event_cb(t, app_launch_cb);
		lv_obj_add_event_cb(t, app_launch_cb, LV_EVENT_CLICKED, c);
		lv_obj_add_event_cb(t, app_cmd_free_cb, LV_EVENT_DELETE, c);
	}
}

/* Our manifests: /usr/share/n72-ui/apps/NAME.app (name=, exec=, icon=, image=, color=#). */
struct app_manifest {
	char name[40], cmd[400], icon[24], image[160];
	unsigned rgb;
	bool have_rgb;
};

static int app_manifest_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct app_manifest *)a)->name,
			  ((const struct app_manifest *)b)->name);
}

/* (tiles in the order of their names, whatever the folder's order) */
static void app_tiles_n72(lv_obj_t *parent)
{
	DIR *d = opendir(APPS_DIR);
	struct app_manifest *m = NULL;
	struct dirent *de;
	int n = 0, cap = 0, i;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[300], line[400];
		struct app_manifest a = { 0 };
		size_t len = strlen(de->d_name);
		FILE *f;

		if (len < 5 || strcmp(de->d_name + len - 4, ".app"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", APPS_DIR, de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "name=", 5))
				snprintf(a.name, sizeof(a.name), "%s", line + 5);
			else if (!strncmp(line, "exec=", 5))
				snprintf(a.cmd, sizeof(a.cmd), "%s", line + 5);
			else if (!strncmp(line, "icon=", 5))
				snprintf(a.icon, sizeof(a.icon), "%s", line + 5);
			else if (!strncmp(line, "image=", 6))
				snprintf(a.image, sizeof(a.image), "%s", line + 6);
			else if (!strncmp(line, "color=#", 7))
				a.have_rgb = sscanf(line + 7, "%x", &a.rgb) == 1;
		}
		fclose(f);
		if (!a.name[0] || !a.cmd[0])
			continue;
		if (n == cap) {
			struct app_manifest *g = realloc(m, (cap ? cap * 2 : 8) * sizeof(*m));

			if (!g)
				break;
			m = g;
			cap = cap ? cap * 2 : 8;
		}
		m[n++] = a;
	}
	closedir(d);
	if (n)
		qsort(m, n, sizeof(*m), app_manifest_cmp);
	for (i = 0; i < n; i++)
		app_tile_cmd(parent, m[i].name, m[i].cmd, m[i].icon, m[i].image,
			     m[i].have_rgb, m[i].rgb);
	free(m);
}

/*
 * Packaged applications: freedesktop .desktop files. Only those that can
 * run here: no desktop session (X11, Wayland) and so no GTK or Qt; told by
 * the program's ELF header, which names the libraries it needs.
 */
#define DESKTOP_DIR "/usr/share/applications"

static bool prog_path(const char *prog, char *out, size_t len)
{
	const char *path = getenv("PATH"), *p;

	if (strchr(prog, '/')) {
		snprintf(out, len, "%s", prog);
		return access(out, X_OK) == 0;
	}
	if (!path || !*path)
		path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	for (p = path; *p; ) {
		size_t n = strcspn(p, ":");

		snprintf(out, len, "%.*s/%s", (int)n, p, prog);
		if (n && access(out, X_OK) == 0)
			return true;
		p += n;
		if (*p == ':')
			p++;
	}
	return false;
}

/* the program needs a desktop session: it links X11, Wayland, GTK or Qt */
static bool needs_desktop(const char *path)
{
	static const char *const libs[] = { "libX11.so", "libwayland-client.so",
		"libgtk-", "libQt5", "libQt6" };
	size_t len = 512 * 1024, got, i;	/* the dynamic section is near the start */
	char *buf = malloc(len);
	bool hit = false;
	FILE *f = fopen(path, "rb");

	if (!buf || !f) {
		free(buf);
		if (f)
			fclose(f);
		return false;
	}
	got = fread(buf, 1, len, f);
	fclose(f);
	for (i = 0; i < sizeof(libs) / sizeof(libs[0]) && !hit; i++)
		hit = memmem(buf, got, libs[i], strlen(libs[i])) != NULL;
	free(buf);
	return hit;
}

static void app_tiles_desktop(lv_obj_t *parent)
{
	DIR *d = opendir(DESKTOP_DIR);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[300], line[512], name[40] = "", cmd[400] = "", icon[24] = "";
		char prog[PATH_MAX], arg0[256];
		unsigned rgb = 0;
		bool have_rgb = false, skip = false, entry = false, app = false;
		size_t n = strlen(de->d_name);
		FILE *f;
		char *p, *q;

		if (n < 9 || strcmp(de->d_name + n - 8, ".desktop"))
			continue;
		snprintf(path, sizeof(path), DESKTOP_DIR "/%s", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0] == '[') {		/* only the main group */
				entry = !strcmp(line, "[Desktop Entry]");
				continue;
			}
			if (!entry)
				continue;
			if (!strncmp(line, "Name=", 5))
				snprintf(name, sizeof(name), "%s", line + 5);
			else if (!strncmp(line, "Exec=", 5))
				snprintf(cmd, sizeof(cmd), "%s", line + 5);
			else if (!strncmp(line, "Type=", 5))
				app = !strcmp(line + 5, "Application");
			else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true") ||
				 !strcmp(line, "Terminal=true"))
				skip = true;
			else if (!strncmp(line, "X-N72-Icon=", 11))
				snprintf(icon, sizeof(icon), "%s", line + 11);
			else if (!strncmp(line, "X-N72-Color=#", 13))
				have_rgb = sscanf(line + 13, "%x", &rgb) == 1;
			else if (!icon[0] && !strncmp(line, "Categories=", 11))
				snprintf(icon, sizeof(icon), "%s",
					 strstr(line, "Game") ? "game" : strstr(line, "WebBrowser") ? "web" :
					 strstr(line, "AudioVideo") ? "video" : strstr(line, "Graphics") ? "image" : "");
		}
		fclose(f);
		if (skip || !app || !name[0] || !cmd[0])
			continue;
		/* field codes (%f %U ...) out: started without a file */
		for (p = q = cmd; *p; p++) {
			if (*p == '%' && p[1]) {
				p++;
				if (*p == '%')
					*q++ = '%';
				continue;
			}
			*q++ = *p;
		}
		*q = 0;
		snprintf(arg0, sizeof(arg0), "%.*s", (int)strcspn(cmd, " \t"), cmd);
		if (!prog_path(arg0, prog, sizeof(prog)) || needs_desktop(prog))
			continue;
		app_tile_cmd(parent, name, cmd, icon, NULL, have_rgb, rgb);
	}
	closedir(d);
}

static void app_tiles(lv_obj_t *parent)
{
	if (!mbx_gpu_on_shell_layer())
		return;
	app_tiles_n72(parent);
	app_tiles_desktop(parent);
}

/*
 * The folder of manifests changed (an application added, renamed or
 * removed; n72-ios-tiles rewrites iOS applications' manifests): the tiles
 * after the built-in ones are made again. Not while an application runs
 * or a card animates (the card's way back is a tile): then once home is
 * back (app_home_ui). A burst of changes is one rebuild, 300 ms after the
 * last.
 */
#include "paging.c"	/* home pages: layout, the swipe, its frames */
#include "dock.c"	/* the dock under them */

static uint32_t home_apps_builtin;	/* tiles of our own, before the manifests' */
static lv_timer_t *apps_timer;

static void apps_reload(void)
{
	if (!home_apps || app_pid != 0 || za.phase) {
		apps_stale = true;
		return;
	}
	apps_stale = false;
	dock_before_reload();	/* (every tile back in the grid's order) */
	while (lv_obj_get_child_count(home_apps) > home_apps_builtin) {
		lv_obj_t *t = lv_obj_get_child(home_apps, -1);

		if (za.tile == t)
			za.tile = NULL;
		lv_obj_delete(t);
	}
	app_icons_free();
	app_tiles(home_apps);
	dock_after_reload();
	pg_layout();
	bp_changed(home_scr);
	lv_obj_invalidate(home_scr);
}

static void apps_timer_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	apps_timer = NULL;
	apps_reload();
}

static void apps_ev(int fd)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	if (apps_timer) {
		lv_timer_reset(apps_timer);
		return;
	}
	apps_timer = lv_timer_create(apps_timer_cb, 300, NULL);
	lv_timer_set_repeat_count(apps_timer, 1);
}

static void apps_watch(void)
{
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	if (fd < 0)
		return;
	/* our manifests, and the .desktop entries of installed programs (apk) */
	bool any = inotify_add_watch(fd, APPS_DIR, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
				     IN_MOVED_TO | IN_MOVED_FROM) >= 0;

	any |= inotify_add_watch(fd, DESKTOP_DIR, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
				 IN_MOVED_TO | IN_MOVED_FROM) >= 0;
	if (!any) {
		close(fd);
		return;
	}
	ui_watch(fd, apps_ev);
}

/* -------------------------------------------------------------- files */

/*
 * Files: a page a folder, from FILES_ROOT (Back goes up a level, as any
 * page). Folders first, then files, by name; video and sound open with
 * n72-video, launched from the row as an application from its tile.
 */
#define FILES_ROOT	"/root"
struct fent {
	char *name;
	bool dir;
	int count;		/* a folder's items, -1 until a row shows it */
	off_t size;
};

static int fent_cmp(const void *a, const void *b)
{
	const struct fent *x = a, *y = b;

	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

/* video, sound, picture or other, by the name's extension (*kind: its word) */
static const char *files_icon(const char *name, bool *media, const char **kind)
{
	static const char *const video[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi",
		"flv", "3gp", "h264", "264", "m2v", "mpg", "mpeg", "ts" };
	static const char *const audio[] = { "mp3", "m4a", "aac", "ogg", "oga", "opus",
		"wav", "flac" };
	static const char *const image[] = { "png", "jpg", "jpeg", "gif", "bmp", "webp" };
	const char *e = strrchr(name, '.');
	size_t i;

	*media = false;
	*kind = "File";
	if (!e)
		return LV_SYMBOL_FILE;
	e++;
	for (i = 0; i < sizeof(video) / sizeof(video[0]); i++)
		if (!strcasecmp(e, video[i]))
			return *media = true, *kind = "Video", LV_SYMBOL_VIDEO;
	for (i = 0; i < sizeof(audio) / sizeof(audio[0]); i++)
		if (!strcasecmp(e, audio[i]))
			return *media = true, *kind = "Audio", LV_SYMBOL_AUDIO;
	for (i = 0; i < sizeof(image) / sizeof(image[0]); i++)
		if (!strcasecmp(e, image[i]))
			return *kind = "Picture", LV_SYMBOL_IMAGE;
	return LV_SYMBOL_FILE;
}

static void files_size(off_t n, char *buf, size_t len)
{
	if (n < 1024)
		snprintf(buf, len, "%ld bytes", (long)n);
	else if (n < 1024 * 1024)
		snprintf(buf, len, "%.1f kB", n / 1024.0);
	else if (n < 1024L * 1024 * 1024)
		snprintf(buf, len, "%.1f MB", n / (1024.0 * 1024));
	else
		snprintf(buf, len, "%.1f GB", n / (1024.0 * 1024 * 1024));
}

/* A folder's items (visible ones; counted up to 999) */
static int files_count(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;
	int n = 0;

	if (!d)
		return -2;
	while (n < 999 && (de = readdir(d)))
		if (de->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

/*
 * A folder's page builds rows only for what is on screen: each card is as
 * tall as all its rows, and a few row objects move and take the entries
 * that scroll into view. With a row for every entry (6 objects each), a
 * folder of 300 made every scroll step move ~1800 objects, and dropped
 * glides to 20-40 fps.
 */
#define FROW_H		64	/* a row (m3_row_ex) */
#define FROW_PAD	4	/* the card's padding */
#define FROW_POOL	14	/* rows a card has: a screen, landscape or portrait, and a row more each side */

struct fpage {
	char *path;
	struct fent *v;
	int n, nd;		/* entries, folders (first) of them */
	lv_obj_t *body;
	struct flist {
		lv_obj_t *card;
		int base, n;	/* its entries: v[base], ... v[base + n - 1] */
		lv_obj_t *row[FROW_POOL];
		int at[FROW_POOL];	/* the entry (of the card) a row shows; -1 none */
	} list[2];		/* folders, files */
};

static int floor_div(int a, int b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static void files_bind(struct fpage *fp, struct flist *l, int j, int i)
{
	struct fent *f = &fp->v[l->base + i];
	lv_obj_t *r = l->row[j], *badge = lv_obj_get_child(r, 0), *col = lv_obj_get_child(r, 1);
	lv_obj_t *glyph = lv_obj_get_child(badge, 0);
	const char *icon;
	lv_color_t bg, fg;
	char sub[48];

	if (f->dir) {
		if (f->count == -1) {
			char full[PATH_MAX];

			snprintf(full, sizeof(full), "%s/%s", fp->path, f->name);
			f->count = files_count(full);
		}
		icon = LV_SYMBOL_DIRECTORY;
		bg = M.primary_c;
		fg = M.on_primary_c;
		snprintf(sub, sizeof(sub), f->count < 0 ? "Folder" : f->count == 0 ? "Empty" :
			 f->count == 1 ? "1 item" : f->count == 999 ? "999+ items" : "%d items",
			 f->count);
	} else {
		const char *kind;
		char size[24];
		bool media;

		icon = files_icon(f->name, &media, &kind);
		bg = media ? M.tertiary_c : M.secondary_c;
		fg = media ? M.on_tertiary_c : M.on_secondary_c;
		files_size(f->size, size, sizeof(size));
		snprintf(sub, sizeof(sub), "%s, %s", kind, size);	/* (the fonts are ASCII) */
	}
	lv_obj_set_pos(r, 0, i * FROW_H);
	lv_obj_set_user_data(r, (void *)(intptr_t)(l->base + i));
	lv_obj_set_style_bg_color(badge, bg, 0);
	lv_obj_set_style_text_color(glyph, fg, 0);
	lv_label_set_text(glyph, icon);
	lv_label_set_text(lv_obj_get_child(col, 0), f->name);
	lv_label_set_text(lv_obj_get_child(col, 1), sub);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
	l->at[j] = i;
}

/* Rows for the entries on screen, a row beyond each edge. */
static void files_sync(struct fpage *fp)
{
	lv_area_t bc, cc;
	int k, i, j;

	lv_obj_get_coords(fp->body, &bc);
	for (k = 0; k < 2; k++) {
		struct flist *l = &fp->list[k];
		int first, last;

		if (!l->card)
			continue;
		lv_obj_get_coords(l->card, &cc);
		first = LV_MAX(floor_div(bc.y1 - cc.y1 - FROW_PAD, FROW_H) - 1, 0);
		last = LV_MIN(floor_div(bc.y2 - cc.y1 - FROW_PAD, FROW_H) + 2, l->n);
		for (j = 0; j < FROW_POOL; j++)
			if (l->row[j] && l->at[j] >= 0 && (l->at[j] < first || l->at[j] >= last)) {
				l->at[j] = -1;
				lv_obj_add_flag(l->row[j], LV_OBJ_FLAG_HIDDEN);
			}
		for (i = first; i < last; i++) {
			int free_j = -1;

			for (j = 0; j < FROW_POOL && l->row[j]; j++) {
				if (l->at[j] == i)
					break;
				if (l->at[j] < 0 && free_j < 0)
					free_j = j;
			}
			if (j < FROW_POOL && l->row[j])
				continue;	/* shown already */
			if (free_j < 0)
				break;		/* (no row left: a taller screen than counted) */
			files_bind(fp, l, free_j, i);
		}
	}
}

static void files_open_dir(const char *path);

static void files_row_cb(lv_event_t *e)
{
	struct fpage *fp = lv_event_get_user_data(e);
	lv_obj_t *r = lv_event_get_current_target(e);
	int i = (int)(intptr_t)lv_obj_get_user_data(r);
	char full[PATH_MAX], q[PATH_MAX * 2 + 16], cmd[PATH_MAX * 2 + 32];
	const char *kind;
	bool media;

	if (i < 0 || i >= fp->n)
		return;
	snprintf(full, sizeof(full), "%s/%s", fp->path, fp->v[i].name);
	if (fp->v[i].dir) {
		files_open_dir(full);
		return;
	}
	files_icon(fp->v[i].name, &media, &kind);
	if (!media)
		return;
	/* opened as an application from its tile */
	shq(full, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "n72-video %s", q);
	app_launch(cmd, r);
}

static void files_page_ev(lv_event_t *e)
{
	struct fpage *fp = lv_event_get_user_data(e);

	if (lv_event_get_code(e) != LV_EVENT_DELETE) {
		files_sync(fp);
		return;
	}
	for (int i = 0; i < fp->n; i++)
		free(fp->v[i].name);
	free(fp->v);
	free(fp->path);
	free(fp);
}

static void files_open_dir(const char *path)
{
	nav_mark(NULL, path);
	const char *title = !strcmp(path, FILES_ROOT) ? "Files" : strrchr(path, '/') + 1;
	struct fpage *fp = calloc(1, sizeof(*fp));
	lv_obj_t *scr, *body, *c, *row;
	struct dirent *de;
	int cap = 0, k, j;
	DIR *d;

	if (!fp || !(fp->path = strdup(path))) {
		free(fp);
		return;
	}
	body = m3_page(title, &scr);
	fp->body = body;
	lv_obj_add_event_cb(body, files_page_ev, LV_EVENT_DELETE, fp);
	d = opendir(path);
	while (d && (de = readdir(d))) {
		struct stat st;

		if (de->d_name[0] == '.' || fstatat(dirfd(d), de->d_name, &st, 0))
			continue;
		if (fp->n == cap) {
			struct fent *nv = realloc(fp->v, (cap ? cap * 2 : 64) * sizeof(*nv));

			if (!nv)
				break;
			fp->v = nv;
			cap = cap ? cap * 2 : 64;
		}
		fp->v[fp->n].name = strdup(de->d_name);
		if (!fp->v[fp->n].name)
			break;
		fp->v[fp->n].dir = S_ISDIR(st.st_mode);
		fp->v[fp->n].count = -1;
		fp->v[fp->n].size = st.st_size;
		fp->n++;
	}
	if (fp->n)
		qsort(fp->v, fp->n, sizeof(*fp->v), fent_cmp);
	if (!fp->n) {
		/* empty state: the icon large, a headline, what to do */
		c = m3_card(body, M.surface_c);
		lv_obj_set_height(c, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(c, 24, 0);
		lv_obj_set_style_pad_row(c, 8, 0);
		m3_badge(c, LV_SYMBOL_DIRECTORY, M.surface_c_highest, M.on_surface_v, 56);
		m3_label(c, d ? "Nothing here yet" : "Cannot open this folder",
			 &lv_font_montserrat_16, M.on_surface);
		row = m3_label(c, d ? "Copy files over SSH or USB into this folder." :
			       "You may not have permission to read it.",
			       &lv_font_montserrat_14, M.on_surface_v);
		lv_obj_set_width(row, lv_pct(100));
		lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
	} else {
		while (fp->nd < fp->n && fp->v[fp->nd].dir)
			fp->nd++;
		fp->list[0].n = fp->nd;
		fp->list[1].base = fp->nd;
		fp->list[1].n = fp->n - fp->nd;
		for (k = 0; k < 2; k++) {	/* a section and its card: folders, then files */
			struct flist *l = &fp->list[k];

			if (!l->n)
				continue;
			m3_section(body, k ? "Files" : "Folders");
			l->card = c = m3_card(body, M.surface_c);
			lv_obj_set_style_pad_all(c, FROW_PAD, 0);
			lv_obj_set_height(c, l->n * FROW_H + 2 * FROW_PAD);
			for (j = 0; j < FROW_POOL && j < l->n; j++) {
				l->row[j] = m3_row_ex(c, LV_SYMBOL_FILE, M.secondary_c, M.on_secondary_c,
						      "", "", files_row_cb, fp, NULL, k == 0);
				lv_obj_add_flag(l->row[j], LV_OBJ_FLAG_HIDDEN);
				l->at[j] = -1;
			}
		}
		lv_obj_add_event_cb(body, files_page_ev, LV_EVENT_SCROLL, fp);
		lv_obj_add_event_cb(body, files_page_ev, LV_EVENT_SIZE_CHANGED, fp);
		lv_obj_update_layout(scr);
		files_sync(fp);
	}
	if (d)
		closedir(d);
	push(scr);
}

static void open_files(lv_event_t *e)
{
	LV_UNUSED(e);
	files_open_dir(FILES_ROOT);
}

#include "ui_youtube.c"	/* YouTube: search, a video's page, Play/Listen */
#include "ui_games.c"	/* Games: the library, a game started in RetroArch */

static void open_paint(lv_event_t *e);

/* An idle moment: the picture of the shade made ready before the next drag. */
static void shade_pic_warm(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (!shade || screen_off || locked || sd.on || sd.scr || za.phase || tr.scr ||
	    lv_anim_count_running() || (touch_indev &&
	    lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		return;
	if (!(shade_pic && !shade_pic_dirty && shade_pic_min == bp_minute()))
		shade_pic_get();
	/* up to date: nothing to do before the minute changes (a change of
	 * the shade's contents brings it back, shade_changed) */
	if (shade_pic && !shade_pic_dirty)
		lv_timer_set_period(t, (uint32_t)(60 - time(NULL) % 60) * 1000 + 100);
}

static lv_obj_t *home_apps;		/* the tile grid */
static bool home_build_quiet;		/* build_home() loads nothing */
static lv_timer_t *shade_warm_timer;

static void build_home(void)
{
	lv_obj_t *bar, *apps;

	conf_load();
	if (!wp.loaded)	/* (before the theme: its colours may come from the wallpaper) */
		wp_load();
	m3_apply_theme();
	home_scr = lv_obj_create(NULL);
	m3_screen(home_scr);
	home_wp = wp_put(home_scr, WP_HOME);
	lv_obj_add_event_cb(home_scr, home_gesture_cb, LV_EVENT_GESTURE, NULL);
	home_geom();	/* (the dock's side: the rest is the bar's, the pages', the dots') */

	/* status bar: time, Wi-Fi and Bluetooth, battery; a tap opens the shade */
	bar = lv_obj_create(home_scr);
	plain(bar);
	lv_obj_set_size(bar, PW, 40);
	lv_obj_set_pos(bar, PX, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(bar, shade_tap_cb, LV_EVENT_CLICKED, (void *)1);
	status_row(bar, &status_time, &status_wifi, &status_bt, &batt_lbl);
	home_bar = bar;
	if (home_wp) {
		lv_obj_set_style_text_color(status_time, WP_TEXT, 0);
		lv_obj_set_style_text_color(status_wifi, WP_TEXT, 0);
		lv_obj_set_style_text_color(status_bt, WP_TEXT, 0);
		lv_obj_set_style_text_color(batt_lbl, WP_TEXT, 0);
	}
	/* the card on page 0 shows the time: the bar there does not (the
	 * label stays, hidden: the bar over applications copies it; on the
	 * other pages it shows, pg_show_page) */
	lv_obj_add_flag(status_time, LV_OBJ_FLAG_HIDDEN);

	/*
	 * The pages (paging.c): one strip under the bar, as wide as all of
	 * them; its first child the card (page 0), then the tiles, placed
	 * by pg_layout(). Dots at the bottom when there is more than one.
	 */
	/* (in a window of their place: turned, the dock is beside it) */
	home_view = lv_obj_create(home_scr);
	lv_obj_remove_style_all(home_view);
	lv_obj_remove_flag(home_view, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(home_view, PX, PG_TOP);
	lv_obj_set_size(home_view, PW, PB - PG_TOP - PG_DOTS_H);
	apps = home_apps = lv_obj_create(home_view);
	plain(apps);
	lv_obj_set_pos(apps, 0, 0);
	lv_obj_set_size(apps, PW, PB - PG_TOP - PG_DOTS_H);
	lv_obj_set_style_bg_opa(apps, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(apps, 0, 0);
	lv_obj_remove_flag(apps, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	/* a widget: the time large, the date, battery and Wi-Fi */
	{
		lv_obj_t *c = lv_obj_create(apps);

		plain(c);
		lv_obj_set_size(c, PW - 32, PG_CARD_H);
		lv_obj_set_pos(c, 16, 4);
		lv_obj_set_style_radius(c, 24, 0);
		lv_obj_set_style_bg_color(c, M.surface_c_high, 0);
		/* over a wallpaper the card lets a little of it through */
		lv_obj_set_style_bg_opa(c, home_wp ? LV_OPA_80 : LV_OPA_COVER, 0);
		lv_obj_set_style_pad_all(c, 0, 0);
		lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(c, shade_tap_cb, LV_EVENT_CLICKED, (void *)1);
		home_clock = m3_label(c, "", &lv_font_montserrat_48, M.on_surface);
		lv_obj_align(home_clock, LV_ALIGN_TOP_LEFT, 18, 8);
		home_date = m3_label(c, "", &lv_font_montserrat_16, M.primary);
		lv_obj_align(home_date, LV_ALIGN_BOTTOM_LEFT, 20, -12);
		wx_card_build(c);	/* the right side: the weather */
		wx_init();
	}

	app_tile(apps, LV_SYMBOL_SETTINGS, "Settings", M.primary_c, M.on_primary_c, open_settings);
	settings_tile = page_tile_anim(apps, open_settings);
	app_tile(apps, LV_SYMBOL_EDIT, "Paint", M.tertiary_c, M.on_tertiary_c, open_paint);
	page_tile_anim(apps, open_paint);
	app_tile(apps, LV_SYMBOL_DIRECTORY, "Files", M.secondary_c, M.on_secondary_c, open_files);
	page_tile_anim(apps, open_files);
	app_tile(apps, LV_SYMBOL_PLAY, "YouTube", M.tertiary_c, M.on_tertiary_c, open_youtube);
	page_tile_anim(apps, open_youtube);
	app_tile(apps, LV_SYMBOL_KEYBOARD, "Games", M.primary_c, M.on_primary_c, open_games);
	page_tile_anim(apps, open_games);
	home_apps_builtin = lv_obj_get_child_count(apps);
	app_tiles(apps);

	home_dots = lv_obj_create(home_scr);
	plain(home_dots);
	lv_obj_set_size(home_dots, PW, PG_DOTS_H);
	lv_obj_set_pos(home_dots, PX, PB - PG_DOTS_H);
	lv_obj_set_style_bg_opa(home_dots, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(home_dots, 0, 0);
	lv_obj_set_style_pad_column(home_dots, 9, 0);
	lv_obj_remove_flag(home_dots, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(home_dots, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	dock_build();	/* (and the pages' layout) */

	build_shade();
	app_bar_build();	/* the bar over applications and pages */

	status_refresh(true);
	home_timer = awake_timer(home_update, 2000, NULL);
	shade_warm_timer = awake_timer(shade_pic_warm, 1000, NULL);
	pg_warm_timer = awake_timer(pg_warm, 1000, NULL);
	home_update(NULL);
	if (app_pid <= 0 && !home_build_quiet)	/* (a turn keeps what shows) */
		lv_screen_load(home_scr);
	/* (ui_reopen: main, once the sound card is known) */
}

/* After a theme or accent change: start afresh (state is in CONF_PATH). */
static void ui_restart(const char *open_page)
{
	conf_save();
	if (open_page)
		setenv("N72_UI_OPEN", open_page, 1);
	if (tty_fd >= 0)
		close(tty_fd);
	mbx_gpu_handoff();	/* the screen keeps this frame until the new one */
	execv("/proc/self/exe", self_argv);
}

/* ------------------------------------------------------------- Wi-Fi */

static lv_obj_t *wifi_page, *wifi_list, *wifi_state_lbl, *wifi_scan_btn_lbl;

static void wifi_page_refresh(void);

static void wifi_cmd_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	wifi_page_refresh();
}

static void wifi_join(const char *ssid, const char *psk)
{
	char q_ssid[160], q_psk[200], cmd[700];

	/* wpa_cli wants the SSID and passphrase in double quotes */
	snprintf(cmd, sizeof(cmd), "\"%s\"", ssid);
	shq(cmd, q_ssid, sizeof(q_ssid));
	if (psk) {
		snprintf(cmd, sizeof(cmd), "\"%s\"", psk);
		shq(cmd, q_psk, sizeof(q_psk));
	}
	snprintf(cmd, sizeof(cmd),
		 "n=$(wpa_cli -i wlan0 add_network | tail -1) && "
		 "wpa_cli -i wlan0 set_network $n ssid %s && "
		 "%s%s%s"
		 "wpa_cli -i wlan0 enable_network $n && wpa_cli -i wlan0 select_network $n && "
		 "wpa_cli -i wlan0 save_config; sleep 6",
		 q_ssid,
		 psk ? "wpa_cli -i wlan0 set_network $n psk " : "wpa_cli -i wlan0 set_network $n key_mgmt NONE",
		 psk ? q_psk : "", " && ");
	label_set_if_changed(wifi_state_lbl, "Connecting...");
	job_run(cmd, wifi_page, wifi_cmd_done, NULL);
}

static lv_obj_t *pw_ta, *pw_scr;
static char pw_ssid[64];

static void pw_done_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_READY) {
		const char *pw = lv_textarea_get_text(pw_ta);

		if (strlen(pw) >= 8)
			wifi_join(pw_ssid, pw);
		go_back();
	} else if (code == LV_EVENT_CANCEL) {
		go_back();
	}
}

static void wifi_ask_password(const char *ssid)
{
	nav_mark(NULL, NULL);	/* (not reopened by a turn) */
	lv_obj_t *body;

	snprintf(pw_ssid, sizeof(pw_ssid), "%s", ssid);
	body = m3_page(ssid, &pw_scr);
	obj_track(&pw_scr);
	m3_label(body, "Password (at least 8 characters)", &lv_font_montserrat_14, M.on_surface_v);
	pw_ta = lv_textarea_create(body);
	obj_track(&pw_ta);
	lv_textarea_set_one_line(pw_ta, true);
	lv_textarea_set_password_mode(pw_ta, true);
	lv_obj_set_width(pw_ta, lv_pct(100));
	lv_obj_set_style_radius(pw_ta, 12, 0);
	lv_obj_set_style_anim_duration(pw_ta, 0, LV_PART_CURSOR);	/* (see the YouTube field) */
	lv_obj_add_event_cb(pw_ta, pw_done_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(pw_ta, pw_done_cb, LV_EVENT_CANCEL, NULL);
	push(pw_scr);
	kbd_attach(pw_ta, NULL, true);	/* the system keyboard, up */
}

struct wifi_net { char ssid[64]; int signal; bool secure; bool saved; int id; };
static struct wifi_net nets[24];
static int n_nets;

static void wifi_net_cb(lv_event_t *e)
{
	struct wifi_net *n = lv_event_get_user_data(e);
	char cmd[96];

	if (n->saved && n->id >= 0) {
		snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 select_network %d; sleep 5", n->id);
		label_set_if_changed(wifi_state_lbl, "Connecting...");
		job_run(cmd, wifi_page, wifi_cmd_done, NULL);
	} else if (n->secure) {
		wifi_ask_password(n->ssid);
	} else {
		wifi_join(n->ssid, NULL);
	}
}

static void wifi_forget_cb(lv_event_t *e)
{
	struct wifi_net *n = lv_event_get_user_data(e);
	char cmd[96];

	if (!n->saved || n->id < 0)
		return;
	snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 remove_network %d && wpa_cli -i wlan0 save_config", n->id);
	job_run(cmd, wifi_page, wifi_cmd_done, NULL);
}

/* saved networks first (the list is cut at 24), then by signal */
static int net_cmp(const void *a, const void *b)
{
	const struct wifi_net *x = a, *y = b;

	if (x->saved != y->saved)
		return y->saved - x->saved;
	return y->signal - x->signal;
}

static void wifi_page_refresh(void);

/* what the list shows now (names, signal bands, flags), "" after a (re)open */
static char wifi_last_sig[24 * 72];

/* a list update put off while it scrolled */
static void wifi_relist_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	wifi_page_refresh();
}

/* scan_results lines: bssid freq signal flags ssid; list_networks: id ssid bssid flags */
static void wifi_lists_done(const char *out, void *ud)
{
	const char *p = out, *saved = strstr(out, "@@SAVED@@");
	int i;

	LV_UNUSED(ud);
	n_nets = 0;
	while (p && *p && (!saved || p < saved) && n_nets < (int)(sizeof(nets) / sizeof(nets[0]))) {
		char bssid[24], flags[128], ssid[64] = "";
		int freq, sig;

		if (sscanf(p, "%23s %d %d %127s %63[^\n]", bssid, &freq, &sig, flags, ssid) >= 4 &&
		    strchr(bssid, ':') && ssid[0]) {
			bool dup = false;

			for (i = 0; i < n_nets; i++)
				if (!strcmp(nets[i].ssid, ssid)) {
					if (sig > nets[i].signal)
						nets[i].signal = sig;
					dup = true;
				}
			if (!dup) {
				struct wifi_net *n = &nets[n_nets++];

				snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
				n->signal = sig;
				n->secure = strstr(flags, "WPA") || strstr(flags, "WEP");
				n->saved = false;
				n->id = -1;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	for (p = saved; p && *p; ) {
		int id;
		char ssid[64];

		if (sscanf(p, "%d\t%63[^\t\n]", &id, ssid) == 2) {
			for (i = 0; i < n_nets; i++)
				if (!strcmp(nets[i].ssid, ssid)) {
					nets[i].saved = true;
					nets[i].id = id;
				}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	qsort(nets, n_nets, sizeof(nets[0]), net_cmp);

	if (!wifi_list || !lv_obj_is_valid(wifi_list))
		return;
	/*
	 * Rebuilding 24 rows takes ~80 ms: not while the list is in use
	 * (scrolling, or touched in the last 1.5 s: try again shortly), and
	 * not at all when nothing shown has changed. The first fill is at once.
	 */
	{
		char sig[sizeof(wifi_last_sig)];
		size_t o = 0;

		if ((sc.owner && lv_obj_get_screen(sc.owner) == lv_obj_get_screen(wifi_list)) ||
		    (wifi_last_sig[0] && lv_display_get_inactive_time(NULL) < 1500)) {
			lv_timer_t *t = lv_timer_create(wifi_relist_cb, 600, NULL);

			lv_timer_set_repeat_count(t, 1);
			return;
		}
		sig[0] = 0;
		for (i = 0; i < n_nets && i < 24 && o < sizeof(sig) - 80; i++)
			o += snprintf(sig + o, sizeof(sig) - o, "%s|%d%d%d%d;", nets[i].ssid,
				      nets[i].signal > -55 ? 3 : nets[i].signal > -67 ? 2 :
				      nets[i].signal > -78 ? 1 : 0, nets[i].secure, nets[i].saved,
				      !strcmp(nets[i].ssid, wifi_ssid) && wifi_up);
		if (!strcmp(sig, wifi_last_sig) && n_nets) {
			if (wifi_scan_btn_lbl && lv_obj_is_valid(wifi_scan_btn_lbl))
				lv_label_set_text(wifi_scan_btn_lbl, "Scan");
			return;
		}
		memcpy(wifi_last_sig, sig, sizeof(sig));
	}
	lv_obj_clean(wifi_list);
	if (!n_nets)
		m3_label(wifi_list, "No networks found yet", &lv_font_montserrat_14, M.on_surface_v);
	/* the network in use first */
	for (i = 1; i < n_nets && wifi_up; i++)
		if (!strcmp(nets[i].ssid, wifi_ssid)) {
			struct wifi_net t = nets[i];

			memmove(&nets[1], &nets[0], i * sizeof(nets[0]));
			nets[0] = t;
			break;
		}
	/* the strongest 24 (sorted): more is only slower to build and scroll */
	for (i = 0; i < n_nets && i < 24; i++) {
		struct wifi_net *n = &nets[i];
		char sub[48];
		const char *bars = n->signal > -55 ? "Excellent" : n->signal > -67 ? "Good" :
				   n->signal > -78 ? "Fair" : "Weak";
		lv_obj_t *r;

		bool cur = !strcmp(n->ssid, wifi_ssid) && wifi_up;

		if (cur)
			snprintf(sub, sizeof(sub), "Connected, %s", bars);
		else
			snprintf(sub, sizeof(sub), "%s%s%s", bars, n->secure ? ", secured" : ", open",
				 n->saved ? ", saved" : "");
		r = m3_row(wifi_list, LV_SYMBOL_WIFI, n->ssid, sub, wifi_net_cb, n, NULL);
		if (cur)
			m3_row_current(r);
		if (n->saved)
			lv_obj_add_event_cb(r, wifi_forget_cb, LV_EVENT_LONG_PRESSED, n);
	}
	if (wifi_scan_btn_lbl && lv_obj_is_valid(wifi_scan_btn_lbl))
		lv_label_set_text(wifi_scan_btn_lbl, "Scan");
}

static void wifi_state_done(const char *out, void *ud)
{
	char ip[32], st[32];

	wifi_status_done(out, ud);
	if (!wifi_state_lbl || !lv_obj_is_valid(wifi_state_lbl))
		return;
	kv_get(out, "wpa_state", st, sizeof(st));
	ip_of("wlan0", ip, sizeof(ip));
	if (wifi_up) {
		char buf[128];

		snprintf(buf, sizeof(buf), "%s\n%s", wifi_ssid, ip);
		label_set_if_changed(wifi_state_lbl, buf);
	} else {
		label_set_if_changed(wifi_state_lbl, !wifi_shown_on() ? "Off" :
				     st[0] ? "Not connected" : "Wi-Fi is not running");
	}
}

static void wifi_page_refresh(void)
{
	if (!wifi_page || !lv_obj_is_valid(wifi_page))
		return;
	job_run("wpa_cli -i wlan0 status", wifi_page, wifi_state_done, NULL);
	job_run("wpa_cli -i wlan0 scan_results; echo @@SAVED@@; wpa_cli -i wlan0 list_networks",
		wifi_page, wifi_lists_done, NULL);
}

static void wifi_scan_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (wifi_scan_btn_lbl)
		lv_label_set_text(wifi_scan_btn_lbl, "Scanning...");
	job_run("wpa_cli -i wlan0 scan; sleep 4", wifi_page, wifi_cmd_done, NULL);
}

static void wifi_switch_cb(lv_event_t *e)
{
	wifi_set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void open_wifi(lv_event_t *e)
{
	nav_mark(open_wifi, NULL);
	lv_obj_t *body, *c, *b;

	LV_UNUSED(e);
	if (!have_tool("wpa_cli")) {
		body = m3_page("Wi-Fi", &wifi_page);
		obj_track(&wifi_page);
		m3_note(m3_card(body, M.surface_c), "wpa_supplicant is not installed.");
		push(wifi_page);
		return;
	}
	body = m3_page("Wi-Fi", &wifi_page);
	obj_track(&wifi_page);
	/* the switch on its own card ... */
	c = m3_card(body, M.primary_c);
	lv_obj_set_height(c, 64);
	lv_obj_align(m3_label(c, "Use Wi-Fi", &lv_font_montserrat_16, M.on_primary_c),
		     LV_ALIGN_LEFT_MID, 0, 0);
	if (have_tool("n72-wifi")) {
		wifi_switch = ios_switch(lv_switch_create(c));
		obj_track(&wifi_switch);
		lv_obj_align(wifi_switch, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_add_event_cb(wifi_switch, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
		switch_show(wifi_switch, wifi_shown_on());
	}
	/* ... and the connection below it */
	c = m3_card(body, M.surface_c);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 2, 0);
	m3_label(c, "Connection", &lv_font_montserrat_14, M.on_surface_v);
	wifi_state_lbl = m3_label(c, "...", &lv_font_montserrat_16, M.on_surface);
	obj_track(&wifi_state_lbl);
	lv_obj_set_width(wifi_state_lbl, lv_pct(100));
	lv_label_set_long_mode(wifi_state_lbl, LV_LABEL_LONG_WRAP);

	m3_section(body, "Networks");
	wifi_last_sig[0] = 0;
	wifi_list = lv_obj_create(body);
	obj_track(&wifi_list);
	plain(wifi_list);
	lv_obj_set_width(wifi_list, lv_pct(100));
	lv_obj_set_height(wifi_list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(wifi_list, M.surface_c, 0);
	lv_obj_set_style_radius(wifi_list, 20, 0);
	lv_obj_set_style_pad_all(wifi_list, 6, 0);
	lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);
	m3_label(wifi_list, "Looking for networks...", &lv_font_montserrat_14, M.on_surface_v);
	b = m3_button(body, "Scan", false, wifi_scan_cb, NULL);
	wifi_scan_btn_lbl = lv_obj_get_child(b, 0);
	obj_track(&wifi_scan_btn_lbl);
	m3_note(body, "Long-press a saved network to forget it.");
	push(wifi_page);
	job_run("wpa_cli -i wlan0 scan; sleep 3", wifi_page, wifi_cmd_done, NULL);
	wifi_page_refresh();
}

/* ---------------------------------------------------------- Bluetooth */

static lv_obj_t *bt_page, *bt_list, *bt_switch, *bt_scan_lbl, *bt_state_lbl;

static void bt_page_refresh(void);

static void bt_cmd_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bt_page_refresh();
}

struct bt_dev { char mac[18]; char name[64]; bool paired, connected; };
static struct bt_dev bdevs[16];
static int n_bdevs;
static char bt_list_sig[16 * 24];	/* what the list shows: rebuilt only on a change */

/*
 * Connect automatically (a device's page): the paired devices the iPod
 * reaches for itself (bt_auto_tick), on unless turned off. Kept on the
 * flash: /var/lib/n72/bt-auto, a line "MAC off" for each one turned off.
 */
#define BT_AUTO_PATH "/var/lib/n72/bt-auto"

/*
 * Automatic and manual connects take turns: two pages at once make the
 * controller refuse the second ("br-connection-busy", then BlueZ's
 * "br-connection-unknown"). The automatic run (bt_auto_tick) holds
 * BT_AUTO_RUN while it pages and gives up between devices as soon as
 * BT_MANUAL_RUN appears; a manual connect sets BT_MANUAL_RUN and waits for
 * the device being paged to finish (8 s at most) before it starts.
 */
#define BT_AUTO_RUN "/run/n72-bt-auto"
#define BT_MANUAL_RUN "/run/n72-bt-manual"
#define BT_MANUAL(cmd) "touch " BT_MANUAL_RUN "; i=0; while [ -e " BT_AUTO_RUN " ] && " \
	"[ $i -lt 40 ]; do sleep 0.25; i=$((i+1)); done; " cmd "; r=$?; rm -f " BT_MANUAL_RUN "; exit $r"

/*
 * Devices the user disconnected: not reconnected automatically until they
 * connect again (by a tap, or by themselves) or Bluetooth is switched off.
 */
#define BT_USER_OFF_MAX 8
static char bt_user_off[BT_USER_OFF_MAX][18];

static bool bt_user_off_has(const char *mac)
{
	for (int i = 0; i < BT_USER_OFF_MAX; i++)
		if (bt_user_off[i][0] && !strcasecmp(bt_user_off[i], mac))
			return true;
	return false;
}

static void bt_user_off_set(const char *mac, bool off)
{
	int free_i = -1;

	for (int i = 0; i < BT_USER_OFF_MAX; i++) {
		if (bt_user_off[i][0] && !strcasecmp(bt_user_off[i], mac)) {
			if (!off)
				bt_user_off[i][0] = 0;
			return;
		}
		if (!bt_user_off[i][0] && free_i < 0)
			free_i = i;
	}
	if (off)
		snprintf(bt_user_off[free_i >= 0 ? free_i : 0], 18, "%s", mac);
}

static bool bt_auto_get(const char *mac)
{
	char line[64];
	bool on = true;
	FILE *f = fopen(BT_AUTO_PATH, "r");

	while (f && fgets(line, sizeof(line), f))
		if (!strncasecmp(line, mac, 17) && strstr(line, " off"))
			on = false;
	if (f)
		fclose(f);
	return on;
}

static void bt_auto_set(const char *mac, bool on)
{
	char line[64], buf[2048] = "";
	size_t o = 0;
	FILE *f = fopen(BT_AUTO_PATH, "r");

	while (f && fgets(line, sizeof(line), f))
		if (strncasecmp(line, mac, 17) && o + strlen(line) < sizeof(buf)) {
			memcpy(buf + o, line, strlen(line));
			o += strlen(line);
		}
	if (f)
		fclose(f);
	buf[o] = 0;
	mkdir("/var/lib/n72", 0755);
	f = fopen(BT_AUTO_PATH ".new", "w");
	if (!f)
		return;
	fputs(buf, f);
	if (!on)
		fprintf(f, "%s off\n", mac);
	fclose(f);
	rename(BT_AUTO_PATH ".new", BT_AUTO_PATH);
}

static void open_bt_dev_cb(lv_event_t *e);

static void bt_dev_cb(lv_event_t *e)
{
	struct bt_dev *d = lv_event_get_user_data(e);
	char cmd[400];

	/*
	 * Pairing needs an agent: bluetoothctl's own, without input or output
	 * (headphones and speakers pair "just works"; one asking for a PIN
	 * fails here). Audio devices connect only when bluez-alsa (bluealsa)
	 * runs: it is what offers BlueZ the A2DP profile.
	 */
	bt_user_off_set(d->mac, d->connected);
	if (d->connected)
		snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s", d->mac);
	else if (d->paired)
		snprintf(cmd, sizeof(cmd), BT_MANUAL("bluetoothctl --timeout 20 connect %s"), d->mac);
	else
		snprintf(cmd, sizeof(cmd), BT_MANUAL("bluetoothctl --agent NoInputNoOutput --timeout 25 "
			 "pair %s && bluetoothctl trust %s && bluetoothctl --timeout 20 connect %s"),
			 d->mac, d->mac, d->mac);
	{
		/* the row says what is going on (its second line) */
		lv_obj_t *col = lv_obj_get_child(lv_event_get_current_target(e), 1);
		lv_obj_t *sub = col ? lv_obj_get_child(col, 1) : NULL;

		if (sub) {
			lv_label_set_text(sub, d->connected ? "Disconnecting..." : "Connecting...");
			lv_obj_remove_flag(sub, LV_OBJ_FLAG_HIDDEN);
		}
	}
	job_run(cmd, bt_page, bt_cmd_done, NULL);
}

static void bt_forget_cb(lv_event_t *e)
{
	struct bt_dev *d = lv_event_get_user_data(e);
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "bluetoothctl remove %s", d->mac);
	job_run(cmd, bt_page, bt_cmd_done, NULL);
}

/* "Device AA:BB:CC:DD:EE:FF Name"; the paired list follows @@PAIRED@@ */
static void bt_devs_done(const char *out, void *ud)
{
	const char *p, *paired = strstr(out, "@@PAIRED@@"), *conn = strstr(out, "@@CONN@@");
	int i;

	LV_UNUSED(ud);
	n_bdevs = 0;
	for (p = out; p && *p && n_bdevs < (int)(sizeof(bdevs) / sizeof(bdevs[0])); ) {
		char mac[18], name[64];

		if (sscanf(p, "Device %17s %63[^\n]", mac, name) == 2) {
			bool is_conn = conn && p > conn;
			bool is_paired = paired && p > paired && !is_conn;
			bool dup = false;

			for (i = 0; i < n_bdevs; i++)
				if (!strcmp(bdevs[i].mac, mac)) {
					bdevs[i].paired |= is_paired;
					bdevs[i].connected |= is_conn;
					dup = true;
				}
			if (!dup && !is_conn) {
				snprintf(bdevs[n_bdevs].mac, 18, "%s", mac);
				snprintf(bdevs[n_bdevs].name, 64, "%s", name);
				bdevs[n_bdevs].paired = is_paired;
				bdevs[n_bdevs].connected = false;
				n_bdevs++;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!bt_list || !lv_obj_is_valid(bt_list))
		return;
	/* the connected device first */
	for (i = 1; i < n_bdevs; i++)
		if (bdevs[i].connected) {
			struct bt_dev t = bdevs[i];

			memmove(&bdevs[1], &bdevs[0], i * sizeof(bdevs[0]));
			bdevs[0] = t;
			break;
		}
	{
		/* (refreshed every few seconds: rebuilt only when it changed) */
		char sig[sizeof(bt_list_sig)];
		size_t o = 0;

		sig[0] = 0;
		for (i = 0; i < n_bdevs && o < sizeof(sig) - 24; i++)
			o += snprintf(sig + o, sizeof(sig) - o, "%.17s%d%d%d;", bdevs[i].mac,
				      bdevs[i].paired, bdevs[i].connected, (int)strlen(bdevs[i].name));
		if (o < sizeof(sig) - 2)
			snprintf(sig + o, sizeof(sig) - o, "%d", bt_powered);
		if (!strcmp(sig, bt_list_sig) && lv_obj_get_child_count(bt_list)) {
			if (bt_scan_lbl && lv_obj_is_valid(bt_scan_lbl))
				lv_label_set_text(bt_scan_lbl, "Scan");
			return;
		}
		memcpy(bt_list_sig, sig, sizeof(sig));
	}
	lv_obj_clean(bt_list);
	if (!n_bdevs)
		m3_label(bt_list, bt_powered ? "No devices yet: tap Scan" : "Bluetooth is off",
			 &lv_font_montserrat_14, M.on_surface_v);
	for (i = 0; i < n_bdevs; i++) {
		lv_obj_t *r = m3_row_ex(bt_list, LV_SYMBOL_BLUETOOTH, M.secondary_c, M.on_secondary_c,
					bdevs[i].name, bdevs[i].connected ? "Connected" :
					bdevs[i].paired ? "Paired" : "Tap to pair",
					bt_dev_cb, &bdevs[i], NULL, false);

		if (bdevs[i].paired || bdevs[i].connected) {
			/* the device's own page: a gear at the end of the row */
			lv_obj_t *g = lv_button_create(r);

			plain(g);
			lv_obj_set_size(g, 40, 40);
			lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
			lv_obj_set_style_bg_color(g, M.on_surface, LV_STATE_PRESSED);
			lv_obj_set_style_bg_opa(g, LV_OPA_10, LV_STATE_PRESSED);
			lv_obj_set_style_shadow_width(g, 0, 0);
			lv_obj_center(m3_label(g, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16,
					       M.on_surface_v));
			lv_obj_add_event_cb(g, open_bt_dev_cb, LV_EVENT_CLICKED, bdevs[i].mac);
		}
		if (bdevs[i].connected)
			m3_row_current(r);
		if (bdevs[i].paired || bdevs[i].connected)
			lv_obj_add_event_cb(r, bt_forget_cb, LV_EVENT_LONG_PRESSED, &bdevs[i]);
	}
	if (bt_scan_lbl && lv_obj_is_valid(bt_scan_lbl))
		lv_label_set_text(bt_scan_lbl, "Scan");
}

static void bt_show_page_done(const char *out, void *ud)
{
	bt_show_done(out, ud);
	net_ui_sync();
}

static void bt_page_refresh(void)
{
	if (!bt_page || !lv_obj_is_valid(bt_page))
		return;
	job_run("bluetoothctl show", bt_page, bt_show_page_done, NULL);
	job_run("bluetoothctl devices; echo @@PAIRED@@; bluetoothctl devices Paired; "
		"echo @@CONN@@; bluetoothctl devices Connected", bt_page, bt_devs_done, NULL);
}

/*
 * Off unbinds the controller's serial device as well: hci_bcm then holds
 * the BCM4325's Bluetooth side in shutdown, and the UART's clock gates
 * close (a powered-off but bound controller keeps the UART clocked). On
 * binds it again (firmware setup, ~3 s) and powers it once BlueZ has it.
 */
#define BT_SERDEV	"/sys/bus/serial/drivers/hci_uart_bcm"
#define BT_OFF_CMD	"bluetoothctl power off; [ -e " BT_SERDEV "/serial0-0 ] && " \
			"echo -n serial0-0 > " BT_SERDEV "/unbind"
#define BT_ON_CMD	"[ -e " BT_SERDEV "/serial0-0 ] || " \
			"echo -n serial0-0 > " BT_SERDEV "/bind; n=0; " \
			"until bluetoothctl show 2>/dev/null | grep -q Powered || [ $n -ge 20 ]; " \
			"do sleep 0.5; n=$((n+1)); done; bluetoothctl power on"

/* the switch is done when BlueZ says so: its answer ends the switch */
static void bt_set_shown(const char *out, void *ud)
{
	bt_show_done(out, ud);
	bt_want = -1;
	net_ui_sync();
	bt_page_refresh();
}

static void bt_set_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	job_run("bluetoothctl show", NULL, bt_set_shown, NULL);
}

/* Bluetooth on or off from any switch; a second one waits for the first. */
static void bt_set(bool on)
{
	if (bt_want >= 0) {
		net_ui_sync();
		return;
	}
	if (conf.bt != on) {
		conf.bt = on;
		conf_save();
	}
	if (on == bt_powered)
		return;
	bt_want = on;
	net_ui_sync();
	job_run(on ? BT_ON_CMD : BT_OFF_CMD, NULL, bt_set_done, NULL);
}

static void bt_switch_cb(lv_event_t *e)
{
	bt_set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void bt_scan_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (bt_scan_lbl)
		lv_label_set_text(bt_scan_lbl, "Scanning...");
	job_run("bluetoothctl --timeout 8 scan on", bt_page, bt_cmd_done, NULL);
}

static void open_bt(lv_event_t *e)
{
	nav_mark(open_bt, NULL);
	lv_obj_t *body, *c, *b;

	LV_UNUSED(e);
	body = m3_page("Bluetooth", &bt_page);
	obj_track(&bt_page);
	if (!have_tool("bluetoothctl")) {
		c = m3_card(body, M.surface_c);
		m3_note(c, "BlueZ (bluetoothctl) is not installed.");
		push(bt_page);
		return;
	}
	/* the switch alone (as Wi-Fi's); the device in use is in the list */
	c = m3_card(body, M.primary_c);
	lv_obj_set_height(c, 64);
	lv_obj_align(m3_label(c, "Use Bluetooth", &lv_font_montserrat_16, M.on_primary_c),
		     LV_ALIGN_LEFT_MID, 0, 0);
	bt_switch = ios_switch(lv_switch_create(c));
	obj_track(&bt_switch);
	lv_obj_align(bt_switch, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(bt_switch, bt_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
	net_ui_sync();

	m3_section(body, "Devices");
	bt_list = lv_obj_create(body);
	obj_track(&bt_list);
	plain(bt_list);
	lv_obj_set_width(bt_list, lv_pct(100));
	lv_obj_set_height(bt_list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(bt_list, M.surface_c, 0);
	lv_obj_set_style_radius(bt_list, 20, 0);
	lv_obj_set_style_pad_all(bt_list, 6, 0);
	lv_obj_set_flex_flow(bt_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(bt_list, LV_OBJ_FLAG_SCROLLABLE);
	b = m3_button(body, "Scan", false, bt_scan_cb, NULL);
	bt_scan_lbl = lv_obj_get_child(b, 0);
	obj_track(&bt_scan_lbl);
	m3_note(body, "Tap a device to connect; its gear for details.");
	bt_list_sig[0] = 0;
	push(bt_page);
	bt_page_refresh();
	/* (live: BlueZ's signals read it again, bz_ev) */
}

/* --------------------------------------------------- a Bluetooth device */

/*
 * A device's page: name, status, address, the sound codec while it plays
 * to it; Connect or Disconnect, Connect automatically, Forget. Live while
 * shown (bluetoothctl info every 2 s).
 */
static char btd_mac[18];
static lv_obj_t *btd_page, *btd_card, *btd_status, *btd_codec_row, *btd_codec,
		*btd_conn_lbl, *btd_auto_sw;
static bool btd_connected;

static void btd_show(bool connected, const char *codec)
{
	btd_connected = connected;
	if (btd_status)
		label_set_if_changed(btd_status, connected ? "Connected" : "Not connected");
	if (btd_conn_lbl)
		label_set_if_changed(btd_conn_lbl, connected ? "Disconnect" : "Connect");
	if (btd_card)
		lv_obj_set_style_bg_color(btd_card, connected ? M.primary_c : M.surface_c, 0);
	if (btd_codec_row) {
		if (connected && codec && *codec) {
			label_set_if_changed(btd_codec, codec);
			lv_obj_remove_flag(btd_codec_row, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(btd_codec_row, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

static void btd_info_done(const char *out, void *ud)
{
	const char *c = strstr(out, "Selected codec:");
	char codec[32] = "";

	LV_UNUSED(ud);
	if (c && sscanf(c, "Selected codec: %31[^\n]", codec) != 1)
		codec[0] = 0;
	btd_show(strstr(out, "Connected: yes") != NULL, codec);
}

static void btd_refresh(void)
{
	char cmd[300], und[18];
	int i;

	if (!btd_page || !lv_obj_is_valid(btd_page))
		return;
	for (i = 0; i < 17; i++)
		und[i] = btd_mac[i] == ':' ? '_' : btd_mac[i];
	und[17] = 0;
	snprintf(cmd, sizeof(cmd), "bluetoothctl info %s; p=$(bluealsa-cli list-pcms | "
		 "grep 'dev_%s/a2dpsrc/sink$' | head -1); [ -n \"$p\" ] && bluealsa-cli info \"$p\"",
		 btd_mac, und);
	job_run(cmd, btd_page, btd_info_done, NULL);
}

static void btd_cmd_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	btd_refresh();
}

static void btd_conn_cb(lv_event_t *e)
{
	char cmd[256];

	LV_UNUSED(e);
	bt_user_off_set(btd_mac, btd_connected);
	snprintf(cmd, sizeof(cmd), btd_connected ? "bluetoothctl disconnect %s" :
		 BT_MANUAL("bluetoothctl --timeout 20 connect %s"), btd_mac);
	if (btd_status)
		lv_label_set_text(btd_status, btd_connected ? "Disconnecting..." : "Connecting...");
	job_run(cmd, btd_page, btd_cmd_done, NULL);
}

static void btd_auto_cb(lv_event_t *e)
{
	bt_auto_set(btd_mac, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void btd_forget_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bt_auto_set(btd_mac, true);	/* (no note of a device that is gone) */
	if (btd_page && lv_obj_is_valid(btd_page) && lv_screen_active() == btd_page)
		go_back();
}

static void btd_forget_cb(lv_event_t *e)
{
	char cmd[64];

	LV_UNUSED(e);
	snprintf(cmd, sizeof(cmd), "bluetoothctl remove %s", btd_mac);
	job_run(cmd, NULL, btd_forget_done, NULL);
}

static void open_bt_dev(lv_event_t *e)
{
	nav_mark(open_bt_dev, NULL);	/* (reopened on a turn: btd_mac stays) */
	lv_obj_t *body, *c, *col, *row, *b;
	const char *name = btd_mac;
	bool connected = false;
	int i;

	LV_UNUSED(e);
	for (i = 0; i < n_bdevs; i++)
		if (!strcmp(bdevs[i].mac, btd_mac)) {
			name = bdevs[i].name;
			connected = bdevs[i].connected;
		}
	body = m3_page("Device", &btd_page);	/* (the card has the name) */
	obj_track(&btd_page);

	/* the device: badge, name, status, address */
	c = btd_card = m3_card(body, connected ? M.primary_c : M.surface_c);
	obj_track(&btd_card);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 16, 0);
	m3_badge(c, LV_SYMBOL_BLUETOOTH, M.primary, M.on_primary, 56);
	col = lv_obj_create(c);
	plain(col);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	b = m3_label(col, name, &lv_font_montserrat_20, M.on_surface);
	lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
	lv_obj_set_width(b, lv_pct(100));
	btd_status = m3_label(col, connected ? "Connected" : "Not connected",
			      &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&btd_status);
	m3_label(col, btd_mac, &lv_font_montserrat_12, M.on_surface_v);

	/* settings */
	c = m3_card(body, M.surface_c);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(c, 4, 0);
	row = m3_row_ex(c, LV_SYMBOL_REFRESH, M.secondary_c, M.on_secondary_c,
			"Auto-connect", "When it is in reach", NULL, NULL, NULL, false);
	btd_auto_sw = ios_switch(lv_switch_create(row));
	obj_track(&btd_auto_sw);
	if (bt_auto_get(btd_mac))
		lv_obj_add_state(btd_auto_sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(btd_auto_sw, btd_auto_cb, LV_EVENT_VALUE_CHANGED, NULL);
	btd_codec_row = m3_row_ex(c, LV_SYMBOL_AUDIO, M.tertiary_c, M.on_tertiary_c,
				  "Sound", "", NULL, NULL, &btd_codec, false);
	obj_track(&btd_codec_row);
	obj_track(&btd_codec);
	lv_obj_add_flag(btd_codec_row, LV_OBJ_FLAG_HIDDEN);

	/* actions */
	row = lv_obj_create(body);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 12, 0);
	lv_obj_set_style_pad_top(row, 4, 0);
	b = m3_button(row, connected ? "Disconnect" : "Connect", true, btd_conn_cb, NULL);
	btd_conn_lbl = lv_obj_get_child(b, 0);
	obj_track(&btd_conn_lbl);
	m3_button(row, "Forget", false, btd_forget_cb, NULL);

	btd_connected = connected;
	push(btd_page);
	btd_refresh();		/* (then on BlueZ's signals, bz_ev) */
}

static void open_bt_dev_cb(lv_event_t *e)
{
	snprintf(btd_mac, sizeof(btd_mac), "%s", (const char *)lv_event_get_user_data(e));
	open_bt_dev(NULL);
}

/*
 * Connect automatically: while the screen is on, Bluetooth is on and no
 * sound device is connected, the paired devices left on are tried in turn
 * every 30 s (a speaker switched on is found within that; one that is off
 * costs a page of the radio, ~5 s). n72-audio-route has the sound follow.
 */
static bool bt_auto_busy;
/*
 * Not a fixed 30 s: after each try that finds nothing the next waits twice
 * as long (30 s to 5 min: a page of a device that is off costs the radio
 * ~5 s); the screen waking, Bluetooth switched on and a sound device going
 * start again from 2 s and 30 s (bt_auto_kick).
 */
#define BT_AUTO_MIN_MS	30000
#define BT_AUTO_MAX_MS	300000
static lv_timer_t *bt_auto_timer;
static uint32_t bt_auto_gap = BT_AUTO_MIN_MS;

static void bt_auto_kick(void)
{
	bt_auto_gap = BT_AUTO_MIN_MS;
	if (bt_auto_timer) {
		lv_timer_set_period(bt_auto_timer, 2000);
		lv_timer_reset(bt_auto_timer);
	}
}

static void bt_auto_try_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bt_auto_busy = false;
	if (bt_page && lv_obj_is_valid(bt_page))
		bt_page_refresh();
}

static void bt_auto_paired_done(const char *out, void *ud)
{
	char cmd[1024], mac[18];
	const char *p;
	size_t o;
	bool connected = false;
	int n = 0;

	LV_UNUSED(ud);
	o = (size_t)snprintf(cmd, sizeof(cmd), "touch " BT_AUTO_RUN "; ");
	for (p = out; p && *p; ) {
		if (sscanf(p, "C Device %17s", mac) == 1) {
			/* connected, by a tap or by itself: automatic again later */
			bt_user_off_set(mac, false);
			connected = true;
		} else if (sscanf(p, "Device %17s", mac) == 1 && bt_auto_get(mac) &&
			   !bt_user_off_has(mac) && o < sizeof(cmd) - 160) {
			/* (each device: not if a manual connect is waiting) */
			o += snprintf(cmd + o, sizeof(cmd) - o, "%s{ [ -e " BT_MANUAL_RUN " ] || "
				      "bluetoothctl --timeout 8 connect %s | grep -q 'Connection successful'; }",
				      n++ ? " || " : "", mac);
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (connected)
		bt_auto_gap = BT_AUTO_MIN_MS;
	if (connected || !n) {
		bt_auto_busy = false;
		return;
	}
	snprintf(cmd + o, sizeof(cmd) - o, "; rm -f " BT_AUTO_RUN);
	if (job_run(cmd, NULL, bt_auto_try_done, NULL))
		bt_auto_busy = false;
}

static void bt_auto_tick(lv_timer_t *t)
{
	if (t->period != bt_auto_gap)
		lv_timer_set_period(t, bt_auto_gap);
	if (!bt_powered)
		memset(bt_user_off, 0, sizeof(bt_user_off));	/* (off and on: a fresh start) */
	if (bt_auto_busy || screen_off || locked || !bt_powered || bt_want >= 0 ||
	    !access("/run/n72-audio", F_OK) || !access(BT_MANUAL_RUN, F_OK) ||
	    !have_tool("bluetoothctl"))
		return;
	bt_auto_busy = true;
	if (getenv("N72_STATUS_LOG"))
		fprintf(stderr, "n72-ui: connect automatically: a try (next in %u s)\n",
			(unsigned)bt_auto_gap / 1000);
	bt_auto_gap = LV_MIN(bt_auto_gap * 2, BT_AUTO_MAX_MS);	/* (a find sets it back) */
	/* paired, and not connected to anything else of ours ("C " lines: connected) */
	if (job_run("c=$(bluetoothctl devices Connected | grep Device); "
		    "if [ -n \"$c\" ]; then echo \"$c\" | sed 's/^/C /'; else bluetoothctl devices Paired; fi",
		    NULL, bt_auto_paired_done, NULL))
		bt_auto_busy = false;
}

/* ------------------------------------------------------------ Display */

static lv_obj_t *bright_val;

static void bright_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));
	char buf[16];

	backlight_set(v);
	conf.bright = v;
	conf_save_later();
	lv_slider_set_value(home_bright, v, LV_ANIM_OFF);
	shade_changed(home_bright);	/* (the panel's picture is taken again: no jump when pulled) */
	snprintf(buf, sizeof(buf), "%d%%", v);
	label_set_if_changed(bright_val, buf);
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
		conf_save();
	}
}

static void plug_cb(lv_event_t *e)
{
	conf.wake_plug = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	conf_save();
}

static void open_display(lv_event_t *e)
{
	nav_mark(open_display, NULL);
	static const char *const theme_map[] = { "Dark", "Light", "" };
	lv_obj_t *scr, *body, *c, *s, *bm, *row, *dd;
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
	lv_slider_set_range(s, 5, 100);
	lv_slider_set_value(s, backlight_get(), LV_ANIM_OFF);
	lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	bright_val = slider_value(c);
	obj_track(&bright_val);
	lv_label_set_text_fmt(bright_val, "%d%%", backlight_get());

	m3_section(body, "Theme");
	bm = lv_buttonmatrix_create(body);
	lv_buttonmatrix_set_map(bm, theme_map);
	lv_buttonmatrix_set_one_checked(bm, true);
	lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
	lv_buttonmatrix_set_button_ctrl(bm, conf.dark ? 0 : 1, LV_BUTTONMATRIX_CTRL_CHECKED);
	lv_obj_set_size(bm, lv_pct(100), 52);
	plain(bm);
	lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(bm, 0, 0);
	lv_obj_set_style_radius(bm, LV_RADIUS_CIRCLE, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(bm, M.surface_c_high, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(bm, M.secondary_c, LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_text_color(bm, M.on_surface, LV_PART_ITEMS);
	lv_obj_set_style_text_color(bm, M.on_secondary_c, LV_PART_ITEMS | LV_STATE_CHECKED);
	/* the default theme gives the buttons a shadow (a halo on light) */
	lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
	lv_obj_set_style_outline_width(bm, 0, LV_PART_ITEMS);
	lv_obj_add_event_cb(bm, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
	m3_note(body, "The screen locks when it turns off. On battery the iPod sleeps after the "
		"timeout; on USB power only the screen turns off.");

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

	if (what == 3) {
		go_home();
		screen_off_or_sleep();
		return;
	}
	sync();
	reboot(what == 1 ? RB_POWER_OFF : RB_AUTOBOOT);
}

static void open_power(lv_event_t *e)
{
	nav_mark(open_power, NULL);
	lv_obj_t *scr, *body;

	LV_UNUSED(e);
	body = m3_page("Power", &scr);
	body = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(body, 4, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	m3_row(body, LV_SYMBOL_EYE_CLOSE, "Sleep now", "Power or Home wakes it",
	       power_do, (void *)3, NULL);
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
		lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
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

static void about_update(lv_timer_t *t)
{
	char buf[160], kernel[96] = "?", line[128], usb[32], wlan[32];
	long mem_total = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
	struct utsname u;
	struct sysinfo si;
	FILE *f;
	int cap, mv, ilim;

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
	snprintf(buf, sizeof(buf), "%d%%, %d.%02d V%s", cap, mv / 1000, mv % 1000 / 10,
		 usb_powered() ? "" : ", on battery");
	if (usb_powered())
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\nUSB power, %d mA limit", ilim);
	label_set_if_changed(about_val[4], buf);
	ip_of("usb0", usb, sizeof(usb));
	ip_of("wlan0", wlan, sizeof(wlan));
	snprintf(buf, sizeof(buf), "USB: %s\nWi-Fi: %s", usb, wlan);
	label_set_if_changed(about_val[5], buf);
}

static void about_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (about_timer)
		lv_timer_delete(about_timer);
	about_timer = NULL;
}

static void open_about(lv_event_t *e)
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
	about_timer = lv_timer_create(about_update, 2000, NULL);
	lv_obj_add_event_cb(scr, about_delete_cb, LV_EVENT_DELETE, NULL);
	push(scr);
}

/* ----------------------------------------------------------- Settings */

/* -------------------------------------------------------------- sound */

/*
 * Volume keys change the sound volume and show a pop-up on the top layer;
 * the jack decides between earphones and speaker (audio.c).
 */
static lv_obj_t *vol_hud, *vol_hud_slider, *vol_hud_icon;
static lv_timer_t *vol_hud_timer;
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

static void vol_hud_opa(void *obj, int32_t v)
{
	lv_obj_set_style_opa(obj, v, 0);
}

static void vol_hud_gone(lv_anim_t *a)
{
	lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

/* Material motion: fade in 150 ms, out 150 ms. */
static void vol_hud_fade(bool in)
{
	lv_anim_t a;

	lv_anim_delete(vol_hud, vol_hud_opa);
	lv_anim_init(&a);
	lv_anim_set_var(&a, vol_hud);
	lv_anim_set_exec_cb(&a, vol_hud_opa);
	lv_anim_set_values(&a, lv_obj_get_style_opa(vol_hud, 0), in ? LV_OPA_COVER : LV_OPA_TRANSP);
	lv_anim_set_duration(&a, 150);
	lv_anim_set_path_cb(&a, in ? ios_path_ease_out : ios_path_ease_in);
	if (!in)
		lv_anim_set_completed_cb(&a, vol_hud_gone);
	lv_anim_start(&a);
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
	if (vol_hud)
		vol_hud_fade(false);
}

static void snd_refresh_ui(void)
{
	char buf[64];

	if (vol_hud && !lv_obj_has_flag(vol_hud, LV_OBJ_FLAG_HIDDEN)) {
		if (!lv_obj_has_state(vol_hud_slider, LV_STATE_PRESSED))
			lv_slider_set_value(vol_hud_slider, conf.vol, LV_ANIM_OFF);
		label_set_if_changed(vol_hud_icon, !conf.vol ? LV_SYMBOL_MUTE :
				     conf.vol < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
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
static void vol_hud_show(void);

static void vol_hud_slider_cb(lv_event_t *e)
{
	volume_set(lv_slider_get_value(lv_event_get_target(e)), false);
	vol_hud_show();		/* keeps it up while dragged */
}

/*
 * The volume pop-up: the brightness control's look (a full-width pill in
 * surface container highest, the icon, a slider), raised over the page by
 * a shadow, below the status bar. The slider can be dragged too.
 */
static void vol_hud_show(void)
{
	if (!vol_hud) {
		vol_hud = lv_obj_create(lv_layer_top());
		plain(vol_hud);
		lv_obj_set_size(vol_hud, W - 2 * STATUS_PAD, 56);
		lv_obj_align(vol_hud, LV_ALIGN_TOP_MID, 0, 36);
		lv_obj_set_style_radius(vol_hud, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(vol_hud, M.surface_c_highest, 0);
		lv_obj_set_style_bg_opa(vol_hud, LV_OPA_COVER, 0);
		lv_obj_set_style_shadow_width(vol_hud, 16, 0);
		lv_obj_set_style_shadow_offset_y(vol_hud, 4, 0);
		lv_obj_set_style_shadow_opa(vol_hud, LV_OPA_30, 0);
		lv_obj_set_style_pad_hor(vol_hud, 18, 0);
		lv_obj_set_style_opa(vol_hud, LV_OPA_TRANSP, 0);
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
	if (lv_obj_has_flag(vol_hud, LV_OBJ_FLAG_HIDDEN) ||
	    lv_anim_get(vol_hud, vol_hud_opa)) {
		lv_obj_remove_flag(vol_hud, LV_OBJ_FLAG_HIDDEN);
		vol_hud_fade(true);
	}
	lv_obj_move_foreground(vol_hud);
	snd_refresh_ui();
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
static void volume_step(int dir)
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
}

static void snd_spk_cb(lv_event_t *e)
{
	conf.spk = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	snd_apply_output(conf.spk);
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

static void open_sound(lv_event_t *e)
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
	m3_note(body, "The volume keys change the volume everywhere (in Logs they scroll).");
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

/* /run changed: n72-audio-route's file (a Bluetooth sound device came or went). */
static void run_ev(int fd)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool ours = false;
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;

		while (p < buf + n) {
			struct inotify_event *ev = (struct inotify_event *)p;

			if (ev->len && !strcmp(ev->name, "n72-audio"))
				ours = true;
			p += sizeof(*ev) + ev->len;
		}
	}
	if (ours && bta_check()) {
		snd_refresh_ui();
		net_ui_sync();	/* the Bluetooth tile and row: the device's name or "On" */
	}
}

/*
 * Events, not looking every 200 ms: the jack's input device, and an
 * inotify watch on /run (n72-audio-route writes /run/n72-audio by a
 * rename and removes it), both in the main loop's poll().
 */
static void sound_ui_init(void)
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

/*
 * Settings > Refresh app list: n72-ios-tiles writes a manifest for every
 * iOS application there is (and removes those of applications gone); it
 * runs once at boot and otherwise only when asked here (nothing watches
 * the applications for it). Exit 10: something changed, the tiles are made
 * again (the apps folder's watch does that too; this does not wait for it).
 */
#define IOS_TILES "/usr/sbin/n72-ios-tiles"
static lv_obj_t *apps_refresh_sub;
static bool apps_refreshing;

static void apps_refresh_done(const char *out, void *ud)
{
	int st = out && *out ? atoi(out) : -1;

	LV_UNUSED(ud);
	apps_refreshing = false;
	if (st == 10)
		apps_reload();
	if (apps_refresh_sub)
		label_set_if_changed(apps_refresh_sub, st == 10 ? "Updated" :
				     st == 0 ? "Up to date" : "Could not look");
}

static void apps_refresh_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (apps_refreshing)
		return;
	apps_refreshing = true;
	if (apps_refresh_sub)
		label_set_if_changed(apps_refresh_sub, "Looking for apps...");
	job_run(IOS_TILES " >/dev/null 2>&1; echo $?", NULL, apps_refresh_done, NULL);
}

#include "wallpaper.c"	/* wallpapers: conversion, frames, the Settings page */
#include "datetime.c"	/* Settings > Date & Time: the zone, network or set time */

static void open_settings(lv_event_t *e)
{
	nav_mark(open_settings, NULL);
	lv_obj_t *scr, *body, *c;
	char sub[80];

	LV_UNUSED(e);
	body = m3_page("Settings", &scr);
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
	m3_row(c, LV_SYMBOL_POWER, "Power", "Sleep, restart, power off", open_power, NULL, NULL);
	m3_row(c, LV_SYMBOL_BELL, "Date & Time", dt_zone_name(), open_datetime, NULL, NULL);
	m3_row(c, LV_SYMBOL_GPS, "Weather", conf.wx_set ? conf.wx_name : "Off", open_weather, NULL, NULL);

	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	{
		struct statvfs v;

		if (!statvfs("/", &v))
			storage_fmt(sub, sizeof(sub), (uint64_t)v.f_bavail * v.f_frsize, " free");
		else
			snprintf(sub, sizeof(sub), "Space used and free");
		m3_row(c, LV_SYMBOL_SD_CARD, "Storage", sub, open_storage, NULL, NULL);
	}
	m3_row(c, LV_SYMBOL_EYE_OPEN, "About", "Device, memory, battery",
	       open_about, NULL, NULL);
	m3_row(c, LV_SYMBOL_FILE, "Logs", "Kernel messages", open_logs, NULL, NULL);
	if (!access(IOS_TILES, X_OK)) {
		m3_row_ex(c, LV_SYMBOL_REFRESH, M.secondary_c, M.on_secondary_c,
			  "Refresh app list", apps_refreshing ? "Looking for apps..." :
			  "Find iOS apps added or removed", apps_refresh_cb, NULL,
			  &apps_refresh_sub, false);
		obj_track(&apps_refresh_sub);
	}
	push(scr);
}

/* --------------------------------------------------------------- logs */

/*
 * The kernel log on the screen, for when there is no USB or Wi-Fi to read
 * it over: the network addresses, then the tail of the ring buffer,
 * refreshed every 2 s and kept scrolled to the end. Volume keys scroll.
 */
#define LOGS_TAIL	6144	/* bytes of log on the page */

/*
 * The log as labels of LOGS_CHUNK lines each: LVGL lays out and draws a
 * label by walking its text from the start, so one 6 KB label cost ~25 ms
 * a frame while scrolling. Chunks off screen are skipped whole. Lines are
 * only ever added to the last chunk and whole chunks dropped from the top,
 * so a new message measures at most LOGS_CHUNK lines again (cutting the
 * text anew at every update moved every chunk's lines: all of them were
 * measured again).
 *
 * New messages come from /dev/kmsg, which the main loop waits on: nothing
 * runs while the kernel is quiet. (Reading the whole ring buffer every
 * 2 s to look for news, as before, took the kernel 31 ms each time with
 * 125 KB in it: a dropped frame or two every 2 s on this page.) The page
 * starts with the end of the ring, which klogctl() returns for a short
 * buffer.
 */
#define LOGS_CHUNK	8	/* small labels: LVGL walks each one from its start */
#define LOGS_MAXCH	96
#define LOGS_LINE_MAX	400	/* bytes of one line shown */

static lv_obj_t *logs_net, *logs_box;
static lv_timer_t *logs_timer;		/* the addresses (and the log, without /dev/kmsg) */
static lv_timer_t *logs_show_t;		/* new lines onto the page, 10 times a second at most */
static int logs_kmsg = -1;
static struct {
	lv_obj_t *label;
	int bytes;
} logs_chunk[LOGS_MAXCH];
static int logs_nchunks, logs_bytes;
static char *logs_open;			/* text of the last chunk, still filling */
static int logs_open_len, logs_open_cap, logs_open_lines;
static bool logs_open_dirty;
static int32_t logs_gone_h;		/* height dropped from the top since the last logs_show() */
static bool logs_at_end = true;		/* the view is at the end of the log */
static char *logs_seen;			/* (without /dev/kmsg) the ring's end as last read */

static void logs_put_open(void)
{
	if (logs_open_dirty && logs_nchunks)
		lv_label_set_text(logs_chunk[logs_nchunks - 1].label, logs_open);
	logs_open_dirty = false;
}

static void logs_drop_first(void)
{
	logs_gone_h += lv_obj_get_height(logs_chunk[0].label);
	lv_obj_delete(logs_chunk[0].label);
	logs_bytes -= logs_chunk[0].bytes;
	logs_nchunks--;
	memmove(&logs_chunk[0], &logs_chunk[1], logs_nchunks * sizeof(logs_chunk[0]));
}

/* One more line at the end (not on the screen until logs_show()). */
static void logs_add_line(const char *p, int len)
{
	if (!logs_box)
		return;
	len = LV_MIN(len, LOGS_LINE_MAX);
	if (!logs_nchunks || logs_open_lines >= LOGS_CHUNK) {
		lv_obj_t *l;

		logs_put_open();
		/* (a flood: no need to build what logs_show() would drop) */
		while (logs_nchunks == LOGS_MAXCH || (logs_bytes > 2 * LOGS_TAIL && logs_nchunks > 1))
			logs_drop_first();
		l = m3_label(logs_box, "", &lv_font_montserrat_12, M.on_surface);
		lv_obj_set_width(l, lv_pct(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		logs_chunk[logs_nchunks].label = l;
		logs_chunk[logs_nchunks].bytes = 0;
		logs_nchunks++;
		logs_open_len = logs_open_lines = 0;
	}
	if (logs_open_len + len + 2 > logs_open_cap) {
		int cap = (logs_open_len + len + 2) * 2;
		char *n = realloc(logs_open, cap);

		if (!n)
			return;
		logs_open = n;
		logs_open_cap = cap;
	}
	if (logs_open_lines)
		logs_open[logs_open_len++] = '\n';
	memcpy(logs_open + logs_open_len, p, len);
	logs_open_len += len;
	logs_open[logs_open_len] = 0;
	logs_open_lines++;
	logs_chunk[logs_nchunks - 1].bytes += len + 1;
	logs_bytes += len + 1;
	logs_open_dirty = true;
}

/*
 * The lines added since the last time onto the page. At the end of the log
 * the view stays at the end; scrolled back, it stays on the lines it shows
 * (also when old chunks leave at the top).
 */
static void logs_show(void)
{
	bool at_end = logs_at_end;	/* (the scroll below asks again, half way) */

	if (!logs_box)
		return;
	logs_put_open();
	while (logs_bytes > LOGS_TAIL && logs_nchunks > 1)
		logs_drop_first();
	lv_obj_update_layout(logs_box);
	if (at_end)
		lv_obj_scroll_to_y(logs_box, LV_COORD_MAX, LV_ANIM_OFF);
	else if (logs_gone_h)
		lv_obj_scroll_to_y(logs_box, LV_MAX(lv_obj_get_scroll_y(logs_box) - logs_gone_h, 0),
				   LV_ANIM_OFF);
	logs_gone_h = 0;
	logs_at_end = at_end;
}

/* Where the reader left the page: at the end of the log (new lines keep it there) or further back. */
static void logs_scrolled_cb(lv_event_t *e)
{
	logs_at_end = lv_obj_get_scroll_bottom(lv_event_get_target(e)) <= 2;
}

static void logs_show_cb(lv_timer_t *t)
{
	logs_show();
	lv_timer_pause(t);	/* until there are new lines (logs_kmsg_ev), or the screen lights */
}

static void logs_clear(void)
{
	while (logs_nchunks)
		logs_drop_first();
	logs_bytes = 0;
	logs_open_len = logs_open_lines = 0;
	logs_open_dirty = false;
	logs_gone_h = 0;
}

/*
 * The end of the kernel's ring buffer, as lines: SYSLOG_ACTION_READ_ALL
 * with a buffer shorter than the log gives the newest whole records that
 * fit. Returns false if it is what @seen holds already (then nothing is
 * added); @seen is brought up to date.
 */
static bool logs_fill(char *seen)
{
	char *raw = malloc(LOGS_TAIL + 1);
	int n, i, j;

	if (!raw)
		return false;
	n72_trace("klog>", 0);
	n = klogctl(3, raw, LOGS_TAIL);		/* SYSLOG_ACTION_READ_ALL */
	n72_trace("klog<", n);
	if (n < 0)
		n = 0;
	raw[n] = 0;
	if (seen) {
		if (!strcmp(seen, raw)) {
			free(raw);
			return false;
		}
		memcpy(seen, raw, n + 1);
		logs_clear();
	}
	for (i = 0; i < n; i = j + 1) {
		for (j = i; j < n && raw[j] != '\n'; j++)
			;
		if (raw[i] == '<') {		/* "<6>": the level */
			int k = i + 1;

			while (k < j && raw[k] >= '0' && raw[k] <= '9')
				k++;
			if (k < j && raw[k] == '>')
				i = k + 1;
		}
		logs_add_line(raw + i, j - i);
	}
	free(raw);
	return true;
}

/*
 * New records on /dev/kmsg, one a read: "level,sequence,microseconds,flags;
 * text\n" and then lines of " KEY=value"; a line break inside the text
 * comes as "\x0a". Shown as dmesg and the lines above show them.
 */
static void logs_kmsg_ev(int fd)
{
	static char rec[8192];		/* (the kernel's largest record) */
	char line[LOGS_LINE_MAX + 32];
	bool any = false;
	int budget;

	for (budget = 256; budget; budget--) {	/* (a flood: back to the main loop in between) */
		ssize_t r = read(fd, rec, sizeof(rec) - 1);
		unsigned long long us = 0;
		char *text, *end, *nl;
		int pre;

		if (r < 0 && (errno == EPIPE || errno == EINVAL))
			continue;	/* records overwritten before we read them, or one too long */
		if (r <= 0)
			break;		/* EAGAIN: that was all */
		rec[r] = 0;
		text = strchr(rec, ';');
		if (!text)
			continue;
		sscanf(rec, "%*u,%*u,%llu", &us);
		text++;
		end = strchr(text, '\n');
		if (end)
			*end = 0;
		pre = snprintf(line, sizeof(line), "[%5llu.%06llu] ", us / 1000000, us % 1000000);
		do {
			int len;

			nl = strstr(text, "\\x0a");
			len = nl ? (int)(nl - text) : (int)strlen(text);
			len = LV_MIN(len, LOGS_LINE_MAX);
			memcpy(line + pre, text, len);
			logs_add_line(line, pre + len);
			text = nl ? nl + 4 : text;
		} while (nl);
		any = true;
	}
	/* dark: the lines wait in the last chunk's text for the screen (logs_show_t is an awake timer) */
	if (any && logs_show_t && !screen_off)
		lv_timer_resume(logs_show_t);
}

static void logs_update(lv_timer_t *t)
{
	char usb[32], wlan[32], buf[96];

	LV_UNUSED(t);
	ip_of("usb0", usb, sizeof(usb));
	ip_of("wlan0", wlan, sizeof(wlan));
	snprintf(buf, sizeof(buf), "usb0 %s    wlan0 %s", usb, wlan);
	label_set_if_changed(logs_net, buf);
	/* no /dev/kmsg: look at the end of the ring (cheap: the kernel formats only that) */
	if (logs_kmsg < 0 && logs_seen && logs_fill(logs_seen))
		logs_show();
}

static bool logs_scroll(int dir)
{
	if (!logs_box)
		return false;
	lv_obj_scroll_by(logs_box, 0, dir * 160, LV_ANIM_OFF);
	return true;
}

static void logs_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (logs_timer)
		lv_timer_delete(logs_timer);
	logs_timer = NULL;
	if (logs_show_t)
		lv_timer_pause(logs_show_t);
	if (logs_kmsg >= 0) {
		ui_unwatch(logs_kmsg);
		close(logs_kmsg);
		logs_kmsg = -1;
	}
	free(logs_seen);
	logs_seen = NULL;
	free(logs_open);
	logs_open = NULL;
	logs_open_cap = logs_open_len = logs_open_lines = 0;
	logs_open_dirty = false;
	logs_box = NULL;	/* (the chunk labels go with the page) */
	logs_nchunks = logs_bytes = 0;
	logs_gone_h = 0;
}

static void open_logs(lv_event_t *e)
{
	nav_mark(open_logs, NULL);
	lv_obj_t *scr, *body;

	LV_UNUSED(e);
	if (logs_box)		/* already open */
		return;
	body = m3_page("Logs", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	logs_net = m3_label(body, "", &lv_font_montserrat_12, M.primary);
	obj_track(&logs_net);
	lv_obj_set_width(logs_net, lv_pct(100));
	logs_box = lv_obj_create(body);
	obj_track(&logs_box);
	plain(logs_box);
	lv_obj_set_width(logs_box, lv_pct(100));
	lv_obj_set_flex_grow(logs_box, 1);
	lv_obj_set_style_pad_all(logs_box, 10, 0);
	lv_obj_set_style_radius(logs_box, 16, 0);
	lv_obj_set_style_bg_color(logs_box, M.surface_c, 0);
	lv_obj_set_scroll_dir(logs_box, LV_DIR_VER);
	fling_accel(logs_box);
	lv_obj_set_flex_flow(logs_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(logs_box, 0, 0);
	logs_nchunks = logs_bytes = 0;
	/*
	 * No scroll cache here: pictures of text chunks cost as much to take
	 * as they save (measured: 16.6 vs 15.8 ms a frame, same hitches), while
	 * the card pages draw ~30% faster with it.
	 */
	/* from now on (opened before the ring's end is read: a message in between shows once) */
	logs_at_end = true;
	lv_obj_add_event_cb(logs_box, logs_scrolled_cb, LV_EVENT_SCROLL, NULL);
	logs_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (logs_kmsg >= 0)
		lseek(logs_kmsg, 0, SEEK_END);
	else if ((logs_seen = malloc(LOGS_TAIL + 1)))
		logs_seen[0] = 0;
	logs_fill(logs_seen);
	logs_show();
	if (logs_kmsg >= 0)
		ui_watch(logs_kmsg, logs_kmsg_ev);
	if (!logs_show_t) {
		logs_show_t = awake_timer(logs_show_cb, 100, NULL);
		if (logs_show_t)
			lv_timer_pause(logs_show_t);
	}
	logs_update(NULL);
	logs_timer = lv_timer_create(logs_update, 2000, NULL);
	lv_obj_add_event_cb(scr, logs_delete_cb, LV_EVENT_DELETE, NULL);
	push(scr);
}

/* After a restart from a settings change: reopen that page. */
static void ui_reopen(void)
{
	const char *p = getenv("N72_UI_OPEN");

	if (!p)
		return;
	/* "display" after a theme change; the others for screenshots */
	if (!strcmp(p, "shade"))
		shade_set(true, false);
	else if (!strcmp(p, "paint"))
		open_paint(NULL);
	else if (!strncmp(p, "files:", 6))	/* (tests: a folder's page) */
		files_open_dir(p + 6);
	else if (!strcmp(p, "games"))		/* (tests: the Games page) */
		open_games(NULL);
	else if (strcmp(p, "logs") && strcmp(p, "about") && strcmp(p, "volume") &&
		 strcmp(p, "keyboard"))
		open_settings(NULL);
	if (!strcmp(p, "display"))
		open_display(NULL);
	else if (!strcmp(p, "wifi"))
		open_wifi(NULL);
	else if (!strcmp(p, "bt"))
		open_bt(NULL);
	else if (!strcmp(p, "sound"))
		open_sound(NULL);
	else if (!strcmp(p, "power"))
		open_power(NULL);
	else if (!strcmp(p, "wallpaper"))
		open_wallpaper(NULL);
	else if (!strcmp(p, "about"))
		open_about(NULL);
	else if (!strcmp(p, "logs"))
		open_logs(NULL);
	else if (!strcmp(p, "volume"))	/* (screenshots of the volume pop-up) */
		vol_hud_show();
	else if (!strcmp(p, "keyboard"))	/* (screenshots: the keyboard as an application gets it) */
		kbd_show_for(NULL, NULL);
	unsetenv("N72_UI_OPEN");
}

/* -------------------------------------------------------------- paint */

/* A finger-painting toy: each stroke gets the next colour round the wheel. */
static lv_obj_t *paint_canvas;
static void *paint_buf;
static lv_point_t paint_last;
static bool paint_down;
static uint32_t paint_hue;

#define PAINT_W (W - 2 * STATUS_PAD)
#define PAINT_H (H - 56 - 16)

static void paint_clear(void)
{
	lv_canvas_fill_bg(paint_canvas, M.surface_c, LV_OPA_COVER);
}

static void paint_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_draw_line_dsc_t d;
	lv_layer_t layer;
	lv_area_t a;
	lv_point_t p;

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		paint_down = false;
		return;
	}
	if (!indev || (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING))
		return;
	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(paint_canvas, &a);
	p.x -= a.x1;
	p.y -= a.y1;
	if (code == LV_EVENT_PRESSED || !paint_down) {
		paint_last = p;
		paint_down = true;
		paint_hue = (paint_hue + 47) % 360;
	}
	lv_canvas_init_layer(paint_canvas, &layer);
	lv_draw_line_dsc_init(&d);
	d.color = tone(paint_hue, 75, conf.dark ? 70 : 50);
	d.width = 10;
	d.round_start = 1;
	d.round_end = 1;
	d.p1.x = paint_last.x;
	d.p1.y = paint_last.y;
	d.p2.x = p.x;
	d.p2.y = p.y;
	lv_draw_line(&layer, &d);
	lv_canvas_finish_layer(paint_canvas, &layer);
	paint_last = p;
}

static void paint_clear_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (paint_buf)
		paint_clear();
}

static void paint_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(paint_buf);
	paint_buf = NULL;
}

static void open_paint(lv_event_t *e)
{
	nav_mark(open_paint, NULL);
	lv_obj_t *scr, *body, *btn;

	LV_UNUSED(e);
	body = m3_page("Paint", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	paint_buf = malloc(LV_CANVAS_BUF_SIZE(PAINT_W, PAINT_H, 32, LV_DRAW_BUF_STRIDE_ALIGN));
	if (!paint_buf) {
		lv_obj_delete(scr);
		return;
	}
	paint_canvas = lv_canvas_create(body);
	obj_track(&paint_canvas);
	lv_canvas_set_buffer(paint_canvas, paint_buf, PAINT_W, PAINT_H, LV_COLOR_FORMAT_XRGB8888);
	lv_obj_add_flag(paint_canvas, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(paint_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(paint_canvas, paint_event, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(scr, paint_delete_cb, LV_EVENT_DELETE, NULL);
	paint_clear();

	/* clear: a tonal icon button at the right of the top bar */
	btn = lv_button_create(lv_obj_get_child(scr, 0));
	plain(btn);
	lv_obj_set_size(btn, 48, 40);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(btn, M.secondary_c, 0);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -8, 0);
	lv_obj_add_event_cb(btn, paint_clear_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(m3_label(btn, LV_SYMBOL_TRASH, &lv_font_montserrat_16, M.on_secondary_c));
	push(scr);
}

/* ------------------------------------------------------------ turning */

/*
 * The screen turns with the iPod when Auto-rotate is on (quick settings):
 * the accelerometer (lis3lv02d, read from sysfs a few times a second while
 * the screen is on) says which way is down; once a new way has held for
 * ROT_HOLD_MS the UI turns. LVGL draws the turned picture upright and the
 * GPU turns it onto the panel as it copies it (mbx_gpu.c: no cost); the
 * screens are built again at the new size (ui_turn), the pages on the
 * stack reopened. A video turns with it (the player turns its pictures).
 *
 * Upright only: Paint, the Wi-Fi password, the lock screen (it stays as
 * the screen was), applications other than the video player (they draw
 * on layer 0 for an upright panel).
 *
 * Gravity on the iPod as the driver reports it (iOS's convention): upright
 * y < 0; on the right side (home button left) x > 0: LVGL's 90 (its 90
 * puts the picture's top at the panel's left edge); on the left side
 * x < 0: 270. Upside down is not used.
 */
#define ROT_POS "/sys/devices/platform/lis3lv02d/position"
#define ROT_G 550		/* mg along an axis to count as down */
#define ROT_HOLD_MS 450
/*
 * The chip's motion wake (kernel "motion"): while a turn is possible the
 * accelerometer watches for a move itself and the kernel notifies
 * "position" (POLLPRI in the main loop's poll()); the timer reads 5 times
 * a second only from then until the reading has been still ROT_SETTLE_MS
 * and the screen is turned to match, then arms the chip again and rests.
 * Without it (an older kernel) the timer reads all the time, as before.
 */
#define ROT_MOTION "/sys/devices/platform/lis3lv02d/motion"
#define ROT_SETTLE_MS 1000
static int rot_pos_fd = -1;		/* "position", kept open: notified, and read */
static bool rot_ev;			/* the kernel has the motion wake */
static bool rot_on;			/* we asked it to watch (the chip kept on) */
static uint32_t rot_move_ms;		/* the last move: an event or a new reading */

static int rot_seen = -1;		/* the way the iPod is held, -1 unknown */
static int rot_side = 90;		/* the sideways it was last held: 90 or 270 */
static uint32_t rot_seen_ms;
static lv_timer_t *rot_timer;
static int video_rot = -1;		/* the player's controls asked for this (-1: follow) */

static bool app_is_video(void);
static void vc_turned(void);

/* The way the iPod is held: 0, 90, 270; -1 flat or upside down. */
static int rot_read(void)
{
	char buf[64];
	int fd, x, y, z;
	ssize_t n;

	if (rot_pos_fd >= 0) {
		n = pread(rot_pos_fd, buf, sizeof(buf) - 1, 0);	/* (also takes the notification) */
	} else {
		fd = open(ROT_POS, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return -1;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
	}
	if (n <= 0)
		return -1;
	buf[n] = 0;
	if (sscanf(buf, "(%d,%d,%d)", &x, &y, &z) != 3)
		return -1;
	if (y < -ROT_G && -y > LV_ABS(x))
		return 0;
	if (x > ROT_G && x > LV_ABS(y))
		return 90;
	if (x < -ROT_G && -x > LV_ABS(y))
		return 270;
	return -1;
}

/*
 * The turn's transition: the screen as it was, turned by the GPU to lie on
 * the panel where it lay, over the new layout, fading out (a GPU blend a
 * frame): a crossfade with nothing jumping.
 */
#define TURN_FADE_MS 300	/* iOS's rotation: 300 ms ease-in-out (a crossfade here) */
static lv_obj_t *turn_img;
static lv_draw_buf_t *turn_pic;

static void turn_fade_exec(void *o, int32_t v)
{
	lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void turn_fade_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	if (turn_img) {
		lv_obj_delete(turn_img);
		turn_img = NULL;
	}
	tr_free(&turn_pic);
}

static void turn_fade_start(lv_draw_buf_t *old, int from, int to)
{
	int delta = ((from - to) % 360 + 360) % 360;
	lv_anim_t a;

	if (turn_img)
		turn_fade_done(NULL);
	turn_pic = old && (delta == 90 || delta == 270) ? mbx_gpu_turn_buf(old, delta) : NULL;
	if (old)
		lv_draw_buf_destroy(old);
	if (!turn_pic)
		return;
	turn_img = lv_image_create(lv_layer_top());
	lv_image_set_src(turn_img, turn_pic);
	lv_obj_set_pos(turn_img, 0, 0);
	lv_obj_remove_flag(turn_img, LV_OBJ_FLAG_CLICKABLE);
	lv_anim_init(&a);
	lv_anim_set_var(&a, turn_img);
	lv_anim_set_exec_cb(&a, turn_fade_exec);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_duration(&a, TURN_FADE_MS);
	lv_anim_set_path_cb(&a, ios_path_ease_in_out);
	lv_anim_set_completed_cb(&a, turn_fade_done);
	lv_anim_start(&a);
}

/* Rebuild what is shown at the new size; false if it could not turn. */
/*
 * Home and the pages were built for another turn (a turn over a video
 * rebuilt only what shows over it): built again before they are shown.
 */
static bool layout_stale;
static bool home_stale;
static bool nav_stale[NAV_MAX];

/* What lies over an application: the shade, the bar, the volume pop-up,
 * the video controls; rebuilt at the current size. */
static void ui_rebuild_overlays_free(void)
{
	int i;

	if (shade && lv_obj_is_valid(shade))
		lv_obj_delete(shade);
	if (scrim && lv_obj_is_valid(scrim))
		lv_obj_delete(scrim);
	shade = scrim = NULL;
	shade_open = false;
	tr_free(&shade_pic);
	shade_pic_dirty = true;
	sd_free_pics();
	tr_free(&sd.home_pic);
	if (app_bar_timer) {
		lv_timer_delete(app_bar_timer);
		app_bar_timer = NULL;
	}
	if (app_bar) {
		psl_stop(app_bar);
		lv_obj_delete(app_bar);
		app_bar = NULL;
		app_bar_shown = false;
	}
	lv_obj_remove_event_cb(lv_layer_top(), app_gesture_cb);
	if (vol_hud_timer) {
		lv_timer_delete(vol_hud_timer);
		vol_hud_timer = NULL;
	}
	if (vol_hud) {
		lv_obj_delete(vol_hud);
		vol_hud = NULL;
	}
	if (lock_scr && !locked) {
		lv_obj_delete(lock_scr);
		lock_scr = NULL;
	}
	{
		lv_obj_t **gone[] = {
			&app_bar_time, &app_bar_batt, &app_bar_wifi, &app_bar_bt,
			&clock_lbl, &date_lbl,
			&qs_wifi, &qs_bt, &qs_wifi_sub, &qs_bt_sub, &qs_rot, &qs_rot_sub,
			&home_bright, &qs_settings_btn, &vol_hud_slider, &vol_hud_icon,
		};

		for (i = 0; i < (int)(sizeof(gone) / sizeof(gone[0])); i++)
			*gone[i] = NULL;
	}
	/* the card's way back: home's picture and the tiles are another turn's */
	za.tile = NULL;
	za.page_tile = NULL;
	tr_free(&za.home);
}

static void ui_rebuild_overlays(void)
{
	ui_rebuild_overlays_free();
	build_shade();
	if (app_pid > 0) {
		app_bar_build();
		lv_obj_set_parent(scrim, lv_layer_top());
		lv_obj_set_parent(shade, lv_layer_top());
	}
	lv_obj_invalidate(lv_layer_top());
	net_ui_sync();
	qs_rot_sync();
	vc_turned();
	kbd_turned();
}

/*
 * Everything at the current size: the pages, home (the shade with it), the
 * bars. @load: show the page or home afterwards (else the screen showing,
 * an application's, stays).
 */
static void ui_rebuild(bool load)
{
	struct nav_re re[NAV_MAX];
	lv_obj_t *tmp = NULL;
	int n = nav_n, from_idx = -2, i;
	uint32_t rb_t0 = lv_tick_get();

	for (i = 0; i < n; i++)
		if (launch_from == nav[i])
			from_idx = i;
	if (launch_from == home_scr)
		from_idx = -1;
	memcpy(re, nav_re, sizeof(re));
	memset(nav_re, 0, sizeof(nav_re));	/* (re owns the folders now) */
	if (load) {
		tmp = lv_obj_create(NULL);
		lv_screen_load(tmp);
	}
	ui_rebuild_overlays_free();
	for (i = n - 1; i >= 0; i--)
		lv_obj_delete(nav[i]);
	nav_n = 0;
	lv_obj_delete(home_scr);
	home_scr = NULL;
	if (home_timer) {
		awake_timer_delete(home_timer);
		home_timer = NULL;
	}
	if (shade_warm_timer) {
		awake_timer_delete(shade_warm_timer);
		shade_warm_timer = NULL;
	}
	pg_home_gone();
	/* what pointed into them (net_ui_sync and the status updates run
	 * while the new ones are built: they skip what is NULL) */
	{
		lv_obj_t **gone[] = {
			&status_time, &status_wifi, &status_bt, &batt_lbl,
			&home_clock, &home_date, &home_batt, &home_net,
			&settings_tile, &home_apps, &home_bar, &home_dots, &home_dock, &home_view,
		};

		for (i = 0; i < (int)(sizeof(gone) / sizeof(gone[0])); i++)
			*gone[i] = NULL;
	}
	uint32_t tb = lv_tick_get();

	build_home();
	if (app_pid > 0) {
		app_bar_build();
		lv_obj_set_parent(scrim, lv_layer_top());
		lv_obj_set_parent(shade, lv_layer_top());
	}
	fprintf(stderr, "turn: delete %u ms, home %u ms;", (unsigned)(tb - rb_t0),
			(unsigned)lv_tick_elaps(tb));
	push_quiet = true;
	for (i = 0; i < n; i++) {
		uint32_t tp = lv_tick_get();

		if (re[i].open)
			re[i].open(NULL);
		else
			files_open_dir(re[i].dir);
		free(re[i].dir);
		fprintf(stderr, " page %d %u ms;", i, (unsigned)lv_tick_elaps(tp));
	}
	fprintf(stderr, "\n");
	push_quiet = false;
	push_quiet_scr = NULL;
	launch_from = from_idx == -1 ? home_scr : from_idx >= 0 && from_idx < nav_n ?
		      nav[from_idx] : NULL;
	if (load) {
		lv_screen_load(nav_n ? nav[nav_n - 1] : home_scr);
		lv_obj_delete(tmp);
	}
	lv_obj_invalidate(lv_layer_top());
	net_ui_sync();
	status_refresh(true);
	qs_rot_sync();
	vc_turned();
	layout_stale = false;
	home_stale = false;
	memset(nav_stale, 0, sizeof(nav_stale));
}

/*
 * A turn on a page rebuilds that page only (and what lies over it): home
 * and the pages under it were built for the other turn, and are built
 * again when they are about to show (going back, Home). Rebuilding all of
 * them took 140-215 ms a turn (YouTube's page).
 */
static bool home_stale;
static bool nav_stale[NAV_MAX];

static void home_fresh(void)
{
	lv_obj_t *old = home_scr;
	int i;

	if (!home_stale)
		return;
	home_stale = false;
	ui_rebuild_overlays_free();
	if (home_timer) {
		awake_timer_delete(home_timer);
		home_timer = NULL;
	}
	if (shade_warm_timer) {
		awake_timer_delete(shade_warm_timer);
		shade_warm_timer = NULL;
	}
	pg_home_gone();
	{
		lv_obj_t **gone[] = {
			&status_time, &status_wifi, &status_bt, &batt_lbl,
			&home_clock, &home_date, &home_batt, &home_net,
			&settings_tile, &home_apps, &home_bar, &home_dots, &home_dock, &home_view,
		};

		for (i = 0; i < (int)(sizeof(gone) / sizeof(gone[0])); i++)
			*gone[i] = NULL;
	}
	if (launch_from == old)
		launch_from = NULL;
	home_build_quiet = true;
	build_home();
	home_build_quiet = false;
	if (app_pid > 0) {
		app_bar_build();
		lv_obj_set_parent(scrim, lv_layer_top());
		lv_obj_set_parent(shade, lv_layer_top());
	}
	if (lv_screen_active() != old)
		lv_obj_delete(old);
	else
		lv_obj_delete_async(old);
	net_ui_sync();
	status_refresh(true);
	qs_rot_sync();
}

/* Page @i of the stack, built again if a turn left it (not showing). */
static void page_fresh(int i)
{
	struct nav_re re;
	lv_obj_t *old;
	int top = nav_n;

	if (i < 0 || i >= nav_n || !nav_stale[i])
		return;
	nav_stale[i] = false;
	if (!nav_re[i].open && !nav_re[i].dir)
		return;
	/* reopened in its place: the stack above it set aside meanwhile */
	re = nav_re[i];
	nav_re[i].dir = NULL;
	old = nav[i];
	{
		lv_obj_t *above[NAV_MAX];
		struct nav_re above_re[NAV_MAX];
		int k, na = top - i - 1;

		for (k = 0; k < na; k++) {
			above[k] = nav[i + 1 + k];
			above_re[k] = nav_re[i + 1 + k];
		}
		nav_n = i;
		lv_obj_delete(old);	/* first: an opener may refuse a second copy */
		push_quiet = true;
		if (re.open)
			re.open(NULL);
		else
			files_open_dir(re.dir);
		push_quiet = false;
		push_quiet_scr = NULL;
		free(re.dir);
		for (k = 0; k < na && nav_n < NAV_MAX; k++) {
			nav[nav_n] = above[k];
			nav_re[nav_n] = above_re[k];
			nav_n++;
		}
	}
	if (launch_from == old)
		launch_from = nav_n > i ? nav[i] : NULL;
}

/* Rebuild what is shown at the new size; false if it could not turn. */
static bool ui_turn(int rot)
{
	lv_draw_buf_t *old_pic = NULL;
	int from = ui_rot, i;
	uint32_t t0 = lv_tick_get();
	bool over_app = app_pid > 0 && lv_screen_active() == app_scr;

	if (rot == ui_rot)
		return true;
	for (i = 0; i < nav_n; i++)
		if (!nav_re[i].open && !nav_re[i].dir)
			return false;		/* a page that cannot be reopened */
	/* the screen as it is, for the crossfade (not over an application:
	 * the layer holds only the launcher's own things there) */
	if (!over_app) {
		lv_refr_now(NULL);
		old_pic = mbx_gpu_grab_screen();
	}
	if (!mbx_gpu_set_rotation(rot)) {
		if (old_pic)
			lv_draw_buf_destroy(old_pic);
		return false;
	}
	ui_rot = rot;
	ui_w = lv_display_get_horizontal_resolution(NULL);
	ui_h = lv_display_get_vertical_resolution(NULL);
	if (over_app) {
		/*
		 * Over a video only what shows: rebuilding home and the pages
		 * too held the main thread 320-450 ms, and the player (below
		 * the launcher) stalled: its sound ran dry. They follow when
		 * the video ends (app_home_ui).
		 */
		ui_rebuild_overlays();
		layout_stale = true;
	} else if (nav_n > 0) {
		/* a page: it alone (and the overlays); home and the pages
		 * under it when they show */
		int top = nav_n - 1;
		lv_obj_t *old = nav[top], *tmp = lv_obj_create(NULL);
		struct nav_re re = nav_re[top];

		lv_screen_load(tmp);
		ui_rebuild_overlays();
		nav_re[top].dir = NULL;
		nav_n = top;
		/* gone first: an opener may refuse a second copy (YouTube's) */
		lv_obj_delete(old);
		push_quiet = true;
		if (re.open)
			re.open(NULL);
		else
			files_open_dir(re.dir);
		push_quiet = false;
		push_quiet_scr = NULL;
		free(re.dir);
		if (launch_from == old)
			launch_from = nav_n > top ? nav[top] : NULL;
		home_stale = true;
		for (i = 0; i < top; i++)
			nav_stale[i] = true;
		/* not reopened: what shows instead is built for this turn first */
		if (nav_n <= top) {
			if (nav_n)
				page_fresh(nav_n - 1);
			else
				home_fresh();
		}
		lv_screen_load(nav_n ? nav[nav_n - 1] : home_scr);
		lv_obj_delete(tmp);
		net_ui_sync();
		qs_rot_sync();
	} else {
		ui_rebuild(true);
	}
	kbd_turned();		/* (every way above) */
	turn_fade_start(old_pic, from, rot);
	fprintf(stderr, "n72-ui: turned %d -> %d in %u ms (%d pages%s)\n", from, rot,
		(unsigned)lv_tick_elaps(t0), nav_n, over_app ? ", over an application" : "");
	return true;
}

/* The turn the screen should have now. */
static int rot_wanted(void)
{
	lv_obj_t *a = lv_screen_active();

	if (app_pid > 0 && a == app_scr) {
		if (app_is_game())
			return game_rot;
		if (!app_is_video())
			return 0;
		if (video_rot >= 0)	/* the controls' turn button holds it */
			return video_rot ? rot_side : 0;
	}
	if (conf.autorot == ROT_LOCKED)
		return conf.rotlock;	/* (as it was held when Auto-rotate went off) */
	return rot_seen >= 0 ? rot_seen : ui_rot;
}

/*
 * Animations that will end: a text area's cursor blinks by an endless one
 * (running whether or not the field has the focus), which must not keep
 * the screen from turning for as long as such a page is on the stack.
 */
static bool finite_anims_running(void)
{
	lv_anim_t *a;

	LV_LL_READ(&LV_GLOBAL_DEFAULT()->anim_state.anim_ll, a)
		if (a->repeat_cnt != LV_ANIM_REPEAT_INFINITE)
			return true;
	return false;
}

/* Whether the UI may be rebuilt now (nothing moving, nothing held). */
static bool rot_can_turn(void)
{
	lv_obj_t *a = lv_screen_active();
	int i;

	if (screen_off || locked || za.phase || tr.scr || sd.on || sd.scr || shade_open ||
	    page_over_app || finite_anims_running() ||
	    (touch_indev && lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		return false;
	if (app_pid != 0 && !(app_pid > 0 && a == app_scr && (app_is_video() || app_is_game())))
		return false;		/* an application starting, or not the player or a game */
	for (i = 0; i < nav_n; i++)
		if (nav_re[i].open == open_paint)
			return false;
	return a == home_scr || (nav_n && a == nav[nav_n - 1]) || (app_pid > 0 && a == app_scr);
}

static void rot_motion(bool on)
{
	int fd = open(ROT_MOTION, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, on ? "1" : "0", 1) != 1)
			on = false;
		close(fd);
	}
	rot_on = on;
}

/* The screen went dark: nothing to watch for (screen_set). */
static void rot_motion_off(void)
{
	if (rot_on)
		rot_motion(false);
}

/* Something that changes what a turn needs: look now (the timer may rest). */
static void rot_kick(void)
{
	if (rot_timer && !screen_off) {
		lv_timer_resume(rot_timer);
		lv_timer_ready(rot_timer);
	}
}

/* The accelerometer saw a move: read it 5 times a second until it settles. */
static void rot_pos_ev(int fd)
{
	char buf[64];

	if (pread(fd, buf, sizeof(buf), 0) < 0)	/* (the notification taken) */
		return;
	rot_move_ms = lv_tick_get();
	rot_kick();
}

static void rot_poll(lv_timer_t *t)
{
	/* the accelerometer only for Auto-rotate and a video: off, it powers down */
	bool reads = conf.autorot == ROT_AUTO || (app_pid > 0 && app_is_video());
	bool need = !screen_off && !locked && (reads || rot_wanted() != ui_rot);
	int r, want;

	if (!rot_ev) {
		/* 5 a second only while a turn can happen (each read is an I2C
		 * transfer and a wake-up); else every 2 s, to notice */
		uint32_t period = need ? 200 : 2000;

		if (t->period != period)
			lv_timer_set_period(t, period);
	} else if (!need) {
		rot_motion_off();
		lv_timer_pause(t);	/* (rot_kick, or the screen lit, looks again) */
		return;
	} else if (t->period != 200) {
		lv_timer_set_period(t, 200);
	}
	if (screen_off || locked)
		return;
	if (reads) {
		r = rot_read();
		if (r >= 0 && r != rot_seen) {
			rot_seen = r;
			rot_seen_ms = rot_move_ms = lv_tick_get();
			if (r)
				rot_side = r;
		}
		if (lv_tick_elaps(rot_seen_ms) < ROT_HOLD_MS)
			return;
	}
	want = rot_wanted();
	if (want != ui_rot && rot_can_turn())
		ui_turn(want);
	/* still, and turned to match: the chip watches from here */
	if (rot_ev && want == ui_rot && lv_tick_elaps(rot_move_ms) >= ROT_SETTLE_MS) {
		if (reads)
			rot_motion(true);	/* (armed around where it is now) */
		else
			rot_motion_off();
		lv_timer_pause(t);
	}
}

static void rot_conf_changed(void)
{
	rot_seen = conf.autorot == ROT_AUTO ? rot_read() : -1;
	if (rot_seen > 0)
		rot_side = rot_seen;
	rot_seen_ms = 0;
	rot_kick();
}

static void rot_init(void)
{
	rot_pos_fd = open(ROT_POS, O_RDONLY | O_CLOEXEC);
	if (rot_pos_fd >= 0 && access(ROT_MOTION, W_OK) == 0) {
		rot_ev = true;
		ui_watch_ev(rot_pos_fd, rot_pos_ev, POLLPRI);
		/* a UI before this one (restarted, or crashed) may have left the
		 * chip watching, and kept on: from nothing */
		rot_motion(false);
	}
	rot_timer = awake_timer(rot_poll, 200, NULL);	/* (dark: no accelerometer reads) */
	if (conf.autorot == ROT_AUTO)
		rot_seen = rot_read();
	if (rot_seen > 0)
		rot_side = rot_seen;
}

/* Upright now, for something that only works upright (an application). */
static void ui_upright(void)
{
	if (ui_rot)
		ui_turn(0);
}

/* ---------------------------------------------------- video controls */

/*
 * Over the video player (n72-player, started as n72-video FILE) the
 * launcher shows its controls on layer 1, Material 3 style: a scrim, the
 * title with Back and a turn button at the top, the play/pause button
 * (filled, primary) between the two skip buttons in the middle, and the
 * time with a slider at the bottom. A tap shows or hides them; playing,
 * they go after VC_HIDE_MS. Commands and the state go over the player's
 * socket "@n72-player" (datagrams; see n72-player.c).
 */
#define VC_HIDE_MS 3000
#define VC_FADE_MS 150


static struct {
	lv_obj_t *root, *ui, *title, *play_sym, *slider, *pos, *dur, *rot_lbl, *rot_btn;
	lv_timer_t *poll, *hide;
	int sock;
	bool up;			/* the player has answered */
	bool shown, playing, dragging;
	int64_t pos_ms, dur_ms;
	int prot;			/* the player's turn */
	uint32_t quiet_ms;		/* after a command: the state may lag */
	char name[128];
} vc = { .sock = -1, .prot = -1 };

static bool cmd_is_video(const char *cmd)
{
	return cmd && (!strncmp(cmd, "n72-video", 9) || !strncmp(cmd, "n72-player", 10));
}

static bool app_is_video(void)
{
	return app_pid > 0 && cmd_is_video(app_cmd);
}

/*
 * A game (ui_games.c): RetroArch started through n72-retro-launch, in
 * landscape on the side game_rot (90 or 270), which the display turns for
 * it; the launcher's layer turns the same way while it plays.
 */
static int game_rot = 90;

static bool cmd_is_game(const char *cmd)
{
	return cmd && strstr(cmd, "n72-retro-launch") != NULL;
}

static bool app_is_game(void)
{
	return app_pid > 0 && cmd_is_game(app_cmd);
}

/* A game in front: the screen stays on (main.c, idle_check). */
static bool game_playing(void)
{
	return app_is_game() && lv_screen_active() == app_scr;
}

/* A video playing in front: the screen stays on (main.c, idle_check). */
static bool video_playing(void)
{
	return app_is_video() && vc.up && vc.playing && lv_screen_active() == app_scr;
}

static void vc_send(const char *m)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	socklen_t l = offsetof(struct sockaddr_un, sun_path) + 1 + strlen("n72-player");

	if (vc.sock < 0)
		return;
	memcpy(a.sun_path + 1, "n72-player", strlen("n72-player"));
	sendto(vc.sock, m, strlen(m), MSG_DONTWAIT, (struct sockaddr *)&a, l);
}

static void vc_fmt(char *b, size_t n, int64_t ms)
{
	long t = ms < 0 ? 0 : (long)(ms / 1000);

	if (t >= 3600)
		snprintf(b, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
	else
		snprintf(b, n, "%ld:%02ld", t / 60, t % 60);
}

static void vc_show(bool on);

static void vc_hide_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	vc.hide = NULL;
	if (vc.playing && !vc.dragging)
		vc_show(false);
}

/* Playing and shown: hide in a while (again from now). */
static void vc_hide_later(void)
{
	if (vc.hide) {
		lv_timer_delete(vc.hide);
		vc.hide = NULL;
	}
	if (!vc.shown || !vc.playing || vc.dragging)
		return;
	vc.hide = lv_timer_create(vc_hide_cb, VC_HIDE_MS, NULL);
	lv_timer_set_repeat_count(vc.hide, 1);
}

static void vc_opa_exec(void *o, int32_t v)
{
	lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void vc_fade_done(lv_anim_t *a)
{
	if (!vc.shown)
		lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_local_style_prop(a->var, LV_STYLE_OPA, 0);	/* no layer at rest */
}

static void vc_show(bool on)
{
	lv_anim_t a;

	if (!vc.ui)
		return;
	vc.shown = on;
	if (on && vc.poll) {
		lv_timer_set_period(vc.poll, 250);	/* the position, fresh */
		lv_timer_ready(vc.poll);
	}
	lv_anim_delete(vc.ui, vc_opa_exec);
	if (on)
		lv_obj_remove_flag(vc.ui, LV_OBJ_FLAG_HIDDEN);
	lv_anim_init(&a);
	lv_anim_set_var(&a, vc.ui);
	lv_anim_set_exec_cb(&a, vc_opa_exec);
	lv_anim_set_values(&a, on ? LV_OPA_TRANSP : LV_OPA_COVER, on ? LV_OPA_COVER : LV_OPA_TRANSP);
	lv_anim_set_duration(&a, VC_FADE_MS);
	lv_anim_set_path_cb(&a, on ? ios_path_ease_out : ios_path_ease_in);
	lv_anim_set_completed_cb(&a, vc_fade_done);
	lv_anim_start(&a);
	vc_hide_later();
}

static void vc_update(void)
{
	char b[24];

	if (!vc.ui)
		return;
	label_set_if_changed(vc.play_sym, vc.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	label_set_if_changed(vc.title, vc.name);
	if (!vc.dragging) {
		vc_fmt(b, sizeof(b), vc.pos_ms);
		label_set_if_changed(vc.pos, b);
		if (vc.dur_ms > 0) {
			lv_slider_set_range(vc.slider, 0, (int32_t)(vc.dur_ms / 100));
			lv_slider_set_value(vc.slider, (int32_t)(vc.pos_ms / 100), LV_ANIM_OFF);
		}
	}
	vc_fmt(b, sizeof(b), vc.dur_ms);
	label_set_if_changed(vc.dur, vc.dur_ms > 0 ? b : "");
}

static void vc_build(void);

/* The player's state, a few times a second. */
/*
 * The player answers "status" requests (it does not send by itself): the
 * answers are read when they come (the socket is watched by the main loop),
 * and the requests go out 4 a second only while the controls show the
 * position, else every 2 s, enough to notice the player pausing itself.
 */
static void vc_rx(int fd)
{
	char buf[256], st[16];
	long long pos, dur;
	int r;
	ssize_t n;

	LV_UNUSED(fd);
	while ((n = recv(vc.sock, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		if (sscanf(buf, "%15s %lld %lld %d", st, &pos, &dur, &r) != 4)
			continue;
		if (!vc.up) {
			const char *nl = strchr(buf, '\n');

			vc.up = true;
			snprintf(vc.name, sizeof(vc.name), "%s", nl ? nl + 1 : "");
			vc.playing = !strcmp(st, "playing");
			vc_build();
			vc_show(true);		/* a moment at the start */
		}
		{
			/* the title from every answer (a stray one goes right) */
			const char *nl = strchr(buf, '\n');

			if (nl && strcmp(vc.name, nl + 1))
				snprintf(vc.name, sizeof(vc.name), "%s", nl + 1);
		}
		vc.prot = r;
		if (lv_tick_elaps(vc.quiet_ms) < 700)
			continue;		/* (a command's effect not in yet) */
		if (vc.playing != !strcmp(st, "playing")) {
			vc.playing = !strcmp(st, "playing");
			/* paused by the player itself (the sound output changed):
			 * the controls up, play at hand */
			if (!vc.playing && !vc.shown)
				vc_show(true);
			vc_hide_later();
		}
		vc.pos_ms = pos;
		vc.dur_ms = dur;
	}
	if (vc.up && vc.prot != ui_rot) {
		snprintf(buf, sizeof(buf), "rot %d", ui_rot);
		vc_send(buf);
		vc.prot = ui_rot;
	}
	vc_update();
}

static void vc_poll(lv_timer_t *t)
{
	uint32_t period = !vc.up || vc.shown || vc.dragging ? 250 : 2000;

	if (t->period != period)
		lv_timer_set_period(t, period);
	if (vc.up && vc.prot != ui_rot)
		vc_rx(vc.sock);		/* (a turn: tell the player now) */
	vc_send("status");
}

static void vc_quiet(void)
{
	vc.quiet_ms = lv_tick_get();
}

static void vc_tap_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	vc_show(!vc.shown);
}

static void vc_play_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	vc_send("toggle");
	vc.playing = !vc.playing;
	vc_quiet();
	vc_update();
	vc_hide_later();
}

static void vc_skip_cb(lv_event_t *e)
{
	int d = (int)(intptr_t)lv_event_get_user_data(e);
	char m[32];

	snprintf(m, sizeof(m), "skip %d", d);
	vc_send(m);
	vc.pos_ms = LV_CLAMP(0, vc.pos_ms + d, vc.dur_ms > 0 ? vc.dur_ms : vc.pos_ms + d);
	vc_quiet();
	vc_update();
	vc_hide_later();
}

static void vc_slider_cb(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	char b[24];

	if (c == LV_EVENT_PRESSED) {
		vc.dragging = true;
		vc_hide_later();	/* (stops it) */
	} else if (c == LV_EVENT_VALUE_CHANGED) {
		vc_fmt(b, sizeof(b), (int64_t)lv_slider_get_value(vc.slider) * 100);
		label_set_if_changed(vc.pos, b);
	} else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
		char m[32];

		vc.dragging = false;
		vc.pos_ms = (int64_t)lv_slider_get_value(vc.slider) * 100;
		snprintf(m, sizeof(m), "seek %lld", (long long)vc.pos_ms);
		vc_send(m);
		vc_quiet();
		vc_hide_later();
	}
}

static void vc_back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	app_end();		/* back to where it was started (Files) */
}

static void vc_rot_show(void);

/*
 * The turn button holds the screen the other way for this video (sideways
 * on the side the iPod was last held, or upright), until it is pressed
 * again or the video ends; the accelerometer does not undo it.
 */
static void vc_rot_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	video_rot = video_rot >= 0 ? -1 : ui_rot ? 0 : 90;	/* (90: sideways) */
	rot_kick();
	vc_rot_show();
	vc_hide_later();
}

/* A round icon button: transparent, white, a state layer when pressed. */
static lv_obj_t *vc_icon_btn(lv_obj_t *parent, const char *sym, int32_t size,
			     const lv_font_t *font, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	plain(b);
	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(b, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);	/* (M3: 12 % over video reads too faint) */
	lv_obj_set_style_shadow_width(b, 0, 0);
	l = m3_label(b, sym, font, lv_color_white());
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

static void vc_build(void)
{
	lv_obj_t *o, *row, *b, *col;
	bool land = W > H;

	if (vc.root || !app_scr)
		return;
	/* the whole screen takes taps: on the video, they show and hide */
	vc.root = lv_obj_create(app_scr);
	lv_obj_remove_style_all(vc.root);
	lv_obj_set_size(vc.root, W, H);
	lv_obj_remove_flag(vc.root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(vc.root, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(vc.root, vc_tap_cb, LV_EVENT_CLICKED, NULL);

	vc.ui = lv_obj_create(vc.root);
	lv_obj_remove_style_all(vc.ui);
	lv_obj_set_size(vc.ui, W, H);
	lv_obj_set_style_bg_color(vc.ui, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(vc.ui, LV_OPA_40, 0);	/* the scrim */
	lv_obj_remove_flag(vc.ui, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(vc.ui, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(vc.ui, vc_tap_cb, LV_EVENT_CLICKED, NULL);

	/* top: Back, the title, the turn button */
	row = lv_obj_create(vc.ui);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, W - 16, 56);
	lv_obj_set_pos(row, 8, 4);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 4, 0);
	vc_icon_btn(row, LV_SYMBOL_LEFT, 48, &lv_font_montserrat_20, vc_back_cb, NULL);
	vc.title = m3_label(row, vc.name, &lv_font_montserrat_16, lv_color_white());
	lv_label_set_long_mode(vc.title, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(vc.title, 1);
	b = vc.rot_btn = vc_icon_btn(row, LV_SYMBOL_REFRESH, 48, &lv_font_montserrat_20,
				     vc_rot_cb, NULL);
	vc.rot_lbl = lv_obj_get_child(b, 0);
	vc_rot_show();

	/* middle: back 10 s, play/pause, on 10 s */
	row = lv_obj_create(vc.ui);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, 264, 96);
	lv_obj_align(row, LV_ALIGN_CENTER, 0, land ? 0 : -8);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	for (int k = 0; k < 3; k++) {
		if (k == 1) {
			/* filled: M3's emphasis for the main action */
			b = lv_button_create(row);
			plain(b);
			lv_obj_set_size(b, 80, 80);
			lv_obj_set_style_radius(b, 28, 0);	/* (M3 large FAB shape) */
			lv_obj_set_style_bg_color(b, M.primary_c, 0);
			lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
			lv_obj_set_style_shadow_width(b, 0, 0);
			lv_obj_set_style_transform_scale(b, 240, LV_STATE_PRESSED);
			vc.play_sym = m3_label(b, vc.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
					       &lv_font_montserrat_28, M.on_primary_c);
			lv_obj_center(vc.play_sym);
			lv_obj_add_event_cb(b, vc_play_cb, LV_EVENT_CLICKED, NULL);
			continue;
		}
		b = vc_icon_btn(row, "", 64, &lv_font_montserrat_20, vc_skip_cb,
				(void *)(intptr_t)(k ? 10000 : -10000));
		lv_obj_delete(lv_obj_get_child(b, 0));
		col = lv_obj_create(b);
		lv_obj_remove_style_all(col);
		lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(col, 0, 0);
		lv_obj_center(col);
		m3_label(col, k ? LV_SYMBOL_NEXT : LV_SYMBOL_PREV, &lv_font_montserrat_20, lv_color_white());
		m3_label(col, "10", &lv_font_montserrat_12, lv_color_white());
	}

	/* bottom: the time and the slider (M3: primary active track, a
	 * light inactive one over video) */
	row = lv_obj_create(vc.ui);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, W - 2 * STATUS_PAD, 64);
	lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	vc.pos = m3_label(row, "0:00", &lv_font_montserrat_12, lv_color_white());
	lv_obj_align(vc.pos, LV_ALIGN_TOP_LEFT, 4, 4);
	vc.dur = m3_label(row, "", &lv_font_montserrat_12, lv_color_hex(0xd0d0d0));
	lv_obj_align(vc.dur, LV_ALIGN_TOP_RIGHT, -4, 4);
	o = vc.slider = lv_slider_create(row);
	lv_obj_set_size(o, lv_pct(100), 4);
	lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -18);
	lv_obj_set_style_bg_color(o, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(o, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_radius(o, 2, LV_PART_MAIN);
	lv_obj_set_style_bg_color(o, M.primary, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(o, M.primary, LV_PART_KNOB);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(o, 6, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(o, 0, LV_PART_KNOB);
	lv_obj_set_style_pad_all(o, 6, LV_PART_KNOB | LV_STATE_PRESSED);
	lv_obj_set_ext_click_area(o, 16);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(o, vc_slider_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(o, vc_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(o, vc_slider_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(o, vc_slider_cb, LV_EVENT_PRESS_LOST, NULL);
	vc_update();
}

/* The turn button filled (primary) while it holds the screen. */
static void vc_rot_show(void)
{
	bool held = video_rot >= 0;

	if (!vc.rot_btn)
		return;
	lv_obj_set_style_bg_color(vc.rot_btn, held ? M.primary_c : lv_color_white(), 0);
	lv_obj_set_style_bg_opa(vc.rot_btn, held ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_text_color(vc.rot_lbl, held ? M.on_primary_c : lv_color_white(), 0);
}

/* Rebuilt at a new size, as it was. */
static void vc_turned(void)
{
	bool shown = vc.shown;

	if (!vc.root)
		return;
	lv_obj_delete(vc.root);
	vc.root = vc.ui = vc.rot_btn = vc.rot_lbl = NULL;
	vc_build();
	if (shown) {
		lv_obj_remove_flag(vc.ui, LV_OBJ_FLAG_HIDDEN);
		vc.shown = true;
		vc_hide_later();
	}
}

/* A video application is starting: listen for its player. */
static void vc_start(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };

	vc.up = vc.shown = vc.dragging = false;
	vc.playing = true;
	vc.pos_ms = vc.dur_ms = 0;
	vc.prot = -1;
	vc.name[0] = 0;
	video_rot = -1;
	if (vc.sock < 0) {
		vc.sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		/* an address of its own (autobind), for the answers */
		if (vc.sock >= 0 && bind(vc.sock, (struct sockaddr *)&a, sizeof(sa_family_t))) {
			close(vc.sock);
			vc.sock = -1;
		}
	}
	if (vc.sock >= 0) {
		/* answers of the last player still queued: not this one's */
		char b[256];

		while (recv(vc.sock, b, sizeof(b), MSG_DONTWAIT) > 0)
			;
	}
	if (vc.sock >= 0)
		ui_watch(vc.sock, vc_rx);
	if (!vc.poll)
		vc.poll = lv_timer_create(vc_poll, 250, NULL);
	fprintf(stderr, "n72-ui: video controls for %s (socket %d)\n", app_cmd, vc.sock);
}

static void vc_stop(void)
{
	if (vc.sock >= 0)
		ui_unwatch(vc.sock);
	if (vc.poll) {
		lv_timer_delete(vc.poll);
		vc.poll = NULL;
	}
	if (vc.hide) {
		lv_timer_delete(vc.hide);
		vc.hide = NULL;
	}
	if (vc.root && lv_obj_is_valid(vc.root))
		lv_obj_delete(vc.root);
	vc.root = vc.ui = vc.rot_btn = vc.rot_lbl = NULL;
	vc.up = vc.shown = false;
	video_rot = -1;
}

/*
 * BlueZ's D-Bus signals (a device connecting or going, the adapter on or
 * off, a device found in a scan) through dbus-monitor, read in the main
 * loop: the Bluetooth views are read again when something changed, at
 * most once a second (a scan sends many), instead of every 2-3 s.
 */
static int bz_fd = -1;
static pid_t bz_pid;
static lv_timer_t *bz_timer;
static uint32_t bz_last;

static void bz_start(lv_timer_t *t);

static void bz_changed(lv_timer_t *t)
{
	LV_UNUSED(t);
	static bool had_audio;
	bool audio = !access("/run/n72-audio", F_OK);

	bz_timer = NULL;
	bz_last = lv_tick_get();
	if (had_audio && !audio)
		bt_auto_kick();		/* the sound device went: try again soon */
	had_audio = audio;
	job_run("bluetoothctl show", NULL, bt_show_done, NULL);	/* on or off */
	if (bt_page && lv_obj_is_valid(bt_page))
		bt_page_refresh();
	if (btd_page && lv_obj_is_valid(btd_page))
		btd_refresh();
}

static void bz_ev(int fd)
{
	char buf[2048];
	ssize_t n;
	bool any = false;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		any = true;
	if (n == 0) {
		/* dbus-monitor ended (the bus restarted?): again in a while */
		ui_unwatch(fd);
		close(fd);
		bz_fd = -1;
		waitpid(bz_pid, NULL, WNOHANG);
		lv_timer_t *r = lv_timer_create(bz_start, 5000, NULL);

		lv_timer_set_repeat_count(r, 1);
		return;
	}
	if (any && !bz_timer) {
		uint32_t since = lv_tick_elaps(bz_last);

		bz_timer = lv_timer_create(bz_changed, since >= 1000 ? 250 : 1250 - since, NULL);
		lv_timer_set_repeat_count(bz_timer, 1);
	}
}

static void bz_start(lv_timer_t *t)
{
	char *const argv[] = { "dbus-monitor", "--system", "type='signal',sender='org.bluez'",
			       NULL };
	posix_spawn_file_actions_t fa;
	int p[2];

	LV_UNUSED(t);
	if (bz_fd >= 0 || !have_tool("dbus-monitor") || pipe2(p, O_CLOEXEC))
		return;
	fcntl(p[0], F_SETFL, O_NONBLOCK);	/* (ours only: dbus-monitor writes and waits) */
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	if (posix_spawnp(&bz_pid, "dbus-monitor", &fa, NULL, argv, environ)) {
		close(p[0]);
		p[0] = -1;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (p[0] < 0)
		return;
	bz_fd = p[0];
	ui_watch(bz_fd, bz_ev);
}

/* The settings kept on the flash, applied once at start. */
static void ui_apply_saved(void)
{
	if (access(CONF_PATH, F_OK))
		conf_save();	/* (the first start: what was in /tmp, onto the flash) */
	if (conf.bright >= 5)
		backlight_set(conf.bright);
	if (conf.wifi == 0 && have_tool("n72-wifi") && wifi_powered())
		job_run("n72-wifi off", NULL, NULL, NULL);
	if (conf.bt == 0 && have_tool("bluetoothctl"))
		job_run(BT_OFF_CMD, NULL, bt_set_done, NULL);
	rtnl_init();		/* the status bar's Wi-Fi follows rtnetlink */
	clockset_init();	/* and the time a clock change */
	unlink(BT_AUTO_RUN);	/* (left by an n72-ui that ended mid-run) */
	unlink(BT_MANUAL_RUN);
	bt_auto_timer = awake_timer(bt_auto_tick, BT_AUTO_MIN_MS, NULL);	/* Connect automatically */
	apps_watch();		/* the home tiles follow APPS_DIR and DESKTOP_DIR */
	bz_start(NULL);		/* BlueZ's signals: the Bluetooth views follow */
}
