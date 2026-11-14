/*
 * Home screen pages (included by ui_m3.c), moving as iOS 4.2.1's
 * SpringBoard pages do (UIScrollView with paging; constants and rules in
 * n72-ramboot/ui/IOS-PAGING.md part 1).
 *
 * Layout: the tiles sit in one strip (home_apps) as wide as all the pages,
 * at y 40 under the fixed status bar; page 0 holds the card and PG_ROWS0
 * rows, the others PG_ROWSN rows; dots at the bottom. The status bar's
 * time is hidden on page 0 (the card shows it) and fades in as the strip
 * leaves it.
 *
 * Touch: a sideways move of 10 px takes the touch (the content then tracks
 * from there, no jump); past the first or last page it moves at half the
 * finger. The lift chooses the page: faster than 0.3 px/ms goes on one page
 * from where it is, otherwise the nearer one; a touch during the settle
 * catches the strip at once, and a flick the same way again during the
 * settle goes one page further. The settle is UIScrollView's spring
 * (ios_pager_step: iOS's positions exactly, 6 multiplies a frame), stepped
 * like iOS by the whole milliseconds of each frame.
 *
 * Frames: nothing is drawn while the strip moves. It is one picture of the
 * strip, taken once (and again only when home changed), moved over the
 * pictures of the bar, the time and the dots:
 *  - over a wallpaper, the launcher's layer is transparent and the
 *    wallpaper itself is on display layer 0 (the console's frame buffer,
 *    /dev/fb0, written once): the pictures are premultiplied and each one
 *    is an exact GPU copy, the display blends them over the still picture;
 *  - on the plain background the pictures are opaque, with the
 *    background in them: GPU copies too;
 *  - turned, or with a program on layer 0: the wallpaper as an image under
 *    the pictures, blended by the CPU (the slow case).
 * At rest the home screen is drawn as always (opaque, over its wallpaper
 * image) at the new page.
 */

#define PG_MAX		8
#define PG_TOP		40		/* the strip starts under the status bar */
#define PG_TILE_W	76
#define PG_TILE_H	96
#define PG_ROW_GAP	8
#define PG_CARD_H	100
#define PG_GRID0	112		/* page 0's grid, from the strip's top */
#define PG_GRIDN	8		/* the other pages' */
#define PG_DOTS_H	24		/* the dots' row at the bottom */
#define PG_SLOP		10		/* UIPanGestureRecognizer hysteresis */
#define PG_FLICK	0.3		/* px/ms: on one page */
#define PG_STILL	0.25		/* px/ms: slower is let go from rest */
#define PG_TIME_LEVELS	16

enum { PG_MODE_LAYER0, PG_MODE_OPAQUE, PG_MODE_BLEND };

static lv_obj_t *home_bar, *home_dots;

static struct {
	int n, cur;			/* pages, the one at rest */
	int cols, rows0, rowsn;
	/* touch */
	bool cand, on, caught;
	int x0, y0;			/* where the finger came down */
	int start_off, tx0;		/* offset and finger at take-over */
	int last_x;
	uint64_t last_us;
	double s_prev, s_cur;		/* velocity samples, px/ms (content way) */
	bool have_prev;
	double vel, prev_vel;		/* per move: velocityInView / 1000 */
	int dir;			/* the settle's way: +1 on, -1 back, 0 nearest */
	/* settle */
	bool settling;
	struct ios_pager sp;
	uint64_t step_us;
	int target;			/* page */
	/* frames */
	int mode;
	int off;			/* the strip's offset now, px */
	int time_lv;			/* the time's picture showing */
	lv_obj_t *scr, *strip_img, *bar_img, *bar_r_img, *time_img, *dots_img, *wp_img;
	lv_draw_buf_t *strip_pic, *bar_pic, *bar_l, *bar_r, *dots_pic, *time_pic[PG_TIME_LEVELS + 1];
	lv_area_t time_area;		/* the time's place in the bar */
	unsigned long pic_ver;		/* home_ver they show */
	int pic_mode, pic_w, pic_dots;
	bool fb0_ok;			/* /dev/fb0 holds the home wallpaper */
	int fb0_w;
} pg = { .n = 1, .pic_ver = ~0UL, .pic_dots = -1 };

