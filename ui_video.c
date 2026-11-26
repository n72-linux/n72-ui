// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): video controls. */
#include "ui_m3_priv.h"

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
#define VC_YT_CACHE "/tmp/n72-yt"
#define VC_YT_OFFLINE "/root/.config/n72-yt/offline"

struct vc_art_img {
	lv_image_dsc_t dsc;
	uint8_t *data;
};

static struct {
	bool audio_only;
	char yt_id[16];
	char artist[96];
	char title[128];
} vc_next;

/* the player in the background (bg_start, below) */
static struct {
	pid_t pid;
	int pidfd;
	char *cmd;
	bool ending;
	bool video;		/* it has pictures to come back to */
	lv_timer_t *kill, *poll;
} bgp = { .pidfd = -1 };

static struct {
	lv_obj_t *root, *ui, *title, *sub, *art, *art_ph, *play_sym, *slider, *pos, *dur, *rot_lbl, *rot_btn;
	lv_timer_t *poll, *hide;
	int sock;
	bool up;			/* the player has answered */
	bool shown, playing, dragging, audio_only;
	int64_t pos_ms, dur_ms;
	int prot;			/* the player's turn */
	uint32_t quiet_ms;		/* after a command: the state may lag */
	char name[128];
	char yt_id[16];
	char artist[96];
	struct vc_art_img *art_img;
	int art_w, art_h;
} vc = { .sock = -1, .prot = -1 };

static bool vc_yt_id_ok(const char *id)
{
	size_t n;

	if (!id)
		return false;
	n = strlen(id);
	if (n < 6 || n > 15)
		return false;
	for (; *id; id++)
		if (!isalnum((unsigned char)*id) && *id != '-' && *id != '_')
			return false;
	return true;
}

void vc_set_audio_meta(const char *yt_id, const char *artist, const char *title)
{
	memset(&vc_next, 0, sizeof(vc_next));
	if (!vc_yt_id_ok(yt_id))
		return;
	vc_next.audio_only = true;
	snprintf(vc_next.yt_id, sizeof(vc_next.yt_id), "%s", yt_id);
	snprintf(vc_next.artist, sizeof(vc_next.artist), "%s", artist ? artist : "");
	snprintf(vc_next.title, sizeof(vc_next.title), "%s", title ? title : "");
}

bool cmd_is_video(const char *cmd)
{
	return cmd && (!strncmp(cmd, "n72-video", 9) || !strncmp(cmd, "n72-player", 10));
}

bool app_is_video(void)
{
	return app_pid > 0 && cmd_is_video(app_cmd);
}

/*
 * A game (ui_games.c): RetroArch started through n72-retro-launch, in
 * landscape on the side game_rot (90 or 270), which the display turns for
 * it; the launcher's layer turns the same way while it plays.
 */
int game_rot = 90;

bool cmd_is_game(const char *cmd)
{
	return cmd && strstr(cmd, "n72-retro-launch") != NULL;
}

bool app_is_game(void)
{
	return app_pid > 0 && cmd_is_game(app_cmd);
}

/* A game in front: the screen stays on (main.c, idle_check). */
bool game_playing(void)
{
	return app_is_game() && lv_screen_active() == app_scr && !gp_up();	/* (paused: not) */
}

/* A video playing in front: the screen stays on (main.c, idle_check). */
bool video_playing(void)
{
	return app_is_video() && vc.up && vc.playing && !vc.audio_only &&
	       lv_screen_active() == app_scr;
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
	if (!vc.audio_only && vc.playing && !vc.dragging)
		vc_show(false);
}

/* Playing and shown: hide in a while (again from now). */
static void vc_hide_later(void)
{
	if (vc.hide) {
		lv_timer_delete(vc.hide);
		vc.hide = NULL;
	}
	if (vc.audio_only || !vc.shown || !vc.playing || vc.dragging)
		return;
	vc.hide = lv_timer_create(vc_hide_cb, VC_HIDE_MS, NULL);
	lv_timer_set_repeat_count(vc.hide, 1);
}

