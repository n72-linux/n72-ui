/*
 * The dock (included by ui_m3.c after paging.c): a card of the clock's
 * shape along the bottom of home, holding up to four icons without names,
 * still while the pages above it swipe. Turned, it stays where it is in
 * the hand: a column on the side that is the bottom, its icons upright
 * (paging.c's home_geom()).
 *
 * The icons are the tiles themselves, moved into the dock object: a tap
 * opens them as from the grid, and the zoom grows from the dock. A long
 * press moves a tile into the dock or back into its place in the grid: it
 * flies there as a picture of its badge (one small GPU copy a frame), the
 * grid takes its new order at once. A full dock shakes instead.
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
	for (i = 0; i < n; i++)
		len += snprintf(conf.dock + len, sizeof(conf.dock) - LV_MIN(len, sizeof(conf.dock)), "%s%s",
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

/* @t into the dock (or out of it), flying there */
static void dock_move(lv_obj_t *t, bool in)
{
	lv_obj_t *badge = lv_obj_get_child(t, 0);
	lv_anim_t a;
	int i, k;

	dock_fly_stop();
	dock_badge_xy(t, &dock_fly.x0, &dock_fly.y0);
	/* the badge (a picture: one small GPU copy a frame; the tile's press
	 * scale is the tile's, not in it) */
	lv_obj_remove_state(t, LV_STATE_PRESSED);
	dock_fly.pic = badge ? snap(badge, LV_COLOR_FORMAT_ARGB8888) : NULL;
	if (in) {
		lv_obj_set_parent(t, home_dock);
		dock_tile_style(t, true);
	} else {
		/* back where it was in the grid's order */
		for (i = 0, k = 0; i < dock_norder && dock_order[i] != t; i++)
			if (lv_obj_is_valid(dock_order[i]) && !tile_docked(dock_order[i]))
				k++;
		lv_obj_set_parent(t, home_apps);
		dock_tile_style(t, false);
		lv_obj_move_to_index(t, k + 1);
	}
	dock_layout();
	pg_layout();
	bp_changed(home_scr);
	dock_save();
	if (!dock_fly.pic)
		return;
	dock_badge_xy(t, &dock_fly.x1, &dock_fly.y1);
	dock_fly.tile = t;
	lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
	dock_fly.img = lv_image_create(lv_layer_top());
	lv_image_set_src(dock_fly.img, dock_fly.pic);
	lv_obj_remove_flag(dock_fly.img, LV_OBJ_FLAG_CLICKABLE);
	dock_fly_x(NULL, 0);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &dock_fly);
	lv_anim_set_exec_cb(&a, dock_fly_x);
	lv_anim_set_values(&a, 0, 1024);
	lv_anim_set_duration(&a, DOCK_FLY_MS);
	lv_anim_set_path_cb(&a, ios_path_ease_in_out);
	lv_anim_set_completed_cb(&a, dock_fly_done);
	lv_anim_start(&a);
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

static void dock_long_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_current_target(e);

	if (!home_dock || za.phase || pg.on || pg.scr || lv_screen_active() != home_scr)
		return;
	lv_indev_wait_release(lv_indev_active());	/* (no tap on the lift) */
	lv_obj_remove_state(t, LV_STATE_PRESSED);
	if (!tile_docked(t) && lv_obj_get_child_count(home_dock) >= DOCK_MAX) {
		dock_shake();
		return;
	}
	dock_move(t, !tile_docked(t));
}
