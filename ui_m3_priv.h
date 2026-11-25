// SPDX-License-Identifier: MIT
/* What the user interface files (ui_m3.c and ui_*.c) share. */
#ifndef N72_UI_M3_PRIV_H
#define N72_UI_M3_PRIV_H

#include "main_priv.h"
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
#include <sys/prctl.h>
#include <sys/un.h>
#include <sys/statvfs.h>
#include <sys/inotify.h>
#include <ctype.h>
#include "ios_motion.h"
extern char **environ;

struct n72_conf {
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
};

/* ui_m3.c and the files of the user interface, used by main.c and sleep.c */
extern struct n72_conf conf;
extern pid_t app_pid;
extern bool locked;
extern bool app_dormant;
bool have_tool(const char *name);
bool usb_powered(void);
void status_kick(bool batt, bool net);
bool game_playing(void);
bool video_playing(void);
void app_dormant_wake(bool full);
void app_dormant_sync(void);

#define QS_COLS (W > H ? 3 : 2)	/* quick-settings tiles a row */

struct np {
	lv_obj_t *card, *title, *sub, *play_sym;
};

struct npl {
	lv_obj_t *card, *title, *sub, *play_sym;
};

/* on the flash, so that a reboot keeps them (/tmp: the old place, read once) */
#define CONF_DIR "/var/lib/n72"

#define CONF_PATH CONF_DIR "/ui.conf"

/* layout: every screen's side margin (status bar, pages, shade, home) */
#define STATUS_PAD	16

#define STATUS_GAP	10	/* between the status bar's icons */

#define ROT_LOCKED	0

#define ROT_AUTO	1

struct accents { const char *name; uint16_t hue; uint8_t sat; };

#define N_ACCENTS ((int)(sizeof(accents) / sizeof(accents[0])))

/* Wallpapers (wallpaper.c) */
enum { WP_HOME, WP_LOCK, WP_KINDS };

struct wp {
	lv_draw_buf_t *pic[WP_KINDS][2];	/* [kind][0 upright, 1 turned] */
	bool set[WP_KINDS];
	int luma[WP_KINDS];			/* mean luma under the app grid / clock */
	int hue, sat;				/* the picture's own accent; hue -1: none */
	int turn[WP_KINDS];			/* the turn pic[kind][1] was made for */
	bool busy;				/* a conversion is running */
	bool loaded;				/* what is set has been read (wp_load) */
};

/* text straight on a wallpaper: white (the picture is darkened under it) */
#define WP_TEXT lv_color_white()

struct M {
	lv_color_t primary, on_primary, primary_c, on_primary_c;
	lv_color_t secondary_c, on_secondary_c, tertiary_c, on_tertiary_c;
	lv_color_t surface, surface_c, surface_c_high, surface_c_highest;
	lv_color_t on_surface, on_surface_v, outline, outline_v, error;
};

#define N72_SYMBOL_GAMEPAD	"\xEE\x80\x80"	/* U+E000 */

struct tr {
	lv_obj_t *scr, *bg, *fg;	/* transition screen and its two pictures */
	lv_draw_buf_t *sbg, *sfg;
	lv_obj_t *keep_for;		/* forward: keep the old page's picture for Back */
	lv_obj_t *to, *from;		/* screen to load at the end, and to delete */
	int kind;			/* 0 slide left, 1 slide right, 2 shade */
	void (*done)(void);
};

typedef void (*psl_sig_fn)(char *buf, size_t len);

/* Back goes to the previous page; pages are a small stack. */
#define NAV_MAX 16	/* (Files: a page a folder) */

#define SC_MAX		64

struct sc {
	lv_obj_t *owner;
	lv_obj_t *live[SC_MAX], *pic[SC_MAX];
	lv_draw_buf_t *buf[SC_MAX];
	long size[SC_MAX];
	long bytes;
	int n;
	lv_timer_t *timer;
	uint32_t last_move;
};

struct nav_re {
	void (*open)(lv_event_t *);
	char *dir;
};

#define KBD_NKEYS	48

