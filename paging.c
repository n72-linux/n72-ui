/*
 * Home screen pages (included by ui_m3.c), using paged scroll physics.
 *
 * Layout: the tiles sit in one strip (home_apps) as wide as all the pages,
 * at y 40 under the fixed status bar; page 0 holds the card and PG_ROWS0
 * rows, the others PG_ROWSN rows; dots at the bottom. The status bar's
 * time is hidden on page 0 (the card shows it) and fades in as the strip
 * leaves it.
 *
 * Touch: a sideways move of 10 px takes the touch (the content then tracks
 * from there, no jump); past the first or last page it moves at half the
 * finger, and so does a home of one page, which then springs back. The lift chooses the page: faster than 0.3 px/ms goes on one page
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

#define PG_TILE_W	76
#define PG_TILE_H	96
#define PG_ROW_GAP	8
#define PG_GRID0	112		/* page 0's grid, from the strip's top */
#define PG_GRIDN	8		/* the other pages' */
#define PG_SLOP		10		/* UIPanGestureRecognizer hysteresis */
#define PG_FLICK	0.3		/* px/ms: on one page */
#define PG_STILL	0.25		/* px/ms: slower is let go from rest */

enum { PG_MODE_LAYER0, PG_MODE_OPAQUE, PG_MODE_BLEND };

static void dock_fly_stop(void);
void dock_carry_stop(void);
lv_obj_t *home_bar, *home_dots, *home_view;	/* home_view: the pages' window */

/*
 * The dock (dock.c) holds a side of the screen: the bottom upright, and
 * turned the side that is the bottom in the hand (the left at 90, the
 * right at 270), its icons upright. The pages, the bar and the dots have
 * the rest: PX, PW (the page's width) and PB (their bottom).
 */
#define DOCK_T		76		/* the dock's thickness */
#define DOCK_M		8		/* from the screen's edge */
#define DOCK_SPACE	(DOCK_T + DOCK_M + 4)
enum { DOCK_BOTTOM, DOCK_LEFT, DOCK_RIGHT };
int home_px, home_pw, home_pb;

static int dock_side(void)
{
	return ui_rot == 90 ? DOCK_LEFT : ui_rot == 270 ? DOCK_RIGHT : DOCK_BOTTOM;
}

void home_geom(void)
{
	int side = dock_side();

	home_px = side == DOCK_LEFT ? DOCK_SPACE : 0;
	home_pw = side == DOCK_BOTTOM ? W : W - DOCK_SPACE;
	home_pb = side == DOCK_BOTTOM ? H - DOCK_SPACE : H;
}

struct pg pg = { .n = 1, .pic_ver = ~0UL, .pic_dots = -1 };

/*
 * A tile with this flag starts a page of its own (an icon carried past the
 * last page made one; ui.conf's order has "/" before its name). Pages also
 * start where the one before is full.
 */
#define PG_BREAK	LV_OBJ_FLAG_USER_3

/* the page child @i of the strip is on (-1: the card, or none) */
static int pg_page_of(int i)
{
	return i > 0 && i < (int)sizeof(pg.page_of) ? pg.page_of[i] : -1;
}

#include <linux/fb.h>

lv_point_t rt_logical(lv_point_t p);	/* (main.c) */

