/*
 * The dock (included by ui_m3.c after paging.c): a card of the clock's
 * shape along the bottom of home, holding up to four icons without names,
 * still while the pages above it swipe. Turned, it stays where it is in
 * the hand: a column on the side that is the bottom, its icons upright
 * (paging.c's home_geom()).
 *
 * The icons are the tiles themselves, moved into the dock object: a tap
 * opens them as from the grid, and the zoom grows from the dock. A long
 * press picks a tile up to carry it anywhere in the grid, onto another
 * page, or into or out of the dock (see "the carrying" below).
 *
 * What is docked is kept as names in ui.conf ("dock=Settings|Files|...");
 * with no such line, the dock holds Settings, Files, YouTube and Browser.
 */

#define DOCK_MAX	4
#define DOCK_ICON	60
#define DOCK_FLY_MS	300	/* iOS's 0.3 s */
#define DOCK_DEFAULT	"Settings|Files|YouTube|Browser"

static lv_obj_t *dock_order[128];	/* every tile, in the grid's order */
static int dock_norder;
static struct {
	lv_obj_t *img, *tile;
	lv_draw_buf_t *pic;
	int32_t x0, y0, x1, y1;
} dock_fly;

/* a tile's name (app_tile() keeps it: the label's text may read "...") */
static const char *tile_name(lv_obj_t *t)
{
	const char *n = lv_obj_get_user_data(t);

	return n ? n : "";
}

static bool tile_docked(lv_obj_t *t)
{
	return home_dock && lv_obj_get_parent(t) == home_dock;
}

/* a tile as an icon of the dock (no name) or of the grid */
static void dock_tile_style(lv_obj_t *t, bool docked)
{
	lv_obj_t *l = lv_obj_get_child(t, -1);

	if (l && lv_obj_check_type(l, &lv_label_class)) {
		if (docked)
			lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
	}
	if (docked)
		lv_obj_set_size(t, DOCK_ICON, DOCK_ICON);
	else
		lv_obj_set_size(t, PG_TILE_W, PG_TILE_H);
}

/* the docked icons spread evenly along the dock */
static void dock_layout(void)
{
	int n, i, len, gap;
	bool side;

	if (!home_dock)
		return;
	n = (int)lv_obj_get_child_count(home_dock);
	side = dock_side() != DOCK_BOTTOM;
	len = side ? H - 2 * STATUS_PAD : W - 2 * STATUS_PAD;	/* (as dock_build() made it: no layout yet) */
	gap = n ? (len - n * DOCK_ICON) / (n + 1) : 0;
	for (i = 0; i < n; i++) {
		lv_obj_t *t = lv_obj_get_child(home_dock, i);
		int32_t along = gap + i * (DOCK_ICON + gap), across = (DOCK_T - DOCK_ICON) / 2;

		if (side)
			lv_obj_set_pos(t, across, along);
		else
			lv_obj_set_pos(t, along, across);
	}
}

/* every tile back into the grid, in its order (before the grid is rebuilt) */
static void dock_undock_all(void)
{
	int i;

	for (i = 0; i < dock_norder; i++) {
		lv_obj_t *t = dock_order[i];

		if (!lv_obj_is_valid(t))
			continue;
		if (tile_docked(t)) {
			lv_obj_set_parent(t, home_apps);
			dock_tile_style(t, false);
		}
		lv_obj_move_to_index(t, i + 1);	/* (child 0 is the card) */
	}
}

static void dock_long_cb(lv_event_t *e);

/* the grid in the order ui.conf keeps (names not there stay after, as they were) */
static void dock_order_apply(void)
{
	char names[sizeof(conf.order)], *save = NULL, *n;
	uint32_t k = 1, i, cnt;

	if (!conf.order_set)
		return;
	snprintf(names, sizeof(names), "%s", conf.order);
	for (n = strtok_r(names, "|", &save); n; n = strtok_r(NULL, "|", &save)) {
		cnt = lv_obj_get_child_count(home_apps);
		for (i = k; i < cnt; i++)
			if (!strcmp(tile_name(lv_obj_get_child(home_apps, i)), n)) {
				lv_obj_move_to_index(lv_obj_get_child(home_apps, i), (int32_t)k++);
				break;
			}
	}
}