struct kbd {
	lv_obj_t *obj;
	bool shown;		/* up, or on its way up */
	lv_obj_t *ta;		/* the launcher's field it types into, or NULL: the application */
	void (*vis_cb)(bool shown, int32_t h);	/* the field's page: it came or went */
	int page;
	int shift;		/* 0 off, 1 the next letter, 2 locked */
	uint32_t shift_t;	/* when Shift went on (a second tap soon locks) */
	bool ctrl;		/* the next key with Ctrl */

	/* geometry, relative to the object (rebuilt by kbd_layout) */
	int32_t w, h, gap, key_h, tb_h, pad;
	int nkeys;
	struct { lv_area_t a; const struct kbd_key *k; } keys[KBD_NKEYS];

	/* the touch */
	int down;		/* the key under the finger, or -1 */
	bool held;		/* held long: its hint (digit) */
	bool cursor;		/* dragging the space bar: the cursor moves */
	int32_t px, last_x;
	lv_timer_t *rep, *hold;

	/* applications */
	int ui_fd;		/* the uinput keyboard */
	int fifo_fd, fifo_wr;	/* /run/n72-kbd (and our own writer: no EOF) */
};

struct sd {
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
};

struct za {
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
};

extern char **environ;

#define PG_MAX		8

#define PG_TOP		40		/* the strip starts under the status bar */

#define PG_CARD_H	100

#define PG_DOTS_H	24		/* the dots' row at the bottom */

#define PG_TIME_LEVELS	16

#define PG_PAGES_MAX	PG_MAX

#define PX	home_px

#define PW	home_pw

#define PB	home_pb

struct pg {
	int n, cur;			/* pages, the one at rest */
	int cols, rows0, rowsn;
	/* touch */
	bool cand, on, caught;
	bool lost;			/* this touch is nobody's (the frames failed) */
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
	lv_obj_t *scr, *view, *strip_img, *strip_img2, *bar_img, *bar_r_img, *time_img, *dots_img, *wp_img;
	int strip_k;			/* the page strip_img shows; strip_img2 the next */
	lv_draw_buf_t *page_pic[PG_PAGES_MAX], *dock_pic, *bar_pic, *bar_l, *bar_r, *dots_pic, *time_pic[PG_TIME_LEVELS + 1];
	lv_area_t time_area;		/* the time's place in the bar */
	unsigned long pic_ver;		/* home_ver they show */
	unsigned long fail_ver;		/* (no memory for them at this home_ver) */
	uint32_t fail_ms;
	int pic_mode, pic_w, pic_dots;
	bool fb0_ok;			/* /dev/fb0 holds the home wallpaper */
	int fb0_w;
	/* the tiles' pages (pg_layout): the page of each child of the strip */
	signed char page_of[160];
	bool new_page;			/* an empty page after the last: an icon carried there */
};

#define FILES_ROOT	"/root"

#define BT_AUTO_RUN "/run/n72-bt-auto"

#define BT_MANUAL_RUN "/run/n72-bt-manual"

#define BT_SERDEV	"/sys/bus/serial/drivers/hci_uart_bcm"

#define BT_OFF_CMD	"bluetoothctl power off; [ -e " BT_SERDEV "/serial0-0 ] && " \
			"echo -n serial0-0 > " BT_SERDEV "/unbind"

#define BT_AUTO_MIN_MS	30000

/* ui_m3.c */
extern char *app_cmd;
extern lv_obj_t *home_dock;
extern struct np np;
extern lv_obj_t *status_np;
extern struct npl npl;
extern const struct accents accents[6];
extern struct wp wp;
extern bool home_wp;
void conf_load(void);
void conf_save(void);
void conf_save_later(void);
extern struct M M;
lv_color_t tone(uint16_t h, uint8_t s, uint8_t l);
void m3_apply_theme(void);
void m3_screen(lv_obj_t *scr);
void obj_track(lv_obj_t **pp);
void plain(lv_obj_t *o);
lv_obj_t *m3_card(lv_obj_t *parent, lv_color_t bg);
lv_obj_t *m3_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *row_slider(lv_obj_t *parent, bool last);
lv_obj_t *slider_value(lv_obj_t *parent);
extern const lv_font_t n72_icon_24;
lv_obj_t *m3_badge(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_color_t fg, int size);
lv_obj_t *ios_switch(lv_obj_t *sw);
lv_obj_t *m3_row_ex(lv_obj_t *parent, const char *icon, lv_color_t bg, lv_color_t fg, const char *title, const char *sub, lv_event_cb_t cb, void *ud, lv_obj_t **sub_lbl, bool chevron);
void m3_row_current(lv_obj_t *r);
lv_obj_t *m3_row(lv_obj_t *parent, const char *icon, const char *title, const char *sub, lv_event_cb_t cb, void *ud, lv_obj_t **sub_lbl);
void m3_section(lv_obj_t *parent, const char *title);
void m3_note(lv_obj_t *parent, const char *text);
lv_obj_t *m3_button(lv_obj_t *parent, const char *text, bool filled, lv_event_cb_t cb, void *ud);