#include <linux/fb.h>

static lv_point_t rt_logical(lv_point_t p);	/* (main.c) */
static lv_timer_t *pg_warm_timer;

/* ------------------------------------------------------------- layout */

static void pg_dots_update(void)
{
	int i;

	if (!home_dots)
		return;
	if (lv_obj_get_child_count(home_dots) != (uint32_t)pg.n) {
		lv_obj_clean(home_dots);
		for (i = 0; i < pg.n; i++) {
			lv_obj_t *d = lv_obj_create(home_dots);

			plain(d);
			lv_obj_set_size(d, 7, 7);
			lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
			lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
		}
	}
	for (i = 0; i < pg.n; i++) {
		lv_obj_t *d = lv_obj_get_child(home_dots, i);
		lv_color_t on = home_wp ? WP_TEXT : M.on_surface;

		lv_obj_set_style_bg_color(d, on, 0);
		lv_obj_set_style_bg_opa(d, i == pg.cur ? LV_OPA_COVER : LV_OPA_40, 0);
	}
	if (pg.n > 1)
		lv_obj_remove_flag(home_dots, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(home_dots, LV_OBJ_FLAG_HIDDEN);
}

/* the status bar's time: shown off page 0 */
static void pg_time_show(bool on)
{
	if (!status_time)
		return;
	if (on)
		lv_obj_remove_flag(status_time, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(status_time, LV_OBJ_FLAG_HIDDEN);
}

static void pg_show_page(int p)
{
	bool pics = pg.pic_ver == home_ver;

	pg.cur = LV_CLAMP(0, p, pg.n - 1);
	if (home_apps)
		lv_obj_set_x(home_apps, -pg.cur * W);
	pg_dots_update();
	pg_time_show(pg.cur > 0);
	bp_changed(home_scr);	/* (pictures of home as it shows) */
	if (pics)
		pg.pic_ver = home_ver;	/* the strip's picture does not depend on the page */
}

/*
 * The tiles into pages: child 0 of the strip is the card, the rest are
 * tiles in order. Called whenever the tiles change.
 */
static void pg_layout(void)
{
	int bottom = H - PG_TOP - PG_DOTS_H, per0, pern, i, k, page, slot, cnt;
	int x0;

	if (!home_apps)
		return;
	pg.cols = LV_MAX(1, (W - 16) / PG_TILE_W);
	pg.rows0 = LV_MAX(1, (bottom - PG_GRID0 + PG_ROW_GAP) / (PG_TILE_H + PG_ROW_GAP));
	pg.rowsn = LV_MAX(1, (bottom - PG_GRIDN + PG_ROW_GAP) / (PG_TILE_H + PG_ROW_GAP));
	per0 = pg.cols * pg.rows0;
	pern = pg.cols * pg.rowsn;
	x0 = (W - pg.cols * PG_TILE_W) / 2;
	cnt = (int)lv_obj_get_child_count(home_apps) - 1;	/* (the card) */
	for (i = 0; i < cnt; i++) {
		lv_obj_t *t = lv_obj_get_child(home_apps, i + 1);
		int row, col, top;

		if (i < per0) {
			page = 0;
			slot = i;
			top = PG_GRID0;
		} else {
			page = 1 + (i - per0) / pern;
			slot = (i - per0) % pern;
			top = PG_GRIDN;
		}
		row = slot / pg.cols;
		col = slot % pg.cols;
		lv_obj_set_pos(t, page * W + x0 + col * PG_TILE_W, top + row * (PG_TILE_H + PG_ROW_GAP));
	}
	k = cnt <= per0 ? 1 : 1 + (cnt - per0 + pern - 1) / pern;
	pg.n = LV_MIN(k, PG_MAX);
	lv_obj_set_size(home_apps, pg.n * W, H - PG_TOP - PG_DOTS_H);	/* (not over the dots: see pg_frames_begin) */
	pg_show_page(pg.cur);
}

/* --------------------------------------------------------- the pictures */

static void pg_pics_free(void)
{
	int i;

	tr_free(&pg.strip_pic);
	tr_free(&pg.bar_pic);
	tr_free(&pg.bar_l);
	tr_free(&pg.bar_r);
	tr_free(&pg.dots_pic);
	for (i = 0; i <= PG_TIME_LEVELS; i++)
		tr_free(&pg.time_pic[i]);
	pg.pic_ver = ~0UL;
	pg.pic_dots = -1;
}

static int pg_mode(void)
{
	if (!home_wp)
		return PG_MODE_OPAQUE;
	if (ui_rot == 0 && app_pid <= 0 && mbx_gpu_on_shell_layer())
		return PG_MODE_LAYER0;
	return PG_MODE_BLEND;
}

/*
 * A picture of @o for the frames: opaque with the screen's colour behind it
 * (on the plain background), else with alpha (premultiplied for layer 0).
 */
static lv_draw_buf_t *pg_snap(lv_obj_t *o, int mode)
{
	lv_draw_buf_t *b;

	if (mode == PG_MODE_OPAQUE) {
		lv_opa_t opa = lv_obj_get_style_bg_opa(o, 0);
		lv_color_t col = lv_obj_get_style_bg_color(o, 0);

		lv_obj_set_style_bg_color(o, M.surface, 0);
		lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
		b = snap(o, LV_COLOR_FORMAT_XRGB8888);
		lv_obj_set_style_bg_opa(o, opa, 0);
		lv_obj_set_style_bg_color(o, col, 0);
		return b;
	}
	b = snap(o, LV_COLOR_FORMAT_ARGB8888);
	if (b && mode == PG_MODE_LAYER0)
		mbx_gpu_premultiply_buf(b);
	return b;
}

/* columns @x1..@x2 of @b as a picture of their own (flags kept) */
static lv_draw_buf_t *pg_crop(const lv_draw_buf_t *b, int x1, int x2)
{
	lv_draw_buf_t *o;
	int y;

	if (x2 < x1)
		return NULL;
	o = lv_draw_buf_create(x2 - x1 + 1, b->header.h, b->header.cf, 0);
	if (!o)
		return NULL;
	for (y = 0; y < (int)b->header.h; y++)
		memcpy(o->data + (size_t)y * o->header.stride, b->data + (size_t)y * b->header.stride + x1 * 4,
		       (size_t)(x2 - x1 + 1) * 4);
	o->header.flags |= b->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED;
	lv_draw_buf_flush_cache(o, NULL);
	return o;
}

/*
 * The time's fade: PG_TIME_LEVELS + 1 small pictures of its place in the
 * bar, from without (0) to with it (all), mixed pixel by pixel (exact for
 * premultiplied and for opaque pixels alike). Each frame copies one.
 */
static void pg_time_pics(int mode)
{
	lv_draw_buf_t *with;
	lv_area_t bar, a;
	int w, h, lv, x, y;

	if (!status_time || !pg.bar_pic)
		return;
	pg_time_show(true);
	with = pg_snap(home_bar, mode);
	pg_time_show(pg.cur > 0);
	if (!with)
		return;
	lv_obj_update_layout(status_time);
	lv_obj_get_coords(home_bar, &bar);
	lv_obj_get_coords(status_time, &a);
	lv_area_move(&a, -bar.x1, -bar.y1);
	a.x1 = LV_MAX(0, a.x1 - 2);
	a.x2 = LV_MIN((int32_t)with->header.w - 1, a.x2 + 2);
	a.y1 = 0;
	a.y2 = (int32_t)with->header.h - 1;
	pg.time_area = a;
	w = lv_area_get_width(&a);
	h = lv_area_get_height(&a);
	for (lv = 1; lv <= PG_TIME_LEVELS; lv++) {
		lv_draw_buf_t *b = lv_draw_buf_create(w, h, with->header.cf, 0);

		if (!b)
			break;
		for (y = 0; y < h; y++) {
			const uint8_t *p0 = pg.bar_pic->data + (size_t)(a.y1 + y) * pg.bar_pic->header.stride + a.x1 * 4;
			const uint8_t *p1 = with->data + (size_t)(a.y1 + y) * with->header.stride + a.x1 * 4;
			uint8_t *o = b->data + (size_t)y * b->header.stride;

			for (x = 0; x < w * 4; x++)
				o[x] = (uint8_t)((p0[x] * (PG_TIME_LEVELS - lv) + p1[x] * lv +
						  PG_TIME_LEVELS / 2) / PG_TIME_LEVELS);
		}
		if (mode == PG_MODE_LAYER0)
			b->header.flags |= LV_IMAGE_FLAGS_PREMULTIPLIED;
		lv_draw_buf_flush_cache(b, NULL);
		pg.time_pic[lv] = b;
	}
	tr_free(&with);
	/* the bar around the time, so that no picture lies over another */
	pg.time_pic[0] = pg_crop(pg.bar_pic, a.x1, a.x2);	/* (none of the time) */
	pg.bar_l = pg_crop(pg.bar_pic, 0, a.x1 - 1);
	pg.bar_r = pg_crop(pg.bar_pic, a.x2 + 1, (int)pg.bar_pic->header.w - 1);
}

/* The pictures, taken again only if home changed (true: they are there). */
static bool pg_pics(void)
{
	int mode = pg_mode(), i;

	if (pg.strip_pic && pg.pic_ver == home_ver && pg.pic_mode == mode && pg.pic_w == W) {
		if (pg.pic_dots != pg.cur) {	/* only the dots moved on */
			tr_free(&pg.dots_pic);
			pg.dots_pic = pg_snap(home_dots, mode);
			pg.pic_dots = pg.cur;
		}
		return true;
	}
	pg_pics_free();
	lv_obj_update_layout(home_scr);
	pg_time_show(false);		/* (the bar's picture without the time) */
	pg.strip_pic = pg_snap(home_apps, mode);
	pg.bar_pic = pg_snap(home_bar, mode);
	pg.dots_pic = pg.n > 1 ? pg_snap(home_dots, mode) : NULL;
	pg_time_show(pg.cur > 0);
	pg_time_pics(mode);
	if (!pg.strip_pic || !pg.bar_pic) {
		pg_pics_free();
		return false;
	}
	for (i = 1; i <= PG_TIME_LEVELS && pg.time_pic[i]; i++)
		;
	pg.pic_ver = home_ver;
	pg.pic_mode = mode;
	pg.pic_w = W;
	pg.pic_dots = pg.cur;
	return true;
}

/*
 * Layer 0 shows the console's frame buffer when no program has the display:
 * the home wallpaper is written there (upright; 1-3 ms), once, and again
 * after something else drew into it (an application's start fills it).
 */
static bool pg_fb0_wallpaper(void)
{
	lv_draw_buf_t *wpic = wp_pic(WP_HOME);
	struct fb_fix_screeninfo fi;
	struct fb_var_screeninfo vi;
	int fd, y, x;
	uint8_t *p;
	size_t len;

	if (pg.fb0_ok && pg.fb0_w == W)
		return true;
	if (!wpic || (fd = open("/dev/fb0", O_RDWR | O_CLOEXEC)) < 0)
		return false;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) || ioctl(fd, FBIOGET_VSCREENINFO, &vi) ||
	    vi.bits_per_pixel != 32) {
		close(fd);
		return false;
	}
	len = (size_t)fi.line_length * vi.yres;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return false;
	for (y = 0; y < (int)vi.yres; y++) {
		uint32_t *row = (uint32_t *)(p + (size_t)y * fi.line_length);

		if (!ui_rot && (int)vi.xres == W) {
			memcpy(row, wpic->data + (size_t)y * wpic->header.stride, (size_t)W * 4);
			continue;
		}
		for (x = 0; x < (int)vi.xres; x++) {	/* (turned: as the display turns layer 1) */
			lv_point_t l = rt_logical((lv_point_t){ x, y });

			row[x] = *(const uint32_t *)(wpic->data + (size_t)l.y * wpic->header.stride + l.x * 4);
		}
	}
	munmap(p, len);
	pg.fb0_ok = true;
	pg.fb0_w = W;
	return true;
}