#ifdef N72_DEBUG
/* (debug, N72_FRAMES) the pages' input and steps, beside main.c's frames */
static bool pg_trace(void)
{
	static int t = -1;

	if (t < 0)
		t = getenv("N72_FRAMES") != NULL;
	return t;
}
#else
static inline bool pg_trace(void) { return false; }
#endif
lv_timer_t *pg_warm_timer;

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
		lv_obj_set_x(home_apps, -pg.cur * PW);
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
	int bottom = PB - PG_TOP - PG_DOTS_H, per0, pern, i, k, page, slot, cnt;	/* (tiles past PG_MAX pages: off to the right, as before) */
	int x0;

	if (!home_apps)
		return;
	pg.cols = LV_MAX(1, (PW - 16) / PG_TILE_W);
	pg.rows0 = LV_MAX(1, (bottom - PG_GRID0 + PG_ROW_GAP) / (PG_TILE_H + PG_ROW_GAP));
	pg.rowsn = LV_MAX(1, (bottom - PG_GRIDN + PG_ROW_GAP) / (PG_TILE_H + PG_ROW_GAP));
	per0 = pg.cols * pg.rows0;
	pern = pg.cols * pg.rowsn;
	x0 = (PW - pg.cols * PG_TILE_W) / 2;
	cnt = (int)lv_obj_get_child_count(home_apps) - 1;	/* (the card) */
	page = 0;
	slot = 0;
	for (i = 0; i < cnt; i++) {
		lv_obj_t *t = lv_obj_get_child(home_apps, i + 1);
		int row, col, top;

		/* the next page: this one full, or the tile starts one */
		if (slot >= (page ? pern : per0) || (slot > 0 && lv_obj_has_flag(t, PG_BREAK))) {
			page++;
			slot = 0;
		}
		if (i + 1 < (int)sizeof(pg.page_of))
			pg.page_of[i + 1] = (signed char)LV_MIN(page, 127);
		top = page ? PG_GRIDN : PG_GRID0;
		row = slot / pg.cols;
		col = slot % pg.cols;
		lv_obj_set_pos(t, page * PW + x0 + col * PG_TILE_W, top + row * (PG_TILE_H + PG_ROW_GAP));
		slot++;
	}
	k = page + 1 + (pg.new_page ? 1 : 0);
	pg.n = LV_MIN(k, PG_MAX);
	lv_obj_set_size(home_apps, pg.n * PW, bottom);	/* (not over the dots: see pg_frames_begin) */
	pg_show_page(pg.cur);
	/* the tiles may have moved: pictures of the pages taken before are old
	 * (pg_show_page keeps them, a change of page alone does not age them) */
	bp_changed(home_scr);
}

/* --------------------------------------------------------- the pictures */

void pg_pics_free(void)
{
	int i;

	if (pg_warm_timer)
		lv_timer_pause(pg_warm_timer);
	for (int k = 0; k < PG_PAGES_MAX; k++)
		tr_free(&pg.page_pic[k]);
	tr_free(&pg.dock_pic);
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
	/*
	 * Layer 0 under a transparent layer 1: upright, and turned when the
	 * display turns layer 1 itself (then LVGL draws as upright; the
	 * wallpaper goes onto layer 0 turned, pg_fb0_wallpaper). Not with the
	 * GPU's turned copy of each frame.
	 */
	if (mbx_gpu_draws_direct() && app_pid <= 0 && mbx_gpu_on_shell_layer())
		return PG_MODE_LAYER0;
	return PG_MODE_BLEND;
}

/*
 * A picture of @a (screen coordinates) of @obj, as lv_snapshot_take() draws
 * all of @obj: one page of the strip at a time. (The whole strip was one
 * picture n pages wide: past 2048 px, five pages turned, the GPU drew it
 * as streaks.)
 */
static lv_draw_buf_t *snap_area(lv_obj_t *obj, lv_color_format_t cf, const lv_area_t *a)
{
	lv_draw_buf_t *b;
	lv_display_t *disp = lv_obj_get_display(obj), *disp_old;
	lv_layer_t layer, *layer_old;

	lv_obj_update_layout(obj);
	b = lv_draw_buf_create(lv_area_get_width(a), lv_area_get_height(a), cf, 0);
	if (!b)
		return NULL;
	lv_draw_buf_clear(b, NULL);
	lv_memzero(&layer, sizeof(layer));
	layer.draw_buf = b;
	layer.buf_area = *a;
	layer.color_format = cf;
	layer._clip_area = *a;
	layer.phy_clip_area = *a;
#if LV_DRAW_TRANSFORM_USE_MATRIX
	lv_matrix_identity(&layer.matrix);
#endif
	disp_old = lv_refr_get_disp_refreshing();
	layer_old = disp->layer_head;
	disp->layer_head = &layer;
	lv_refr_set_disp_refreshing(disp);
	lv_obj_redraw(&layer, obj);
	while (layer.draw_task_head) {
		lv_draw_dispatch_wait_for_request();
		lv_draw_dispatch();
	}
	disp->layer_head = layer_old;
	lv_refr_set_disp_refreshing(disp_old);
	lv_draw_buf_flush_cache(b, NULL);	/* the GPU reads it */
	return b;
}

/*
 * A picture of @o (of @a of it if given) for the frames: opaque with the
 * screen's colour behind it (on the plain background), else with alpha
 * (premultiplied for layer 0).
 */