/* ui_nav.c */
extern struct tr tr;
void tr_free(lv_draw_buf_t **b);
long bp_minute(void);
extern unsigned long home_ver;
void bp_changed(lv_obj_t *scr);
void tr_finish(void);
void quiet_begin(void);
void quiet_end(void);
lv_draw_buf_t *snap(lv_obj_t *obj, lv_color_format_t cf);
bool psl_covers(lv_obj_t *live, int x, int y);
bool psl_start_sig(lv_obj_t *live, int32_t to, uint32_t ms, lv_anim_path_cb_t path, bool hide, psl_sig_fn sig);
bool psl_start(lv_obj_t *live, int32_t to, uint32_t ms, lv_anim_path_cb_t path, bool hide);
void psl_touch(lv_obj_t *live);
void psl_check(lv_obj_t *live);
void psl_warm(lv_obj_t *live, psl_sig_fn sig);
void psl_stop(lv_obj_t *live);
bool psl_sliding(lv_obj_t *live);
void nav_caches_clear(void);
void pf_timer_sync(void);
lv_draw_buf_t *grab_or_snap(lv_obj_t *scr);
bool tr_start(lv_draw_buf_t *bg, lv_draw_buf_t *fg, int kind, lv_obj_t *to, lv_obj_t *del, int32_t from_v, int32_t to_v);
extern lv_obj_t *nav[NAV_MAX];
extern int nav_n;
extern lv_obj_t *launch_from;
bool on_page(void);
extern lv_obj_t *app_scr;
void go_home(void);
void go_back(void);
extern struct sc sc;
void fling_accel(lv_obj_t *obj);
void glide_init(void);
void pf_label_changed(lv_obj_t *lbl);
void pf_init(void);
lv_obj_t *m3_page(const char *title, lv_obj_t **scr_out);
void obj_rest(lv_obj_t *t);
extern lv_obj_t *push_quiet_scr;
extern bool push_quiet;
extern struct nav_re nav_re[NAV_MAX];
extern struct nav_re nav_cur;
void nav_mark(void (*open)(lv_event_t *), const char *dir);
void push(lv_obj_t *scr);

/* ui_sys.c */
int job_run(const char *cmd, lv_obj_t *owner, void (*done)(const char *out, void *ud), void *ud);
void shq(const char *s, char *out, size_t len);
int read_sysfs_int(const char *path, int def);
void label_set_if_changed(lv_obj_t *lbl, const char *text);
void ip_of(const char *ifname, char *out, size_t len);
void kv_get(const char *text, const char *key, char *out, size_t len);
extern char wifi_ssid[64];
bool wifi_powered(void);
extern bool wifi_up;
extern bool bt_powered;
void wifi_status_done(const char *out, void *ud);
void bt_show_done(const char *out, void *ud);
void status_refresh(bool with_bt);