/* (layer 0 was written by someone else: an application's start) */
static void pg_fb0_dirty(void)
{
	pg.fb0_ok = false;
}

/* ------------------------------------------------------------- frames */

static int pg_max_off(void)
{
	return (pg.n - 1) * W;
}

/* The pictures where the strip's offset @off puts them. */
static void pg_frame(int off)
{
	int lv;

	pg.off = off;
	if (!pg.scr)
		return;
	lv_obj_set_x(pg.strip_img, -off);
	/* the time: nothing on page 0, all of it a page on */
	lv = LV_CLAMP(0, (off * PG_TIME_LEVELS + W / 2) / W, PG_TIME_LEVELS);
	if (pg.time_img && lv != pg.time_lv && pg.time_pic[lv]) {
		pg.time_lv = lv;		/* (a new picture only when the level moves) */
		lv_image_set_src(pg.time_img, pg.time_pic[lv]);
	}
}

static lv_obj_t *pg_img(lv_draw_buf_t *b, int32_t x, int32_t y)
{
	lv_obj_t *i = lv_image_create(pg.scr);

	lv_image_set_src(i, b);
	lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(i, x, y);
	return i;
}

/* The frames' screen, in place of home. False: no memory for the pictures. */
static bool pg_frames_begin(void)
{
	int mode;

	if (pg.scr)
		return true;
	if (!pg_pics())
		return false;
	mode = pg.pic_mode;
	if (mode == PG_MODE_LAYER0 && !pg_fb0_wallpaper())
		return false;
	pg.mode = mode;
	pg.scr = lv_obj_create(NULL);
	if (mode == PG_MODE_LAYER0) {
		lv_obj_remove_style_all(pg.scr);	/* nothing under the pictures */
	} else {
		m3_screen(pg.scr);
		if (mode == PG_MODE_BLEND)
			pg.wp_img = pg_img(wp_pic(WP_HOME), 0, 0);
	}
	lv_obj_remove_flag(pg.scr, LV_OBJ_FLAG_SCROLLABLE);
	/*
	 * No picture over another: with layer 0 each one is an exact GPU copy
	 * that the flush must leave alone, and it can only when nothing is
	 * drawn over it later (else it premultiplies it again on the CPU:
	 * slower, and soft edges too dark). The strip ends above the dots, the
	 * bar is cut around the time.
	 */
	pg.strip_img = pg_img(pg.strip_pic, -pg.cur * W, PG_TOP);
	{
		lv_area_t b;

		lv_obj_get_coords(home_bar, &b);
		if (pg.time_pic[PG_TIME_LEVELS] && pg.time_pic[0]) {
			if (pg.bar_l)
				pg.bar_img = pg_img(pg.bar_l, b.x1, b.y1);
			if (pg.bar_r)
				pg.bar_r_img = pg_img(pg.bar_r, b.x1 + pg.time_area.x2 + 1, b.y1);
			pg.time_img = pg_img(pg.time_pic[PG_TIME_LEVELS],
					     b.x1 + pg.time_area.x1, b.y1 + pg.time_area.y1);
		} else {
			pg.bar_img = pg_img(pg.bar_pic, b.x1, b.y1);
		}
	}
	if (pg.dots_pic) {
		lv_area_t d;

		lv_obj_get_coords(home_dots, &d);
		pg.dots_img = pg_img(pg.dots_pic, d.x1, d.y1);
	}
	if (mode == PG_MODE_LAYER0)
		mbx_gpu_set_transparent(true);
	pg.time_lv = -1;
	pg_frame(pg.cur * W);
	lv_screen_load(pg.scr);
	return true;
}

