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
#include <sys/wait.h>

static bool system_sleep(void);

/* ------------------------------------------------------------ settings */

#define CONF_PATH "/tmp/n72-ui.conf"

static struct {
	bool dark;
	int accent;		/* index into accents[] */
	int idle_s;		/* screen idle timeout, 0 = never */
} conf = { true, 0, 120 };

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
	}
	fclose(f);
}

static void conf_save(void)
{
	FILE *f = fopen(CONF_PATH, "w");

	if (!f)
		return;
	fprintf(f, "dark=%d\naccent=%d\nidle=%d\n", conf.dark, conf.accent, conf.idle_s);
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

static void tr_finish(void)
{
	lv_obj_t *scr = tr.scr, *from = tr.from;
	void (*done)(void) = tr.done;

	if (!scr)
		return;
	tr.scr = NULL;
	tr.from = NULL;
	tr.done = NULL;
	lv_anim_delete(&tr, tr_exec);
	lv_screen_load(tr.to);
	lv_obj_delete(scr);
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
static lv_draw_buf_t *grab_or_snap(lv_obj_t *scr)
{
	lv_draw_buf_t *b = NULL;

	if (scr == lv_screen_active()) {
		lv_refr_now(NULL);	/* pending changes (a pressed row) first */
		b = mbx_gpu_grab_screen();
	}
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

	tr_finish();
	from = lv_screen_active();
	if (from == to)
		return;
	if (!tr_start(grab_or_snap(from), snap(to, LV_COLOR_FORMAT_XRGB8888),
		      left ? 0 : 1, to, del_from ? from : NULL, 0, left ? -W : W))
		lv_screen_load_anim(to, left ? LV_SCR_LOAD_ANIM_MOVE_LEFT :
				    LV_SCR_LOAD_ANIM_MOVE_RIGHT, TR_MS, 0, del_from);
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

static void go_home(void)
{
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
		go_home();
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
#define SC_MAX		16
#define SC_BUDGET	(3 * 1024 * 1024)	/* bytes of snapshots at most */

static struct {
	lv_obj_t *owner;
	lv_obj_t *live[SC_MAX], *pic[SC_MAX];
	lv_draw_buf_t *buf[SC_MAX];
	int n;
	lv_timer_t *timer;
	uint32_t last_move;
} sc;

static void sc_restore(bool delete_pics)
{
	int i;

	for (i = 0; i < sc.n; i++) {
		if (delete_pics && lv_obj_is_valid(sc.pic[i]))
			lv_obj_delete(sc.pic[i]);
		if (lv_obj_is_valid(sc.live[i]))
			lv_obj_set_style_opa_layered(sc.live[i], LV_OPA_COVER, 0);
		lv_image_cache_drop(sc.buf[i]);
		lv_draw_buf_destroy(sc.buf[i]);
	}
	sc.n = 0;
	sc.owner = NULL;
}

static void sc_settle(lv_timer_t *t)
{
	lv_obj_t *body = lv_timer_get_user_data(t);

	if (sc.owner == body && lv_tick_elaps(sc.last_move) < 150)
		return;			/* still moving: look again */
	lv_timer_delete(t);
	sc.timer = NULL;
	if (sc.owner == body)
		sc_restore(true);
}

static void sc_begin(lv_obj_t *body)
{
	uint32_t i, cnt = lv_obj_get_child_count(body);
	long budget = SC_BUDGET;

	if (sc.owner)
		sc_restore(true);
	if (sc.timer) {
		lv_timer_delete(sc.timer);
		sc.timer = NULL;
	}
	for (i = 0; i < cnt && sc.n < SC_MAX; i++) {
		lv_obj_t *c = lv_obj_get_child(body, i), *p;
		lv_draw_buf_t *b;
		long sz = (long)lv_obj_get_width(c) * lv_obj_get_height(c) * 4;

		if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) || sz <= 0 || sz > budget)
			continue;
		b = lv_snapshot_take(c, LV_COLOR_FORMAT_ARGB8888);
		if (!b)
			break;
		lv_draw_buf_flush_cache(b, NULL);	/* the GPU reads it */
		/*
		 * Flatten it onto the page colour once: frames then copy opaque
		 * pictures instead of blending them (a third of the memory
		 * traffic, and the GPU is memory bound).
		 */
		{
			lv_draw_buf_t *flat = mbx_gpu_flatten(b, M.surface);

			if (flat) {
				lv_draw_buf_destroy(b);
				b = flat;
			}
		}
		budget -= sz;
		p = lv_image_create(body);
		lv_image_set_src(p, b);
		lv_obj_add_flag(p, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
		lv_obj_set_pos(p, lv_obj_get_x(c), lv_obj_get_y(c));
		lv_obj_set_style_opa_layered(c, LV_OPA_TRANSP, 0);
		sc.live[sc.n] = c;
		sc.pic[sc.n] = p;
		sc.buf[sc.n] = b;
		sc.n++;
	}
	sc.owner = body;
	if (getenv("N72_FPS"))
		fprintf(stderr, "scroll cache: %d pictures\n", sc.n);
}

static void sc_event(lv_event_t *e)
{
	lv_obj_t *body = lv_event_get_current_target(e);

	switch (lv_event_get_code(e)) {
	case LV_EVENT_SCROLL:
		sc.last_move = lv_tick_get();
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
		if (sc.timer) {
			lv_timer_delete(sc.timer);
			sc.timer = NULL;
		}
		break;
	default:
		break;
	}
}

static void scroll_cached(lv_obj_t *body)
{
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
	lv_obj_set_style_pad_ver(body, 4, 0);
	lv_obj_set_style_pad_row(body, 8, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
	scroll_cached(body);
	*scr_out = scr;
	return body;
}

static void push(lv_obj_t *scr)
{
	if (nav_n < NAV_MAX)
		nav[nav_n++] = scr;
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
		    void (*done)(const char *out, void *ud), void *ud)
{
	struct job *j = calloc(1, sizeof(*j));
	int p[2];

	if (!j || pipe(p)) {
		free(j);
		return;
	}
	j->pid = fork();
	if (j->pid == 0) {
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
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
static void label_set_if_changed(lv_obj_t *lbl, const char *text)
{
	const char *cur = lv_label_get_text(lbl);

	if (!cur || strcmp(cur, text))
		lv_label_set_text(lbl, text);
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
static bool wifi_up;
static bool bt_powered;

static void wifi_status_done(const char *out, void *ud)
{
	char st[32];

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
	lv_obj_set_style_bg_color(tile, on ? M.primary_c : M.surface_c_high, 0);
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
	battery_text(buf, sizeof(buf));
	label_set_if_changed(batt_lbl, buf);
	snprintf(buf, sizeof(buf), "%s%s", wifi_up ? LV_SYMBOL_WIFI "  " : "",
		 bt_powered ? LV_SYMBOL_BLUETOOTH : "");
	label_set_if_changed(status_icons, buf);
	label_set_if_changed(qs_wifi_sub, wifi_up ? wifi_ssid : "Not connected");
	if (set_wifi_sub && lv_obj_is_valid(set_wifi_sub))
		label_set_if_changed(set_wifi_sub, wifi_up ? wifi_ssid : "Not connected");
	if (set_bt_sub && lv_obj_is_valid(set_bt_sub))
		label_set_if_changed(set_bt_sub, !have_tool("/usr/bin/bluetoothctl") ?
				     "Not installed" : bt_powered ? "On" : "Off");
	label_set_if_changed(qs_bt_sub, !have_tool("/usr/bin/bluetoothctl") ? "Unavailable" :
			     bt_powered ? "On" : "Off");
	qs_style(qs_wifi, wifi_up);
	qs_style(qs_bt, bt_powered);
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

static lv_obj_t *home_bright, *shade, *scrim;
static bool shade_open;

/* Screen back on: bring the clock up to date now, not at the next run. */
static void home_wake(void)
{
	if (home_timer)
		lv_timer_ready(home_timer);
}

static void home_bright_cb(lv_event_t *e)
{
	backlight_set(lv_slider_get_value(lv_event_get_target(e)));
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

/* Tiles in the shade open their page, closing the shade behind them. */
static void qs_open_wifi(lv_event_t *e)
{
	shade_set(false, false);
	open_wifi(e);
}

static void qs_open_bt(lv_event_t *e)
{
	shade_set(false, false);
	open_bt(e);
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

static void open_paint(lv_event_t *e);

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
	batt_lbl = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(batt_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
	status_icons = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_align(status_icons, LV_ALIGN_RIGHT_MID, -74, 0);

	/* apps: a launcher grid, four to a row */
	apps = lv_obj_create(home_scr);
	plain(apps);
	lv_obj_set_size(apps, W - 16, H - 56);
	lv_obj_set_pos(apps, 8, 52);
	lv_obj_set_style_bg_opa(apps, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(apps, 0, 0);
	lv_obj_set_style_pad_row(apps, 12, 0);
	lv_obj_remove_flag(apps, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(apps, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	app_tile(apps, LV_SYMBOL_SETTINGS, "Settings", M.primary_c, M.on_primary_c, open_settings);
	app_tile(apps, LV_SYMBOL_EDIT, "Paint", M.tertiary_c, M.on_tertiary_c, open_paint);

	build_shade();

	status_refresh(true);
	home_timer = lv_timer_create(home_update, 2000, NULL);
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

static int net_cmp(const void *a, const void *b)
{
	return ((const struct wifi_net *)b)->signal - ((const struct wifi_net *)a)->signal;
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
	lv_obj_clean(wifi_list);
	if (!n_nets)
		m3_label(wifi_list, "No networks found yet", &lv_font_montserrat_14, M.on_surface_v);
	for (i = 0; i < n_nets; i++) {
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

static void bt_switch_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

	job_run(on ? "bluetoothctl power on" : "bluetoothctl power off", bt_page,
		bt_cmd_done, NULL);
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
		if (!system_sleep()) {
			screen_off = true;
			backlight_power(false);
		}
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
	lv_label_set_text_static(logs_text, logs_buf);
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
	logs_text = m3_label(logs_box, "", &lv_font_montserrat_12, M.on_surface);
	lv_obj_set_width(logs_text, lv_pct(100));
	lv_label_set_long_mode(logs_text, LV_LABEL_LONG_WRAP);
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
	else if (strcmp(p, "logs") && strcmp(p, "about"))
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