/* the grid's tiles as they are now: their order, and the long press */
static void dock_note_order(void)
{
	int i, n = (int)lv_obj_get_child_count(home_apps) - 1;

	dock_norder = 0;
	for (i = 0; i < n && dock_norder < (int)(sizeof(dock_order) / sizeof(dock_order[0])); i++) {
		lv_obj_t *t = lv_obj_get_child(home_apps, i + 1);

		dock_order[dock_norder++] = t;
		if (!lv_obj_has_flag(t, LV_OBJ_FLAG_USER_2)) {
			lv_obj_add_flag(t, LV_OBJ_FLAG_USER_2);	/* (once) */
			lv_obj_add_event_cb(t, dock_long_cb, LV_EVENT_LONG_PRESSED, NULL);
		}
	}
}

/* into the dock what ui.conf names, in its order */
static void dock_apply(void)
{
	char names[sizeof(conf.dock)], *save = NULL, *n;
	int i;

	if (!home_dock)
		return;
	dock_undock_all();
	snprintf(names, sizeof(names), "%s", conf.dock_set ? conf.dock : DOCK_DEFAULT);
	for (n = strtok_r(names, "|", &save); n && lv_obj_get_child_count(home_dock) < DOCK_MAX;
	     n = strtok_r(NULL, "|", &save))
		for (i = 0; i < dock_norder; i++) {
			lv_obj_t *t = dock_order[i];

			if (lv_obj_is_valid(t) && !tile_docked(t) && !strcmp(tile_name(t), n)) {
				lv_obj_set_parent(t, home_dock);
				dock_tile_style(t, true);
				break;
			}
		}
	if (getenv("N72_FPS")) {
		fprintf(stderr, "dock: \"%s\" (%s), %d tiles:", conf.dock_set ? conf.dock : DOCK_DEFAULT,
			conf.dock_set ? "ui.conf" : "default", dock_norder);
		for (i = 0; i < dock_norder && i < 8; i++)
			fprintf(stderr, " [%s]", tile_name(dock_order[i]));
		fprintf(stderr, "; docked %u\n", lv_obj_get_child_count(home_dock));
	}
	dock_layout();
	pg_layout();
}

/* what the dock holds now, into ui.conf */
static void dock_save(void)
{
	uint32_t i, n = lv_obj_get_child_count(home_dock);
	size_t len = 0;

	conf.dock[0] = 0;
	for (i = 0; i < n && len < sizeof(conf.dock); i++)
		len += snprintf(conf.dock + len, sizeof(conf.dock) - len, "%s%s",
				i ? "|" : "", tile_name(lv_obj_get_child(home_dock, i)));
	conf.dock_set = true;
	conf_save_later();
}

/* The dock and its icons, on a home being built (after its tiles). */
static void dock_build(void)
{
	lv_obj_t *d = home_dock = lv_obj_create(home_scr);
	int side = dock_side();

	plain(d);
	if (side == DOCK_BOTTOM) {
		lv_obj_set_size(d, W - 2 * STATUS_PAD, DOCK_T);
		lv_obj_set_pos(d, STATUS_PAD, H - DOCK_M - DOCK_T);
	} else {
		/* the same place in the hand: a column at the side */
		lv_obj_set_size(d, DOCK_T, H - 2 * STATUS_PAD);
		lv_obj_set_pos(d, side == DOCK_LEFT ? DOCK_M : W - DOCK_M - DOCK_T, STATUS_PAD);
	}
	/* the card's shape and colour */
	lv_obj_set_style_radius(d, 24, 0);
	lv_obj_set_style_bg_color(d, M.surface_c_high, 0);
	lv_obj_set_style_bg_opa(d, home_wp ? LV_OPA_80 : LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(d, 0, 0);
	lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	dock_order_apply();
	dock_note_order();
	dock_apply();
}

/* The grid's manifests change (apps_reload): every tile back first. */
static void dock_before_reload(void)
{
	if (home_dock)
		dock_undock_all();
}

static void dock_after_reload(void)
{
	if (!home_dock)
		return;
	dock_order_apply();
	dock_note_order();
	dock_apply();
}

/* ------------------------------------------------------------ the moves */

static void dock_fly_x(void *var, int32_t v)
{
	LV_UNUSED(var);
	if (dock_fly.img)
		lv_obj_set_pos(dock_fly.img, dock_fly.x0 + (dock_fly.x1 - dock_fly.x0) * v / 1024,
			       dock_fly.y0 + (dock_fly.y1 - dock_fly.y0) * v / 1024);
}

static void dock_fly_end(void)
{
	if (dock_fly.img) {
		lv_obj_delete(dock_fly.img);
		dock_fly.img = NULL;
	}
	tr_free(&dock_fly.pic);
	if (dock_fly.tile && lv_obj_is_valid(dock_fly.tile))
		lv_obj_remove_flag(dock_fly.tile, LV_OBJ_FLAG_HIDDEN);
	dock_fly.tile = NULL;
}

static void dock_fly_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	dock_fly_end();
}