static lv_draw_buf_t *pg_snap_area(lv_obj_t *o, int mode, const lv_area_t *a)
{
	lv_draw_buf_t *b;

	if (mode == PG_MODE_OPAQUE) {
		lv_opa_t opa = lv_obj_get_style_bg_opa(o, 0);
		lv_color_t col = lv_obj_get_style_bg_color(o, 0);

		lv_obj_set_style_bg_color(o, M.surface, 0);
		lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
		b = a ? snap_area(o, LV_COLOR_FORMAT_XRGB8888, a) : snap(o, LV_COLOR_FORMAT_XRGB8888);
		lv_obj_set_style_bg_opa(o, opa, 0);
		lv_obj_set_style_bg_color(o, col, 0);
		return b;
	}
	b = a ? snap_area(o, LV_COLOR_FORMAT_ARGB8888, a) : snap(o, LV_COLOR_FORMAT_ARGB8888);
	if (b && mode == PG_MODE_LAYER0)
		mbx_gpu_premultiply_buf(b);
	return b;
}

static lv_draw_buf_t *pg_snap(lv_obj_t *o, int mode)
{
	return pg_snap_area(o, mode, NULL);
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
	/*
	 * From the bar's left edge, and wide enough (4096 px and more) for
	 * the GPU's exact copy: smaller pictures are drawn by the CPU, which
	 * with layer 0 means premultiplied twice.
	 */
	a.x1 = 0;
	a.x2 = LV_MIN((int32_t)with->header.w - 1, LV_MAX(a.x2 + 2, 4096 / (int32_t)with->header.h + 1));
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
static bool pg_pics_take(void)
{
	int mode = pg_mode(), i;

	if (pg.page_pic[0] && pg.pic_ver == home_ver && pg.pic_mode == mode && pg.pic_w == W) {
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
	{
		lv_area_t s, a;
		int k;

		lv_obj_get_coords(home_apps, &s);	/* (page 0 at x PX - cur * PW) */
		for (k = 0; k < pg.n && k < PG_PAGES_MAX; k++) {
			a = s;
			a.x1 = s.x1 + k * PW;
			a.x2 = a.x1 + PW - 1;
			if (!(pg.page_pic[k] = pg_snap_area(home_apps, mode, &a)))
				break;
		}
	}
	pg.bar_pic = pg_snap(home_bar, mode);
	pg.dock_pic = home_dock ? pg_snap(home_dock, mode) : NULL;
	pg.dots_pic = pg.n > 1 ? pg_snap(home_dots, mode) : NULL;
	pg_time_show(pg.cur > 0);
	pg_time_pics(mode);
	if (!pg.page_pic[LV_MIN(pg.n, PG_PAGES_MAX) - 1] || !pg.bar_pic) {
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
 * (The time hidden and shown again, the plain background set and taken
 * off: home's look is back as it was, so nothing is drawn again for it.)
 */
static bool pg_pics(void)
{
	bool ok;

	if (home_scr)
		lv_obj_update_layout(home_scr);	/* (real changes marked, outside the quiet) */
	quiet_begin();
	ok = pg_pics_take();
	quiet_end();
	if (!ok) {
		pg.fail_ver = home_ver;
		pg.fail_ms = lv_tick_get();
	}
	return ok;
}

/*
 * Layer 0 shows the console's frame buffer when no program has the display:
 * the home wallpaper is written there (upright; 1-3 ms), once, and again
 * after something else drew into it (an application's start fills it).
 */
static bool pg_fb0_wallpaper(void)
{
	lv_draw_buf_t *wpic = wp_upright(WP_HOME);	/* (layer 0 is not turned: upright always) */
	struct fb_fix_screeninfo fi;
	struct fb_var_screeninfo vi;
	int fd, y;
	uint8_t *p;
	size_t len;

	if (pg.fb0_ok)
		return true;	/* (the same upright frame whichever way the screen is) */
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

		if (y < (int)wpic->header.h)
			memcpy(row, wpic->data + (size_t)y * wpic->header.stride,
			       (size_t)LV_MIN((int)vi.xres, (int)wpic->header.w) * 4);
	}
	munmap(p, len);
	pg.fb0_ok = true;
	pg.fb0_w = W;
	return true;
}

/* (layer 0 was written by someone else: an application's start) */
void pg_fb0_dirty(void)
{
	pg.fb0_ok = false;
	pg_warm_kick();
}

/* ------------------------------------------------------------- frames */

static int pg_max_off(void)
{
	return (pg.n - 1) * PW;
}

/* The pictures where the strip's offset @off puts them. */
static void pg_frame(int off)
{
	int lv;

	pg.off = off;
	if (!pg.scr)
		return;
	{
		/* pages k and k + 1 in view (k -1 or the last: pulled past an end) */
		int k = off >= 0 ? off / PW : -1, n = LV_MIN(pg.n, PG_PAGES_MAX);

		if (k != pg.strip_k) {
			pg.strip_k = k;
			if (k >= 0 && k < n) {
				lv_image_set_src(pg.strip_img, pg.page_pic[k]);
				lv_obj_remove_flag(pg.strip_img, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(pg.strip_img, LV_OBJ_FLAG_HIDDEN);
			}
			if (k + 1 >= 0 && k + 1 < n) {
				lv_image_set_src(pg.strip_img2, pg.page_pic[k + 1]);
				lv_obj_remove_flag(pg.strip_img2, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(pg.strip_img2, LV_OBJ_FLAG_HIDDEN);
			}
		}
		lv_obj_set_x(pg.strip_img, k * PW - off);
		lv_obj_set_x(pg.strip_img2, (k + 1) * PW - off);
	}
	/* the time: nothing on page 0, all of it a page on (pulled past the
	 * last page: as on it, so a single page never shows it) */
	lv = LV_CLAMP(0, (LV_MIN(off, pg_max_off()) * PG_TIME_LEVELS + PW / 2) / PW, PG_TIME_LEVELS);
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
		/* LVGL draws its bottom layer under a screen that is not opaque,
		 * and the theme gave it a background (also app_launch) */
		lv_obj_remove_style_all(lv_layer_bottom());
	} else {
		m3_screen(pg.scr);
		if (mode == PG_MODE_BLEND)
		{
			lv_draw_buf_t *b = wp_pic(WP_HOME);

			if (b && (pg.wp_img = pg_img(b, 0, 0)) && !wp_hold(pg.wp_img, b)) {
				lv_obj_delete(pg.wp_img);
				pg.wp_img = NULL;
			}
		}
	}
	lv_obj_remove_flag(pg.scr, LV_OBJ_FLAG_SCROLLABLE);
	/*
	 * No picture over another: with layer 0 each one is an exact GPU copy
	 * that the flush must leave alone, and it can only when nothing is
	 * drawn over it later (else it premultiplies it again on the CPU:
	 * slower, and soft edges too dark). The strip ends above the dots, the
	 * bar is cut around the time.
	 */
	/*
	 * The page in view and the next: one GPU copy each, side by side, in
	 * a window of the pages' place (turned, the dock is beside them).
	 */
	pg.view = lv_obj_create(pg.scr);
	lv_obj_remove_style_all(pg.view);
	lv_obj_remove_flag(pg.view, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_pos(pg.view, PX, PG_TOP);
	lv_obj_set_size(pg.view, PW, PB - PG_TOP - PG_DOTS_H);
	pg.strip_img = lv_image_create(pg.view);
	lv_image_set_src(pg.strip_img, pg.page_pic[pg.cur]);
	pg.strip_img2 = lv_image_create(pg.view);
	lv_image_set_src(pg.strip_img2, pg.page_pic[pg.cur]);
	if (pg.dock_pic) {
		lv_area_t d;

		lv_obj_get_coords(home_dock, &d);
		pg_img(pg.dock_pic, d.x1, d.y1);
	}
	pg.strip_k = -2;
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
	pg_frame(pg.cur * PW);
	lv_screen_load(pg.scr);
	return true;
}

/* At rest on page @p: home as it is drawn always. */
static void pg_frames_end(int p)
{
	lv_obj_t *scr = pg.scr;

	n72_trace("pg-end>", 0);
	pg.on = pg.settling = false;
	lv_anim_delete(&pg, NULL);
	pg_show_page(p);
	n72_trace("pg-show", 0);
	wp_apply_kick();
	if (!scr)
		return;
	pg.scr = NULL;
	pg.view = pg.strip_img = pg.strip_img2 = pg.bar_img = pg.bar_r_img = pg.time_img = pg.dots_img = pg.wp_img = NULL;
	if (pg.mode == PG_MODE_LAYER0)
		mbx_gpu_set_transparent(false);
	n72_trace("pg-opaque", 0);
	lv_screen_load(home_scr);
	n72_trace("pg-load", 0);
	lv_obj_delete(scr);
	n72_trace("pg-end<", 0);
}

/* ------------------------------------------------------------- settle */

/* UIScrollView's _prepareToPage...: the page to settle on. */
static int pg_decide(double v)
{
	double x = pg.off;
	int t;

	if (pg.off % PW == 0) {
		t = pg.off / PW;		/* aligned: stays */
	} else if (v > PG_FLICK) {
		/* on from where it is; a flick again during the settle: one more */
		t = (int)ceil((pg.dir > 0 ? (pg.target + 1) * (double)PW : x) / PW);
		pg.dir = 1;
	} else if (v < -PG_FLICK) {
		t = (int)floor((pg.dir < 0 ? (pg.target - 1) * (double)PW : x) / PW);
		pg.dir = -1;
	} else {
		t = (int)floor(x / PW + 0.5);
		pg.dir = 0;
	}
	return LV_CLAMP(0, t, pg.n - 1);
}

/*
 * At the page: this frame still shows it as the pictures, on time, and live
 * home takes over a frame later. Home's first frame is a full redraw
 * (~30 ms with a wallpaper) and the switch out of the transparent mode;
 * done in the same step as the last move, that move came ~65 ms late.
 * A frame late where nothing moves shows nothing.
 */
static lv_timer_t *pg_end_t;

static void pg_end_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	pg_end_t = NULL;	/* (one run: LVGL deletes it) */
	if (pg.scr && !pg.on && !pg.settling)
		pg_frames_end(pg.target);
}

static void pg_arrive(void)
{
	pg.settling = false;
	pg.dir = 0;
	lv_anim_delete(&pg, NULL);
	if (!pg_end_t) {
		pg_end_t = lv_timer_create(pg_end_cb, 20, NULL);
		lv_timer_set_repeat_count(pg_end_t, 1);
	}
}

/* (a touch while home waits to take over: now) */
static void pg_end_now(void)
{
	if (!pg_end_t)
		return;
	lv_timer_delete(pg_end_t);
	pg_end_t = NULL;
	if (pg.scr && !pg.on && !pg.settling)
		pg_frames_end(pg.target);
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
		pg_arrive();
		return;
	}
	pg_frame((int)pg.sp.x);
}

/* (a stall of seconds outlasted the animation: at the page now) */
static void pg_settle_late(lv_anim_t *a)
{
	LV_UNUSED(a);
	if (!pg.settling)
		return;
	pg_frame(pg.target * PW);
	pg.dir = 0;
	pg_frames_end(pg.target);
}

static void pg_settle_from(double v0);

static void pg_settle(double v)
{
	bool inside = pg.off >= 0 && pg.off <= pg_max_off();
	double v0;

	pg.target = pg_decide(pg.vel);
	if (inside && pg.vel * pg.vel < PG_STILL * PG_STILL)
		pg.vel = pg.prev_vel = 0;	/* (_endPanWithEvent) */
	v0 = pg.prev_vel != 0 ? 0.75 * pg.prev_vel + 0.25 * pg.vel : pg.vel;	/* (_startTimer) */
	LV_UNUSED(v);
	pg_settle_from(v0);
}

/* the settle to pg.target from pg.off at @v0 (px/ms) */
static void pg_settle_from(double v0)
{
	lv_anim_t a;

	ios_pager_init(&pg.sp, (float)PW);
	pg.sp.x = (float)pg.off;
	pg.sp.v = v0;
	pg.sp.target = (float)(pg.target * PW);
	if (pg.off == pg.target * PW) {
		pg_frames_end(pg.target);
		return;
	}
	pg.settling = true;
	pg.step_us = mono_us();
	lv_anim_delete(&pg, NULL);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &pg);
	lv_anim_set_exec_cb(&a, pg_settle_exec);
	/*
	 * Its value the ms gone: LVGL runs an animation's step only when the
	 * value changes (0..1 over the 3 s ran it twice, at the start and at
	 * the end: the strip stood still after the lift, the jump came 3 s
	 * later).
	 */
	lv_anim_set_values(&a, 0, 3000);
	lv_anim_set_duration(&a, 3000);		/* (ends itself well before) */
	lv_anim_set_completed_cb(&a, pg_settle_late);
	lv_anim_start(&a);
}

/*
 * To page @p as a swipe would settle there, from rest (an icon held at the
 * edge, dock.c). False: not now (a swipe on, home not showing).
 */
static bool pg_glide_to(int p)
{
	if (p < 0 || p >= pg.n || p == pg.cur || pg.scr || pg.on || lv_screen_active() != home_scr)
		return false;
	if (!pg_frames_begin())
		return false;
	pg.dir = 0;
	pg.target = p;
	pg.off = pg.cur * PW;
	pg_settle_from(0);
	return true;
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
bool pg_drag(int phase, int x, int y)
{
	if (pg.lost) {		/* (to the lift, then free) */
		if (phase == 2 || phase == 0)
			pg.lost = false;
		if (phase != 0)
			return true;
	}
	if (phase == 0) {
		pg_end_now();
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
		/* (one page too: it follows the finger at half its way and
		 * springs back, as a single-page UIScrollView that bounces) */
		if (lv_screen_active() != home_scr || tr.scr || za.phase ||
		    shade_open || sd.on || screen_off)
			return false;
		if (home_dock && in_obj(home_dock, x, y))
			return false;	/* (the dock stays: no swipe from it, as iOS) */
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
		if (!pg_frames_begin()) {
			/* no memory for the frames: no swipe, and not a tile's
			 * press either (lifting over one would launch it) */
			pg.lost = true;
			return true;
		}
		pg_take(x, pg.cur * PW);
		pg.tx0 = pg.x0 + (dx > 0 ? PG_SLOP : -PG_SLOP);	/* no jump by the slop */
		pg.dir = 0;	/* (a fresh drag: no settle to add to) */
	}
	if (phase == 2) {
		pg.on = false;
		/* caught, then dragged back against the settle: no page on from its target */
		if (pg.caught && (pg.off - pg.start_off) * pg.dir < 0)
			pg.dir = 0;
		if (mono_us() - pg.last_us > 80000)
			pg.vel = pg.prev_vel = 0;	/* held still before the lift */
		pg_settle(pg.vel);
		return true;
	}
	pg_sample(x);
	pg_frame(pg_track(x));
	if (pg_trace())
		fprintf(stderr, "P %.1f x %d off %d\n", mono_us() / 1000.0, x, pg.off);
	return true;
}

/* ------------------------------------------------------------ warming */

/*
 * While home is still and lit: the pictures ready before a swipe needs
 * them (taking them is a few tens of ms) and the wallpaper on layer 0.
 */
void pg_warm_kick(void)
{
	if (pg_warm_timer) {
		lv_timer_resume(pg_warm_timer);
		lv_timer_reset(pg_warm_timer);
	}
}

void pg_warm(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (app_pid > 0 || app_dormant || screen_off || locked || pg.scr ||
	    lv_screen_active() != home_scr || tr.scr || za.phase || sd.on ||
	    lv_anim_count_running() ||
	    (touch_indev && lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		return;
	n72_trace("pg-warm", 0);
	if (pg.pic_ver != home_ver || pg.pic_mode != pg_mode() || pg.pic_w != W) {
		/* (short of memory: not every second, again when home changes) */
		if (pg.fail_ver == home_ver && lv_tick_elaps(pg.fail_ms) < 30000)
			return;
		pg_pics();
	}
	if (pg.pic_ver == home_ver && pg.pic_mode == PG_MODE_LAYER0 && !pg.fb0_ok)
		pg_fb0_wallpaper();
	if (pg_warm_timer && pg.pic_ver == home_ver && pg.pic_mode == pg_mode() &&
	    pg.pic_w == W && (pg.pic_mode != PG_MODE_LAYER0 || pg.fb0_ok))
		lv_timer_pause(pg_warm_timer);
}

/* Home is being built again (a turn, a theme): its pictures go with it. */
void pg_home_gone(void)
{
	dock_carry_stop();	/* (a carried icon is put down first) */
	dock_fly_stop();	/* (its picture flies over the old home) */
	if (pg_warm_timer) {
		awake_timer_delete(pg_warm_timer);
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
void pg_abort(void)
{
	if (!pg.scr)
		return;
	pg.dir = 0;
	pg_frames_end(pg.settling ? pg.target : LV_CLAMP(0, (pg.off + PW / 2) / PW, pg.n - 1));
}
