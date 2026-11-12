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
#include "lvgl/src/indev/lv_indev_private.h"	/* the throw velocity */
#include "lvgl/src/core/lv_obj_scroll_private.h"	/* lv_obj_scroll_by_raw() */
#include <sys/wait.h>

extern char **environ;

static bool system_sleep(void);

/* ------------------------------------------------------------ settings */

#define CONF_PATH "/tmp/n72-ui.conf"

static struct {
	bool dark;
	int accent;		/* index into accents[] */
	int idle_s;		/* screen idle timeout, 0 = never */
	int vol;		/* sound volume, 0..100 % (0 = mute) */
	bool spk;		/* speaker when no earphones are plugged */
} conf = { true, 0, 120, 60, true };

static const struct { const char *name; uint16_t hue; uint8_t sat; } accents[] = {
	{ "Violet", 265, 48 }, { "Blue", 215, 60 }, { "Teal", 178, 55 },
	{ "Green", 135, 40 }, { "Amber", 38, 70 }, { "Rose", 340, 55 },
};
#define N_ACCENTS ((int)(sizeof(accents) / sizeof(accents[0])))

static void conf_load(void)
{
	char line[64];
	FILE *f = fopen(CONF_PATH, "r");
	const char *e = getenv("N72_IDLE_SLEEP");

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
	}
	fclose(f);
}

static void conf_save(void)
{
	FILE *f = fopen(CONF_PATH, "w");

	if (!f)
		return;
	fprintf(f, "dark=%d\naccent=%d\nidle=%d\nvol=%d\nspk=%d\n", conf.dark, conf.accent,
		conf.idle_s, conf.vol, conf.spk);
	fclose(f);
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
	uint16_t h = accents[conf.accent].hue, h2 = (h + 60) % 360;
	uint8_t s = accents[conf.accent].sat, sn = 8, snv = 14;

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

/*
 * A 64 px list row: badge, title and supporting text, optional trailing
 * widget area (returned in *trail). Clicking runs cb.
 */
static lv_obj_t *m3_row(lv_obj_t *parent, const char *icon, const char *title,
			const char *sub, lv_event_cb_t cb, void *ud, lv_obj_t **sub_lbl)
{
	lv_obj_t *r = lv_obj_create(parent), *t, *col;

	plain(r);
	lv_obj_set_size(r, lv_pct(100), 64);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(r, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_style_radius(r, 16, 0);
	lv_obj_set_style_pad_hor(r, 8, 0);
	lv_obj_set_style_pad_ver(r, 0, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 14, 0);
	if (icon)
		m3_badge(r, icon, M.secondary_c, M.on_secondary_c, 40);
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
	if (cb) {
		lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
		m3_label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, M.on_surface_v);
	}
	return r;
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
#define TR_MS 220

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

static lv_draw_buf_t *snap(lv_obj_t *obj, lv_color_format_t cf)
{
	lv_draw_buf_t *b;

	lv_obj_update_layout(obj);
	b = lv_snapshot_take(obj, cf);
	if (b)
		lv_draw_buf_flush_cache(b, NULL);	/* the GPU reads it */
	return b;
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
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, tr_done_cb);
	lv_anim_start(&a);
	return true;
}

/* Slide to @to: left for forward, right for back; @del_from: delete the old screen. */
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
#define NAV_MAX 6
static lv_obj_t *nav[NAV_MAX];
static int nav_n;

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

static void go_home(void)
{
	if (locked)
		return;		/* Home does not unlock */
	/*
	 * A shade drag, its settling or an app animation has a screen of its
	 * own on display, which it deletes itself: end those first, or the
	 * slide below would delete that screen as well.
	 */
	ui_settle_all();
	if (page_home_over_app())
		return;
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
 * Flick and glide. LVGL's own throw stopped hard: its velocity is a running
 * average of the last input periods that the lift itself halves (the last
 * read moves 0), and it lost 10 % per input period with integer steps (a
 * time constant of ~150 ms). Instead: the velocity is measured over the
 * last 80 ms of touch samples, and the page glides with an exponential
 * decay (time constant GLIDE_TAU_MS, sub-pixel steps), as iOS and Android
 * do, stopping at the end of the page; a touch catches it at once.
 *
 * Fling acceleration, also as on iOS: a flick in the same direction within
 * FLING_GAP_MS of the last one throws 1.6x faster each time, up to 5x; a
 * slow flick, a pause or a change of direction starts again at 1x.
 */
#define GLIDE_TAU_MS	480.0f	/* speed falls to 1/e in this time (iOS ~500) */
#define GLIDE_MIN_V	30.0f	/* px/s: slower than this is a stop */
#define FLICK_WINDOW_US	80000	/* release velocity from this much of the drag */
#define FLING_GAP_MS	700
#define FLING_MIN_V	400.0f	/* px/s: slower is a drag, not a flick */

#define TS_N	16
static struct {
	uint64_t t[TS_N];
	int32_t x[TS_N], y[TS_N];
	int n, head;
} ts;			/* recent touch samples while pressed */

static struct {
	lv_obj_t *obj;
	bool hor;
	float v0;	/* px/s at the lift, signed as the finger moved */
	float tau_ms;	/* this glide's time constant */
	int32_t done;	/* px moved so far */
	bool on;
} gl;

static void glide_exec(void *var, int32_t ms);

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
	glide_end("slowed down");
}

/*
 * One step per frame: an LVGL animation (stepped at the start of every
 * refresh), whose value is the time since the lift. Position at time t of
 * an exponential glide: v0 * tau * (1 - e^(-t/tau)).
 */
static void glide_exec(void *var, int32_t ms)
{
	float tau = gl.tau_ms / 1000.0f;
	int32_t target, step, room;

	LV_UNUSED(var);
	if (!gl.on || !gl.obj || !lv_obj_is_valid(gl.obj)) {
		gl.on = false;
		gl.obj = NULL;
		return;
	}
	target = (int32_t)lroundf(gl.v0 * tau * (1.0f - expf(-ms / gl.tau_ms)));
	step = target - gl.done;
	if (!step)
		return;
	/* positive: content follows a finger moving down/right (its top/left comes in) */
	room = step > 0 ? (gl.hor ? lv_obj_get_scroll_left(gl.obj) : lv_obj_get_scroll_top(gl.obj))
			: (gl.hor ? lv_obj_get_scroll_right(gl.obj) : lv_obj_get_scroll_bottom(gl.obj));
	if (room <= 0) {
		glide_end("at the end");
		lv_anim_delete(&gl, glide_exec);
		return;
	}
	if (LV_ABS(step) > room)
		step = step > 0 ? room : -room;
	gl.done += step;
	/* raw: lv_obj_scroll_by() wraps each step in SCROLL_BEGIN/END */
	if (gl.hor)
		lv_obj_scroll_by_raw(gl.obj, step, 0);
	else
		lv_obj_scroll_by_raw(gl.obj, 0, step);
}

static void glide_start(lv_obj_t *obj, bool hor, float v)
{
	lv_anim_t a;
	float tau_ms = GLIDE_TAU_MS;
	int32_t room, dur;

	/*
	 * Would it run past the end of the page (the whole glide covers
	 * v * tau)? Then a shorter time constant for this one, so it slows
	 * to a stop at the end instead of hitting it at speed.
	 */
	room = v > 0 ? (hor ? lv_obj_get_scroll_left(obj) : lv_obj_get_scroll_top(obj))
		     : (hor ? lv_obj_get_scroll_right(obj) : lv_obj_get_scroll_bottom(obj));
	if (room > 0 && fabsf(v) * tau_ms / 1000.0f > room)
		tau_ms = LV_MAX(1000.0f * room / fabsf(v), 20.0f);
	/* until the speed falls below GLIDE_MIN_V */
	dur = (int32_t)(tau_ms * logf(fabsf(v) / GLIDE_MIN_V));
	glide_stop();
	if (dur <= 0 || room <= 0)
		return;
	gl.tau_ms = tau_ms;
	gl.obj = obj;
	gl.hor = hor;
	gl.v0 = v;
	gl.done = 0;
	gl.on = true;
	lv_obj_send_event(obj, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &gl);
	lv_anim_set_exec_cb(&a, glide_exec);
	lv_anim_set_values(&a, 0, dur);
	lv_anim_set_duration(&a, dur);
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_set_completed_cb(&a, glide_done);
	lv_anim_start(&a);
}

/* a press anywhere stops a glide */
static void ts_press_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	glide_stop();
}