/* ui_home.c */
extern lv_obj_t *clock_lbl;
extern lv_obj_t *date_lbl;
extern lv_obj_t *batt_lbl;
extern lv_obj_t *status_time;
extern lv_obj_t *home_clock;
extern lv_obj_t *home_date;
extern lv_obj_t *home_batt;
extern lv_obj_t *home_net;
extern lv_obj_t *qs_wifi_sub;
extern lv_obj_t *qs_bt_sub;
extern lv_obj_t *qs_wifi;
extern lv_obj_t *qs_bt;
extern lv_obj_t *set_wifi_sub;
extern lv_obj_t *set_bt_sub;
extern lv_timer_t *home_timer;
void status_row(lv_obj_t *bar, lv_obj_t **time, lv_obj_t **wifi, lv_obj_t **bt, lv_obj_t **batt);
extern int wifi_want;
extern int bt_want;
extern lv_obj_t *wifi_switch;
extern lv_obj_t *status_wifi;
extern lv_obj_t *status_bt;
extern lv_obj_t *app_bar_wifi;
extern lv_obj_t *app_bar_bt;
extern lv_obj_t *lock_bar_time;
extern lv_obj_t *lock_bar_wifi;
extern lv_obj_t *lock_bar_bt;
extern lv_obj_t *lock_bar_batt;
extern lv_obj_t *lock_bar_np;
bool wifi_shown_on(void);
void switch_show(lv_obj_t *sw, bool on);
void net_ui_sync(void);
void rtnl_init(void);
void home_update(lv_timer_t *t);
extern lv_draw_buf_t *icon_shadow;
void app_tile(lv_obj_t *parent, const char *icon, const char *name, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb);
extern lv_obj_t *home_bright;
extern lv_obj_t *shade;
extern lv_obj_t *scrim;
extern lv_obj_t *qs_settings_btn;
extern bool shade_open;
void clockset_init(void);
void home_wake(void);
extern int32_t shade_h;
extern int32_t shade_base_h;
extern int32_t shade_card_y;
extern lv_obj_t *shade_bright_row;
void shade_set(bool open, bool anim);
void home_gesture_cb(lv_event_t *e);
void shade_tap_cb(lv_event_t *e);
void wifi_set(bool on);
extern lv_obj_t *settings_tile;
extern lv_obj_t *qs_rot;
extern lv_obj_t *qs_rot_sub;
extern int ui_rot;
void qs_rot_sync(void);
void build_shade(void);
extern lv_timer_t *app_timer;
extern lv_obj_t *app_bar;
extern lv_obj_t *app_bar_time;
extern lv_obj_t *app_bar_batt;
extern bool app_bar_shown;
extern lv_timer_t *app_bar_timer;
void app_shade_slide(bool open);
void app_bar_set(bool show);
void app_gesture_cb(lv_event_t *e);
void app_bar_build(void);
void app_bar_update(void);
extern struct kbd kbd;
void kbd_show_for(lv_obj_t *ta, void (*vis_cb)(bool shown, int32_t h));
void kbd_hide_now(bool anim);
void kbd_hide(void);
void kbd_attach(lv_obj_t *ta, void (*vis_cb)(bool shown, int32_t h), bool show);
void kbd_toggle_app(void);
void kbd_init(void);
void kbd_app_gone(void);
void kbd_turned(void);
void wx_card_build(lv_obj_t *card);
void wx_init(void);
void open_weather(lv_event_t *e);
bool ui_wants_touch(int x, int y);
extern struct sd sd;
void sd_free_pics(void);
extern lv_draw_buf_t *shade_pic;
extern bool shade_pic_dirty;
extern long shade_pic_min;
void shade_changed(lv_obj_t *o);
lv_draw_buf_t *shade_pic_get(void);
void shade_pic_free(void);
bool kbd_is_open(void);
extern lv_obj_t *lock_scr;
extern lv_obj_t *lock_time;
extern lv_obj_t *lock_date;
extern lv_obj_t *lock_batt;
void lock_update(void);
void ui_lock(void);
void lock_wake(void);
bool ui_drag(int phase, int x, int y);
extern struct za za;
void za_finish(void);
void ui_settle_all(void);
lv_obj_t *page_tile_anim(lv_obj_t *parent, void (*open)(lv_event_t *));
bool za_page_close(void);
extern bool page_over_app;
bool page_back_to_app(void);
bool page_home_over_app(void);
void za_close(void);
void app_poll(lv_timer_t *t);
extern int app_pidfd;
void app_pidfd_ev(int fd);
bool app_end_ex(bool bg);
bool app_end(void);
void app_scr_make(void);
void app_launch(const char *cmd, lv_obj_t *tile);
void app_launch_cb(lv_event_t *e);
void app_tiles(lv_obj_t *parent);
extern lv_obj_t *home_bar;
extern lv_obj_t *home_dots;
extern lv_obj_t *home_view;
extern int home_px;
extern int home_pw;
extern int home_pb;
void home_geom(void);
extern struct pg pg;
extern lv_timer_t *pg_warm_timer;
void pg_fb0_dirty(void);
void pg_warm_kick(void);
void pg_pics_free(void);
bool pg_drag(int phase, int x, int y);
void pg_warm(lv_timer_t *t);
void pg_home_gone(void);
void pg_abort(void);
void dock_build(void);
void dock_carry_stop(void);
void apps_reload(void);
void apps_watch(void);

