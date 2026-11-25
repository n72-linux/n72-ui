// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): navigation. */
#include "ui_m3_priv.h"

/* ------------------------------------------------------------ navigation */


static void back_cb(lv_event_t *e);

/*
 * Transitions. LVGL's own screen animations redraw both screens, text and
 * all, every frame (15-30 ms on this CPU: choppy). Instead each picture is
 * rendered once with lv_snapshot_take() into GPU memory, and the frames
 * only move those pictures, which the GPU copies (a few ms per frame).
 */
#define TR_MS 350	/* iOS's navigation push/pop: 350 ms ease-in-out */

struct tr tr;

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

void tr_free(lv_draw_buf_t **b)
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

long bp_minute(void)
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
unsigned long home_ver;	/* bumped when anything on home changes */

void bp_changed(lv_obj_t *scr)
{
	if (scr && scr == home_scr) {
		home_ver++;
		pg_warm_kick();
	}
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

void tr_finish(void)
{
#ifdef N72_DEBUG
	if (tr.scr && getenv("N72_FPS")) {
		extern uint64_t n72_mark_us;

		fprintf(stderr, "slide end at %.0f ms after the mark\n", (mono_us() - n72_mark_us) / 1000.0);
	}
#endif
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
void quiet_begin(void)	/* (the layout brought up to date first, outside) */
{
	lv_display_enable_invalidation(NULL, false);
}

void quiet_end(void)
{
	lv_display_enable_invalidation(NULL, true);
}

lv_draw_buf_t *snap(lv_obj_t *obj, lv_color_format_t cf)
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
	struct psl *s = a->var;
	bool hid = s && s->hide;

	psl_finish(s);
	if (hid)
		app_dormant_sync();
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
bool psl_covers(lv_obj_t *live, int x, int y)
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
bool psl_start_sig(lv_obj_t *live, int32_t to, uint32_t ms, lv_anim_path_cb_t path,
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

bool psl_start(lv_obj_t *live, int32_t to, uint32_t ms, lv_anim_path_cb_t path, bool hide)
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
void psl_touch(lv_obj_t *live)
{
	struct psl *s = psl_of(live);

	if (!s || !s->img || (s->hide && !s->resting))
		return;
	if (!s->resting)
		psl_finish(s);
	psl_wake(live);
}

/* Resting, and its look changed since the picture: the object again. */
void psl_check(lv_obj_t *live)
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
void psl_warm(lv_obj_t *live, psl_sig_fn sig)
{
	struct psl *s;

	if (!lv_obj_has_flag(live, LV_OBJ_FLAG_HIDDEN) || !(s = psl_slot(live, sig)) || s->img)
		return;
	psl_take(s, live);
}

/* Stop @live's slide where it ends, and forget it (a rebuild, a jump). */
void psl_stop(lv_obj_t *live)
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

bool psl_sliding(lv_obj_t *live)
{
	struct psl *s = psl_of(live);

	return s && s->img && !s->resting;
}

static void sc_restore(bool delete_pics);
static void pf_clear(void);

void nav_caches_clear(void)
{
	int i;

	for (i = 0; i < BP_MAX; i++)
		bp_drop(i);
	for (i = 0; i < PSL_MAX; i++) {
		if (psl[i].live && !psl[i].img) {
			tr_free(&psl[i].pic);
			psl[i].pic_sig[0] = 0;
		}
	}
	if (sc.owner && !sc.timer)
		sc_restore(false);
	pf_clear();
}

/* The screen on display: copied from the display buffer when it is current. */
#ifdef N72_DEBUG
static uint32_t gs_refr_ms, gs_grab_ms;	/* (statistics) */
static uint32_t ui_release_ms;		/* (statistics) the last finger lift */

static void ui_release_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_release_ms = lv_tick_get();
}
#else
static uint32_t gs_refr_ms, gs_grab_ms;
#endif
static bool gs_grabbed;

lv_draw_buf_t *grab_or_snap(lv_obj_t *scr)
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
bool tr_start(lv_draw_buf_t *bg, lv_draw_buf_t *fg, int kind, lv_obj_t *to,
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
void app_bar_set(bool show);
pid_t app_pid;

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
#ifdef N72_DEBUG
	{
		extern uint64_t n72_mark_us;

		n72_mark_us = mono_us();
	}
#endif
	t0 = lv_tick_get();
	bg = grab_or_snap(from);
	t1 = lv_tick_get();
	fg = bp_take(to);
	reused = fg != NULL;
	if (!fg)
		fg = snap(to, LV_COLOR_FORMAT_XRGB8888);
	t2 = lv_tick_get();
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		fprintf(stderr, "slide %s: %u ms after the lift; old page %u ms (refresh %u, %s %u), new page %u ms%s\n",
			left ? "forward" : "back", (unsigned)(t0 - ui_release_ms), (unsigned)(t1 - t0),
			(unsigned)gs_refr_ms,
			gs_grabbed ? "grab" : "grab failed, snapshot", (unsigned)gs_grab_ms,
			(unsigned)(t2 - t1), reused ? " (kept picture)" : "");
#else
	(void)t0;
	(void)t1;
	(void)t2;
	(void)reused;
#endif
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

lv_obj_t *nav[NAV_MAX];
int nav_n;
/* where an application was launched from (home, or a page such as Files):
 * the picture behind its card, and the screen it ends on */
lv_obj_t *launch_from;
void app_bar_set(bool show);
bool locked;

/* A page (Settings and the pages under it, Paint) is on the screen. */
bool on_page(void)
{
	return !locked && nav_n > 0 && lv_screen_active() == nav[nav_n - 1];
}

void shade_set(bool open, bool anim);

bool app_end(void);
lv_obj_t *app_scr;	/* the empty screen over a running application */
bool locked;		/* the lock screen is up */
void lock_wake(void);
void za_finish(void);
pid_t app_pid;
void app_shade_slide(bool open);
void app_bar_update(void);
extern lv_obj_t *vol_hud;

void ui_settle_all(void);
bool za_page_close(void);
bool page_back_to_app(void);
bool page_home_over_app(void);

void home_fresh(void);
void page_fresh(int i);

void go_home(void)
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

void go_back(void)
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
#define SC_BUDGET	(3 * 1024 * 1024)	/* bytes of snapshots at most */
#define SC_KEEP_MS	1500			/* pictures stay this long after the page stops */

struct sc sc;

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
 * Flick and glide in closed form of the time since the lift (gives the same
 * positions within 0.16 px without per-frame stepping):
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
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		fprintf(stderr, "glide: %d px, %s\n", (int)LV_ABS(gl.done), why);
#else
	(void)why;
#endif
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
#ifdef N72_DEBUG
			if (getenv("N72_FPS") && !fs.slow)
				fprintf(stderr, "fast scroll off: step %d px in %.1f ms (sign %d)\n", (int)d, dt / 1000.0, fs.sign);
#endif
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
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		fprintf(stderr, "flick: %.0f px/s (fast %d, x%.2f)\n", v, fs.count, fs.mult);
#endif
	if (fabsf(v) < GLIDE_MIN_V)
		return;		/* iOS: a stop, no glide */
	room = v > 0 ? (hor ? lv_obj_get_scroll_left(obj) : lv_obj_get_scroll_top(obj))
		     : (hor ? lv_obj_get_scroll_right(obj) : lv_obj_get_scroll_bottom(obj));
	glide_start(obj, hor, v, LV_MAX(room, 0));
}

void fling_accel(lv_obj_t *obj)
{
	lv_obj_add_event_cb(obj, ts_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(obj, ts_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(obj, fling_cb, LV_EVENT_SCROLL_THROW_BEGIN, NULL);
}

/* the touch path of every pointer device (after drags_init) */
void glide_init(void)
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
void pf_label_changed(lv_obj_t *lbl)
{
	for (int i = pf.n - 1; i >= 0; i--)
		for (lv_obj_t *o = lbl; o; o = lv_obj_get_parent(o))
			if (o == pf.child[i]) {
				pf_drop(i);
				break;
			}
}

/* A scroll picture of @c: rendered directly onto what is behind, in GPU memory. */
static lv_draw_buf_t *sc_picture(lv_obj_t *c)
{
	lv_draw_buf_t *b;
	lv_display_t *disp = lv_obj_get_display(c), *disp_old;
	lv_layer_t layer, *layer_old;
	lv_area_t snapshot_area;
	int32_t ext, w, h, x, y;
	uint32_t bg;

	lv_obj_update_layout(c);
	ext = lv_obj_get_ext_draw_size(c);
	if (lv_obj_get_height(c) + 2 * ext > SC_MAX_H)
		return NULL;
	b = lv_snapshot_create_draw_buf(c, LV_COLOR_FORMAT_XRGB8888);
	if (!b)
		return NULL;
	bg = 0xff000000u | lv_color_to_u32(sc_bg_of(c));
	w = (int32_t)b->header.w;
	h = (int32_t)b->header.h;
	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(b->data + (size_t)y * b->header.stride);

		for (x = 0; x < w; x++)
			row[x] = bg;
	}
	lv_obj_get_coords(c, &snapshot_area);
	lv_area_increase(&snapshot_area, ext, ext);
	lv_memzero(&layer, sizeof(layer));
	layer.draw_buf = b;
	layer.buf_area.x1 = snapshot_area.x1;
	layer.buf_area.y1 = snapshot_area.y1;
	layer.buf_area.x2 = snapshot_area.x1 + w - 1;
	layer.buf_area.y2 = snapshot_area.y1 + h - 1;
	layer.color_format = LV_COLOR_FORMAT_XRGB8888;
	layer._clip_area = snapshot_area;
	layer.phy_clip_area = snapshot_area;
#if LV_DRAW_TRANSFORM_USE_MATRIX
	lv_matrix_identity(&layer.matrix);
#endif
	disp_old = lv_refr_get_disp_refreshing();
	layer_old = disp->layer_head;
	disp->layer_head = &layer;
	lv_refr_set_disp_refreshing(disp);
	lv_obj_redraw(&layer, c);
	while (layer.draw_task_head) {
		lv_draw_dispatch_wait_for_request();
		lv_draw_dispatch();
	}
	disp->layer_head = layer_old;
	lv_refr_set_disp_refreshing(disp_old);
	lv_draw_buf_flush_cache(b, NULL);	/* the GPU reads it */
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
#ifdef N72_DEBUG
	if (lv_tick_elaps(t0) > 10 && getenv("N72_FPS"))
		fprintf(stderr, "scroll cache: picture %dx%d took %u ms\n", (int)lv_obj_get_width(c),
			(int)lv_obj_get_height(c), (unsigned)lv_tick_elaps(t0));
#else
	(void)t0;
#endif
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
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		fprintf(stderr, "scroll cache: %d pictures, %d prefetched before\n", sc.n, pf_had);
#else
	(void)pf_had;
#endif
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

	if (screen_off || app_dormant || (app_pid > 0 && !page_over_app)) {
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
	if (pf_timer && pf_timer->paused && !screen_off && !app_dormant &&
	    !(app_pid > 0 && !page_over_app))
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

void pf_timer_sync(void)
{
	if (!pf_timer)
		return;
	if (screen_off || app_dormant || (app_pid > 0 && !page_over_app))
		lv_timer_pause(pf_timer);
	else
		lv_timer_resume(pf_timer);
}

void pf_init(void)
{
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
			lv_indev_add_event_cb(in, ui_release_cb, LV_EVENT_RELEASED, NULL);
#endif
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
lv_obj_t *m3_page(const char *title, lv_obj_t **scr_out)
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
void obj_rest(lv_obj_t *t)
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

lv_obj_t *push_quiet_scr;	/* set: push() only records the page */
bool push_quiet;

/*
 * How each page on the stack was opened, so that a turn of the screen can
 * build them all again at the new size (ui_turn): the opener, or the
 * folder of a Files page. Each opener marks itself; push() files it.
 * A page with neither (the Wi-Fi password) keeps the screen as it is.
 */
struct nav_re nav_re[NAV_MAX], nav_cur;

void nav_mark(void (*open)(lv_event_t *), const char *dir)
{
	free(nav_cur.dir);
	nav_cur.open = open;
	nav_cur.dir = dir ? strdup(dir) : NULL;
}

void push(lv_obj_t *scr)
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
