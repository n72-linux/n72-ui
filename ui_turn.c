// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): turning. */
#include "ui_m3_priv.h"

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
int rot_side = 90;		/* the sideways it was last held: 90 or 270 */
static uint32_t rot_seen_ms;
static lv_timer_t *rot_timer;
int video_rot = -1;		/* the player's controls asked for this (-1: follow) */

bool app_is_video(void);
void vc_turned(void);

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
bool layout_stale;
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
		psl_stop(vol_hud);
		lv_obj_delete(vol_hud);
		vol_hud = NULL;
	}
	if (lock_scr && !locked) {
		lv_obj_t **lk[] = {
			&lock_scr, &lock_bar_time, &lock_bar_wifi, &lock_bar_bt, &lock_bar_batt, &lock_bar_np,
			&npl.card, &npl.title, &npl.sub, &npl.play_sym,
		};

		lv_obj_delete(lock_scr);
		for (i = 0; i < (int)(sizeof(lk) / sizeof(lk[0])); i++)
			*lk[i] = NULL;
	}
	{
		lv_obj_t **gone[] = {
			&app_bar_time, &app_bar_batt, &app_bar_wifi, &app_bar_bt,
			&clock_lbl, &date_lbl,
			&qs_wifi, &qs_bt, &qs_wifi_sub, &qs_bt_sub, &qs_rot, &qs_rot_sub,
			&home_bright, &qs_settings_btn, &vol_hud_slider, &vol_hud_icon,
			&np.card, &np.title, &np.sub, &np.play_sym, &shade_bright_row,
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
void ui_rebuild(bool load)
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
			&status_time, &status_wifi, &status_bt, &batt_lbl, &status_np,
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

void home_fresh(void)
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
			&status_time, &status_wifi, &status_bt, &batt_lbl, &status_np,
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
void page_fresh(int i)
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
bool ui_turn(int rot)
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
void rot_motion_off(void)
{
	if (rot_on)
		rot_motion(false);
}

/* Something that changes what a turn needs: look now (the timer may rest). */
void rot_kick(void)
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
	/* the accelerometer only for Auto-rotate (outside fixed-layout apps) and video */
	bool reads = app_pid <= 0 ? conf.autorot == ROT_AUTO : app_is_video();
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

void rot_conf_changed(void)
{
	rot_seen = conf.autorot == ROT_AUTO ? rot_read() : -1;
	if (rot_seen > 0)
		rot_side = rot_seen;
	rot_seen_ms = 0;
	rot_kick();
}

void rot_init(void)
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