/* At rest on page @p: home as it is drawn always. */
static void pg_frames_end(int p)
{
	lv_obj_t *scr = pg.scr;

	pg.on = pg.settling = false;
	lv_anim_delete(&pg, NULL);
	pg_show_page(p);
	if (!scr)
		return;
	pg.scr = NULL;
	pg.strip_img = pg.bar_img = pg.bar_r_img = pg.time_img = pg.dots_img = pg.wp_img = NULL;
	if (pg.mode == PG_MODE_LAYER0)
		mbx_gpu_set_transparent(false);
	lv_screen_load(home_scr);
	lv_obj_delete(scr);
}

/* ------------------------------------------------------------- settle */

/* UIScrollView's _prepareToPage...: the page to settle on. */
static int pg_decide(double v)
{
	double x = pg.off;
	int t;

	if (pg.off % W == 0) {
		t = pg.off / W;		/* aligned: stays */
	} else if (v > PG_FLICK) {
		/* on from where it is; a flick again during the settle: one more */
		t = (int)ceil((pg.dir > 0 ? (pg.target + 1) * (double)W : x) / W);
		pg.dir = 1;
	} else if (v < -PG_FLICK) {
		t = (int)floor((pg.dir < 0 ? (pg.target - 1) * (double)W : x) / W);
		pg.dir = -1;
	} else {
		t = (int)floor(x / W + 0.5);
		pg.dir = 0;
	}
	return LV_CLAMP(0, t, pg.n - 1);
}