/* (a flight cut short: home rebuilt, a swipe, the screen off) */
static void dock_fly_stop(void)
{
	if (!dock_fly.img && !dock_fly.tile)
		return;
	lv_anim_delete(&dock_fly, dock_fly_x);
	dock_fly_end();
}

static void dock_badge_xy(lv_obj_t *t, int32_t *x, int32_t *y)
{
	lv_area_t a;

	lv_obj_update_layout(t);
	lv_obj_get_coords(lv_obj_get_child(t, 0), &a);
	*x = a.x1;
	*y = a.y1;
}

/* a full dock: a short shake, nothing moves in */
static void dock_shake_x(void *var, int32_t v)
{
	/* 3 swings, fading: v is 0..1024 of the time */
	float t = v / 1024.0f;

	int32_t d = (int32_t)lroundf(8.0f * sinf(t * 6.0f * (float)M_PI) * (1.0f - t));

	/* (along the dock: across the screen upright, up and down turned) */
	if (dock_side() == DOCK_BOTTOM)
		lv_obj_set_style_translate_x(var, d, 0);
	else
		lv_obj_set_style_translate_y(var, d, 0);
}

static void dock_shake(void)
{
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, home_dock);
	lv_anim_set_exec_cb(&a, dock_shake_x);
	lv_anim_set_values(&a, 0, 1024);
	lv_anim_set_duration(&a, 360);
	lv_anim_start(&a);
}

/* ------------------------------------------------------- the carrying */

/*
 * A long press picks a tile up: a picture of its badge follows the finger
 * (one small GPU copy a frame) while the tile itself, hidden, holds the
 * place it will land in. Over the grid that place moves to the slot under
 * the finger, over the dock into the dock (while it has room: a full one
 * shakes once), and the tiles between slide over (0.2 s, only those that
 * show). Held at a side of the pages for 0.6 s, the next page glides in.
 * On the lift the picture settles into the place; the order of the grid
 * and the dock goes into ui.conf ("order=", "dock="). The touch is the
 * router's from the long press on (ui_drag): no swipe, no shade, no tap.
 */
#define CARRY_SLIDE_MS	200
#define CARRY_EDGE	20	/* px from a side of the pages */
#define CARRY_EDGE_MS	600

static struct {
	bool on;
	lv_obj_t *tile, *img;
	lv_draw_buf_t *pic;
	int32_t dx, dy;		/* the picture's corner from the finger */
	int32_t x, y;		/* the finger */
	int edge;		/* -1, +1: held at a side; 0 not */
	uint32_t edge_ms;
	bool shook;		/* (the full dock shook for this pass over it) */
	lv_timer_t *timer;
} carry;

static void carry_slide_x(void *o, int32_t v)
{
	lv_obj_set_x(o, v);
}

static void carry_slide_y(void *o, int32_t v)
{
	lv_obj_set_y(o, v);
}

/* @o from where it was (@x0, @y0 in its parent) to where it is now */
static void carry_slide(lv_obj_t *o, int32_t x0, int32_t y0)
{
	int32_t x1 = lv_obj_get_x(o), y1 = lv_obj_get_y(o);
	lv_anim_t a;

	if (x0 == x1 && y0 == y1)
		return;
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_duration(&a, CARRY_SLIDE_MS);
	lv_anim_set_path_cb(&a, ios_path_ease_out);
	lv_anim_set_exec_cb(&a, carry_slide_x);
	lv_anim_set_values(&a, x0, x1);
	lv_anim_start(&a);
	lv_anim_set_exec_cb(&a, carry_slide_y);
	lv_anim_set_values(&a, y0, y1);
	lv_anim_start(&a);
	lv_obj_set_pos(o, x0, y0);
}