/* ui_files.c */
void files_open_dir(const char *path);
int games_thumb_main(int argc, char **argv);
void open_games(lv_event_t *e);
bool gp_up(void);
bool gp_home(void);
void gp_gone(void);
void games_art_resume(void);
extern lv_obj_t *home_apps;
extern bool home_build_quiet;
extern lv_timer_t *shade_warm_timer;
void build_home(void);
void ui_restart(const char *open_page);

/* ui_wifi.c */
void wifi_page_refresh(void);
void open_wifi(lv_event_t *e);

/* ui_bt.c */
extern lv_obj_t *bt_page;
extern lv_obj_t *bt_list;
extern lv_obj_t *bt_switch;
extern lv_obj_t *bt_scan_lbl;
extern lv_obj_t *bt_state_lbl;
void bt_page_refresh(void);
void bt_set_done(const char *out, void *ud);
void bt_set(bool on);
void open_bt(lv_event_t *e);
extern lv_obj_t *btd_page;
extern lv_obj_t *btd_card;
extern lv_obj_t *btd_status;
extern lv_obj_t *btd_codec_row;
extern lv_obj_t *btd_codec;
extern lv_obj_t *btd_conn_lbl;
extern lv_obj_t *btd_auto_sw;
void btd_refresh(void);
extern lv_timer_t *bt_auto_timer;
void bt_auto_kick(void);
void bt_auto_tick(lv_timer_t *t);

/* ui_settings.c */
void open_display(lv_event_t *e);
void open_power(lv_event_t *e);
void open_about(lv_event_t *e);
extern lv_obj_t *vol_hud;
extern lv_obj_t *vol_hud_slider;
extern lv_obj_t *vol_hud_icon;
extern lv_timer_t *vol_hud_timer;
bool vol_hud_visible(void);
void vol_hud_show(void);
void volume_step(int dir);
void open_sound(lv_event_t *e);
void sound_ui_init(void);
int wp_convert_main(int argc, char **argv);
bool wp_hold(lv_obj_t *img, lv_draw_buf_t *b);
void wp_suspend(void);
void wp_resume(void);
void wp_load(void);
lv_draw_buf_t *wp_upright(int kind);
lv_draw_buf_t *wp_pic(int kind);
bool wp_put(lv_obj_t *scr, int kind);
void wp_apply_kick(void);
void open_wallpaper(lv_event_t *e);
void open_apps(lv_event_t *e);
void open_ipa_install(const char *path);
void open_settings(lv_event_t *e);

/* ui_logs.c */
bool logs_scroll(int dir);
void open_logs(lv_event_t *e);
void ui_reopen(void);

/* ui_paint.c */
void open_paint(lv_event_t *e);

/* ui_turn.c */
extern int rot_side;
extern int video_rot;
extern bool layout_stale;
void ui_rebuild(bool load);
void home_fresh(void);
void page_fresh(int i);
bool ui_turn(int rot);
void rot_motion_off(void);
void rot_kick(void);
void rot_conf_changed(void);
void rot_init(void);

/* ui_video.c */
bool cmd_is_video(const char *cmd);
bool app_is_video(void);
extern int game_rot;
bool cmd_is_game(const char *cmd);
bool app_is_game(void);
void vc_turned(void);
void vc_start(void);
void vc_stop(void);
bool vc_is_visible(void);
void bz_stop(void);
void bz_ensure(void);
bool bg_busy(void);
bool bg_start(void);
void bg_end(void);
void np_build(lv_obj_t *parent, int32_t y);
void np_lock_build(lv_obj_t *scr);
void np_lock_sync(void);
void np_sync(void);
void np_opened(void);
void ui_apply_saved(void);

#endif