static void pg_settle_exec(void *var, int32_t v)
{
	uint64_t now = mono_us();
	int n = (int)((now - pg.step_us) / 1000);	/* whole ms; the rest dropped, as iOS */

	LV_UNUSED(var);
	LV_UNUSED(v);
	if (!pg.settling || n < 1)
		return;
	pg.step_us = now;
	if (ios_pager_step(&pg.sp, n)) {
		pg_frame((int)pg.sp.x);
		pg.dir = 0;
		pg_frames_end(pg.target);
		return;
	}
	pg_frame((int)pg.sp.x);
}

static void pg_settle(double v)
{
	lv_anim_t a;
	bool inside = pg.off >= 0 && pg.off <= pg_max_off();
	double v0;

	pg.target = pg_decide(pg.vel);
	if (inside && pg.vel * pg.vel < PG_STILL * PG_STILL)
		pg.vel = pg.prev_vel = 0;	/* (_endPanWithEvent) */
	v0 = pg.prev_vel != 0 ? 0.75 * pg.prev_vel + 0.25 * pg.vel : pg.vel;	/* (_startTimer) */
	LV_UNUSED(v);
	ios_pager_init(&pg.sp, (float)W);
	pg.sp.x = (float)pg.off;
	pg.sp.v = v0;
	pg.sp.target = (float)(pg.target * W);
	if (pg.off == pg.target * W) {
		pg_frames_end(pg.target);
		return;
	}
	pg.settling = true;
	pg.step_us = mono_us();
	lv_anim_delete(&pg, NULL);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &pg);
	lv_anim_set_exec_cb(&a, pg_settle_exec);
	lv_anim_set_values(&a, 0, 1);
	lv_anim_set_duration(&a, 3000);		/* (ends itself well before) */
	lv_anim_set_completed_cb(&a, NULL);
	lv_anim_start(&a);
}