/*
 * The touch path, sampled at each scroll step the finger makes (LVGL sends
 * no PRESSING to input devices); a glide's own steps have no active input.
 */
static void ts_cb(lv_event_t *e)
{
	lv_indev_t *in = lv_indev_active();
	lv_point_t p;

	if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN)
		ts.n = 0;
	if (!in || lv_indev_get_state(in) != LV_INDEV_STATE_PRESSED)
		return;
	lv_indev_get_point(in, &p);
	ts.head = (ts.head + 1) % TS_N;
	ts.t[ts.head] = mono_us();
	ts.x[ts.head] = p.x;
	ts.y[ts.head] = p.y;
	if (ts.n < TS_N)
		ts.n++;
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

static void fling_cb(lv_event_t *e)
{
	static uint32_t last_ms;
	static int last_sign, mult10 = 10;
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_indev_t *in = lv_event_get_param(e);
	bool hor;
	float v;
	int sign, room_back;

	if (!in)
		return;
	hor = in->pointer.scroll_dir == LV_DIR_HOR;
	/* LVGL's throw off: the glide below replaces it */
	in->pointer.scroll_throw_vect.x = in->pointer.scroll_throw_vect.y = 0;
	in->pointer.scroll_throw_vect_ori = in->pointer.scroll_throw_vect;
	/* pulled past an end: LVGL springs it back */
	room_back = hor ? LV_MIN(lv_obj_get_scroll_left(obj), lv_obj_get_scroll_right(obj)) :
			  LV_MIN(lv_obj_get_scroll_top(obj), lv_obj_get_scroll_bottom(obj));
	if (room_back < 0)
		return;
	v = ts_velocity(hor);
	sign = v > 0 ? 1 : -1;
	if (fabsf(v) < FLING_MIN_V) {
		mult10 = 10;
		last_sign = 0;
	} else {
		if (sign == last_sign && lv_tick_elaps(last_ms) < FLING_GAP_MS)
			mult10 = LV_MIN(mult10 * 16 / 10, 50);
		else
			mult10 = 10;
		last_sign = sign;
		last_ms = lv_tick_get();
		v = v * mult10 / 10;
	}
	if (getenv("N72_FPS"))
		fprintf(stderr, "flick: %.0f px/s (x%d.%d)\n", v, mult10 / 10, mult10 % 10);
	if (fabsf(v) >= GLIDE_MIN_V)
		glide_start(obj, hor, v);
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
		if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER)
			lv_indev_add_event_cb(in, ts_press_cb, LV_EVENT_PRESSED, NULL);
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

/* One picture per tick, while nothing moves and nobody touches. */
static void pf_tick(lv_timer_t *t)
{
	lv_obj_t *body, *units[SC_UNITS], *best = NULL;
	lv_area_t bc, cc;
	uint32_t i, cnt;
	int32_t best_d = INT32_MAX, d;
	lv_draw_buf_t *b;
	int k;

	LV_UNUSED(t);
	if (screen_off || tr.scr || sc.owner || lv_display_get_inactive_time(NULL) < 300)
		return;
	body = pf_body_on(lv_screen_active());
	if (body != pf.owner) {
		pf_clear();
		pf.owner = body;
	}
	if (!body || pf.n >= PREF_MAX || pf.bytes >= PREF_BUDGET)
		return;
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
	if (!best || !(b = sc_picture(best)))
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
	lv_timer_create(pf_tick, 80, NULL);
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
 * The scroll bar fades in as a page starts to move, stays a moment after it
 * stops and fades out (LVGL's "active" mode switched it on and off with
 * the scroll, a flash).
 */
#define SB_IN_MS	120
#define SB_HOLD_MS	600
#define SB_OUT_MS	300

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
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, sb_opa);
	lv_anim_set_values(&a, now, in ? LV_OPA_COVER : LV_OPA_TRANSP);
	lv_anim_set_duration(&a, in ? SB_IN_MS : SB_OUT_MS);
	lv_anim_set_delay(&a, in ? 0 : SB_HOLD_MS);
	lv_anim_set_path_cb(&a, in ? lv_anim_path_ease_out : lv_anim_path_ease_in_out);
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
	lv_obj_set_style_pad_hor(body, 12, 0);
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

static void push(lv_obj_t *scr)
{
	if (nav_n < NAV_MAX)
		nav[nav_n++] = scr;
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
	char buf[8192];
	size_t len;
	void (*done)(const char *out, void *ud);
	void *ud;
	lv_obj_t *owner;
};

static void job_poll(lv_timer_t *t)
{
	struct job *j = lv_timer_get_user_data(t);
	ssize_t n;
	int st;

	while ((n = read(j->fd, j->buf + j->len, sizeof(j->buf) - 1 - j->len)) > 0)
		j->len += n;
	if (waitpid(j->pid, &st, WNOHANG) == 0)
		return;
	while ((n = read(j->fd, j->buf + j->len, sizeof(j->buf) - 1 - j->len)) > 0)
		j->len += n;
	j->buf[j->len] = 0;
	close(j->fd);
	lv_timer_delete(t);
	if (j->done && (!j->owner || lv_obj_is_valid(j->owner)))
		j->done(j->buf, j->ud);
	free(j);
}