/* the carried tile to @idx of the grid or of the dock, the others sliding over */
static void carry_place(bool in_dock, int idx)
{
	lv_obj_t *t = carry.tile, *seen[48];
	int32_t ox[48], oy[48];
	lv_obj_t *par = in_dock ? home_dock : home_apps;
	int n = 0, i, first, last;

	/* where the tiles that show are now: those of this page, and the dock's */
	first = 1;
	last = (int)lv_obj_get_child_count(home_apps) - 1;
	for (i = first; i <= last && n < 40; i++) {
		lv_obj_t *o = lv_obj_get_child(home_apps, i);
		int32_t x = lv_obj_get_x(o);

		if (o != t && x >= pg.cur * PW - PG_TILE_W && x < (pg.cur + 1) * PW) {
			seen[n] = o;
			ox[n] = x;
			oy[n++] = lv_obj_get_y(o);
		}
	}
	for (i = 0; i < (int)lv_obj_get_child_count(home_dock) && n < 48; i++) {
		lv_obj_t *o = lv_obj_get_child(home_dock, i);

		if (o != t) {
			seen[n] = o;
			ox[n] = lv_obj_get_x(o);
			oy[n++] = lv_obj_get_y(o);
		}
	}
	if (lv_obj_get_parent(t) != par) {
		lv_obj_set_parent(t, par);
		dock_tile_style(t, in_dock);
	}
	lv_obj_move_to_index(t, in_dock ? idx : idx + 1);	/* (the card first in the grid) */
	dock_layout();
	pg_layout();
	for (i = 0; i < n; i++)
		carry_slide(seen[i], ox[i], oy[i]);
	bp_changed(home_scr);
}

/* where the finger at (@x, @y) would put the tile: grid or dock, and the index */
static bool carry_target(int32_t x, int32_t y, bool *in_dock, int *idx)
{
	bool docked = tile_docked(carry.tile);

	if (home_dock && in_obj(home_dock, x, y)) {
		int n = (int)lv_obj_get_child_count(home_dock) + (docked ? 0 : 1), along, len;
		lv_area_t d;

		if (!docked && n > DOCK_MAX) {
			if (!carry.shook) {
				dock_shake();
				carry.shook = true;
			}
			return false;
		}
		lv_obj_get_coords(home_dock, &d);
		along = dock_side() == DOCK_BOTTOM ? x - d.x1 : y - d.y1;
		len = dock_side() == DOCK_BOTTOM ? lv_area_get_width(&d) : lv_area_get_height(&d);
		*in_dock = true;
		*idx = LV_CLAMP(0, along * n / LV_MAX(len, 1), n - 1);
		return true;
	}
	carry.shook = false;
	if (x < PX || x >= PX + PW || y < PG_TOP || y >= PB - PG_DOTS_H)
		return false;
	{
		int top = pg.cur ? PG_GRIDN : PG_GRID0, rows = pg.cur ? pg.rowsn : pg.rows0;
		int x0 = (PW - pg.cols * PG_TILE_W) / 2, gx = x - PX - x0, gy = y - PG_TOP - top;
		int col = LV_CLAMP(0, gx / PG_TILE_W, pg.cols - 1);
		int row = LV_CLAMP(0, gy / (PG_TILE_H + PG_ROW_GAP), rows - 1);
		int slot = row * pg.cols + col;
		int per0 = pg.cols * pg.rows0, pern = pg.cols * pg.rowsn;
		int count = (int)lv_obj_get_child_count(home_apps) - 1 + (docked ? 1 : 0);

		if (gy < -PG_ROW_GAP)
			return false;	/* (over the card) */
		*in_dock = false;
		*idx = LV_CLAMP(0, pg.cur ? per0 + (pg.cur - 1) * pern + slot : slot, count - 1);
	}
	return true;
}

static void carry_follow(void)
{
	bool in_dock;
	int idx;

	if (carry.img)
		lv_obj_set_pos(carry.img, carry.x + carry.dx, carry.y + carry.dy);
	if (pg.scr)
		return;		/* (a page gliding in: the place is found after) */
	if (!carry_target(carry.x, carry.y, &in_dock, &idx))
		return;
	if (in_dock == tile_docked(carry.tile) &&
	    idx == (int)lv_obj_get_index(carry.tile) - (in_dock ? 0 : 1))
		return;
	carry_place(in_dock, idx);
}

/* held at a side: the next page */
static void carry_tick(lv_timer_t *t)
{
	int e;

	LV_UNUSED(t);
	if (!carry.on)
		return;
	if (pg.scr) {
		carry.edge_ms = lv_tick_get();	/* (the next turn counts from the glide's end) */
		return;
	}
	carry_follow();		/* (a finger held still after a glide: its place on this page) */
	e = carry.x < PX + CARRY_EDGE ? -1 : carry.x >= PX + PW - CARRY_EDGE ? 1 : 0;
	if (carry.y < PG_TOP || carry.y >= PB)
		e = 0;
	if (e != carry.edge) {
		carry.edge = e;
		carry.edge_ms = lv_tick_get();
		return;
	}
	if (e && lv_tick_elaps(carry.edge_ms) >= CARRY_EDGE_MS && pg_glide_to(pg.cur + e))
		carry.edge_ms = lv_tick_get();
}