/* ---------------------------------------------------------------- touch */

/* the offset for the finger at @x: 1:1, half the way past either end */
static int pg_track(int x)
{
	double raw = pg.start_off - (x - pg.tx0), max = pg_max_off();

	if (raw < 0)
		raw = raw * 0.5;
	else if (raw > max)
		raw = max + (raw - max) * 0.5;
	return (int)lround(raw);
}

/* velocity samples as UIPanGestureRecognizer keeps them (px/ms, content way) */
static void pg_sample(int x)
{
	uint64_t now = mono_us();
	double dt = (now - pg.last_us) / 1000.0;

	if (dt <= 1.0)
		return;
	pg.s_prev = pg.s_cur;
	pg.have_prev = true;
	pg.s_cur = -(x - pg.last_x) / dt;
	pg.last_x = x;
	pg.last_us = now;
	pg.prev_vel = pg.vel;
	pg.vel = pg.have_prev ? 0.75 * pg.s_prev + 0.25 * pg.s_cur : pg.s_cur;
}

static void pg_take(int x, int off)
{
	pg.on = true;
	pg.start_off = off;
	pg.tx0 = x;
	pg.last_x = x;
	pg.last_us = mono_us();
	pg.s_prev = pg.s_cur = 0;
	pg.have_prev = false;
	pg.vel = pg.prev_vel = 0;
}