static void job_run(const char *cmd, lv_obj_t *owner,
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

static void job_run(const char *cmd, lv_obj_t *owner,
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
			return;
		}
		free(l);
	}
	j = calloc(1, sizeof(*j));
	if (!j || pipe(p)) {
		free(j);
		return;
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
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	j->fd = p[0];
	j->done = done;
	j->ud = ud;
	j->owner = owner;
	lv_timer_create(job_poll, 150, j);
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

static bool have_tool(const char *path)
{
	return access(path, X_OK) == 0;
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
	const char *cur = lv_label_get_text(lbl);

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

static void wifi_status_done(const char *out, void *ud)
{
	char st[32] = "";

	LV_UNUSED(ud);
	kv_get(out, "wpa_state", st, sizeof(st));
	wifi_up = !strcmp(st, "COMPLETED");
	kv_get(out, "ssid", wifi_ssid, sizeof(wifi_ssid));
	if (!wifi_up)
		wifi_ssid[0] = 0;
}

static void bt_show_done(const char *out, void *ud)
{
	LV_UNUSED(ud);
	bt_powered = strstr(out, "Powered: yes") != NULL;
}

/*
 * wpa_cli is a small static binary (~10 ms); bluetoothctl starts glib and
 * D-Bus (~160 ms), so Bluetooth is only asked at start-up: after that its
 * state changes only through the Bluetooth page, which reads it back.
 */
static void status_refresh(bool with_bt)
{
	if (have_tool("/usr/sbin/wpa_cli"))
		job_run("wpa_cli -i wlan0 status", NULL, wifi_status_done, NULL);
	if (with_bt && have_tool("/usr/bin/bluetoothctl"))
		job_run("bluetoothctl show", NULL, bt_show_done, NULL);
}

/* ------------------------------------------------------------- home */

static lv_obj_t *clock_lbl, *date_lbl, *batt_lbl, *status_icons, *status_time;
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
	snprintf(buf, len, "%s%d%%  %s", strncmp(st, "Charging", 8) ? "" :
		 LV_SYMBOL_CHARGE " ", cap, icon);
}

static lv_timer_t *home_timer;

static void qs_style(lv_obj_t *tile, bool on)
{
	lv_color_t c = on ? M.primary_c : M.surface_c_high;

	if (!lv_color_eq(lv_obj_get_style_bg_color(tile, 0), c)) {
		lv_obj_set_style_bg_color(tile, c, 0);
		shade_changed(tile);
	}
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
	if (now - last_status >= 30 || now < last_status) {
		status_refresh(false);	/* lands by the next run */
		last_status = now;
	}
	localtime_r(&now, &tm);
	/*
	 * No seconds on screen: run again just after the next minute starts,
	 * or in 5 s while a Wi-Fi answer is pending.
	 */
	if (t)
		lv_timer_set_period(t, now - last_status < 2 ? 5000 :
				    (60 - tm.tm_sec) * 1000 + 50);
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
	if (home_net)
		label_set_if_changed(home_net, wifi_up ? wifi_ssid : "Wi-Fi off");
	snprintf(buf, sizeof(buf), "%s%s", wifi_up ? LV_SYMBOL_WIFI "  " : "",
		 bt_powered ? LV_SYMBOL_BLUETOOTH : "");
	label_set_if_changed(status_icons, buf);
	label_set_if_changed(qs_wifi_sub, !wifi_powered() ? "Off" :
			     wifi_up ? wifi_ssid : "Not connected");
	if (set_wifi_sub && lv_obj_is_valid(set_wifi_sub))
		label_set_if_changed(set_wifi_sub, wifi_up ? wifi_ssid : "Not connected");
	if (set_bt_sub && lv_obj_is_valid(set_bt_sub))
		label_set_if_changed(set_bt_sub, !have_tool("/usr/bin/bluetoothctl") ?
				     "Not installed" : bt_powered ? "On" : "Off");
	label_set_if_changed(qs_bt_sub, !have_tool("/usr/bin/bluetoothctl") ? "Unavailable" :
			     bt_powered ? "On" : "Off");
	qs_style(qs_wifi, wifi_powered());
	qs_style(qs_bt, bt_powered);
	app_bar_update();
}

static void open_settings(lv_event_t *e);
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
	lv_obj_set_size(t, 142, 64);
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
	lv_obj_set_size(col, 86, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	m3_label(col, title, &lv_font_montserrat_14, M.on_surface);
	*sub = m3_label(col, "", &lv_font_montserrat_12, M.on_surface_v);
	lv_label_set_long_mode(*sub, LV_LABEL_LONG_DOT);
	lv_obj_set_size(*sub, 86, 16);
	lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
	return t;
}

/* An app tile on the home screen. */
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
	m3_label(t, name, &lv_font_montserrat_14, M.on_surface);
	lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *home_bright, *shade, *scrim, *qs_settings_btn;
static bool shade_open;

/* Screen back on: bring the clock up to date now, not at the next run. */
static void home_wake(void)
{
	if (home_timer)
		lv_timer_ready(home_timer);
	lock_wake();
}

static void home_bright_cb(lv_event_t *e)
{
	backlight_set(lv_slider_get_value(lv_event_get_target(e)));
	shade_changed(home_bright);
}

/* ------------------------------------------------- quick settings shade */

#define SHADE_H 312

static void shade_closed(void)
{
	lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
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

static void qs_refresh_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	status_refresh(true);
	home_update(NULL);
}

static void qs_open_wifi(lv_event_t *e)
{
	bool on = !wifi_powered();

	LV_UNUSED(e);
	if (!have_tool("/usr/sbin/n72-wifi"))
		return;
	qs_style(qs_wifi, on);
	label_set_if_changed(qs_wifi_sub, on ? "Turning on..." : "Off");
	/* (on: the card's power-up and firmware take a few seconds) */
	job_run(on ? "n72-wifi on; sleep 4" : "n72-wifi off", NULL, qs_refresh_done, NULL);
}

static void qs_open_bt(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!have_tool("/usr/bin/bluetoothctl"))
		return;
	bt_set(!bt_powered);
	qs_style(qs_bt, !bt_powered);
	label_set_if_changed(qs_bt_sub, !bt_powered ? "Turning on..." : "Off");
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
	lv_obj_align(clock_lbl, LV_ALIGN_TOP_LEFT, 20, 24);
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
	lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, 22, 84);

	row = lv_obj_create(shade);
	plain(row);
	lv_obj_set_size(row, W - 24, 64);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 128);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	qs_wifi = qs_tile(row, LV_SYMBOL_WIFI, "Wi-Fi", &qs_wifi_sub, qs_open_wifi);
	qs_bt = qs_tile(row, LV_SYMBOL_BLUETOOTH, "Bluetooth", &qs_bt_sub, qs_open_bt);

	row = lv_obj_create(shade);
	plain(row);
	lv_obj_set_size(row, W - 24, 56);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 204);
	lv_obj_set_style_bg_color(row, M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(row, 18, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);
	m3_label(row, LV_SYMBOL_IMAGE, &lv_font_montserrat_16, M.on_surface_v);
	home_bright = lv_slider_create(row);
	lv_obj_set_flex_grow(home_bright, 1);
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
 * "exec=<shell command>"; each becomes a tile on the home screen.
 */
#define APPS_DIR "/usr/share/n72-ui/apps"

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

static lv_obj_t *app_bar, *app_bar_time, *app_bar_icons, *app_bar_batt;
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
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (done)
		lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

static void app_shade_slide(bool open)
{
	app_slide(shade, open ? 0 : -SHADE_H, open ? NULL : app_shade_done);
}

static void app_bar_set(bool show);

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
		lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
		app_slide(app_bar, 0, NULL);
	} else {
		if (!app_bar_shown)
			return;
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
	if (app_pid <= 0)
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

static void app_bar_build(void)
{
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
	lv_obj_set_style_pad_hor(app_bar, 16, 0);
	lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* its own */
	app_bar_time = m3_label(app_bar, "", &lv_font_montserrat_14, M.on_surface);
	lv_obj_align(app_bar_time, LV_ALIGN_LEFT_MID, 0, 0);
	app_bar_batt = m3_label(app_bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(app_bar_batt, LV_ALIGN_RIGHT_MID, 0, 0);
	app_bar_icons = m3_label(app_bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(app_bar_icons, LV_ALIGN_RIGHT_MID, -74, 0);
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
	label_set_if_changed(app_bar_icons, lv_label_get_text(status_icons));
	label_set_if_changed(app_bar_batt, lv_label_get_text(batt_lbl));
}

static bool in_obj(lv_obj_t *o, int x, int y)
{
	lv_area_t a;

	if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
		return false;
	lv_obj_get_coords(o, &a);
	return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

/* Does a touch that begins at (x, y) belong to the launcher? */
static bool ui_wants_touch(int x, int y)
{
	if (locked || app_pid <= 0 || lv_screen_active() != app_scr)
		return true;
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

static void shade_changed(lv_obj_t *o)
{
	for (; o; o = lv_obj_get_parent(o))
		if (o == shade) {
			shade_pic_dirty = true;
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
		lv_obj_set_y(shade, sd.y);
		lv_obj_set_style_bg_opa(scrim, sd_scrim_opa(sd.y), 0);
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
		if (app_pid > 0 && !sd.to_page)
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
	    !(lv_screen_active() == home_scr || (app_scr && lv_screen_active() == app_scr)))
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
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
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
	bool home = lv_screen_active() == home_scr;
	lv_draw_buf_t *bg = NULL, *fg;

	if (touch)
		ui_touch_lost();
	tr_finish();
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
	tr_free(&sd.home_pic);
	fg = shade_pic_get();
	sd.fg_cached = fg != NULL;
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
		if (app_bar && !bg)
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
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
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

	lock_date = m3_label(lock_scr, "", &lv_font_montserrat_20, lv_color_hex(0xe8e0f0));
	lv_obj_align(lock_date, LV_ALIGN_TOP_MID, 0, 64);
	lock_time = m3_label(lock_scr, "", clock_font ? clock_font : &lv_font_montserrat_48,
			     lv_color_white());
	lv_obj_align(lock_time, LV_ALIGN_TOP_MID, 0, 90);
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
	za_finish();
	sd_settle_now();
	tr_finish();
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
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
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
static bool ui_drag(int phase, int x, int y)
{
	uint32_t now = lv_tick_get();

	if (locked || ul.scr)
		return ul_drag(phase, y);
	if (phase == 0) {
		bool home = lv_screen_active() == home_scr && !tr.scr;
		bool app = app_pid > 0 && lv_screen_active() == app_scr;

		sd_settle_now();
		sd.cand = 0;
		sd.px = x;
		sd.py = y;
		sd.last_y = y;
		sd.last_t = now;
		if (!shade || screen_off || (!home && !app))
			return false;
		if (shade_open && !in_obj(home_bright, x, y))
			sd.cand = -1;
		else if (!shade_open && (home || (app_bar_shown && y < APP_BAR_H)))
			sd.cand = 1;
		return false;
	}
	if (!sd.on) {
		int dy = y - sd.py, dx = x - sd.px;

		if (!sd.cand || phase == 2)
			return false;
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
 * (GPU: the picture, a rounded fill), stays while the application starts
 * below, and fades to show it. Closing runs it backwards: the card fades
 * in over the application, then shrinks into its icon over home, and
 * home's icon shows through as it ends. Fades are drawn with alpha by the
 * CPU (the launcher's transparent mode); the moves are XRGB, on the GPU.
 */
#define ZA_GROW_MS	330
#define ZA_HOLD_MS	60
#define ZA_FADE_MS	170
#define ZA_SHRINK_MS	320

static struct {
	int phase;		/* 0 idle, 1 grow, 2 hold, 3 fade out, 4 fade in, 5 shrink */
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

static void za_rect(int32_t v)	/* 0 = the icon, 1024 = the screen */
{
	int32_t x1 = za.icon.x1 + (0 - za.icon.x1) * v / 1024;
	int32_t y1 = za.icon.y1 + (0 - za.icon.y1) * v / 1024;
	int32_t x2 = za.icon.x2 + (W - 1 - za.icon.x2) * v / 1024;
	int32_t y2 = za.icon.y2 + (H - 1 - za.icon.y2) * v / 1024;

	lv_obj_set_pos(za.card, x1, y1);
	lv_obj_set_size(za.card, x2 - x1 + 1, y2 - y1 + 1);
	lv_obj_set_style_radius(za.card, 20 + (28 - 20) * v / 1024, 0);
}

/*
 * The card's opacity as its fill's and its symbol's own, not the object's:
 * object opacity renders the card into a layer of its own and blends it,
 * all on the CPU in the launcher's transparent mode; a plain translucent
 * fill is one GPU fill (mbx_gpu.c, mbx_fill_straight).
 */
static void za_card_opa(lv_opa_t o)
{
	if (lv_obj_get_style_bg_opa(za.card, 0) == o)
		return;
	lv_obj_set_style_bg_opa(za.card, o, 0);
	lv_obj_set_style_text_opa(za.sym, o, 0);
}

static void za_exec(void *var, int32_t v)
{
	LV_UNUSED(var);
	switch (za.phase) {
	case 1:
		za_rect(v);
		break;
	case 5:
		za_rect(v);
		/* home's icon shows through in the last third */
		za_card_opa(v > 340 ? LV_OPA_COVER : (lv_opa_t)(255 * v / 340));
		break;
	case 3:
	case 4:
	case 6:
	case 7:
		za_card_opa((lv_opa_t)v);
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
	l = lv_obj_get_child(badge, 0);
	za.sym = m3_label(za.card, l ? lv_label_get_text(l) : "", &lv_font_montserrat_28,
			  lv_color_white());
	lv_obj_center(za.sym);
}

/* Home's picture, retaken if it went stale (the card covers the screen). */
static void za_home_fresh(void)
{
	if (!za.home_stale && za.home)
		return;
	lv_image_set_src(za.bg, NULL);
	tr_free(&za.home);
	za.home = snap(home_scr, LV_COLOR_FORMAT_XRGB8888);
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
			lv_obj_set_style_radius(za.card, 0, 0);
			za_run(6, LV_OPA_COVER, LV_OPA_TRANSP, ZA_FADE_MS, lv_anim_path_ease_in);
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
			za_run(5, 1024, 0, ZA_SHRINK_MS, lv_anim_path_ease_out);
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
	case 1:			/* grown: hold while the application starts */
		lv_obj_add_flag(za.bg, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_radius(za.card, 0, 0);
		za_run(2, 0, 1, ZA_HOLD_MS, lv_anim_path_linear);
		break;
	case 2:			/* fade the card: the application shows */
		/* the card alone: one GPU fill a frame (mbx_fill_straight) */
		lv_obj_add_flag(za.sym, LV_OBJ_FLAG_HIDDEN);
		mbx_gpu_set_transparent(true);
		za_run(3, LV_OPA_COVER, LV_OPA_TRANSP, ZA_FADE_MS, lv_anim_path_ease_in);
		break;
	case 3:
		za.phase = 0;
		lv_screen_load(app_pid > 0 ? app_scr : home_scr);
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
		za_run(5, 1024, 0, ZA_SHRINK_MS, lv_anim_path_ease_out);
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
	za_run(1, 0, 1024, ZA_GROW_MS, lv_anim_path_ease_out);
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
	lv_obj_set_style_radius(za.card, 0, 0);
	lv_screen_load(za.scr);
	za_run(7, LV_OPA_TRANSP, LV_OPA_COVER, ZA_FADE_MS, lv_anim_path_ease_out);
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
		lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
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
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
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
static void app_home_ui(void)
{
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
	} else if (lv_screen_active() != home_scr && !locked) {
		lv_screen_load(home_scr);
	}
	if (locked)
		lock_from = home_scr;
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
	lv_obj_set_style_radius(za.card, 0, 0);
	lv_screen_load(za.scr);
	za_run(4, LV_OPA_TRANSP, LV_OPA_COVER, ZA_FADE_MS, lv_anim_path_ease_out);
}

/* The application has gone (reaped). */
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
	if (app_pid > 0 && waitpid(app_pid, &st, WNOHANG) == app_pid)
		app_closed();
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
	envp = calloc(n + 2, sizeof(*envp));
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
		envp[i] = touch;
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
	/* idle while the card grows (the attribute alone did not take here);
	 * za_next gives it full priority when the grow ends */
}

static void app_launch_cb(lv_event_t *e)
{
	const char *cmd = lv_event_get_user_data(e);

	if (app_pid != 0 || za.phase || !mbx_gpu_on_shell_layer())
		return;
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
	za.tile = lv_event_get_current_target(e);
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
	za.home = grab_or_snap(home_scr);
	za.home_min = bp_minute();

	za.cmd = cmd;
	/* the shade comes down over the application from the top layer */
	lv_obj_set_parent(scrim, lv_layer_top());
	lv_obj_set_parent(shade, lv_layer_top());
	{
		/* started once the first frame of the grow is out (za_spawn_cb) */
		lv_timer_t *st = lv_timer_create(za_spawn_cb, 20, NULL);

		lv_timer_set_repeat_count(st, 1);
	}
	app_timer = lv_timer_create(app_poll, 200, NULL);
	app_pid = -1;		/* (starting: no second launch meanwhile) */

	za_build();
	lv_screen_load(za.scr);
	za_run(1, 0, 1024, ZA_GROW_MS, lv_anim_path_ease_out);
}

/* One tile for every manifest in APPS_DIR. */
static void app_tiles(lv_obj_t *parent)
{
	DIR *d = opendir(APPS_DIR);
	struct dirent *de;

	if (!d || !mbx_gpu_on_shell_layer()) {
		if (d)
			closedir(d);
		return;
	}
	while ((de = readdir(d))) {
		char path[300], line[400], name[40] = "", cmd[400] = "", icon[24] = "";
		unsigned rgb = 0;
		bool have_rgb = false;
		size_t n = strlen(de->d_name);
		FILE *f;

		if (n < 5 || strcmp(de->d_name + n - 4, ".app"))
			continue;
		snprintf(path, sizeof(path), APPS_DIR "/%s", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "name=", 5))
				snprintf(name, sizeof(name), "%s", line + 5);
			else if (!strncmp(line, "exec=", 5))
				snprintf(cmd, sizeof(cmd), "%s", line + 5);
			else if (!strncmp(line, "icon=", 5))
				snprintf(icon, sizeof(icon), "%s", line + 5);
			else if (!strncmp(line, "color=#", 7))
				have_rgb = sscanf(line + 7, "%x", &rgb) == 1;
		}
		fclose(f);
		if (name[0] && cmd[0]) {
			uint32_t before = lv_obj_get_child_count(parent);

			/* icon=<name> picks a symbol, color=#rrggbb the badge */
			static const struct { const char *n, *sym; } syms[] = {
				{ "video", LV_SYMBOL_VIDEO }, { "image", LV_SYMBOL_IMAGE },
				{ "music", LV_SYMBOL_AUDIO }, { "gears", LV_SYMBOL_SETTINGS },
				{ "shuffle", LV_SYMBOL_SHUFFLE }, { "eye", LV_SYMBOL_EYE_OPEN },
				{ "file", LV_SYMBOL_FILE }, { "list", LV_SYMBOL_LIST },
				{ "terminal", LV_SYMBOL_KEYBOARD }, { "gps", LV_SYMBOL_GPS },
				{ "charge", LV_SYMBOL_CHARGE }, { "home", LV_SYMBOL_HOME },
			};
			const char *sym = LV_SYMBOL_PLAY;
			size_t k;

			for (k = 0; k < sizeof(syms) / sizeof(syms[0]); k++)
				if (!strcmp(icon, syms[k].n))
					sym = syms[k].sym;
			app_tile(parent, sym, name,
				 have_rgb ? lv_color_hex(rgb) : M.secondary_c,
				 have_rgb ? lv_color_white() : M.on_secondary_c, app_launch_cb);
			/* the manifest's command rides on the tile's click */
			if (lv_obj_get_child_count(parent) > before) {
				lv_obj_t *t = lv_obj_get_child(parent, before);

				lv_obj_remove_event_cb(t, app_launch_cb);
				lv_obj_add_event_cb(t, app_launch_cb, LV_EVENT_CLICKED, strdup(cmd));
			}
		}
	}
	closedir(d);
}

static void open_paint(lv_event_t *e);

/* An idle moment: the picture of the shade made ready before the next drag. */
static void shade_pic_warm(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (!shade || screen_off || locked || sd.on || sd.scr || za.phase || tr.scr ||
	    lv_anim_count_running() || (touch_indev &&
	    lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		return;
	if (shade_pic && !shade_pic_dirty && shade_pic_min == bp_minute())
		return;
	shade_pic_get();
}

static void build_home(void)
{
	lv_obj_t *bar, *apps;

	conf_load();
	m3_apply_theme();
	home_scr = lv_obj_create(NULL);
	m3_screen(home_scr);
	lv_obj_add_event_cb(home_scr, home_gesture_cb, LV_EVENT_GESTURE, NULL);

	/* status bar: time, Wi-Fi and Bluetooth, battery; a tap opens the shade */
	bar = lv_obj_create(home_scr);
	plain(bar);
	lv_obj_set_size(bar, W, 40);
	lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(bar, shade_tap_cb, LV_EVENT_CLICKED, (void *)1);
	status_time = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface);
	lv_obj_align(status_time, LV_ALIGN_LEFT_MID, 0, 0);
	/* the card below shows the time: the bar on home does not (the
	 * label stays, hidden: the bar over applications copies it) */
	lv_obj_add_flag(status_time, LV_OBJ_FLAG_HIDDEN);
	batt_lbl = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(batt_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
	status_icons = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(status_icons, LV_ALIGN_RIGHT_MID, -74, 0);

	/* a widget: the time large, the date, battery and Wi-Fi */
	{
		lv_obj_t *c = lv_obj_create(home_scr);

		plain(c);
		lv_obj_set_size(c, W - 32, 112);
		lv_obj_set_pos(c, 16, 44);
		lv_obj_set_style_radius(c, 24, 0);
		lv_obj_set_style_bg_color(c, M.surface_c_high, 0);
		lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_all(c, 0, 0);
		lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(c, shade_tap_cb, LV_EVENT_CLICKED, (void *)1);
		home_clock = m3_label(c, "", &lv_font_montserrat_48, M.on_surface);
		lv_obj_align(home_clock, LV_ALIGN_TOP_LEFT, 18, 12);
		home_date = m3_label(c, "", &lv_font_montserrat_16, M.primary);
		lv_obj_align(home_date, LV_ALIGN_BOTTOM_LEFT, 20, -14);
		/* (the right side is left for weather) */
	}

	/* apps: a launcher grid, four to a row, the whole width */
	apps = lv_obj_create(home_scr);
	plain(apps);
	lv_obj_set_size(apps, W - 16, H - 172);
	lv_obj_set_pos(apps, 8, 172);
	lv_obj_set_style_bg_opa(apps, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(apps, 0, 0);
	lv_obj_set_style_pad_row(apps, 12, 0);
	lv_obj_set_style_pad_column(apps, 0, 0);	/* 4 x 76 = the width */
	lv_obj_remove_flag(apps, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(apps, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	app_tile(apps, LV_SYMBOL_SETTINGS, "Settings", M.primary_c, M.on_primary_c, open_settings);
	settings_tile = page_tile_anim(apps, open_settings);
	app_tile(apps, LV_SYMBOL_EDIT, "Paint", M.tertiary_c, M.on_tertiary_c, open_paint);
	page_tile_anim(apps, open_paint);
	app_tiles(apps);

	build_shade();

	status_refresh(true);
	home_timer = lv_timer_create(home_update, 2000, NULL);
	lv_timer_create(shade_pic_warm, 1000, NULL);
	home_update(NULL);
	lv_screen_load(home_scr);
	ui_reopen();
}

/* After a theme or accent change: start afresh (state is in CONF_PATH). */
static void ui_restart(const char *open_page)
{
	conf_save();
	if (open_page)
		setenv("N72_UI_OPEN", open_page, 1);
	if (tty_fd >= 0)
		close(tty_fd);
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
	lv_obj_t *body, *kb;

	snprintf(pw_ssid, sizeof(pw_ssid), "%s", ssid);
	body = m3_page(ssid, &pw_scr);
	m3_label(body, "Password (at least 8 characters)", &lv_font_montserrat_14, M.on_surface_v);
	pw_ta = lv_textarea_create(body);
	lv_textarea_set_one_line(pw_ta, true);
	lv_textarea_set_password_mode(pw_ta, true);
	lv_obj_set_width(pw_ta, lv_pct(100));
	lv_obj_set_style_radius(pw_ta, 12, 0);
	kb = lv_keyboard_create(pw_scr);
	lv_obj_set_size(kb, W, 200);
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_keyboard_set_textarea(kb, pw_ta);
	lv_obj_add_event_cb(kb, pw_done_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, pw_done_cb, LV_EVENT_CANCEL, NULL);
	push(pw_scr);
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
	/* the strongest 24 (sorted): more is only slower to build and scroll */
	for (i = 0; i < n_nets && i < 24; i++) {
		struct wifi_net *n = &nets[i];
		char sub[48];
		const char *bars = n->signal > -55 ? "Excellent" : n->signal > -67 ? "Good" :
				   n->signal > -78 ? "Fair" : "Weak";
		lv_obj_t *r;

		snprintf(sub, sizeof(sub), "%s%s%s", bars, n->secure ? ", secured" : ", open",
			 !strcmp(n->ssid, wifi_ssid) && wifi_up ? ", connected" :
			 n->saved ? ", saved" : "");
		r = m3_row(wifi_list, LV_SYMBOL_WIFI, n->ssid, sub, wifi_net_cb, n, NULL);
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

		snprintf(buf, sizeof(buf), "Connected to %s\n%s", wifi_ssid, ip);
		label_set_if_changed(wifi_state_lbl, buf);
	} else {
		label_set_if_changed(wifi_state_lbl, st[0] ? "Not connected" :
				     "Wi-Fi is not running");
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

static void open_wifi(lv_event_t *e)
{
	lv_obj_t *body, *c, *b;

	LV_UNUSED(e);
	if (!have_tool("/usr/sbin/wpa_cli")) {
		body = m3_page("Wi-Fi", &wifi_page);
		m3_note(m3_card(body, M.surface_c), "wpa_supplicant is not installed.");
		push(wifi_page);
		return;
	}
	body = m3_page("Wi-Fi", &wifi_page);
	c = m3_card(body, M.primary_c);
	wifi_state_lbl = m3_label(c, "...", &lv_font_montserrat_16, M.on_primary_c);
	lv_obj_set_width(wifi_state_lbl, lv_pct(100));
	lv_label_set_long_mode(wifi_state_lbl, LV_LABEL_LONG_WRAP);

	m3_section(body, "Networks");
	wifi_last_sig[0] = 0;
	wifi_list = lv_obj_create(body);
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

struct bt_dev { char mac[18]; char name[64]; bool paired; };
static struct bt_dev bdevs[16];
static int n_bdevs;

static void bt_dev_cb(lv_event_t *e)
{
	struct bt_dev *d = lv_event_get_user_data(e);
	char cmd[200];

	if (d->paired)
		snprintf(cmd, sizeof(cmd), "bluetoothctl connect %s", d->mac);
	else
		snprintf(cmd, sizeof(cmd), "bluetoothctl pair %s && bluetoothctl trust %s && "
			 "bluetoothctl connect %s", d->mac, d->mac, d->mac);
	if (bt_state_lbl)
		lv_label_set_text_fmt(bt_state_lbl, "Connecting to %s...", d->name);
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
	const char *p, *paired = strstr(out, "@@PAIRED@@");
	int i;

	LV_UNUSED(ud);
	n_bdevs = 0;
	for (p = out; p && *p && n_bdevs < (int)(sizeof(bdevs) / sizeof(bdevs[0])); ) {
		char mac[18], name[64];

		if (sscanf(p, "Device %17s %63[^\n]", mac, name) == 2) {
			bool is_paired = paired && p > paired;
			bool dup = false;

			for (i = 0; i < n_bdevs; i++)
				if (!strcmp(bdevs[i].mac, mac)) {
					bdevs[i].paired |= is_paired;
					dup = true;
				}
			if (!dup) {
				snprintf(bdevs[n_bdevs].mac, 18, "%s", mac);
				snprintf(bdevs[n_bdevs].name, 64, "%s", name);
				bdevs[n_bdevs].paired = is_paired;
				n_bdevs++;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!bt_list || !lv_obj_is_valid(bt_list))
		return;
	lv_obj_clean(bt_list);
	if (!n_bdevs)
		m3_label(bt_list, bt_powered ? "No devices yet: tap Scan" : "Bluetooth is off",
			 &lv_font_montserrat_14, M.on_surface_v);
	for (i = 0; i < n_bdevs; i++) {
		lv_obj_t *r = m3_row(bt_list, LV_SYMBOL_BLUETOOTH, bdevs[i].name,
				     bdevs[i].paired ? "Paired" : "Tap to pair", bt_dev_cb, &bdevs[i], NULL);

		if (bdevs[i].paired)
			lv_obj_add_event_cb(r, bt_forget_cb, LV_EVENT_LONG_PRESSED, &bdevs[i]);
	}
	if (bt_scan_lbl && lv_obj_is_valid(bt_scan_lbl))
		lv_label_set_text(bt_scan_lbl, "Scan");
}

static void bt_show_page_done(const char *out, void *ud)
{
	bt_show_done(out, ud);
	if (!bt_switch || !lv_obj_is_valid(bt_switch))
		return;
	if (bt_powered)
		lv_obj_add_state(bt_switch, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(bt_switch, LV_STATE_CHECKED);
	if (bt_state_lbl)
		label_set_if_changed(bt_state_lbl, bt_powered ? "On" : "Off");
}

static void bt_page_refresh(void)
{
	if (!bt_page || !lv_obj_is_valid(bt_page))
		return;
	job_run("bluetoothctl show", bt_page, bt_show_page_done, NULL);
	job_run("bluetoothctl devices; echo @@PAIRED@@; bluetoothctl devices Paired",
		bt_page, bt_devs_done, NULL);
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

static void bt_set(bool on)
{
	job_run(on ? BT_ON_CMD : BT_OFF_CMD, NULL, qs_refresh_done, NULL);
}

static void bt_switch_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

	job_run(on ? BT_ON_CMD : BT_OFF_CMD, bt_page, bt_cmd_done, NULL);
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
	lv_obj_t *body, *c, *b;

	LV_UNUSED(e);
	body = m3_page("Bluetooth", &bt_page);
	if (!have_tool("/usr/bin/bluetoothctl")) {
		c = m3_card(body, M.surface_c);
		m3_note(c, "BlueZ (bluetoothctl) is not installed.");
		push(bt_page);
		return;
	}
	c = m3_card(body, M.primary_c);
	lv_obj_set_height(c, 72);
	bt_state_lbl = m3_label(c, "...", &lv_font_montserrat_16, M.on_primary_c);
	lv_obj_align(m3_label(c, "Use Bluetooth", &lv_font_montserrat_14, M.on_primary_c),
		     LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_align(bt_state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	bt_switch = lv_switch_create(c);
	lv_obj_align(bt_switch, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(bt_switch, bt_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

	m3_section(body, "Devices");
	bt_list = lv_obj_create(body);
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
	m3_note(body, "Long-press a paired device to forget it.");
	push(bt_page);
	bt_page_refresh();
}

/* ------------------------------------------------------------ Display */

static lv_obj_t *bright_val;

static void bright_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));
	char buf[16];

	backlight_set(v);
	lv_slider_set_value(home_bright, v, LV_ANIM_OFF);
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

	if (i == conf.accent)
		return;
	conf.accent = i;
	ui_restart("display");
}

static const int idle_opts[] = { 15, 30, 60, 120, 300, 0 };

static void idle_cb(lv_event_t *e)
{
	uint32_t i = lv_dropdown_get_selected(lv_event_get_target(e));

	if (i < sizeof(idle_opts) / sizeof(idle_opts[0])) {
		conf.idle_s = idle_opts[i];
		conf_save();
	}
}

static void open_display(lv_event_t *e)
{
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
	s = lv_slider_create(c);
	lv_obj_set_flex_grow(s, 1);
	lv_slider_set_range(s, 5, 100);
	lv_slider_set_value(s, backlight_get(), LV_ANIM_OFF);
	lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	bright_val = m3_label(c, "", &lv_font_montserrat_14, M.on_surface_v);
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
		lv_obj_set_size(dot, 42, 42);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(dot, tone(accents[i].hue, accents[i].sat, conf.dark ? 70 : 45), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		if ((int)i == conf.accent) {
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
	m3_note(body, "On battery the iPod sleeps after the timeout; on USB power only the screen turns off.");
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
	snprintf(buf, sizeof(buf), "%s", access("/dev/nandsystem", F_OK) ? "not attached" :
		 "iOS System and Data volumes (read-only)");
	label_set_if_changed(about_val[6], buf);
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
	static const char *const titles[] = { "Device", "Kernel", "Uptime", "Memory",
		"Battery", "Network", "NAND" };
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

static const char *snd_output_name(void)
{
	if (!snd_present)
		return "No sound card";
	if (jack_in)
		return "Earphones";
	return conf.spk ? "Speaker" : "Earphones (speaker off)";
}

static void snd_summary(char *buf, size_t len)
{
	if (!snd_present)
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
	lv_anim_set_path_cb(&a, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
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
		lv_obj_set_size(vol_hud, W - 24, 56);
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
		vol_hud_slider = lv_slider_create(vol_hud);
		lv_obj_set_flex_grow(vol_hud_slider, 1);
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
	snd_apply_volume(conf.vol);
	conf_save();
	if (hud)
		vol_hud_show();
	else
		snd_refresh_ui();
}

/* volume keys: 16 steps of about 4 dB at the top, finer near mute */
static void volume_step(int dir)
{
	int v = conf.vol;

	if (!snd_present)
		return;
	v += dir * ((dir > 0 ? v : v - 1) >= 30 ? 6 : 3);
	volume_set(v, true);
}

static void jack_check(lv_timer_t *t)
{
	LV_UNUSED(t);
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
	if (have_tool("/usr/bin/n72tone"))
		job_run("/usr/bin/n72tone -f 660 -a -12 -t 0.6", NULL, NULL, NULL);
}

static void open_sound(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *row, *sw;

	LV_UNUSED(e);
	body = m3_page("Sound", &scr);
	snd_page = scr;

	m3_section(body, "Volume");
	c = m3_card(body, M.surface_c);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 12, 0);
	m3_label(c, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_20, M.on_surface_v);
	snd_slider = lv_slider_create(c);
	lv_obj_set_flex_grow(snd_slider, 1);
	lv_slider_set_range(snd_slider, 0, 100);
	lv_slider_set_value(snd_slider, conf.vol, LV_ANIM_OFF);
	lv_obj_add_event_cb(snd_slider, snd_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
	snd_val = m3_label(c, "", &lv_font_montserrat_14, M.on_surface_v);

	m3_section(body, "Output");
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_AUDIO, "Playing on", snd_output_name(), NULL, NULL, &snd_out_lbl);
	row = m3_row(c, LV_SYMBOL_VOLUME_MAX, "Speaker", "When no earphones are plugged in",
		     NULL, NULL, NULL);
	sw = lv_switch_create(row);
	if (conf.spk)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, snd_spk_cb, LV_EVENT_VALUE_CHANGED, NULL);
	m3_note(body, "The volume keys change the volume everywhere (in Logs they scroll).");
	if (have_tool("/usr/bin/n72tone"))
		m3_button(body, "Test sound", false, snd_test_cb, NULL);
	snd_refresh_ui();
	push(scr);
}

static void sound_ui_init(void)
{
	pf_init();
	glide_init();
	snd_init(conf.vol, conf.spk);
	lv_timer_create(jack_check, 200, NULL);
}

static void open_settings(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c;
	char sub[80];

	LV_UNUSED(e);
	body = m3_page("Settings", &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_WIFI, "Wi-Fi", wifi_up ? wifi_ssid : "Not connected",
	       open_wifi, NULL, &set_wifi_sub);
	m3_row(c, LV_SYMBOL_BLUETOOTH, "Bluetooth",
	       !have_tool("/usr/bin/bluetoothctl") ? "Not installed" : bt_powered ? "On" : "Off",
	       open_bt, NULL, &set_bt_sub);

	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	snprintf(sub, sizeof(sub), "%s, %s, %d%%", conf.dark ? "Dark" : "Light",
		 accents[conf.accent].name, backlight_get());
	m3_row(c, LV_SYMBOL_IMAGE, "Display", sub, open_display, NULL, NULL);
	snd_summary(sub, sizeof(sub));
	m3_row(c, LV_SYMBOL_VOLUME_MAX, "Sound", sub, open_sound, NULL, &set_snd_sub);
	m3_row(c, LV_SYMBOL_POWER, "Power", "Sleep, restart, power off", open_power, NULL, NULL);

	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_EYE_OPEN, "About", "Device, memory, battery",
	       open_about, NULL, NULL);
	m3_row(c, LV_SYMBOL_FILE, "Logs", "Kernel messages", open_logs, NULL, NULL);
	push(scr);
}

/* --------------------------------------------------------------- logs */

/*
 * The kernel log on the screen, for when there is no USB or Wi-Fi to read
 * it over: the network addresses, then the tail of the ring buffer,
 * refreshed every 2 s and kept scrolled to the end. Volume keys scroll.
 */
#define LOGS_TAIL 6144

static lv_obj_t *logs_net, *logs_text, *logs_box;
static lv_timer_t *logs_timer;
static char *logs_buf;
static int logs_len = -1;
static bool logs_follow = true;

/*
 * The log as labels of LOGS_CHUNK lines each: LVGL lays out and draws a
 * label by walking its text from the start, so one 6 KB label cost ~25 ms
 * a frame while scrolling. Chunks off screen are skipped whole, and an
 * update only touches the chunks whose text changed.
 */
#define LOGS_CHUNK	8	/* small labels: LVGL walks each one from its start */
#define LOGS_MAXCH	96
static lv_obj_t *logs_chunk[LOGS_MAXCH];
static int logs_nchunks;

static void logs_set_chunks(const char *text)
{
	const char *p = text;
	int c = 0;

	while (*p && c < LOGS_MAXCH) {
		const char *q = p;
		int lines = 0;
		char *part;
		size_t len;

		while (*q && lines < LOGS_CHUNK) {
			if (*q == '\n')
				lines++;
			q++;
		}
		len = q - p;
		if (len && p[len - 1] == '\n')
			len--;			/* the label's own line break */
		part = malloc(len + 1);
		if (!part)
			break;
		memcpy(part, p, len);
		part[len] = 0;
		if (c == logs_nchunks) {
			logs_chunk[c] = m3_label(logs_box, "", &lv_font_montserrat_12, M.on_surface);
			lv_obj_set_width(logs_chunk[c], lv_pct(100));
			lv_label_set_long_mode(logs_chunk[c], LV_LABEL_LONG_WRAP);
			logs_nchunks++;
		}
		if (strcmp(lv_label_get_text(logs_chunk[c]), part))
			lv_label_set_text(logs_chunk[c], part);
		free(part);
		p = q;
		c++;
	}
	while (logs_nchunks > c)		/* the log got shorter */
		lv_obj_delete(logs_chunk[--logs_nchunks]);
}

static void logs_update(lv_timer_t *t)
{
	char usb[32], wlan[32], buf[96];
	int size, n, i, o, start;
	char *raw;

	LV_UNUSED(t);
	ip_of("usb0", usb, sizeof(usb));
	ip_of("wlan0", wlan, sizeof(wlan));
	snprintf(buf, sizeof(buf), "usb0 %s    wlan0 %s", usb, wlan);
	label_set_if_changed(logs_net, buf);

	size = klogctl(10, NULL, 0);		/* SYSLOG_ACTION_SIZE_BUFFER */
	if (size <= 0)
		size = 1 << 17;
	raw = malloc(size + 1);
	if (!raw)
		return;
	n = klogctl(3, raw, size);		/* SYSLOG_ACTION_READ_ALL */
	if (n < 0)
		n = 0;
	if (n == logs_len) {
		free(raw);
		return;
	}
	logs_len = n;
	start = n > LOGS_TAIL ? n - LOGS_TAIL : 0;
	while (start > 0 && start < n && raw[start - 1] != '\n')
		start++;
	for (i = start, o = 0; i < n && o < LOGS_TAIL; ) {
		if (raw[i] == '<' && (i == start || raw[i - 1] == '\n')) {
			int j = i + 1;

			while (j < n && raw[j] >= '0' && raw[j] <= '9')
				j++;
			if (j < n && raw[j] == '>') {
				i = j + 1;
				continue;
			}
		}
		logs_buf[o++] = raw[i++];
	}
	logs_buf[o] = 0;
	free(raw);
	logs_set_chunks(logs_buf);
	lv_obj_update_layout(logs_box);
	if (logs_follow)
		lv_obj_scroll_to_y(logs_box, LV_COORD_MAX, LV_ANIM_OFF);
}

static bool logs_scroll(int dir)
{
	if (!logs_box)
		return false;
	lv_obj_scroll_by(logs_box, 0, dir * 160, LV_ANIM_OFF);
	logs_follow = lv_obj_get_scroll_bottom(logs_box) <= 0;
	return true;
}

static void logs_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (logs_timer)
		lv_timer_delete(logs_timer);
	logs_timer = NULL;
	free(logs_buf);
	logs_buf = NULL;
	logs_box = NULL;
	logs_nchunks = 0;
}

static void open_logs(lv_event_t *e)
{
	lv_obj_t *scr, *body;

	LV_UNUSED(e);
	if (logs_box)		/* already open */
		return;
	logs_buf = malloc(LOGS_TAIL + 1);
	if (!logs_buf)
		return;
	logs_buf[0] = 0;
	logs_len = -1;
	logs_follow = true;
	body = m3_page("Logs", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	logs_net = m3_label(body, "", &lv_font_montserrat_12, M.primary);
	lv_obj_set_width(logs_net, lv_pct(100));
	logs_box = lv_obj_create(body);
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
	logs_nchunks = 0;
	/*
	 * No scroll cache here: pictures of text chunks cost as much to take
	 * as they save (measured: 16.6 vs 15.8 ms a frame, same hitches), while
	 * the card pages draw ~30% faster with it.
	 */
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
	else if (strcmp(p, "logs") && strcmp(p, "about") && strcmp(p, "volume"))
		open_settings(NULL);
	if (!strcmp(p, "display"))
		open_display(NULL);
	else if (!strcmp(p, "wifi"))
		open_wifi(NULL);
	else if (!strcmp(p, "bt"))
		open_bt(NULL);
	else if (!strcmp(p, "power"))
		open_power(NULL);
	else if (!strcmp(p, "about"))
		open_about(NULL);
	else if (!strcmp(p, "logs"))
		open_logs(NULL);
	else if (!strcmp(p, "volume"))	/* (screenshots of the volume pop-up) */
		vol_hud_show();
	unsetenv("N72_UI_OPEN");
}

/* -------------------------------------------------------------- paint */

/* A finger-painting toy: each stroke gets the next colour round the wheel. */
static lv_obj_t *paint_canvas;
static void *paint_buf;
static lv_point_t paint_last;
static bool paint_down;
static uint32_t paint_hue;

#define PAINT_W (W - 24)
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