/* the grid's and the dock's order, kept (ui.conf) */
static void carry_save(void)
{
	size_t len = 0;
	uint32_t i, n;

	conf.order[0] = 0;
	n = lv_obj_get_child_count(home_apps);
	for (i = 1; i < n && len < sizeof(conf.order); i++)
		len += snprintf(conf.order + len, sizeof(conf.order) - len, "%s%s", len ? "|" : "",
				tile_name(lv_obj_get_child(home_apps, i)));
	conf.order_set = true;
	dock_save();	/* (and conf_save_later) */
	/* the order every rebuild comes back to (dock_undock_all) */
	dock_norder = 0;
	for (i = 1; i < n && dock_norder < (int)(sizeof(dock_order) / sizeof(dock_order[0])); i++)
		dock_order[dock_norder++] = lv_obj_get_child(home_apps, i);
	n = lv_obj_get_child_count(home_dock);
	for (i = 0; i < n && dock_norder < (int)(sizeof(dock_order) / sizeof(dock_order[0])); i++)
		dock_order[dock_norder++] = lv_obj_get_child(home_dock, i);
}

static void carry_end(bool drop)
{
	lv_obj_t *t = carry.tile;

	carry.on = false;
	if (carry.timer) {
		lv_timer_delete(carry.timer);
		carry.timer = NULL;
	}
	if (!t || !lv_obj_is_valid(t)) {
		if (carry.img)
			lv_obj_delete(carry.img);
		tr_free(&carry.pic);
		carry.img = NULL;
		carry.tile = NULL;
		return;
	}
	if (drop)
		carry_save();
	/* the picture settles into the place (dock_fly's flight, from here) */
	dock_fly_stop();
	dock_fly.img = carry.img;
	dock_fly.pic = carry.pic;
	dock_fly.tile = t;
	dock_fly.x0 = lv_obj_get_x(carry.img);
	dock_fly.y0 = lv_obj_get_y(carry.img);
	dock_badge_xy(t, &dock_fly.x1, &dock_fly.y1);
	carry.img = NULL;
	carry.pic = NULL;
	carry.tile = NULL;
	{
		lv_anim_t a;

		lv_anim_init(&a);
		lv_anim_set_var(&a, &dock_fly);
		lv_anim_set_exec_cb(&a, dock_fly_x);
		lv_anim_set_values(&a, 0, 1024);
		lv_anim_set_duration(&a, CARRY_SLIDE_MS);
		lv_anim_set_path_cb(&a, ios_path_ease_out);
		lv_anim_set_completed_cb(&a, dock_fly_done);
		lv_anim_start(&a);
	}
}

/* The router's touch while a tile is carried (ui_drag): true, it is ours. */
static bool dock_carry_touch(int phase, int x, int y)
{
	if (!carry.on)
		return false;
	carry.x = x;
	carry.y = y;
	if (phase == 2) {
		carry_follow();	/* (the place under the lift, if a glide skipped the last move) */
		carry_end(true);
	}
	else
		carry_follow();
	return true;
}

/* (home rebuilt, the screen off, an application: put it down where it is) */
static void dock_carry_stop(void)
{
	if (carry.on)
		carry_end(true);
}

static void dock_long_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_current_target(e), *badge = lv_obj_get_child(t, 0);
	lv_indev_t *in = lv_indev_active();
	lv_point_t p;
	int32_t bx, by;

	if (!home_dock || carry.on || za.phase || pg.on || pg.scr || lv_screen_active() != home_scr || !in)
		return;
	dock_fly_stop();
	lv_indev_wait_release(in);	/* (no tap on the lift) */
	lv_obj_remove_state(t, LV_STATE_PRESSED);
	lv_indev_get_point(in, &p);
	dock_badge_xy(t, &bx, &by);
	carry.pic = badge ? snap(badge, LV_COLOR_FORMAT_ARGB8888) : NULL;
	if (!carry.pic)
		return;
	carry.tile = t;
	carry.x = p.x;
	carry.y = p.y;
	carry.dx = bx - p.x;
	carry.dy = by - p.y;
	carry.edge = 0;
	carry.shook = false;
	carry.img = lv_image_create(lv_layer_top());
	lv_image_set_src(carry.img, carry.pic);
	lv_obj_remove_flag(carry.img, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(carry.img, bx, by);
	lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);	/* (it holds its place) */
	carry.on = true;
	carry.timer = lv_timer_create(carry_tick, 100, NULL);
}