/*
 * A touch on home, from ui_drag(): phase 0 down, 1 move, 2 lift. True once
 * the touch is the pages' (the router then keeps it from LVGL).
 */
static bool pg_drag(int phase, int x, int y)
{
	if (phase == 0) {
		pg.cand = false;
		pg.caught = false;
		if (pg.settling && pg.scr) {
			/* caught: the strip stops under the finger, no slop */
			pg.settling = false;
			lv_anim_delete(&pg, NULL);
			pg.caught = true;
			pg_take(x, pg.off);
			return true;
		}
		if (pg.n < 2 || lv_screen_active() != home_scr || tr.scr || za.phase ||
		    shade_open || sd.on || screen_off)
			return false;
		pg.cand = true;
		pg.x0 = x;
		pg.y0 = y;
		return false;
	}
	if (!pg.on) {
		int dx = x - pg.x0, dy = y - pg.y0;

		if (!pg.cand || phase == 2)
			return false;
		if (LV_ABS(dy) > SD_SLOP && LV_ABS(dy) >= LV_ABS(dx)) {
			pg.cand = false;	/* up or down: the shade's */
			return false;
		}
		if (LV_ABS(dx) < PG_SLOP)
			return false;
		pg.cand = false;
		ui_touch_lost();	/* (not a tile's press) */
		if (!pg_frames_begin())
			return false;
		pg_take(x, pg.cur * W);
		pg.tx0 = pg.x0 + (dx > 0 ? PG_SLOP : -PG_SLOP);	/* no jump by the slop */
		pg.dir = 0;	/* (a fresh drag: no settle to add to) */
	}
	if (phase == 2) {
		pg.on = false;
		if (mono_us() - pg.last_us > 80000)
			pg.vel = pg.prev_vel = 0;	/* held still before the lift */
		pg_settle(pg.vel);
		return true;
	}
	pg_sample(x);
	pg_frame(pg_track(x));
	return true;
}

/* ------------------------------------------------------------ warming */

/*
 * While home is still and lit: the pictures ready before a swipe needs
 * them (taking them is a few tens of ms) and the wallpaper on layer 0.
 */
static void pg_warm(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (pg.n < 2 || screen_off || locked || pg.scr || lv_screen_active() != home_scr ||
	    tr.scr || za.phase || sd.on || lv_anim_count_running() ||
	    (touch_indev && lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		return;
	if (pg.pic_ver != home_ver || pg.pic_mode != pg_mode() || pg.pic_w != W)
		pg_pics();
	else if (pg.pic_mode == PG_MODE_LAYER0)
		pg_fb0_wallpaper();
}

/* Home is being built again (a turn, a theme): its pictures go with it. */
static void pg_home_gone(void)
{
	if (pg_warm_timer) {
		lv_timer_delete(pg_warm_timer);
		pg_warm_timer = NULL;
	}
	if (pg.scr) {
		lv_obj_t *scr = pg.scr;

		pg.scr = NULL;
		pg.on = pg.settling = false;
		lv_anim_delete(&pg, NULL);
		if (pg.mode == PG_MODE_LAYER0)
			mbx_gpu_set_transparent(false);
		if (lv_screen_active() == scr)
			lv_screen_load(lv_obj_create(NULL));	/* (the rebuild loads its own) */
		lv_obj_delete(scr);
	}
	pg_pics_free();
	pg.fb0_ok = false;
}

/* A swipe cut short (Home, the screen off): home at the nearer page. */
static void pg_abort(void)
{
	if (!pg.scr)
		return;
	pg.dir = 0;
	pg_frames_end(pg.settling ? pg.target : LV_CLAMP(0, (pg.off + W / 2) / W, pg.n - 1));
}