static void vc_opa_exec(void *o, int32_t v)
{
	lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

bool vc_is_visible(void)
{
	return vc.shown || (vc.ui && !lv_obj_has_flag(vc.ui, LV_OBJ_FLAG_HIDDEN));
}

static void vc_fade_done(lv_anim_t *a)
{
	if (!vc.shown)
		lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_local_style_prop(a->var, LV_STYLE_OPA, 0);	/* no layer at rest */
	if (!vc.shown)
		app_dormant_sync();
}

static void vc_show(bool on)
{
	lv_anim_t a;

	if (!vc.ui)
		return;
	if (vc.audio_only) {
		vc.shown = true;
		lv_obj_remove_flag(vc.ui, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	if (on && app_dormant)
		app_dormant_wake(false);
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
	if (vc.sub)
		label_set_if_changed(vc.sub, vc.artist);
	if (!vc.dragging) {
		vc_fmt(b, sizeof(b), vc.pos_ms);
		label_set_if_changed(vc.pos, b);
		if (vc.dur_ms > 0) {
			lv_slider_set_range(vc.slider, 0, (int32_t)(vc.dur_ms / 100));
			lv_slider_set_value(vc.slider, (int32_t)(vc.pos_ms / 100), LV_ANIM_OFF);
		}
	}
	vc_fmt(b, sizeof(b), vc.dur_ms);
	label_set_if_changed(vc.dur, vc.dur_ms > 0 ? b : (vc.audio_only && !vc.up) ? "Loading..." : "");
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
		int bgf = 0, vid = 1;

		buf[n] = 0;
		if (sscanf(buf, "%15s %lld %lld %d %d %d", st, &pos, &dur, &r, &bgf, &vid) < 4)
			continue;
		if (bgp.pid > 0 && app_pid < 0)
			continue;	/* (the ending background player's, not the next one's) */
		if (bgp.pid > 0 && app_pid == 0) {
			/* in the background: the shade's card follows it */
			const char *nl = strchr(buf, '\n');

			if (nl && nl[1] && strcmp(vc.name, nl + 1))
				snprintf(vc.name, sizeof(vc.name), "%s", nl + 1);
			bgp.video = vid != 0;
			if (lv_tick_elaps(vc.quiet_ms) >= 700) {
				vc.playing = !strcmp(st, "playing");
				vc.pos_ms = pos;
				vc.dur_ms = dur;
			}
			np_sync();
			continue;
		}
		if (!vc.up) {
			const char *nl = strchr(buf, '\n');

			vc.up = true;
			if (nl && nl[1])
				snprintf(vc.name, sizeof(vc.name), "%s", nl + 1);
			vc.playing = !strcmp(st, "playing");
			vc_build();
			vc_show(true);		/* a moment at the start */
		}
		{
			/* the title from every answer (a stray one goes right) */
			const char *nl = strchr(buf, '\n');

			if (nl && nl[1] && strcmp(vc.name, nl + 1))
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
	if (locked)
		np_lock_sync();	/* (the player in front, under the lock) */
	if (app_pid == 0)
		return;		/* (no controls over it: the background's) */
	if (vc.up && vc.prot != ui_rot) {
		snprintf(buf, sizeof(buf), "rot %d", ui_rot);
		vc_send(buf);
		vc.prot = ui_rot;
	}
	vc_update();
}

static void vc_poll(lv_timer_t *t)
{
	uint32_t period = bgp.pid > 0 && app_pid == 0 ? (shade_open ? 500 : 2000) :
			  vc.audio_only ? (!vc.up || vc.dragging ? 250 : 500) :
			  !vc.up || vc.shown || vc.dragging ? 250 : 2000;

	if (bgp.pid > 0 && app_pid == 0) {
		if (t->period != period)
			lv_timer_set_period(t, period);
		vc_send("status");
		return;
	}

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
	if (!vc.audio_only)
		vc_show(!vc.shown);
}

static void vc_play_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!vc.up)
		return;
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

	if (!vc.up)
		return;
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

	if (!vc.up || vc.dur_ms <= 0)
		return;
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
	app_end_ex(false);	/* back to where it was started (Files); Back stops it */
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

/* A round icon button: transparent, a state layer when pressed. */
static lv_obj_t *vc_icon_btn(lv_obj_t *parent, const char *sym, int32_t size,
			     const lv_font_t *font, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(parent), *l;
	lv_color_t fg = vc.audio_only ? M.on_surface : lv_color_white();

	plain(b);
	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(b, fg, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(b, vc.audio_only ? LV_OPA_10 : LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(b, 0, 0);
	l = m3_label(b, sym, font, fg);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

static lv_obj_t *vc_skip_btn(lv_obj_t *parent, int32_t size, int delta, lv_color_t fg)
{
	lv_obj_t *b = vc_icon_btn(parent, "", size, &lv_font_montserrat_20, vc_skip_cb,
				  (void *)(intptr_t)delta);
	lv_obj_t *col = lv_obj_create(b);

	lv_obj_delete(lv_obj_get_child(b, 0));
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 0, 0);
	lv_obj_center(col);
	m3_label(col, delta > 0 ? LV_SYMBOL_NEXT : LV_SYMBOL_PREV, &lv_font_montserrat_20, fg);
	m3_label(col, "10", &lv_font_montserrat_12, fg);
	return b;
}

static void vc_play_fab(lv_obj_t *parent, int32_t size, int32_t radius)
{
	lv_obj_t *b = lv_button_create(parent);

	plain(b);
	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, radius, 0);
	lv_obj_set_style_bg_color(b, M.primary_c, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_transform_scale(b, 240, LV_STATE_PRESSED);
	vc.play_sym = m3_label(b, vc.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
			       size >= 72 ? &lv_font_montserrat_28 : &lv_font_montserrat_24,
			       M.on_primary_c);
	lv_obj_center(vc.play_sym);
	lv_obj_add_event_cb(b, vc_play_cb, LV_EVENT_CLICKED, NULL);
}

static void vc_slider_row(lv_obj_t *parent, int32_t h, int32_t y_ofs, bool light_ui)
{
	lv_obj_t *row = lv_obj_create(parent), *o;

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, W - 2 * STATUS_PAD, h);
	lv_obj_set_style_pad_hor(row, 8, 0);
	lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, y_ofs);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
	vc.pos = m3_label(row, "0:00", &lv_font_montserrat_12,
			  light_ui ? M.on_surface : lv_color_white());
	lv_obj_align(vc.pos, LV_ALIGN_TOP_LEFT, 0, 4);
	vc.dur = m3_label(row, "", &lv_font_montserrat_12,
			  light_ui ? M.on_surface_v : lv_color_hex(0xd0d0d0));
	lv_obj_align(vc.dur, LV_ALIGN_TOP_RIGHT, 0, 4);
	o = vc.slider = lv_slider_create(row);
	lv_obj_set_size(o, lv_pct(100), 4);
	lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -18);
	lv_obj_set_style_bg_color(o, light_ui ? M.surface_c_highest : lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(o, light_ui ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
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
}

static void vc_art_free_buf(void)
{
	if (!vc.art_img)
		return;
	lv_image_cache_drop(&vc.art_img->dsc);
	free(vc.art_img->data);
	free(vc.art_img);
	vc.art_img = NULL;
}

static void vc_art_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	vc_art_free_buf();
	vc.art = NULL;
	vc.art_ph = NULL;
}

static bool vc_art_try_load(int w, int h)
{
	size_t sz = (size_t)w * h * 2;
	struct vc_art_img *t;
	char path[160];
	FILE *f;

	if (!vc.art || !vc.yt_id[0])
		return false;
	snprintf(path, sizeof(path), VC_YT_OFFLINE "/%s/%dx%d.565", vc.yt_id, w, h);
	f = fopen(path, "rb");
	if (!f) {
		snprintf(path, sizeof(path), VC_YT_CACHE "/%s-%dx%d.565", vc.yt_id, w, h);
		f = fopen(path, "rb");
	}
	if (!f)
		return false;
	t = calloc(1, sizeof(*t));
	if (t)
		t->data = malloc(sz);
	if (!t || !t->data || fread(t->data, 1, sz, f) != sz) {
		fclose(f);
		if (t)
			free(t->data);
		free(t);
		return false;
	}
	fclose(f);
	t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	t->dsc.header.w = w;
	t->dsc.header.h = h;
	t->dsc.header.stride = w * 2;
	t->dsc.data_size = sz;
	t->dsc.data = t->data;
	lv_image_set_src(vc.art, NULL);
	vc_art_free_buf();
	vc.art_img = t;
	lv_image_set_src(vc.art, &t->dsc);
	if (vc.art_ph)
		lv_obj_add_flag(vc.art_ph, LV_OBJ_FLAG_HIDDEN);
	return true;
}

static void vc_art_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	if (vc.art && vc.audio_only && vc.yt_id[0])
		vc_art_try_load(vc.art_w, vc.art_h);
}

static void vc_art_load(void)
{
	static const int fallbacks[][2] = { { 288, 162 }, { 217, 122 }, { 112, 63 } };
	char cmd[128];

	if (!vc.art || !vc.yt_id[0])
		return;
	if (vc_art_try_load(vc.art_w, vc.art_h))
		return;
	for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
		if (fallbacks[i][0] == vc.art_w && fallbacks[i][1] == vc.art_h)
			continue;
		if (vc_art_try_load(fallbacks[i][0], fallbacks[i][1]))
			break;
	}
	snprintf(cmd, sizeof(cmd), "n72-yt thumbs %d %d %s", vc.art_w, vc.art_h, vc.yt_id);
	job_run(cmd, vc.art, vc_art_done, NULL);
}

static void vc_build_audio(void)
{
	lv_obj_t *row, *b, *col, *t;
	bool land = W > H;
	int lh = lv_font_get_line_height(&lv_font_montserrat_16);

	vc.root = lv_obj_create(app_scr);
	lv_obj_remove_style_all(vc.root);
	lv_obj_set_size(vc.root, W, H);
	lv_obj_set_style_bg_color(vc.root, M.surface, 0);
	lv_obj_set_style_bg_opa(vc.root, LV_OPA_COVER, 0);
	lv_obj_remove_flag(vc.root, LV_OBJ_FLAG_SCROLLABLE);

	vc.ui = lv_obj_create(vc.root);
	lv_obj_remove_style_all(vc.ui);
	lv_obj_set_size(vc.ui, W, H);
	lv_obj_remove_flag(vc.ui, LV_OBJ_FLAG_SCROLLABLE);
	vc.shown = true;

	/* top: Back, "Now playing", turn button */
	row = lv_obj_create(vc.ui);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, W - 16, land ? 48 : 52);
	lv_obj_set_pos(row, 8, 4);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 4, 0);
	vc_icon_btn(row, LV_SYMBOL_LEFT, 48, &lv_font_montserrat_20, vc_back_cb, NULL);
	t = m3_label(row, "Now playing", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_set_flex_grow(t, 1);
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	b = vc.rot_btn = vc_icon_btn(row, LV_SYMBOL_REFRESH, 48, &lv_font_montserrat_20,
				     vc_rot_cb, NULL);
	vc.rot_lbl = lv_obj_get_child(b, 0);
	vc_rot_show();

	vc.art_w = land ? 217 : W - 2 * STATUS_PAD;
	vc.art_h = vc.art_w * 9 / 16;

	if (!land) {
		vc.art = lv_image_create(vc.ui);
		lv_obj_set_size(vc.art, vc.art_w, vc.art_h);
		lv_image_set_inner_align(vc.art, LV_IMAGE_ALIGN_STRETCH);
		lv_obj_set_style_radius(vc.art, 16, 0);
		lv_obj_set_style_bg_color(vc.art, M.surface_c_high, 0);
		lv_obj_set_style_bg_opa(vc.art, LV_OPA_COVER, 0);
		lv_obj_remove_flag(vc.art, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_align(vc.art, LV_ALIGN_TOP_MID, 0, 64);
		lv_obj_add_event_cb(vc.art, vc_art_deleted, LV_EVENT_DELETE, NULL);
		vc.art_ph = m3_label(vc.art, LV_SYMBOL_AUDIO, &lv_font_montserrat_28, M.on_surface_v);
		lv_obj_center(vc.art_ph);

		col = lv_obj_create(vc.ui);
		lv_obj_remove_style_all(col);
		lv_obj_set_size(col, W - 2 * STATUS_PAD, LV_SIZE_CONTENT);
		lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 238);
		lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(col, 4, 0);
		vc.title = m3_label(col, vc.name, &lv_font_montserrat_16, M.on_surface);
		lv_obj_set_size(vc.title, lv_pct(100), 2 * lh);
		lv_label_set_long_mode(vc.title, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_align(vc.title, LV_TEXT_ALIGN_CENTER, 0);
		vc.sub = m3_label(col, vc.artist, &lv_font_montserrat_14, M.on_surface_v);
		lv_obj_set_width(vc.sub, lv_pct(100));
		lv_label_set_long_mode(vc.sub, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_align(vc.sub, LV_TEXT_ALIGN_CENTER, 0);

		row = lv_obj_create(vc.ui);
		lv_obj_remove_style_all(row);
		lv_obj_set_size(row, 264, 80);
		lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 312);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		vc_skip_btn(row, 64, -10000, M.on_surface);
		vc_play_fab(row, 76, 26);
		vc_skip_btn(row, 64, 10000, M.on_surface);

		vc_slider_row(vc.ui, 64, -10, true);
	} else {
		lv_obj_t *pane = lv_obj_create(vc.ui);

		lv_obj_remove_style_all(pane);
		lv_obj_set_size(pane, W - 2 * STATUS_PAD, 186);
		lv_obj_align(pane, LV_ALIGN_TOP_MID, 0, 58);
		lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(pane, 16, 0);

		vc.art = lv_image_create(pane);
		lv_obj_set_size(vc.art, vc.art_w, vc.art_h);
		lv_image_set_inner_align(vc.art, LV_IMAGE_ALIGN_STRETCH);
		lv_obj_set_style_radius(vc.art, 14, 0);
		lv_obj_set_style_bg_color(vc.art, M.surface_c_high, 0);
		lv_obj_set_style_bg_opa(vc.art, LV_OPA_COVER, 0);
		lv_obj_remove_flag(vc.art, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(vc.art, vc_art_deleted, LV_EVENT_DELETE, NULL);
		vc.art_ph = m3_label(vc.art, LV_SYMBOL_AUDIO, &lv_font_montserrat_28, M.on_surface_v);
		lv_obj_center(vc.art_ph);

		col = lv_obj_create(pane);
		lv_obj_remove_style_all(col);
		lv_obj_set_height(col, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(col, 1);
		lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(col, 6, 0);
		vc.title = m3_label(col, vc.name, &lv_font_montserrat_16, M.on_surface);
		lv_obj_set_size(vc.title, lv_pct(100), 2 * lh);
		lv_label_set_long_mode(vc.title, LV_LABEL_LONG_DOT);
		vc.sub = m3_label(col, vc.artist, &lv_font_montserrat_12, M.on_surface_v);
		lv_obj_set_width(vc.sub, lv_pct(100));
		lv_label_set_long_mode(vc.sub, LV_LABEL_LONG_DOT);

		row = lv_obj_create(col);
		lv_obj_remove_style_all(row);
		lv_obj_set_size(row, lv_pct(100), 68);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		vc_skip_btn(row, 52, -10000, M.on_surface);
		vc_play_fab(row, 64, 22);
		vc_skip_btn(row, 52, 10000, M.on_surface);

		vc_slider_row(vc.ui, 56, -6, true);
	}
	vc_art_load();
	vc_update();
}

static void vc_build(void)
{
	lv_obj_t *row, *b;
	bool land = W > H;

	if (vc.root || !app_scr)
		return;
	if (vc.audio_only) {
		vc_build_audio();
		return;
	}
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
	vc_skip_btn(row, 64, -10000, lv_color_white());
	vc_play_fab(row, 80, 28);
	vc_skip_btn(row, 64, 10000, lv_color_white());

	/* bottom: the time and the slider (M3: primary active track, a
	 * light inactive one over video) */
	vc_slider_row(vc.ui, 64, -8, false);
	vc_update();
}

/* The turn button filled (primary) while it holds the screen. */
static void vc_rot_show(void)
{
	bool held = video_rot >= 0;
	lv_color_t fg = vc.audio_only ? M.on_surface : lv_color_white();

	if (!vc.rot_btn)
		return;
	lv_obj_set_style_bg_color(vc.rot_btn, held ? M.primary_c : fg, 0);
	lv_obj_set_style_bg_opa(vc.rot_btn, held ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_text_color(vc.rot_lbl, held ? M.on_primary_c : fg, 0);
}

/* Rebuilt at a new size, as it was. */
void vc_turned(void)
{
	bool shown = vc.shown;

	if (!vc.root)
		return;
	lv_obj_delete(vc.root);
	vc.root = vc.ui = vc.rot_btn = vc.rot_lbl = vc.sub = vc.art = vc.art_ph = NULL;
	vc_build();
	if (!vc.audio_only && shown) {
		lv_obj_remove_flag(vc.ui, LV_OBJ_FLAG_HIDDEN);
		vc.shown = true;
		vc_hide_later();
	}
}

/* A video application is starting: listen for its player. */
void vc_start(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };

	if (vc.root && lv_obj_is_valid(vc.root))
		lv_obj_delete(vc.root);
	vc.root = vc.ui = vc.rot_btn = vc.rot_lbl = vc.sub = vc.art = vc.art_ph = NULL;
	vc.up = vc.shown = vc.dragging = vc.audio_only = false;
	vc.playing = true;
	vc.pos_ms = vc.dur_ms = 0;
	vc.prot = -1;
	vc.name[0] = 0;
	vc.yt_id[0] = 0;
	vc.artist[0] = 0;
	if (vc_next.audio_only) {
		vc.audio_only = true;
		snprintf(vc.yt_id, sizeof(vc.yt_id), "%s", vc_next.yt_id);
		snprintf(vc.artist, sizeof(vc.artist), "%s", vc_next.artist);
		snprintf(vc.name, sizeof(vc.name), "%s", vc_next.title);
		memset(&vc_next, 0, sizeof(vc_next));
	}
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
	if (vc.audio_only && app_scr)
		vc_build();
	fprintf(stderr, "n72-ui: video controls for %s (socket %d)\n", app_cmd, vc.sock);
}



void vc_stop(void)
{
	if (vc.hide) {
		lv_timer_delete(vc.hide);
		vc.hide = NULL;
	}
	if (vc.root && lv_obj_is_valid(vc.root))
		lv_obj_delete(vc.root);
	vc.root = vc.ui = vc.rot_btn = vc.rot_lbl = vc.sub = vc.art = vc.art_ph = NULL;
	vc.shown = false;
	video_rot = -1;
	if (bgp.pid > 0)
		return;		/* (the player plays on: its state for the shade) */
	vc.audio_only = false;
	vc.yt_id[0] = 0;
	vc.artist[0] = 0;
	if (vc.sock >= 0)
		ui_unwatch(vc.sock);
	if (vc.poll) {
		lv_timer_delete(vc.poll);
		vc.poll = NULL;
	}
	vc.up = false;
	idle_timer_sync();
}

/*
 * Background playback. Home while the player plays (a video, a song,
 * YouTube's Listen) does not end it: it is told "bg" (the picture off the
 * screen, its video decoder idle, the sound going on) and leaves the front;
 * the launcher is back as after any application, and the shade has a card
 * for it (the title, where it is, back 10 s, play/pause, on 10 s, close).
 * A tap on the card brings it to the front again ("fg"), the controls up;
 * starting anything else, or the close button, ends it. Its end (the media
 * over, an error) is a pidfd event as for applications. The status replies
 * keep coming over the controls' socket, every 2 s (twice a second while
 * the shade is open). The screen and sleep need nothing of their own: sound
 * playing keeps the system up with the screen off (main.c,
 * screen_off_or_sleep), and paused it sleeps as usual.
 */
bool bg_busy(void)
{
	return bgp.pid > 0;
}

static void bg_reaped(void)
{
	if (bgp.pidfd >= 0) {
		ui_unwatch(bgp.pidfd);
		close(bgp.pidfd);
		bgp.pidfd = -1;
	}
	if (bgp.kill) {
		lv_timer_delete(bgp.kill);
		bgp.kill = NULL;
	}
	if (bgp.poll) {
		lv_timer_delete(bgp.poll);
		bgp.poll = NULL;
	}
	free(bgp.cmd);
	bgp.cmd = NULL;
	bgp.pid = 0;
	bgp.ending = false;
	if (app_pid == 0)
		vc_stop();	/* (now with the socket: nothing plays) */
	np_sync();
}

static void bg_check(void)
{
	int st;

	if (bgp.pid > 0 && waitpid(bgp.pid, &st, WNOHANG) == bgp.pid)
		bg_reaped();
}

static void bg_pidfd_ev(int fd)
{
	LV_UNUSED(fd);
	bg_check();
}

static void bg_poll_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	bg_check();
}

static void bg_kill_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	bgp.kill = NULL;
	if (bgp.pid > 0)
		kill(-bgp.pid, SIGKILL);	/* (reaped by its pidfd event) */
}

/* The application (a playing n72-player) to the background: true if it went. */
bool bg_start(void)
{
	if (!app_is_video() || !vc.up || !vc.playing || lv_screen_active() != app_scr ||
	    bgp.pid > 0 || locked)
		return false;
	za_finish();
	vc_send("bg");
	bgp.pid = app_pid;
	bgp.cmd = app_cmd;
	app_cmd = NULL;
	bgp.ending = false;
	if (app_pidfd >= 0) {
		ui_unwatch(app_pidfd);
		bgp.pidfd = app_pidfd;
		app_pidfd = -1;
		ui_watch(bgp.pidfd, bg_pidfd_ev);
	} else {
		bgp.poll = lv_timer_create(bg_poll_cb, 1000, NULL);	/* (no pidfd: look) */
	}
	if (app_timer) {
		lv_timer_delete(app_timer);
		app_timer = NULL;
	}
	app_pid = 0;
	kbd_app_gone();
	rot_kick();		/* (no player in front: the launcher's own turn) */
	fprintf(stderr, "n72-ui: %s to the background (pid %d)\n", bgp.cmd ? bgp.cmd : "player",
		(int)bgp.pid);
	np_sync();
	za_close();		/* as an application ends: into its icon */
	return true;
}

/* The player in the background ends (the shade's close, anything started). */
void bg_end(void)
{
	if (bgp.pid <= 0 || bgp.ending)
		return;
	bgp.ending = true;
	kill(-bgp.pid, SIGTERM);
	bgp.kill = lv_timer_create(bg_kill_cb, 600, NULL);
	lv_timer_set_repeat_count(bgp.kill, 1);
	np_sync();		/* (the card goes now) */
}

/* The card: the player to the front again, its controls up. */
static void bg_front(void)
{
	if (bgp.pid <= 0 || bgp.ending || app_pid != 0 || za.phase || locked ||
	    !mbx_gpu_on_shell_layer())
		return;
	ui_settle_all();
	tr_finish();
	shade_set(false, false);
	free(app_cmd);
	app_cmd = bgp.cmd;
	bgp.cmd = NULL;
	app_pid = bgp.pid;
	if (bgp.pidfd >= 0) {
		ui_unwatch(bgp.pidfd);
		app_pidfd = bgp.pidfd;
		bgp.pidfd = -1;
		ui_watch(app_pidfd, app_pidfd_ev);
	} else if (!app_timer) {
		app_timer = lv_timer_create(app_poll, 200, NULL);
	}
	if (bgp.poll) {
		lv_timer_delete(bgp.poll);
		bgp.poll = NULL;
	}
	bgp.pid = 0;
	app_scr_make();
	launch_from = lv_screen_active();	/* (home, or a page) */
	za.tile = NULL;				/* (no icon to grow from: it simply shows) */
	lv_obj_set_parent(scrim, lv_layer_top());
	lv_obj_set_parent(shade, lv_layer_top());
	/* the controls are built at its next answer, shown a moment */
	vc.up = vc.shown = vc.dragging = false;
	vc.prot = -1;			/* (its turn told again) */
	video_rot = -1;
	vc_send("fg");
	if (vc.audio_only)
		vc_build();
	mbx_gpu_set_transparent(true);
	lv_screen_load(app_scr);
	rot_kick();
	np_sync();
	if (vc.poll)
		lv_timer_ready(vc.poll);
}

/* ---- the shade's card */

static void np_play_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	vc_send("toggle");
	vc.playing = !vc.playing;
	vc_quiet();
	np_sync();
}

static void np_skip_cb(lv_event_t *e)
{
	char m[32];

	snprintf(m, sizeof(m), "skip %d", (int)(intptr_t)lv_event_get_user_data(e));
	vc_send(m);
	vc_quiet();
}

static void np_close_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	bg_end();
}

static void np_front_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	bg_front();
}

/* A round icon button of the card (tonal when @filled). */
static lv_obj_t *np_btn(lv_obj_t *parent, const char *sym, bool filled, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	plain(b);
	lv_obj_set_size(b, 40, 40);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(b, filled ? M.primary : M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(b, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_transform_scale(b, 240, LV_STATE_PRESSED);
	l = m3_label(b, sym, &lv_font_montserrat_16, filled ? M.on_primary : M.on_surface);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
	return b;
}

#define NP_H 72		/* the card; 8 above it */

/*
 * In the shade, under the tiles: a rounded surface card, the title and
 * where it is (a tap there: to the front), the buttons on the right.
 * Hidden while nothing plays in the background.
 */
void np_build(lv_obj_t *parent, int32_t y)
{
	lv_obj_t *c, *col, *row;

	c = np.card = lv_obj_create(parent);
	plain(c);
	lv_obj_set_size(c, W - 2 * STATUS_PAD, NP_H);
	lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
	lv_obj_set_style_bg_color(c, M.surface_c_high, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(c, 20, 0);
	lv_obj_set_style_pad_hor(c, 14, 0);
	lv_obj_set_style_pad_ver(c, 0, 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(c, np_front_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 6, 0);

	col = lv_obj_create(c);
	lv_obj_remove_style_all(col);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	np.title = m3_label(col, "", &lv_font_montserrat_14, M.on_surface);
	lv_obj_set_width(np.title, lv_pct(100));
	lv_label_set_long_mode(np.title, LV_LABEL_LONG_DOT);
	np.sub = m3_label(col, "", &lv_font_montserrat_12, M.on_surface_v);

	row = lv_obj_create(c);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 2, 0);
	np_btn(row, LV_SYMBOL_PREV, false, np_skip_cb, (void *)(intptr_t)-10000);
	np.play_sym = lv_obj_get_child(np_btn(row, LV_SYMBOL_PAUSE, true, np_play_cb, NULL), 0);
	np_btn(row, LV_SYMBOL_NEXT, false, np_skip_cb, (void *)(intptr_t)10000);
	np_btn(row, LV_SYMBOL_CLOSE, false, np_close_cb, NULL);
	lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
	np_sync();
}

/*
 * On the lock screen, while something plays (in the background, or the
 * player in front under the lock): what it is, where it is, and back,
 * play/pause, on. No tap to the front: that would need the unlock. The
 * buttons are LVGL's (the unlock takes only a swipe up).
 */
void np_lock_build(lv_obj_t *scr)
{
	lv_obj_t *c, *row;

	c = npl.card = lv_obj_create(scr);
	plain(c);
	lv_obj_set_size(c, W - 2 * STATUS_PAD, 104);
	lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -100);
	lv_obj_set_style_bg_color(c, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_50, 0);
	lv_obj_set_style_radius(c, 20, 0);
	lv_obj_set_style_pad_hor(c, 14, 0);
	lv_obj_set_style_pad_top(c, 10, 0);
	lv_obj_set_style_pad_bottom(c, 6, 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(c, 2, 0);
	npl.title = m3_label(c, "", &lv_font_montserrat_14, lv_color_white());
	lv_obj_set_width(npl.title, lv_pct(100));
	lv_obj_set_style_text_align(npl.title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(npl.title, LV_LABEL_LONG_DOT);
	npl.sub = m3_label(c, "", &lv_font_montserrat_12, lv_color_hex(0xc8c0d0));

	row = lv_obj_create(c);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	lv_obj_set_style_pad_top(row, 4, 0);
	{
		lv_obj_t *b;

		b = np_btn(row, LV_SYMBOL_PREV, false, np_skip_cb, (void *)(intptr_t)-10000);
		lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_white(), 0);
		npl.play_sym = lv_obj_get_child(np_btn(row, LV_SYMBOL_PAUSE, true, np_play_cb, NULL), 0);
		b = np_btn(row, LV_SYMBOL_NEXT, false, np_skip_cb, (void *)(intptr_t)10000);
		lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_white(), 0);
	}
	lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
	np_lock_sync();
}

/* something the controls can act on: the background player, or the player in front */
static bool np_playing_any(void)
{
	return (bgp.pid > 0 && !bgp.ending) || (app_is_video() && vc.up);
}

/* The lock screen's card as the player is now (its position while the lock screen shows). */
void np_lock_sync(void)
{
	bool on = np_playing_any();
	char b[64], p[24], d[24];

	if (!npl.card || !lv_obj_is_valid(npl.card))
		return;
	if (lv_obj_has_flag(npl.card, LV_OBJ_FLAG_HIDDEN) == on) {
		if (on)
			lv_obj_remove_flag(npl.card, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(npl.card, LV_OBJ_FLAG_HIDDEN);
	}
	if (!on)
		return;
	label_set_if_changed(npl.title, vc.name[0] ? vc.name : "Playing");
	label_set_if_changed(npl.play_sym, vc.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	if (!locked || screen_off)
		return;		/* (where it is: only while it shows) */
	vc_fmt(p, sizeof(p), vc.pos_ms);
	if (vc.dur_ms > 0) {
		vc_fmt(d, sizeof(d), vc.dur_ms);
		snprintf(b, sizeof(b), "%s / %s%s", p, d, vc.playing ? "" : "  Paused");
	} else {
		snprintf(b, sizeof(b), "%s%s", p, vc.playing ? "" : "  Paused");
	}
	label_set_if_changed(npl.sub, b);
}

static void shade_relayout(void);

/*
 * The shade as tall as what it holds: the background player's card adds
 * its height (NP_H and 8) above the brightness slider. Its pictures are
 * taken again; closed, it waits above the screen at the new height.
 */
static void shade_relayout(void)
{
	bool card = np.card && !lv_obj_has_flag(np.card, LV_OBJ_FLAG_HIDDEN);
	int32_t h = shade_base_h + (card ? NP_H + 8 : 0);

	if (!shade || !shade_bright_row || h == shade_h)
		return;
	shade_h = h;
	lv_obj_set_height(shade, shade_h);
	lv_obj_align(shade_bright_row, LV_ALIGN_TOP_MID, 0, shade_h - SHADE_FOOT_H - SHADE_BRIGHT_H);
	if (!shade_open && !sd.on && !sd.scr)
		lv_obj_set_y(shade, -shade_h);
	shade_pic_dirty = true;
}

/* The card and the status bar's symbol as the background player is now. */
void np_sync(void)
{
	bool on = bgp.pid > 0 && !bgp.ending;
	char b[64], p[24], d[24];

	np_lock_sync();
	for (int i = 0; i < 2; i++) {
		lv_obj_t *l = i ? lock_bar_np : status_np;

		if (l && lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN) == on) {
			if (on)
				lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (!np.card)
		return;
	if (lv_obj_has_flag(np.card, LV_OBJ_FLAG_HIDDEN) == on) {
		if (on)
			lv_obj_remove_flag(np.card, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(np.card, LV_OBJ_FLAG_HIDDEN);
		shade_relayout();
	}
	if (!on)
		return;
	label_set_if_changed(np.title, vc.name[0] ? vc.name : "Playing");
	label_set_if_changed(np.play_sym, vc.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	/*
	 * Where it is only while the shade is open: each change of a label in
	 * the shade has its picture taken again (shade_changed), every 2 s
	 * for nothing while it is closed. Opening it asks the player at once
	 * (np_opened).
	 */
	if (!shade_open && lv_label_get_text(np.sub)[0])
		return;
	vc_fmt(p, sizeof(p), vc.pos_ms);
	if (vc.dur_ms > 0) {
		vc_fmt(d, sizeof(d), vc.dur_ms);
		snprintf(b, sizeof(b), "%s / %s%s", p, d, vc.playing ? "" : "  Paused");
	} else {
		snprintf(b, sizeof(b), "%s%s", p, vc.playing ? "" : "  Paused");
	}
	label_set_if_changed(np.sub, b);
}

/* The shade opened: the card's position fresh now, then twice a second. */
void np_opened(void)
{
	if (bgp.pid > 0 && app_pid == 0 && vc.poll) {
		lv_timer_set_period(vc.poll, 500);
		lv_timer_ready(vc.poll);
	}
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

#define BZ_MATCH "type='signal',sender='org.bluez'"

/*
 * Monitors an earlier n72-ui left behind (it ended, crashed or was
 * restarted: the child lived on under init, and with Bluetooth quiet it
 * never wrote, so never saw the broken pipe). 34 of them, 17 MB, after a
 * night of restarts.
 */
static void bz_reap_orphans(void)
{
	DIR *d = opendir("/proc");
	struct dirent *de;

	while (d && (de = readdir(d))) {
		char path[64], buf[160], *q;
		int pid = atoi(de->d_name), ppid = -1;
		ssize_t n;
		FILE *f;
		int fd;

		if (pid <= 1 || pid == getpid())
			continue;
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		if (!(f = fopen(path, "r")))
			continue;
		n = (ssize_t)fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n > 0 ? n : 0] = 0;
		/* "pid (comm) S ppid": after the last ')' */
		if (!(q = strrchr(buf, ')')) || sscanf(q + 1, " %*c %d", &ppid) != 1 || ppid != 1 ||
		    !strstr(buf, "(dbus-monitor)"))
			continue;
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		for (ssize_t i = 0; i < n; i++)
			if (!buf[i])
				buf[i] = ' ';
		if (strstr(buf, BZ_MATCH))
			kill(pid, SIGTERM);
	}
	if (d)
		closedir(d);
}

static void bz_start(lv_timer_t *t)
{
	static bool reaped;
	int p[2], nul;
	pid_t me = getpid(), pid;

	LV_UNUSED(t);
	if (app_dormant)
		return;
	if (!reaped) {
		reaped = true;
		bz_reap_orphans();
	}
	if (bz_fd >= 0 || !have_tool("dbus-monitor") || pipe2(p, O_CLOEXEC))
		return;
	fcntl(p[0], F_SETFL, O_NONBLOCK);	/* (ours only: dbus-monitor writes and waits) */
	nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
	/*
	 * vfork, not posix_spawn: the child asks to be ended when n72-ui
	 * ends (PR_SET_PDEATHSIG), however n72-ui ends. (Only syscalls run in
	 * the child before the exec.)
	 */
	pid = vfork();
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != me)
			_exit(0);	/* (n72-ui gone already) */
		dup2(p[1], 1);
		if (nul >= 0)
			dup2(nul, 2);
		execlp("dbus-monitor", "dbus-monitor", "--system", BZ_MATCH, (char *)NULL);
		_exit(127);
	}
	bz_pid = pid;
	if (nul >= 0)
		close(nul);
	if (pid < 0) {
		close(p[0]);
		p[0] = -1;
	}
	close(p[1]);
	if (p[0] < 0)
		return;
	bz_fd = p[0];
	ui_watch(bz_fd, bz_ev);
}

void bz_stop(void)
{
	if (bz_timer) {
		lv_timer_delete(bz_timer);
		bz_timer = NULL;
	}
	if (bz_fd >= 0) {
		ui_unwatch(bz_fd);
		close(bz_fd);
		bz_fd = -1;
	}
	if (bz_pid > 0) {
		kill(bz_pid, SIGTERM);
		waitpid(bz_pid, NULL, 0);
		bz_pid = 0;
	}
}

void bz_ensure(void)
{
	if (bz_fd < 0 && !app_dormant)
		bz_start(NULL);
}

/* The settings kept on the flash, applied once at start. */
void ui_apply_saved(void)
{
	if (access(CONF_PATH, F_OK))
		conf_save();	/* (the first start: what was in /tmp, onto the flash) */
	if (conf.bright >= 5)
		backlight_set(conf.bright);
	auto_bright_sync();
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
