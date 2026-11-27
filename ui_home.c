// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): the home screen, the quick settings shade, applications and the lock screen. */
#include "ui_m3_priv.h"

/* ------------------------------------------------------------- home */

lv_obj_t *clock_lbl, *date_lbl, *batt_lbl, *status_time;
lv_obj_t *home_card, *home_clock, *home_date, *home_batt, *home_net;	/* the widget */
lv_obj_t *qs_wifi_sub, *qs_bt_sub, *qs_wifi, *qs_bt;
lv_obj_t *set_wifi_sub, *set_bt_sub;	/* on the Settings page */

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
	/*
	 * The bolt while on external power, charging or not: plugged in and
	 * topped up the battery reads "Full" (or "Not charging"), and the bar
	 * looked as on battery.
	 */
	snprintf(buf, len, "%s%d%%  %s",
		 !strncmp(st, "Charging", 8) ||
		 read_sysfs_int("/sys/class/power_supply/usb/online", 0) == 1 ? LV_SYMBOL_CHARGE " " : "",
		 cap, icon);
}

enum batt_lvl battery_cap_level(void)
{
	char buf[24] = "";
	FILE *f = fopen("/sys/class/power_supply/battery/capacity_level", "r");

	if (!f)
		return BATT_NORMAL;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	buf[strcspn(buf, "\r\n")] = 0;
	if (!strcmp(buf, "Critical"))
		return BATT_CRITICAL;
	if (!strcmp(buf, "Low"))
		return BATT_LOW;
	return BATT_NORMAL;
}

static bool battery_warn(void)
{
	return !usb_powered() && battery_cap_level() != BATT_NORMAL;
}

static void battery_style_label(lv_obj_t *lbl, lv_color_t normal_col, bool warn)
{
	lv_color_t c;

	if (!lbl || !lv_obj_is_valid(lbl))
		return;
	c = warn ? M.error : normal_col;
	if (!lv_color_eq(lv_obj_get_style_text_color(lbl, 0), c))
		lv_obj_set_style_text_color(lbl, c, 0);
}

static lv_obj_t *crit_banner, *crit_lbl;
static lv_timer_t *crit_timer;
static int crit_secs;

static void battery_crit_stop(void)
{
	if (crit_timer) {
		lv_timer_delete(crit_timer);
		crit_timer = NULL;
	}
	if (crit_banner && lv_obj_is_valid(crit_banner))
		lv_obj_delete(crit_banner);
	crit_banner = NULL;
	crit_lbl = NULL;
}

static void battery_crit_tick(lv_timer_t *t)
{
	char msg[80];

	LV_UNUSED(t);
	if (usb_powered() || battery_cap_level() != BATT_CRITICAL) {
		battery_crit_stop();
		return;
	}
	if (--crit_secs <= 0) {
		battery_crit_stop();
		sync();
		reboot(RB_POWER_OFF);
		return;
	}
	if (crit_lbl && lv_obj_is_valid(crit_lbl)) {
		snprintf(msg, sizeof(msg),
			 LV_SYMBOL_WARNING "  Battery critically low — shutting down in %d s",
			 crit_secs);
		label_set_if_changed(crit_lbl, msg);
	}
}

void battery_crit_check(void)
{
	const char *no_off = getenv("N72_NO_CRIT_OFF");

	if (usb_powered() || battery_cap_level() != BATT_CRITICAL ||
	    (no_off && strcmp(no_off, "0") != 0)) {
		battery_crit_stop();
		return;
	}
	if (crit_timer)
		return;
	crit_secs = 10;
	crit_banner = lv_obj_create(lv_layer_top());
	plain(crit_banner);
	obj_track(&crit_banner);
	lv_obj_set_size(crit_banner, LV_MIN(W - 32, 288), LV_SIZE_CONTENT);
	lv_obj_align(crit_banner, LV_ALIGN_TOP_MID, 0, 48);
	lv_obj_set_style_radius(crit_banner, 18, 0);
	lv_obj_set_style_bg_color(crit_banner, M.error, 0);
	lv_obj_set_style_bg_opa(crit_banner, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(crit_banner, 14, 0);
	lv_obj_remove_flag(crit_banner, LV_OBJ_FLAG_SCROLLABLE);
	crit_lbl = m3_label(crit_banner,
			    LV_SYMBOL_WARNING "  Battery critically low — shutting down in 10 s",
			    &lv_font_montserrat_14, lv_color_black());
	obj_track(&crit_lbl);
	lv_obj_set_width(crit_lbl, lv_pct(100));
	lv_label_set_long_mode(crit_lbl, LV_LABEL_LONG_WRAP);
	crit_timer = lv_timer_create(battery_crit_tick, 1000, NULL);
}

lv_timer_t *home_timer;
bool app_dormant;
static bool dormant_status_dirty;

/*
 * A status bar's contents, the same on home and in the bar that comes down
 * over applications and pages: the time on the left; Wi-Fi, Bluetooth and
 * the battery on the right, in a row with fixed gaps (an icon that is not
 * shown takes its gap with it).
 */

void status_row(lv_obj_t *bar, lv_obj_t **time, lv_obj_t **wifi, lv_obj_t **bt,
		       lv_obj_t **batt)
{
	lv_obj_t *sp;

	lv_obj_set_style_pad_hor(bar, STATUS_PAD, 0);
	lv_obj_set_style_pad_ver(bar, 0, 0);
	lv_obj_set_style_pad_column(bar, STATUS_GAP, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	*time = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface);
	sp = lv_obj_create(bar);		/* pushes the rest to the right */
	lv_obj_remove_style_all(sp);
	lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(sp, 0, 1);
	lv_obj_set_flex_grow(sp, 1);
	*wifi = m3_label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, M.on_surface_v);
	*bt = m3_label(bar, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_14, M.on_surface_v);
	*batt = m3_label(bar, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_add_flag(*wifi, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(*bt, LV_OBJ_FLAG_HIDDEN);
}

static void qs_style(lv_obj_t *tile, bool on)
{
	lv_color_t c = on ? M.primary_c : M.surface_c_high;

	if (!tile)
		return;

	if (!lv_color_eq(lv_obj_get_style_bg_color(tile, 0), c)) {
		lv_obj_set_style_bg_color(tile, c, 0);
		shade_changed(tile);
	}
}

/*
 * Wi-Fi and Bluetooth as every view shows them (the shade's tiles, the
 * Settings rows, the Wi-Fi and Bluetooth pages, the status bar), all from
 * one state and all updated together (net_ui_sync). While a switch is
 * being carried out (*_want >= 0) the views show where it is going, and
 * status reads that land meanwhile cannot flip them back.
 */
int wifi_want = -1, bt_want = -1;	/* -1: no switch in progress */
lv_obj_t *wifi_switch;
extern lv_obj_t *bt_switch, *bt_state_lbl;
lv_obj_t *status_wifi, *status_bt, *app_bar_wifi, *app_bar_bt;
/* the lock screen's bar (lock_build): its icons follow home's */
lv_obj_t *lock_bar_time, *lock_bar_wifi, *lock_bar_bt, *lock_bar_batt, *lock_bar_np;

bool wifi_shown_on(void)
{
	return wifi_want >= 0 ? wifi_want : wifi_powered();
}

static bool bt_shown_on(void)
{
	return bt_want >= 0 ? bt_want : bt_powered;
}

void switch_show(lv_obj_t *sw, bool on)
{
	if (!sw || !lv_obj_is_valid(sw) || lv_obj_has_state(sw, LV_STATE_CHECKED) == on)
		return;
	if (on)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

static void label_show(lv_obj_t *l, const char *text)
{
	if (l && lv_obj_is_valid(l))
		label_set_if_changed(l, text);
}

static void icon_show(lv_obj_t *l, bool on)
{
	if (!l || lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN) == !on)
		return;
	if (on)
		lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
	shade_changed(l);
	bp_changed(lv_obj_get_screen(l));	/* (pictures of home: the status icons) */
}

/* The sound device connected (n72-audio-route's /run/n72-audio), if any. */
static bool bt_audio_name(char *name, size_t n)
{
	char line[96];
	FILE *f = fopen("/run/n72-audio", "r");
	bool found = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "name=", 5)) {
			snprintf(name, n, "%s", line + 5);
			name[strcspn(name, "\n")] = 0;
			found = name[0] != 0;
		}
	fclose(f);
	return found;
}

void net_ui_sync(void)
{
	bool w = wifi_shown_on(), b = bt_shown_on(), bt_tool = have_tool("bluetoothctl");
	const char *wsub = wifi_want == 1 && !wifi_powered() ? "Turning on..." :
			   !w ? "Off" : wifi_up ? wifi_ssid : "Not connected";
	char bt_dev[64];
	const char *bsub = !bt_tool ? "Not installed" :
			   bt_want == 1 && !bt_powered ? "Turning on..." :
			   bt_want == 0 && bt_powered ? "Turning off..." :
			   b && bt_audio_name(bt_dev, sizeof(bt_dev)) ? bt_dev : b ? "On" : "Off";

	qs_style(qs_wifi, w);
	qs_style(qs_bt, b);
	label_show(qs_wifi_sub, wsub);
	label_show(qs_bt_sub, bt_tool ? bsub : "Unavailable");
	label_show(set_wifi_sub, wsub);
	label_show(set_bt_sub, bsub);
	switch_show(wifi_switch, w);
	switch_show(bt_switch, b);
	label_show(bt_state_lbl, bsub);
	icon_show(status_wifi, w && wifi_up && wifi_want < 0);
	icon_show(status_bt, b && bt_want < 0);
	icon_show(app_bar_wifi, w && wifi_up && wifi_want < 0);
	icon_show(app_bar_bt, b && bt_want < 0);
	icon_show(lock_bar_wifi, w && wifi_up && wifi_want < 0);
	icon_show(lock_bar_bt, b && bt_want < 0);
	if (home_net)
		label_set_if_changed(home_net, wifi_up ? wifi_ssid : "Wi-Fi off");
	app_bar_update();
}

/*
 * The status bar follows events, not a poll: the kernel's power_supply
 * uevents (USB in or out, the battery gauge's new level: main.c's
 * uevent_ev) and rtnetlink's link and address messages for wlan0 (Wi-Fi
 * associated, lost, switched off), each read only when it comes; Bluetooth
 * through BlueZ's signals (bz_ev). An event marks what is stale and one
 * short timer reads it, so a burst (an association sends several) costs
 * one sysfs read or one wpa_cli. While the screen is dark only the marks
 * are kept: the wake path's home_update reads what they name.
 */
static bool status_batt_dirty, status_net_dirty;
static lv_timer_t *status_ev_timer;
static int rtnl_fd = -1;

static void status_batt_update(void)
{
	char buf[64];
	bool warn = battery_warn();

	battery_text(buf, sizeof(buf));
	label_set_if_changed(batt_lbl, buf);
	battery_style_label(batt_lbl, home_wp ? WP_TEXT : M.on_surface_v, warn);
	if (home_batt) {
		label_set_if_changed(home_batt, buf);
		battery_style_label(home_batt, M.on_surface_v, warn);
	}
	battery_crit_check();
	app_bar_update();
}

static void status_ev_run(lv_timer_t *t)
{
	LV_UNUSED(t);
	status_ev_timer = NULL;
	if (getenv("N72_STATUS_LOG"))
		fprintf(stderr, "n72-ui: status event: battery %d, Wi-Fi %d%s\n", status_batt_dirty,
			status_net_dirty, screen_off ? " (dark: on wake)" : "");
	if (screen_off || app_dormant)
		return;
	if (status_batt_dirty) {
		status_batt_dirty = false;
		status_batt_update();
	}
	if (status_net_dirty) {
		status_net_dirty = false;
		status_refresh(false);	/* (wifi_status_done: net_ui_sync) */
	}
}

void status_kick(bool batt, bool net)
{
	status_batt_dirty |= batt;
	status_net_dirty |= net;
	if (screen_off || app_dormant || status_ev_timer)
		return;
	status_ev_timer = lv_timer_create(status_ev_run, net ? 300 : 100, NULL);
	lv_timer_set_repeat_count(status_ev_timer, 1);
}

/* Is this rtnetlink message about wlan0 (by name for links, by index for addresses)? */
static bool rtnl_is_wlan(struct nlmsghdr *h, unsigned int wl_idx)
{
	if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK) {
		struct ifinfomsg *ifi = NLMSG_DATA(h);
		int len = (int)IFLA_PAYLOAD(h);

		for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, len); a = RTA_NEXT(a, len))
			if (a->rta_type == IFLA_IFNAME)
				return !strcmp(RTA_DATA(a), "wlan0");
		return wl_idx && (unsigned int)ifi->ifi_index == wl_idx;
	}
	if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR) {
		struct ifaddrmsg *ifa = NLMSG_DATA(h);

		return wl_idx && ifa->ifa_index == wl_idx;
	}
	return false;
}

static void rtnl_ev(int fd)
{
	char buf[4096] __attribute__((aligned(4)));
	unsigned int wl_idx = if_nametoindex("wlan0");
	bool wl = false;
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		int len = (int)n;

		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, len);
		     h = NLMSG_NEXT(h, len))
			wl |= rtnl_is_wlan(h, wl_idx);
	}
	if (wl)
		status_kick(false, true);
}

void rtnl_init(void)
{
	struct sockaddr_nl a = { .nl_family = AF_NETLINK,
				 .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR };
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);

	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		fd = -1;
	}
	rtnl_fd = fd;
	if (fd >= 0)
		ui_watch(fd, rtnl_ev);
}

void home_update(lv_timer_t *t)
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

	if (screen_off || app_dormant) {
		dormant_status_dirty = true;
		return;		/* (the wake path calls this again) */
	}
	/* (Wi-Fi: rtnetlink's events mark it; a poll only without them) */
	if (rtnl_fd >= 0 ? status_net_dirty : now - last_status >= 30 || now < last_status) {
		status_net_dirty = false;
		status_refresh(false);	/* lands in a moment: wifi_status_done */
		last_status = now;
	}
	status_batt_dirty = false;	/* (read below) */
	localtime_r(&now, &tm);
	/* No seconds on screen: run again just after the next minute starts. */
	if (t)
		lv_timer_set_period(t, (60 - tm.tm_sec) * 1000 + 50);
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
	{
		bool warn = battery_warn();

		battery_style_label(batt_lbl, home_wp ? WP_TEXT : M.on_surface_v, warn);
		if (home_batt) {
			label_set_if_changed(home_batt, buf);
			battery_style_label(home_batt, M.on_surface_v, warn);
		}
	}
	battery_crit_check();
	net_ui_sync();
}

void open_settings(lv_event_t *e);
void open_wallpaper(lv_event_t *e);
void open_logs(lv_event_t *e);
void open_wifi(lv_event_t *e);
void open_bt(lv_event_t *e);
void ui_reopen(void);

/* A quick-settings tile: rounded rectangle, icon, title and state line. */
static lv_obj_t *qs_tile(lv_obj_t *parent, const char *icon, const char *title,
			 lv_obj_t **sub, lv_event_cb_t cb)
{
	lv_obj_t *t = lv_obj_create(parent), *col;

	plain(t);
	int32_t tw = (W - 2 * STATUS_PAD - 8 * (QS_COLS - 1)) / QS_COLS;	/* 8 apart */

	lv_obj_set_size(t, tw, SHADE_TILE_H);
	lv_obj_set_style_radius(t, 20, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(t, 14, 0);
	lv_obj_set_style_pad_ver(t, 0, 0);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(t, 10, 0);
	m3_label(t, icon, icon_font(icon, false), M.on_surface);
	col = lv_obj_create(t);
	plain(col);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_size(col, tw - 28 - 20 - 10, LV_SIZE_CONTENT);	/* (padding, icon, gap) */
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	m3_label(col, title, &lv_font_montserrat_14, M.on_surface);
	*sub = m3_label(col, "", &lv_font_montserrat_12, M.on_surface_v);
	lv_label_set_long_mode(*sub, LV_LABEL_LONG_DOT);
	lv_obj_set_size(*sub, tw - 28 - 20 - 10, 16);
	lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
	return t;
}

/* An app tile on the home screen. */
/*
 * An application's own picture (a manifest's image=): .n72i, "N72I", u16
 * width, u16 height (little endian), then width x height pixels as B, G, R,
 * A bytes (LVGL's ARGB8888, straight alpha). Loaded once per path; the
 * tiles' reload (apps_reload) lets them all go.
 */
struct app_icon {
	char path[160];
	lv_image_dsc_t dsc;
	lv_color_t avg;		/* its colour, for the card of the open animation */
};
#define APP_ICONS 48
static struct app_icon app_icons[APP_ICONS];
static int app_nicons;

static struct app_icon *app_icon_load(const char *path)
{
	struct app_icon *ic;
	uint8_t hd[8], *px;
	uint32_t w, h, n, i, r = 0, g = 0, b = 0, a = 0;
	FILE *f;

	for (i = 0; i < (uint32_t)app_nicons; i++)
		if (!strcmp(app_icons[i].path, path))
			return &app_icons[i];
	if (app_nicons >= APP_ICONS || !(f = fopen(path, "rb")))
		return NULL;
	if (fread(hd, 1, 8, f) != 8 || memcmp(hd, "N72I", 4)) {
		fclose(f);
		return NULL;
	}
	w = hd[4] | hd[5] << 8;
	h = hd[6] | hd[7] << 8;
	n = w * h * 4;
	if (!w || !h || w > 256 || h > 256 || !(px = malloc(n))) {
		fclose(f);
		return NULL;
	}
	if (fread(px, 1, n, f) != n) {
		fclose(f);
		free(px);
		return NULL;
	}
	fclose(f);
	for (i = 0; i < n; i += 4) {	/* (weighted by alpha: the corners do not count) */
		b += px[i] * px[i + 3];
		g += px[i + 1] * px[i + 3];
		r += px[i + 2] * px[i + 3];
		a += px[i + 3];
	}
	ic = &app_icons[app_nicons++];
	memset(ic, 0, sizeof(*ic));
	snprintf(ic->path, sizeof(ic->path), "%s", path);
	ic->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	ic->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
	ic->dsc.header.w = w;
	ic->dsc.header.h = h;
	ic->dsc.header.stride = w * 4;
	ic->dsc.data_size = n;
	ic->dsc.data = px;
	ic->avg = a ? lv_color_make(r / a, g / a, b / a) : lv_color_black();
	return ic;
}

static void app_icons_free(void)
{
	for (int i = 0; i < app_nicons; i++) {
		lv_image_cache_drop(&app_icons[i].dsc);
		free((void *)app_icons[i].dsc.data);
	}
	app_nicons = 0;
}

void app_tile(lv_obj_t *parent, const char *icon, const char *name,
		     lv_color_t bg, lv_color_t fg, lv_event_cb_t cb);

/*
 * A tile with the application's picture where the symbol badge is: a clear
 * 60 px box (its background colour the picture's, for the open animation's
 * card) with the picture in it, scaled down only if larger.
 */
static void app_tile_img(lv_obj_t *parent, struct app_icon *ic, const char *name,
			 lv_event_cb_t cb)
{
	lv_obj_t *t, *box, *im;
	uint32_t before = lv_obj_get_child_count(parent);
	int32_t side = LV_MAX(ic->dsc.header.w, ic->dsc.header.h);

	app_tile(parent, "", name, ic->avg, lv_color_white(), cb);
	if (lv_obj_get_child_count(parent) <= before)
		return;
	t = lv_obj_get_child(parent, before);
	box = lv_obj_get_child(t, 0);
	lv_obj_clean(box);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	im = lv_image_create(box);
	lv_image_set_src(im, &ic->dsc);
	if (side > 60)
		lv_image_set_scale(im, 256 * 60 / side);
	lv_obj_center(im);
}

/*
 * Icon shadows, over a wallpaper only (Material 3 shows depth by tone on a
 * plain background): one soft shadow of the 60 px badge (radius 20), made
 * once by blurring its shape (three box passes, ~ a gaussian of 5 px),
 * 3 px down. Each tile draws it under its badge from its own draw event,
 * so it costs a small image blend when a page is drawn, nothing a frame
 * while pages slide (it is in their pictures). Darker over a light picture,
 * lighter over a dark one, where it would not show anyway (mean luma under
 * the grid, wallpaper.meta).
 */
#define SHADOW_R	10	/* blur reach around the badge */
#define SHADOW_DY	3

lv_draw_buf_t *icon_shadow;

static void blur_box(uint8_t *a, uint8_t *tmp, int w, int h, int r)
{
	int x, y, k;

	for (y = 0; y < h; y++) {		/* rows */
		int sum = 0;

		for (k = -r; k <= r; k++)
			sum += a[y * w + LV_CLAMP(0, k, w - 1)];
		for (x = 0; x < w; x++) {
			tmp[y * w + x] = (uint8_t)(sum / (2 * r + 1));
			sum += a[y * w + LV_MIN(x + r + 1, w - 1)] - a[y * w + LV_MAX(x - r, 0)];
		}
	}
	for (x = 0; x < w; x++) {		/* columns */
		int sum = 0;

		for (k = -r; k <= r; k++)
			sum += tmp[LV_CLAMP(0, k, h - 1) * w + x];
		for (y = 0; y < h; y++) {
			a[y * w + x] = (uint8_t)(sum / (2 * r + 1));
			sum += tmp[LV_MIN(y + r + 1, h - 1) * w + x] - tmp[LV_MAX(y - r, 0) * w + x];
		}
	}
}

static lv_draw_buf_t *icon_shadow_make(void)
{
	const int side = 60, rad = 20, n = side + 2 * SHADOW_R;
	uint8_t *a = calloc(n * n, 1), *tmp = malloc(n * n);
	int x, y, i, peak = 40 + 70 * LV_CLAMP(0, wp.luma[WP_HOME], 255) / 255;	/* 16..43 % */
	lv_draw_buf_t *b = NULL;

	if (!a || !tmp)
		goto out;
	for (y = 0; y < side; y++)		/* the badge's shape (anti-aliased corners) */
		for (x = 0; x < side; x++) {
			float dx = LV_MAX(0, LV_MAX(rad - x - 0.5f, x + 0.5f - (side - rad))),
			      dy = LV_MAX(0, LV_MAX(rad - y - 0.5f, y + 0.5f - (side - rad)));
			float d = sqrtf(dx * dx + dy * dy) - rad;

			a[(y + SHADOW_R) * n + x + SHADOW_R] = d <= -0.5f ? 255 : d >= 0.5f ? 0 :
				(uint8_t)(255 * (0.5f - d));
		}
	for (i = 0; i < 3; i++)
		blur_box(a, tmp, n, n, 3);
	b = lv_draw_buf_create(n, n, LV_COLOR_FORMAT_A8, 0);
	if (!b)
		goto out;
	for (y = 0; y < n; y++) {
		uint8_t *row = b->data + (size_t)y * b->header.stride;

		for (x = 0; x < n; x++)
			row[x] = (uint8_t)(a[y * n + x] * peak / 255);
	}
out:
	free(a);
	free(tmp);
	return b;
}

static void tile_shadow_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_current_target(e), *badge;
	lv_draw_image_dsc_t d;
	lv_area_t a;

	if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
		lv_event_set_ext_draw_size(e, SHADOW_R);
		return;
	}
	if (!icon_shadow || !(badge = lv_obj_get_child(t, 0)))
		return;
	lv_obj_get_coords(badge, &a);
	lv_area_set(&a, a.x1 - SHADOW_R, a.y1 - SHADOW_R + SHADOW_DY,
		    a.x1 - SHADOW_R + icon_shadow->header.w - 1,
		    a.y1 - SHADOW_R + SHADOW_DY + icon_shadow->header.h - 1);
	lv_draw_image_dsc_init(&d);
	d.src = icon_shadow;
	d.recolor = lv_color_black();
	lv_draw_image(lv_event_get_layer(e), &d, &a);
}

static void tile_name_free(lv_event_t *e)
{
	free(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

void app_tile(lv_obj_t *parent, const char *icon, const char *name,
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
	{
		/*
		 * The press shows after 150 ms, as iOS hands a touch to the
		 * icons only then (UIScrollView delaysContentTouches): a swipe
		 * of the pages (10 px) takes it first and no icon flinches.
		 */
		static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X,
							  LV_STYLE_TRANSFORM_SCALE_Y, 0 };
		static lv_style_transition_dsc_t late;
		static bool init;

		if (!init) {
			lv_style_transition_dsc_init(&late, props, lv_anim_path_step, 1, 150, NULL);
			init = true;
		}
		lv_obj_set_style_transition(t, &late, LV_STATE_PRESSED);
	}
	if (home_wp) {
		if (!icon_shadow)
			icon_shadow = icon_shadow_make();
		lv_obj_add_event_cb(t, tile_shadow_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
		lv_obj_add_event_cb(t, tile_shadow_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
		lv_obj_refresh_ext_draw_size(t);
	}
	{
		/* a long name ends in an ellipsis, as on iOS */
		lv_obj_t *l = m3_label(t, name, &lv_font_montserrat_14, home_wp ? WP_TEXT : M.on_surface);

		lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
		/* one line: the dots need a height to cut at (unbounded, a long
		 * name wrapped, and the tile, centred, moved its badge up) */
		lv_obj_set_size(l, 74, lv_font_get_line_height(&lv_font_montserrat_14));
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	}
	lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
	/* its name, kept (the label's own text may be cut to "..." at any time) */
	lv_obj_set_user_data(t, strdup(name));
	lv_obj_add_event_cb(t, tile_name_free, LV_EVENT_DELETE, NULL);
}

lv_obj_t *home_bright, *shade, *scrim, *qs_settings_btn;
bool shade_open;

/* Screen back on: bring the clock up to date now, not at the next run. */
/*
 * The clock set (Date & Time, chrony's first step after the network came
 * up): the shown time follows at once, not at the minute timer's next run.
 * A timerfd armed years ahead with TFD_TIMER_CANCEL_ON_SET turns readable
 * only then: nothing wakes in between.
 */
static int clockset_fd = -1;

static void clockset_arm(void)
{
	struct itimerspec it = { .it_value = { .tv_sec = time(NULL) + 10L * 365 * 86400 } };

	if (timerfd_settime(clockset_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, NULL) &&
	    errno != ECANCELED)
		fprintf(stderr, "n72-ui: clock-set timer: %s\n", strerror(errno));
}

static void clockset_ev(int fd)
{
	uint64_t n;

	if (read(fd, &n, sizeof(n)) < 0 && errno != ECANCELED && errno != EAGAIN)
		return;
	clockset_arm();
	if (getenv("N72_STATUS_LOG"))
		fprintf(stderr, "n72-ui: status event: clock set\n");
	if (home_timer)
		lv_timer_ready(home_timer);	/* (dark: paused, the wake runs it) */
}

void clockset_init(void)
{
	clockset_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	if (clockset_fd < 0)
		return;
	clockset_arm();
	ui_watch(clockset_fd, clockset_ev);
}

void home_wake(void)
{
	if (home_timer)
		lv_timer_ready(home_timer);
	bt_auto_kick();
	lock_wake();
}

static void home_bright_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));

	ui_auto_bright_off();
	backlight_set(v);
	conf.bright = v;
	conf_save_later();
	shade_changed(home_bright);
}

/* ------------------------------------------------- quick settings shade */

/* 312 upright (two tiles a row, two rows), 240 turned (three in one row) */
int32_t shade_h = 312;
#define SHADE_H shade_h
int32_t shade_base_h = 312, shade_card_y;	/* (shade_relayout) */
lv_obj_t *shade_bright_row;

static void shade_closed(void)
{
	lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	/* opened over a page it was on the top layer: home's again */
	if (app_pid <= 0 && lv_obj_get_parent(shade) != home_scr) {
		lv_obj_set_parent(scrim, home_scr);
		lv_obj_set_parent(shade, home_scr);
	}
}

/* Over a page the shade comes down on the top layer, as over applications. */
static void shade_to_top(void)
{
	if (lv_obj_get_parent(shade) == lv_layer_top())
		return;
	lv_obj_set_parent(scrim, lv_layer_top());
	lv_obj_set_parent(shade, lv_layer_top());
}

/*
 * Open or close the shade. Animated: a picture of home (with the scrim,
 * without the shade) and a picture of the shade, moved by the GPU.
 */
static bool sd_animate(bool open);

void shade_set(bool open, bool anim)
{
	lv_draw_buf_t *bg, *fg;

	if (!shade || open == shade_open)
		return;
	if (open && app_pid > 0 && lv_screen_active() == app_scr) {
		if (app_dormant)
			app_dormant_wake(false);
		bz_ensure();
	}
	tr_finish();
	if (open && on_page())
		shade_to_top();
	shade_open = open;
	if (open) {
		lv_slider_set_value(home_bright, backlight_get(), LV_ANIM_OFF);
		home_wake();
		np_opened();
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
	if (!open) {
		shade_closed();
		app_dormant_sync();
	}
}

/* Swipe up closes an open shade on home (opening is drag-down from the status bar). */
void home_gesture_cb(lv_event_t *e)
{
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());

	LV_UNUSED(e);
	if (d == LV_DIR_TOP && shade_open)
		shade_set(false, true);
}

void shade_tap_cb(lv_event_t *e)
{
	shade_set(lv_event_get_user_data(e) != NULL, true);
}

/*
 * The tiles are switches: Wi-Fi disconnects or reconnects (wpa_supplicant
 * keeps the network; the radio stays up), Bluetooth powers the controller
 * off or on as its page does. The tile changes at once; the real state
 * follows from the status read a moment later.
 */
void bt_set(bool on);

void wifi_page_refresh(void);

static void wifi_set_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	wifi_want = -1;		/* the card is there or gone now (sysfs) */
	status_refresh(false);
	net_ui_sync();
	wifi_page_refresh();
}

/* Wi-Fi on or off from any switch; a second one waits for the first. */
void wifi_set(bool on)
{
	if (wifi_want >= 0 || !have_tool("n72-wifi")) {
		net_ui_sync();		/* (puts a switch that moved back) */
		return;
	}
	if (conf.wifi != on) {
		conf.wifi = on;
		conf_save();
	}
	if (on == wifi_powered())
		return;
	wifi_want = on;
	if (!on)
		wifi_up = false;
	net_ui_sync();
	/* (on: the card's power-up, firmware and join take a few seconds) */
	job_run(on ? "n72-wifi on; sleep 4" : "n72-wifi off", NULL, wifi_set_done, NULL);
}

static void qs_open_wifi(lv_event_t *e)
{
	LV_UNUSED(e);
	wifi_set(!wifi_shown_on());
}

static void qs_open_bt(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!have_tool("bluetoothctl"))
		return;
	bt_set(!bt_shown_on());
}

static void page_open_from(lv_obj_t *tile, void (*open)(lv_event_t *));
lv_obj_t *settings_tile;

static void qs_to_page(void (*open)(lv_event_t *));

/* The gear and power buttons at the top right of the shade. */
static void qs_settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	qs_to_page(open_settings);
}

static void qs_power_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	qs_to_page(open_power);
}

lv_obj_t *qs_rot, *qs_rot_sub, *qs_night, *qs_night_sub;
void rot_conf_changed(void);

int ui_rot;			/* the screen's turn now */

/* The Auto-rotate tile: lit while on; below, the way the screen is now. */
void qs_rot_sync(void)
{
	if (!qs_rot)
		return;
	qs_style(qs_rot, conf.autorot == ROT_AUTO);
	label_set_if_changed(qs_rot_sub, ui_rot ? "Landscape" : "Portrait");
	shade_changed(qs_rot);
}

/* The Night Shift tile: lit while on; subtitle shows warmth % or Off. */
void qs_night_sync(void)
{
	char buf[16];

	if (!qs_night)
		return;
	qs_style(qs_night, conf.night_shift);
	if (conf.night_shift)
		snprintf(buf, sizeof(buf), "%d%%", conf.night_warmth);
	else
		snprintf(buf, sizeof(buf), "Off");
	label_set_if_changed(qs_night_sub, buf);
	shade_changed(qs_night);
}

static void qs_night_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	conf.night_shift = !conf.night_shift;
	display_night_apply();
	conf_save();
	qs_night_sync();
	ui_night_sync_widgets();
}

/* A tap: on, or off held the way it is now. */
static void qs_rot_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	conf.autorot = conf.autorot == ROT_AUTO ? ROT_LOCKED : ROT_AUTO;
	if (conf.autorot == ROT_LOCKED)
		conf.rotlock = ui_rot;
	conf_save();
	qs_rot_sync();
	rot_conf_changed();
}

static void qs_sleep_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	shade_set(false, false);
	screen_off_or_sleep();
}

void build_shade(void)
{
	lv_obj_t *row, *h, *qs_sleep, *sleep_sub;
	bool land = W > H;
	int32_t tiles_y = land ? 62 : 70;
	int32_t bright_gap = land ? 10 : 12;

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

	clock_lbl = m3_label(shade, "", &lv_font_montserrat_24, M.on_surface);
	lv_obj_align(clock_lbl, LV_ALIGN_TOP_LEFT, STATUS_PAD, land ? 10 : 14);
	{
		/* Power options and Settings: round tonal buttons at the top right */
		lv_obj_t *b = lv_button_create(shade);

		plain(b);
		lv_obj_set_size(b, 40, 40);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, M.surface_c_highest, 0);
		lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -(STATUS_PAD + 40 + 8), land ? 12 : 16);
		lv_obj_add_event_cb(b, qs_power_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_center(m3_label(b, LV_SYMBOL_POWER, &lv_font_montserrat_20, M.on_surface));

		b = lv_button_create(shade);
		plain(b);
		lv_obj_set_size(b, 40, 40);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, M.surface_c_highest, 0);
		lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -STATUS_PAD, land ? 12 : 16);
		lv_obj_add_event_cb(b, qs_settings_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_center(m3_label(b, LV_SYMBOL_SETTINGS, &lv_font_montserrat_20, M.on_surface));
		qs_settings_btn = b;
	}
	date_lbl = m3_label(shade, "", &lv_font_montserrat_14, M.primary);
	lv_label_set_long_mode(date_lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(date_lbl, W - 2 * STATUS_PAD - 40 - 8 - 40 - 12);
	lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, STATUS_PAD + 1, land ? 38 : 43);

	/* the tiles: a grid, QS_COLS a row, 8 apart */
	{
		bool has_night = mbx_gpu_gamma_supported();
		int ntiles = 4 + (has_night ? 1 : 0);
		int rows = (ntiles + QS_COLS - 1) / QS_COLS;
		int32_t th = rows * SHADE_TILE_H + (rows - 1) * 8;

		row = lv_obj_create(shade);
		plain(row);
		lv_obj_set_size(row, W - 2 * STATUS_PAD, th);
		lv_obj_align(row, LV_ALIGN_TOP_MID, 0, tiles_y);
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_set_style_pad_all(row, 0, 0);
		lv_obj_set_style_pad_column(row, 8, 0);
		lv_obj_set_style_pad_row(row, 8, 0);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		qs_wifi = qs_tile(row, LV_SYMBOL_WIFI, "Wi-Fi", &qs_wifi_sub, qs_open_wifi);
		qs_bt = qs_tile(row, LV_SYMBOL_BLUETOOTH, "Bluetooth", &qs_bt_sub, qs_open_bt);
		qs_rot = qs_tile(row, LV_SYMBOL_REFRESH, "Auto-rotate", &qs_rot_sub, qs_rot_cb);
		qs_rot_sync();
		if (has_night) {
			qs_night = qs_tile(row, LV_SYMBOL_TINT, "Night Shift", &qs_night_sub,
					   qs_night_cb);
			qs_night_sync();
		}
		qs_sleep = qs_tile(row, N72_SYMBOL_SLEEP, "Sleep", &sleep_sub, qs_sleep_cb);
		lv_label_set_text(sleep_sub, "Screen off");
		qs_style(qs_sleep, false);
		lv_obj_set_style_bg_color(qs_sleep, M.surface_c_highest, LV_STATE_PRESSED);
		/* the slider below; the handle at the bottom (the background
		 * player's card, when there is one, between: shade_relayout) */
		shade_base_h = tiles_y + th + bright_gap + SHADE_BRIGHT_H + SHADE_FOOT_H;
		shade_card_y = tiles_y + th + 8;
		shade_h = shade_base_h;
		lv_obj_set_height(shade, shade_h);
		lv_obj_set_y(shade, -shade_h);
	}

	row = shade_bright_row = lv_obj_create(shade);
	plain(row);
	lv_obj_set_size(row, W - 2 * STATUS_PAD, SHADE_BRIGHT_H);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, shade_h - SHADE_FOOT_H - SHADE_BRIGHT_H);
	lv_obj_set_style_bg_color(row, M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(row, 18, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);
	m3_label(row, LV_SYMBOL_IMAGE, &lv_font_montserrat_16, M.on_surface_v);
	home_bright = row_slider(row, true);
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
	lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, land ? -12 : -14);
	/* (padding taller than the handle would make it scroll, with a scrollbar) */
	lv_obj_set_style_pad_all(h, 0, 0);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

	np_build(shade, shade_card_y);	/* (hidden while nothing plays: np_sync) */
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
 * "exec=<shell command>"; each becomes a tile on the home screen. Also
 * icon=<symbol name>, color=#rrggbb, or image=<path of a .n72i picture>
 * (app_icon_load). The tiles follow the folder (apps_watch).
 */
/* (tests: N72_APPS_DIR, another folder of manifests) */
#define APPS_DIR (getenv("N72_APPS_DIR") ? getenv("N72_APPS_DIR") : "/usr/share/n72-ui/apps")

lv_timer_t *app_timer;

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
#define APP_BAR_SLIDE_MS 350	/* iOS's status bar: 350 ms ease-in-out, as a picture */

lv_obj_t *app_bar, *app_bar_time, *app_bar_batt;
bool app_bar_shown;
lv_timer_t *app_bar_timer;

static void app_y(void *obj, int32_t v)
{
	lv_obj_set_y(obj, v);
}

static void app_hide_done(lv_anim_t *a)
{
	lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
	app_dormant_sync();
}

static void app_shade_done(lv_anim_t *a)
{
	LV_UNUSED(a);
	shade_closed();
	app_dormant_sync();
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
	lv_anim_set_path_cb(&a, ios_path_ease_out);
	if (done)
		lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

void app_shade_slide(bool open)
{
	app_slide(shade, open ? 0 : -SHADE_H, open ? NULL : app_shade_done);
}

void app_bar_set(bool show);

/*
 * What the bar shows, for its kept picture (psl_start_sig): it is taken
 * again only when this changes (the minute, the battery, the icons).
 */
static void app_bar_sig(char *buf, size_t len)
{
	snprintf(buf, len, "%s|%s|%d%d|%d", app_bar_time ? lv_label_get_text(app_bar_time) : "",
		 app_bar_batt ? lv_label_get_text(app_bar_batt) : "",
		 app_bar_wifi && !lv_obj_has_flag(app_bar_wifi, LV_OBJ_FLAG_HIDDEN),
		 app_bar_bt && !lv_obj_has_flag(app_bar_bt, LV_OBJ_FLAG_HIDDEN), (int)W);
}

static void app_bar_expire(lv_timer_t *t)
{
	LV_UNUSED(t);
	app_bar_timer = NULL;
	if (shade_open || sd.on || sd.hw || sd.scr) {
		app_bar_set(true);	/* again once the shade is closed */
		return;
	}
	app_bar_set(false);
}

void app_bar_set(bool show)
{
	if (!app_bar)
		return;
	if (app_bar_timer) {
		lv_timer_delete(app_bar_timer);
		app_bar_timer = NULL;
	}
	if (show) {
		if (app_dormant)
			app_dormant_wake(false);
		app_bar_timer = lv_timer_create(app_bar_expire, APP_BAR_MS, NULL);
		lv_timer_set_repeat_count(app_bar_timer, 1);
		if (app_bar_shown)
			return;
		home_update(NULL);
		if (!psl_start_sig(app_bar, 0, APP_BAR_SLIDE_MS, ios_path_ease_in_out, false, app_bar_sig)) {
			lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
			app_slide(app_bar, 0, NULL);
		}
	} else {
		if (!app_bar_shown)
			return;
		if (!psl_start_sig(app_bar, -APP_BAR_H, APP_BAR_SLIDE_MS, ios_path_ease_in_out, true, app_bar_sig))
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
void app_gesture_cb(lv_event_t *e)
{
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());

	LV_UNUSED(e);
	if (app_pid <= 0 && !on_page())
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

static void app_bar_kbd_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	app_bar_set(false);
	kbd_toggle_app();
}

void app_bar_build(void)
{
	if (app_bar)
		return;
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
	lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(app_bar, app_bar_touch_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* its own */
	status_row(app_bar, &app_bar_time, &app_bar_wifi, &app_bar_bt, &app_bar_batt);
	{	/* the keyboard, for an application that does not ask for it */
		lv_obj_t *b = lv_obj_create(app_bar);

		plain(b);
		lv_obj_set_size(b, 40, 32);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, M.surface_c_highest, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(b, M.secondary_c, LV_STATE_PRESSED);
		lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
		lv_obj_center(m3_label(b, LV_SYMBOL_KEYBOARD, &lv_font_montserrat_16, M.on_surface));
		lv_obj_add_event_cb(b, app_bar_kbd_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_move_to_index(b, 2);	/* after the time and the spacer */
	}
	net_ui_sync();
	lv_obj_add_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
	/* gestures on the bar, the shade and the scrim bubble up to here */
	lv_obj_add_event_cb(lv_layer_top(), app_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* The bar shows what the home screen's bar shows (home_update). */
void app_bar_update(void)
{
	if (!app_bar)
		return;
	label_set_if_changed(app_bar_time, lv_label_get_text(status_time));
	label_set_if_changed(app_bar_batt, lv_label_get_text(batt_lbl));
	battery_style_label(app_bar_batt, M.on_surface_v, battery_warn());
	/* showing as its picture: the bar itself if that is old now; hidden:
	 * the picture for the next slide, while nothing moves */
	psl_check(app_bar);
	/* (only over the application: elsewhere the bar is not needed) */
	if (!app_dormant && !mbx_gpu_is_suspended() && !app_bar_shown &&
	    lv_screen_active() == app_scr && !lv_anim_count_running() && !(touch_indev &&
	    lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		psl_warm(app_bar, app_bar_sig);
}

static bool in_obj(lv_obj_t *o, int x, int y)
{
	lv_area_t a;

	if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
		return false;
	lv_obj_get_coords(o, &a);
	return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

#include "kbd.c"		/* the system keyboard */
#include "weather.c"	/* the weather: Open-Meteo, on the home card and the lock screen */

/* Does a touch that begins at (x, y) belong to the launcher? */
bool ui_wants_touch(int x, int y)
{
	if (locked || app_pid <= 0 || lv_screen_active() != app_scr)
		return true;
	if (gp_up())
		return true;		/* a game's pause panel: all of the screen */
	if (kbd_wants(x, y))
		return true;
	if (app_is_video())
		return true;		/* the player has no touch input: its controls */
	if (shade_open || y < APP_EDGE_H)
		return true;
	if (app_bar_shown && y < APP_BAR_H)
		return true;
	return in_obj(vol_hud, x, y) || psl_covers(vol_hud, x, y);
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

struct sd sd;

void sd_free_pics(void)
{
	if (sd.hw) {
		mbx_gpu_hw_slide_end(false);
		sd.hw = false;
	}
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
lv_draw_buf_t *shade_pic;
bool shade_pic_dirty = true;
long shade_pic_min;

extern lv_timer_t *shade_warm_timer;

void shade_changed(lv_obj_t *o)
{
	for (; o; o = lv_obj_get_parent(o))
		if (o == shade) {
			shade_pic_dirty = true;
			/* the warmer, resting till the next minute, looks again soon */
			if (shade_warm_timer && shade_warm_timer->period != 1000) {
				lv_timer_set_period(shade_warm_timer, 1000);
				lv_timer_reset(shade_warm_timer);
			}
			return;
		}
}

lv_draw_buf_t *shade_pic_get(void)
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
	lv_obj_update_layout(shade);
	quiet_begin();
	lv_obj_set_y(shade, 0);
	shade_pic = snap(shade, LV_COLOR_FORMAT_ARGB8888);
	lv_obj_set_y(shade, y);
	quiet_end();
	shade_pic_dirty = false;
	shade_pic_min = bp_minute();
	return shade_pic;
}

void shade_pic_free(void)
{
	if (sd.sfg == shade_pic)
		return;
	tr_free(&shade_pic);
	shade_pic_dirty = true;
}

static lv_opa_t sd_scrim_opa(int32_t y)
{
	return (lv_opa_t)(LV_OPA_50 * (y + SHADE_H) / SHADE_H);
}

static void sd_set(int32_t y)
{
	sd.y = LV_CLAMP(-SHADE_H, y, 0);
	if (sd.hw) {
		mbx_gpu_hw_slide_move(sd.y);
		return;
	}
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
		/*
		 * Over an application (the launcher draws with alpha, on the
		 * CPU): the scrim at one opacity, so that a frame redraws only
		 * the strip the shade moves through. Following the shade, it
		 * changed the whole screen every frame: drawn and premultiplied
		 * whole, 18-23 ms a frame (31-38 fps) over a video.
		 */
		lv_obj_set_y(shade, sd.y);
		if (lv_obj_get_style_bg_opa(scrim, 0) != LV_OPA_50)
			lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);	/* (as settled) */
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
	if (sd.hw) {
		mbx_gpu_hw_slide_end(open);
		sd.hw = false;
		tr_free(&sd.sfg);
		sd.fg_cached = false;
		lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
		if (app_bar && app_bar_shown)
			lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
	}
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
		np_opened();
	} else {
		shade_closed();
		if ((app_pid > 0 || on_page()) && !sd.to_page)
			app_bar_set(true);	/* the bar's timeout starts again */
	}
	sd.to_page = false;
	if (!open)
		app_dormant_sync();
}

static void sd_hw_abort(void *owner)
{
	LV_UNUSED(owner);
	if (!sd.hw)
		return;
	lv_anim_delete(&sd, sd_anim_exec);
	sd.on = false;
	sd_end(sd.y > -SHADE_H / 2);
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

	if (sd.on || sd.scr || sd.hw ||
	    !(lv_screen_active() == home_scr || (app_scr && lv_screen_active() == app_scr) ||
	      on_page()))
		return false;
	shade_open = !open;	/* (sd_begin starts from where it is) */
	sd_begin(false);
	sd.on = false;
	if (!sd.scr && !sd.hw) {
		shade_open = open;
		return false;
	}
	lv_anim_init(&a);
	lv_anim_set_var(&a, &sd);
	lv_anim_set_exec_cb(&a, sd_anim_exec);
	lv_anim_set_values(&a, sd.y, open ? 0 : -SHADE_H);
	lv_anim_set_duration(&a, 240);
	lv_anim_set_path_cb(&a, ios_path_ease_out);
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
	bool home = lv_screen_active() == home_scr, page = on_page();
	lv_draw_buf_t *bg = NULL, *fg;

	if (!home && !page && app_pid > 0) {
		if (app_dormant)
			app_dormant_wake(false);
		bz_ensure();
	}

	mbx_gpu_hw_slide_abort(&sd);
	if (touch)
		ui_touch_lost();
	tr_finish();
	if (page) {
		/* the page as it is, under the shade (which is on the top layer) */
		shade_to_top();
		bg = snap(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
	}
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
	if (!page)
		tr_free(&sd.home_pic);
	fg = shade_pic_get();
	sd.fg_cached = fg != NULL;
	if (fg && !home && !page) {
		/*
		 * Over an application: take ownership of shade_pic (freed after
		 * the drag anyway, instead of 600 KiB dup) and premultiply it
		 * once in place so CPU/GPU copies (or hardware-scrolled layer 1)
		 * skip blending and per-frame premultiplication.
		 */
		if (fg == shade_pic) {
			shade_pic = NULL;
			shade_pic_dirty = true;
		}
		sd.fg_cached = false;
		mbx_gpu_premultiply_buf(fg);
		if (app_bar)
			lv_obj_add_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
		if (mbx_gpu_hw_slide_begin(fg, 0, sd.base, &sd, sd_hw_abort)) {
			sd.hw = true;
			sd.sfg = fg;
			sd.on = true;
			sd.v = 0;
			sd_set(sd.base);
			return true;
		}
		if (app_bar && app_bar_shown)
			lv_obj_remove_flag(app_bar, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(shade, LV_OBJ_FLAG_HIDDEN);
		if (shade_open)
			lv_obj_remove_flag(scrim, LV_OBJ_FLAG_HIDDEN);
	}
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
		if (app_bar)
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
	lv_anim_set_path_cb(&a, ios_path_ease_out);
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

lv_obj_t *lock_scr, *lock_time, *lock_date, *lock_batt;
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

void lock_update(void)
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
	if (lock_bar_batt && batt_lbl) {
		label_set_if_changed(lock_bar_batt, lv_label_get_text(batt_lbl));
		battery_style_label(lock_bar_batt, lv_color_hex(0xe8e0f0), battery_warn());
	}
	np_lock_sync();
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
	wp_put(lock_scr, WP_LOCK);	/* (a picture, if one is set, over the gradient) */

	/* the status bar, as home's: its time hidden (the clock below shows it) */
	o = lv_obj_create(lock_scr);
	plain(o);
	lv_obj_set_size(o, W, 40);
	lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(o, 0, 0);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	status_row(o, &lock_bar_time, &lock_bar_wifi, &lock_bar_bt, &lock_bar_batt);
	lv_obj_add_flag(lock_bar_time, LV_OBJ_FLAG_HIDDEN);
	lock_bar_np = m3_label(o, LV_SYMBOL_AUDIO, &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_move_to_index(lock_bar_np, (int32_t)lv_obj_get_index(lock_bar_wifi));
	lv_obj_add_flag(lock_bar_np, LV_OBJ_FLAG_HIDDEN);
	{
		lv_obj_t *const l[] = { lock_bar_np, lock_bar_wifi, lock_bar_bt, lock_bar_batt };

		for (unsigned i = 0; i < sizeof(l) / sizeof(l[0]); i++)
			lv_obj_set_style_text_color(l[i], lv_color_hex(0xe8e0f0), 0);
	}

	lock_date = m3_label(lock_scr, "", &lv_font_montserrat_20, lv_color_hex(0xe8e0f0));
	lv_obj_align(lock_date, LV_ALIGN_TOP_MID, 0, 64);
	lock_time = m3_label(lock_scr, "", clock_font ? clock_font : &lv_font_montserrat_48,
			     lv_color_white());
	lv_obj_align(lock_time, LV_ALIGN_TOP_MID, 0, 90);
	wx_lock_build(lock_scr, 196);	/* (hidden while there is none) */
	np_lock_build(lock_scr);	/* (hidden while nothing plays) */
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
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -12);
	net_ui_sync();		/* (the bar's icons as they are now) */
	np_sync();
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
void ui_lock(void)
{
	lv_obj_t *cur;

	if (locked || !home_scr)
		return;
	kbd_hide_now(false);	/* (the lock screen has no field) */
	za_finish();
	sd_settle_now();
	tr_finish();
	dock_carry_stop();
	pg_abort();
	if (shade_open)
		sd_end(false);
	app_bar_set(false);
	if (app_pid > 0)
		app_dormant_wake(true);
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
void lock_wake(void)
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
		rot_kick();	/* (unlocked: a turn may be due) */
		wp_apply_kick();
		lv_screen_load(lock_from && lv_obj_is_valid(lock_from) ? lock_from : home_scr);
		ul_free();
		if (lock_from == app_scr && app_pid > 0)
			mbx_gpu_set_transparent(true);
		else
			mbx_gpu_set_transparent(false);
		home_update(NULL);
		app_dormant_sync();
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
	/* back down: iOS's slide-to-unlock snap back (300 ms x the way, ease-in-out) */
	lv_anim_set_path_cb(&a, open ? ios_path_ease_out : ios_path_ease_in_out);
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
static bool dock_carry_touch(int phase, int x, int y);

bool ui_drag(int phase, int x, int y)
{
	uint32_t now = lv_tick_get();

	if (locked || ul.scr)
		return ul_drag(phase, y);
	if (dock_carry_touch(phase, x, y))	/* an icon carried (dock.c): its touch */
		return true;
	if (phase == 0 && app_bar && y < APP_BAR_H)
		psl_touch(app_bar);	/* (the bar itself takes its touches) */
	if (phase == 0 && vol_hud && psl_covers(vol_hud, x, y))
		psl_touch(vol_hud);
	if (phase == 0 && kbd.obj && kbd_wants(x, y))
		psl_touch(kbd.obj);
	if (pg_drag(phase, x, y))	/* sideways on home: the pages */
		return true;
	if (phase == 0) {
		bool home = lv_screen_active() == home_scr && !tr.scr;
		bool app = app_pid > 0 && lv_screen_active() == app_scr;
		bool page = on_page() && !tr.scr;

		sd_settle_now();
		sd.cand = 0;
		sd.px = x;
		sd.py = y;
		sd.last_y = y;
		sd.last_t = now;
		if (!shade || screen_off || (!home && !app && !page))
			return false;
		if (shade_open && !in_obj(home_bright, x, y))
			sd.cand = -1;
		else if (!shade_open && ((home && in_obj(home_bar, x, y)) ||
					   (app_bar_shown && y < APP_BAR_H)))
			sd.cand = 1;
		else if (page && !shade_open && y < APP_EDGE_H)
			sd.cand = 2;	/* the page's top edge: brings the bar */
		return false;
	}
	if (!sd.on) {
		int dy = y - sd.py, dx = x - sd.px;

		if (!sd.cand || phase == 2)
			return false;
		if (sd.cand == 2) {
			if (LV_ABS(dx) > SD_SLOP && LV_ABS(dx) > LV_ABS(dy)) {
				sd.cand = 0;
				return false;
			}
			if (dy <= SD_SLOP)
				return false;
			sd.cand = 0;
			ui_touch_lost();	/* (not the page's scroll or tap) */
			app_bar_set(true);
			return true;
		}
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
 * (GPU: the picture, a rounded fill) while the application starts below,
 * and clears to show it. Closing runs it backwards: the card comes up over
 * the application, then goes into its icon over home, and home's icon
 * shows through as it ends. The card over the application is drawn in the
 * launcher's transparent mode (one GPU fill a frame); the moves are XRGB,
 * on the GPU.
 *
 * The motion is a spring's, not a timed curve's (za_path_spring,
 * za_path_glide):
 * - Opening, the card is pulled to the screen by a critically damped
 *   spring: from rest, fastest after ~55 ms, 90 % of the way by ~210 ms,
 *   then a long soft landing. On the way its symbol dissolves (a glyph of
 *   fixed size on a growing card looks stiff) and its corners, a little
 *   rounder at first, square off as they near the screen's. It clears as
 *   soon as it has landed, fast first: the landing is the pause, there is
 *   no hold. (The application gets as long as before to start: the grow
 *   is longer by the hold that went.)
 * - Closing is shorter, as leaving should be. The card comes up at an
 *   even rate and leaves gently (a spring following a smoothed pull: no
 *   speed in the first frame, fastest after ~60 ms), its symbol back on
 *   the way; near the icon it melts into home's.
 * Nothing is drawn that was not before: a frame of a move is home's
 * picture, one rounded fill (its corners never larger than they were) and
 * the symbol, for less of the way.
 */
#define ZA_GROW_MS	380	/* spring, ZA_SPRING_K time constants long */
#define ZA_REVEAL_MS	180	/* the card clears: the application, the page */
#define ZA_COVER_MS	100	/* the card comes up over it */
#define ZA_SHRINK_MS	280	/* glide, ZA_GLIDE_K time constants long */
#define ZA_SPRING_K	7.0f
#define ZA_GLIDE_K	9.0f
#define ZA_SYM_FROM	384	/* the symbol dissolves between these (of 1024) */
#define ZA_SYM_TO	896
#define ZA_MELT		160	/* closing: the card melts into the icon below this */

struct za za;

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

	pg_fb0_dirty();		/* (the home wallpaper is no longer there) */
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

/*
 * The paths. Both are the step response of a damped spring, cut where it
 * has all but arrived (@k time constants) and scaled to end on the value:
 * time runs on the animation's clock, so a late frame shows where the
 * card is by then, not the next step.
 */
static int32_t za_path(const lv_anim_t *a, float k, bool glide)
{
	float x, fx, fk;

	if (a->act_time <= 0)
		return a->start_value;
	if (a->act_time >= a->duration)
		return a->end_value;
	x = k * (float)a->act_time / (float)a->duration;
	if (glide) {	/* the pull itself comes on smoothly: no speed, no force at 0 */
		fx = 1.0f - (1.0f + x + 0.5f * x * x) * expf(-x);
		fk = 1.0f - (1.0f + k + 0.5f * k * k) * expf(-k);
	} else {	/* critically damped: no speed at 0, no overshoot */
		fx = 1.0f - (1.0f + x) * expf(-x);
		fk = 1.0f - (1.0f + k) * expf(-k);
	}
	return a->start_value + (int32_t)lroundf((float)(a->end_value - a->start_value) * fx / fk);
}

static int32_t za_path_spring(const lv_anim_t *a)
{
	return za_path(a, ZA_SPRING_K, false);
}

static int32_t za_path_glide(const lv_anim_t *a)
{
	return za_path(a, ZA_GLIDE_K, true);
}

static void za_rect(int32_t v)	/* 0 = the icon, 1024 = the screen */
{
	int32_t x1 = za.icon.x1 + (0 - za.icon.x1) * v / 1024;
	int32_t y1 = za.icon.y1 + (0 - za.icon.y1) * v / 1024;
	int32_t x2 = za.icon.x2 + (W - 1 - za.icon.x2) * v / 1024;
	int32_t y2 = za.icon.y2 + (H - 1 - za.icon.y2) * v / 1024;

	lv_obj_set_pos(za.card, x1, y1);
	lv_obj_set_size(za.card, x2 - x1 + 1, y2 - y1 + 1);
	/* the icon's 20, a card's 26 half way, the screen's square corners */
	lv_obj_set_style_radius(za.card, v < 512 ? 20 + 6 * v / 512 : 26 * (1024 - v) / 512, 0);
}

/* The symbol's opacity on a card @v of the way: gone well before the screen. */
static lv_opa_t za_sym_opa(int32_t v)
{
	if (v <= ZA_SYM_FROM)
		return LV_OPA_COVER;
	if (v >= ZA_SYM_TO)
		return LV_OPA_TRANSP;
	return (lv_opa_t)(255 * (ZA_SYM_TO - v) / (ZA_SYM_TO - ZA_SYM_FROM));
}

/*
 * The card's opacity as its fill's and its symbol's own, not the object's:
 * object opacity renders the card into a layer of its own and blends it,
 * all on the CPU in the launcher's transparent mode; a plain translucent
 * fill is one GPU fill (mbx_gpu.c, mbx_fill_straight).
 */
static void za_look(lv_opa_t card, lv_opa_t sym)
{
	if (lv_obj_get_style_bg_opa(za.card, 0) != card)
		lv_obj_set_style_bg_opa(za.card, card, 0);
	if (lv_obj_check_type(za.sym, &lv_image_class)) {
		if (lv_obj_get_style_image_opa(za.sym, 0) != sym)
			lv_obj_set_style_image_opa(za.sym, sym, 0);
	} else if (lv_obj_get_style_text_opa(za.sym, 0) != sym) {
		lv_obj_set_style_text_opa(za.sym, sym, 0);
	}
}

static void za_exec(void *var, int32_t v)
{
	lv_opa_t o;

	LV_UNUSED(var);
	switch (za.phase) {
	case 1:
		za_rect(v);
		za_look(LV_OPA_COVER, za_sym_opa(v));
		break;
	case 5:
		za_rect(v);
		/* home's icon shows through as the card settles on it */
		o = v >= ZA_MELT ? LV_OPA_COVER : (lv_opa_t)(255 * v / ZA_MELT);
		za_look(o, LV_MIN(o, za_sym_opa(v)));
		break;
	case 3:
	case 4:
	case 6:
	case 7:
		za_look((lv_opa_t)v, LV_OPA_TRANSP);	/* (the symbol went on the way) */
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
	/* a video opens on black: the card, and layer 0 around the picture
	 * (a Play button is no badge to take a colour from either) */
	if (app_cmd && cmd_is_video(app_cmd) && app_pid != 0 && !za.page)
		za.col = lv_color_black();
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
	/* the badge's own symbol, or the application's picture: the card
	 * starts, and ends, as the icon */
	l = lv_obj_get_child(badge, 0);
	if (l && lv_obj_check_type(l, &lv_image_class)) {
		za.sym = lv_image_create(za.card);
		lv_image_set_src(za.sym, lv_image_get_src(l));
		lv_image_set_scale(za.sym, lv_image_get_scale(l));
	} else {
		za.sym = m3_label(za.card, l ? lv_label_get_text(l) : "",
				  l ? lv_obj_get_style_text_font(l, 0) : &lv_font_montserrat_24,
				  l ? lv_obj_get_style_text_color(l, 0) : lv_color_white());
	}
	lv_obj_center(za.sym);
}

/* Home's picture, retaken if it went stale (the card covers the screen). */
static void za_home_fresh(void)
{
	if (!za.home_stale && za.home)
		return;
	lv_image_set_src(za.bg, NULL);
	tr_free(&za.home);
	za.home = snap(launch_from && lv_obj_is_valid(launch_from) ? launch_from : home_scr,
		       LV_COLOR_FORMAT_XRGB8888);
	za.home_min = bp_minute();
	za.home_stale = false;
}

/* One phase ends: the next begins. */
static void za_next(void)
{
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		fprintf(stderr, "za: phase %d ends at %u ms\n", za.phase, (unsigned)lv_tick_get());
#endif
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
			za_run(6, LV_OPA_COVER, LV_OPA_TRANSP, ZA_REVEAL_MS, ios_path_ease_out);
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
			za_run(5, 1024, 0, ZA_SHRINK_MS, za_path_glide);
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
	case 1:			/* grown: the card clears, the application shows */
		lv_obj_add_flag(za.bg, LV_OBJ_FLAG_HIDDEN);
		/* the card alone: one GPU fill a frame (mbx_fill_straight) */
		lv_obj_add_flag(za.sym, LV_OBJ_FLAG_HIDDEN);
		mbx_gpu_set_transparent(true);
		za_run(3, LV_OPA_COVER, LV_OPA_TRANSP, ZA_REVEAL_MS, ios_path_ease_out);
		break;
	case 3:
		za.phase = 0;
		lv_screen_load(app_pid > 0 ? app_scr : home_scr);
		/* a game turns the launcher's layer to its side now that it is in
		 * front (rot_wanted; during the grow it was not) */
		if (app_is_game())
			rot_kick();
		za_free();
		if (app_pid <= 0)
			app_home_ui();
		else
			app_dormant_sync();
		break;
	case 4:			/* covered: shrink into the icon over home */
		lv_obj_remove_flag(za.sym, LV_OBJ_FLAG_HIDDEN);
		mbx_gpu_set_transparent(false);
		za_home_fresh();
		lv_image_set_src(za.bg, za.home);
		lv_obj_remove_flag(za.bg, LV_OBJ_FLAG_HIDDEN);
		za_run(5, 1024, 0, ZA_SHRINK_MS, za_path_glide);
		break;
	case 5:
		za.phase = 0;
		app_home_ui();
		za_free();
		break;
	}
}

/* Ending, the animation: finish whatever runs now, at once. */
void za_finish(void)
{
	while (za.phase) {
		lv_anim_delete(&za, za_exec);
		za_next();
	}
}

/* Every animation of the launcher's own ends now (drags included). */
void ui_settle_all(void)
{
	if (sd.on) {			/* the finger is still on the shade */
		sd.on = false;
		sd.cand = 0;
		sd_end(sd.y > -SHADE_H / 2);
	}
	sd_settle_now();
	za_finish();
	tr_finish();
	pg_abort();
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
	za_run(1, 0, 1024, ZA_GROW_MS, za_path_spring);
}

static void page_launch_cb(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_current_target(e);

	obj_rest(t);	/* (its transition would be pictured) */
	page_open_from(t, (void (*)(lv_event_t *))lv_event_get_user_data(e));
}

/* The tile just added to @parent opens @open's page with the card. */
lv_obj_t *page_tile_anim(lv_obj_t *parent, void (*open)(lv_event_t *))
{
	lv_obj_t *t = lv_obj_get_child(parent, lv_obj_get_child_count(parent) - 1);

	lv_obj_remove_event_cb(t, open);
	lv_obj_add_event_cb(t, page_launch_cb, LV_EVENT_CLICKED, (void *)open);
	return t;
}

/* Home (or Back from the first page) on a page opened from a tile. */
bool za_page_close(void)
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
	lv_screen_load(za.scr);
	za_run(7, LV_OPA_TRANSP, LV_OPA_COVER, ZA_COVER_MS, lv_anim_path_linear);
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
bool page_over_app;

static void qs_to_page(void (*open)(lv_event_t *))
{
	lv_draw_buf_t *bg, *fg;
	bool over_app = app_pid > 0 && lv_screen_active() == app_scr;
	bool on_pg = on_page();

	if (!shade_open || sd.on || sd.scr || za.phase || locked || !settings_tile ||
	    (!over_app && !on_pg && lv_screen_active() != home_scr))
		return;
	if (on_pg && nav_n > 0 && nav_re[nav_n - 1].open == open) {
		shade_set(false, true);
		return;
	}
	if (over_app)
		app_dormant_wake(true);
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
	if (!on_pg)
		za.page_tile = open == open_settings ? settings_tile : NULL;
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
		lv_anim_set_path_cb(&a, ios_path_ease_in_out);
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
	else
		app_dormant_sync();
}

static void app_dormant_enter(void)
{
	app_dormant = true;
	if (home_timer)
		lv_timer_pause(home_timer);
	if (shade_warm_timer)
		lv_timer_pause(shade_warm_timer);
	pf_timer_sync();
	idle_timer_sync();
	bz_stop();
	tr_free(&za.home);
	za.home_stale = true;
	tr_free(&sd.home_pic);
	shade_pic_free();
	pg_pics_free();
	nav_caches_clear();
	wp_suspend();
	mbx_gpu_set_layer_enabled(false);
	mbx_gpu_suspend();
	mbx_gpu_sync_ext_overlay();
}

void app_dormant_wake(bool full)
{
	if (!app_dormant && !mbx_gpu_is_suspended())
		return;
	if (full) {
		app_dormant = false;
		mbx_gpu_resume();
		wp_resume();
		if (home_timer)
			lv_timer_resume(home_timer);
		if (shade_warm_timer)
			lv_timer_resume(shade_warm_timer);
		pf_timer_sync();
		idle_timer_sync();
		bz_ensure();
		if (dormant_status_dirty || status_batt_dirty || status_net_dirty) {
			dormant_status_dirty = false;
			if (status_net_dirty)
				status_kick(status_batt_dirty, true);
			home_update(NULL);
		}
		if (lv_screen_active())
			lv_obj_invalidate(lv_screen_active());
	} else if (app_dormant) {
		bool had_ext = mbx_gpu_ext_overlay_active();

		app_dormant = false;
		mbx_gpu_set_layer_enabled(true);
		idle_timer_sync();
		if (dormant_status_dirty || status_batt_dirty) {
			dormant_status_dirty = false;
			home_update(NULL);
		}
		if (had_ext || !mbx_gpu_is_suspended() || !mbx_gpu_draws_direct() ||
		    !app_scr || lv_screen_active() != app_scr) {
			if (lv_screen_active())
				lv_obj_invalidate(lv_screen_active());
			lv_obj_invalidate(lv_layer_top());
		}
	}
}

void app_dormant_sync(void)
{
	bool want = app_pid > 0 && !za.phase && app_scr && lv_screen_active() == app_scr &&
		    !locked && !shade_open && !sd.on && !sd.scr && !sd.hw && !app_bar_shown &&
		    !psl_sliding(app_bar) && !(app_bar && !lv_obj_has_flag(app_bar, LV_OBJ_FLAG_HIDDEN) &&
		    lv_anim_get(app_bar, app_y)) && !gp_up() && !kbd_is_open() &&
		    !vol_hud_visible() && !vc_is_visible() && !page_over_app && !pb.scr && !tr.scr;

	if (want && !app_dormant)
		app_dormant_enter();
}

bool page_back_to_app(void)
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
	lv_anim_set_path_cb(&a, ios_path_ease_in);
	lv_anim_set_completed_cb(&a, pb_done);
	lv_anim_start(&a);
	return true;
}

static void app_kill_cb(lv_timer_t *t);

/*
 * How long an application has to end after SIGTERM before SIGKILL. A game
 * gets longer: RetroArch writes the game's state, its save RAM and its
 * settings on the way out (~1 s for a SNES game on this CPU); killed
 * halfway they may be lost.
 */
static uint32_t app_kill_ms(void)
{
	return app_is_game() ? 5000 : 600;
}

/* Home on Settings over an application: the application ends into its icon. */
bool page_home_over_app(void)
{
	if (!page_over_app || app_pid <= 0)
		return false;
	page_over_app = false;
	kill(-app_pid, SIGTERM);
	if (!za.kill_timer) {
		za.kill_timer = lv_timer_create(app_kill_cb, app_kill_ms(), NULL);
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
void ui_rebuild(bool load);
extern bool layout_stale;

static bool apps_stale;
void apps_reload(void);

static void app_home_ui(void)
{
	app_dormant_wake(true);
	wp_apply_kick();	/* (a restart for the palette waits for no application) */
	vc_stop();
	if (apps_stale)
		apps_reload();	/* (the folder changed while it ran) */
	if (layout_stale)
		ui_rebuild(false);	/* (turned over a video: home and pages now) */
	else
		home_fresh();
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
	} else {
		/* back where the application was started: a page (Files) still
		 * on the stack, else home (Home was pressed: the pages go) */
		lv_obj_t *to = launch_from && launch_from != home_scr && lv_obj_is_valid(launch_from) &&
			       nav_n > 0 && nav[nav_n - 1] == launch_from ? launch_from : home_scr;

		if (to == home_scr)
			while (nav_n > 0)
				lv_obj_delete_async(nav[--nav_n]);
		if (lv_screen_active() != to && !locked)
			lv_screen_load(to);
		if (locked)
			lock_from = to;
	}
	lv_obj_invalidate(lv_layer_top());
	home_update(NULL);
	pg_warm_kick();
	games_art_resume();
}

/* Close: the card covers the application, then goes into its icon. */
void za_close(void)
{
	app_dormant_wake(true);
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
	lv_screen_load(za.scr);
	za_run(4, LV_OPA_TRANSP, LV_OPA_COVER, ZA_COVER_MS, lv_anim_path_linear);
}

/* The application has gone (reaped). */
static void app_pidfd_close(void);

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
	mbx_gpu_clear_ext_overlay();
	app_pidfd_close();
	gp_gone();		/* (a game's pause panel) */
	kbd_app_gone();
	rot_kick();		/* (a player gone: no reads needed) */
	vc_stop();
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
	app_dormant_wake(true);
	if (!was_ending && lv_screen_active() == app_scr)
		za_close();		/* it quit by itself */
	else
		app_home_ui();
}

void app_poll(lv_timer_t *t)
{
	int st;

	LV_UNUSED(t);
	if (app_pid > 0 && waitpid(app_pid, &st, WNOHANG) == app_pid) {
		if (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st)))
			fprintf(stderr, "n72-ui: application ended: %s %d\n",
				WIFSIGNALED(st) ? "signal" : "exit status",
				WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		app_closed();
	}
}

/* The application's pidfd turned readable: it has ended (no 200 ms look). */
int app_pidfd = -1;

void app_pidfd_ev(int fd)
{
	LV_UNUSED(fd);
	app_poll(NULL);
}

static void app_pidfd_close(void)
{
	if (app_pidfd < 0)
		return;
	ui_unwatch(app_pidfd);
	close(app_pidfd);
	app_pidfd = -1;
}

static void app_kill_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	za.kill_timer = NULL;
	if (app_pid > 0)
		kill(-app_pid, SIGKILL);	/* app_poll reaps it */
}

/*
 * End the application, if one runs (without waiting for it). Home with
 * sound playing (a video, a song: n72-player) sends the player to the
 * background instead (@bg): the sound goes on, the shade has its controls.
 */
bool app_end_ex(bool bg)
{
	if (app_pid <= 0)
		return false;
	if (za.kill_timer || (za.phase && za.phase >= 4))
		return true;		/* already ending */
	if (bg && bg_start())
		return true;
	za_finish();
	kill(-app_pid, SIGTERM);
	za.kill_timer = lv_timer_create(app_kill_cb, app_kill_ms(), NULL);
	lv_timer_set_repeat_count(za.kill_timer, 1);
	za_close();
	return true;
}

/* Home: playing sound goes to the background, anything else ends. */
bool app_end(void)
{
	return app_end_ex(true);
}

/*
 * The application starts once the grow's first frame is out: layer 0
 * filled with the card's colour first (za_fill_layer0, ~5 ms), then the
 * program, by posix_spawn (no copy-on-write of the launcher's heap, whose
 * faults stalled the next frames after fork), in its own process group.
 * (Lowering its priority during the grow did not take here, by any route
 * tried; with the fades on the GPU it matters little.)
 */

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
	if (bg_busy()) {
		/* the player in the background is still ending (it holds the
		 * player's socket name and the sound): once it has gone */
		lv_timer_t *again = lv_timer_create(za_spawn_cb, 30, NULL);

		lv_timer_set_repeat_count(again, 1);
		return;
	}
	za_fill_layer0(za.col);
	while (environ[n])
		n++;
	envp = calloc(n + 3, sizeof(*envp));	/* (N72_TOUCH, N72_KBD, the end) */
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
		if (i == n)
			n++;
		envp[i] = touch;
	}
	{	/* where it asks for the keyboard */
		for (i = 0; i < n && strncmp(envp[i], "N72_KBD=", 8); i++)
			;
		if (i == n)
			n++;
		envp[i] = "N72_KBD=" KBD_FIFO;
	}
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&at, 0);	/* Home ends everything it starts */
	if (posix_spawn(&pid, "/bin/sh", NULL, &at, argv, envp))
		pid = 0;
#ifdef N72_DEBUG
	if (getenv("N72_FPS"))
		fprintf(stderr, "spawn: pid %d at %u ms, phase %d\n", (int)pid,
			(unsigned)lv_tick_get(), za.phase);
#endif
	posix_spawnattr_destroy(&at);
	free(envp);
	app_pid = pid;
	rot_kick();		/* (a player turns with the iPod) */
	if (pid > 0) {
		/* its end as an event: a pidfd in the main loop's poll() */
		app_pidfd_close();
		app_pidfd = syscall(SYS_pidfd_open, pid, 0);
		if (app_pidfd >= 0) {
			fcntl(app_pidfd, F_SETFD, FD_CLOEXEC);
			ui_watch(app_pidfd, app_pidfd_ev);
		} else if (!app_timer) {
			app_timer = lv_timer_create(app_poll, 200, NULL);	/* (no pidfd: look) */
		}
	}
	/* idle while the card grows (the attribute alone did not take here);
	 * za_next gives it full priority when the grow ends */
}

/* The screen over applications (once). */
void app_scr_make(void)
{
	if (app_scr)
		return;
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

/* Start @cmd as the application, growing out of @tile (a tile, or a row). */
void app_launch(const char *cmd, lv_obj_t *tile)
{
	if (app_pid != 0 || za.phase || !mbx_gpu_on_shell_layer())
		return;
	bg_end();		/* one player at a time: the one in the background ends */
	free(app_cmd);
	app_cmd = strdup(cmd ? cmd : "");
	if (!app_cmd)
		return;
	cmd = app_cmd;		/* (the row's copy goes with its page in a turn) */
	if (ui_rot && !cmd_is_video(cmd) && !cmd_is_game(cmd)) {
		/* applications draw for an upright panel: turn first, and
		 * find the tile again in the new home */
		lv_obj_t *par = lv_obj_get_parent(tile);
		bool docked = home_dock && par == home_dock;	/* (the dock is rebuilt in its order) */
		int idx = par == home_apps || docked ? (int)lv_obj_get_index(tile) : -1;

		if (idx < 0 || !ui_turn(0))
			return;
		tile = lv_obj_get_child(docked ? home_dock : home_apps, idx);
		if (!tile)
			return;
	}
	app_scr_make();
	if (cmd_is_video(cmd))
		vc_start();
	tr_finish();
	shade_set(false, false);

	/* home as it is now, for the grow (and the close) */
	za.tile = tile;
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
	launch_from = lv_screen_active();	/* home, or a page (Files) */
	za.home = grab_or_snap(launch_from);
	za.home_min = bp_minute();

	za.cmd = cmd;
	/* the shade comes down over the application from the top layer */
	lv_obj_set_parent(scrim, lv_layer_top());
	lv_obj_set_parent(shade, lv_layer_top());
	{
		/* started once the first frame of the grow is out (za_spawn_cb) */
		/* (a video player: once the grow has landed; its start-up,
		 * GStreamer's plugins and the stream opening, made the grow
		 * jitter, and it shows nothing for a second anyway) */
		lv_timer_t *st = lv_timer_create(za_spawn_cb,
						  cmd_is_video(cmd) ? ZA_GROW_MS + 20 : 20, NULL);

		lv_timer_set_repeat_count(st, 1);
	}
	app_pid = -1;		/* (starting: no second launch meanwhile) */

	za_build();
	lv_screen_load(za.scr);
	za_run(1, 0, 1024, ZA_GROW_MS, za_path_spring);
}

void app_launch_cb(lv_event_t *e)
{
	app_launch(lv_event_get_user_data(e), lv_event_get_current_target(e));
}

/* One tile for every manifest in APPS_DIR. */
/* A tile that starts @cmd: icon=<name> picks a symbol, @rgb the badge. */
static void app_cmd_free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/* a tile made from a manifest or a .desktop file, not one of our own */
#define APP_TILE_MANIFEST	LV_OBJ_FLAG_USER_4

static void app_tile_cmd(lv_obj_t *parent, const char *name, const char *cmd,
			 const char *icon, const char *image, bool have_rgb, unsigned rgb)
{
	static const struct { const char *n, *sym; } syms[] = {
		{ "video", LV_SYMBOL_VIDEO }, { "image", LV_SYMBOL_IMAGE },
		{ "music", LV_SYMBOL_AUDIO }, { "gears", LV_SYMBOL_SETTINGS },
		{ "shuffle", LV_SYMBOL_SHUFFLE }, { "eye", LV_SYMBOL_EYE_OPEN },
		{ "file", LV_SYMBOL_FILE }, { "list", LV_SYMBOL_LIST },
		{ "terminal", LV_SYMBOL_KEYBOARD }, { "gps", LV_SYMBOL_GPS },
		{ "charge", LV_SYMBOL_CHARGE }, { "home", LV_SYMBOL_HOME },
		{ "folder", LV_SYMBOL_DIRECTORY }, { "game", N72_SYMBOL_GAMEPAD },
		{ "web", N72_SYMBOL_WEB },
	};
	uint32_t before = lv_obj_get_child_count(parent);
	const char *sym = LV_SYMBOL_PLAY;
	size_t k;

	struct app_icon *ic = image && image[0] ? app_icon_load(image) : NULL;

	for (k = 0; k < sizeof(syms) / sizeof(syms[0]); k++)
		if (!strcmp(icon, syms[k].n))
			sym = syms[k].sym;
	if (ic)
		app_tile_img(parent, ic, name, app_launch_cb);
	else
		app_tile(parent, sym, name,
			 have_rgb ? lv_color_hex(rgb) : M.secondary_c,
			 have_rgb ? lv_color_white() : M.on_secondary_c, app_launch_cb);
	/* the command rides on the tile's click */
	if (lv_obj_get_child_count(parent) > before) {
		lv_obj_t *t = lv_obj_get_child(parent, before);
		char *c = strdup(cmd);

		lv_obj_add_flag(t, APP_TILE_MANIFEST);	/* (apps_reload makes it again) */

		lv_obj_remove_event_cb(t, app_launch_cb);
		lv_obj_add_event_cb(t, app_launch_cb, LV_EVENT_CLICKED, c);
		lv_obj_add_event_cb(t, app_cmd_free_cb, LV_EVENT_DELETE, c);
	}
}

/* Our manifests: /usr/share/n72-ui/apps/NAME.app (name=, exec=, icon=, image=, color=#). */
struct app_manifest {
	char name[40], cmd[400], icon[24], image[160];
	unsigned rgb;
	bool have_rgb;
};

static int app_manifest_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct app_manifest *)a)->name,
			  ((const struct app_manifest *)b)->name);
}

/* (tiles in the order of their names, whatever the folder's order) */
static void app_tiles_n72(lv_obj_t *parent)
{
	DIR *d = opendir(APPS_DIR);
	struct app_manifest *m = NULL;
	struct dirent *de;
	int n = 0, cap = 0, i;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[300], line[400];
		struct app_manifest a = { 0 };
		size_t len = strlen(de->d_name);
		FILE *f;

		if (len < 5 || strcmp(de->d_name + len - 4, ".app"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", APPS_DIR, de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "name=", 5))
				snprintf(a.name, sizeof(a.name), "%s", line + 5);
			else if (!strncmp(line, "exec=", 5))
				snprintf(a.cmd, sizeof(a.cmd), "%s", line + 5);
			else if (!strncmp(line, "icon=", 5))
				snprintf(a.icon, sizeof(a.icon), "%s", line + 5);
			else if (!strncmp(line, "image=", 6))
				snprintf(a.image, sizeof(a.image), "%s", line + 6);
			else if (!strncmp(line, "color=#", 7))
				a.have_rgb = sscanf(line + 7, "%x", &a.rgb) == 1;
		}
		fclose(f);
		if (!a.name[0] || !a.cmd[0])
			continue;
		if (n == cap) {
			struct app_manifest *g = realloc(m, (cap ? cap * 2 : 8) * sizeof(*m));

			if (!g)
				break;
			m = g;
			cap = cap ? cap * 2 : 8;
		}
		m[n++] = a;
	}
	closedir(d);
	if (n)
		qsort(m, n, sizeof(*m), app_manifest_cmp);
	for (i = 0; i < n; i++)
		app_tile_cmd(parent, m[i].name, m[i].cmd, m[i].icon, m[i].image,
			     m[i].have_rgb, m[i].rgb);
	free(m);
}

/*
 * Packaged applications: freedesktop .desktop files. Only those that can
 * run here: no desktop session (X11, Wayland) and so no GTK or Qt; told by
 * the program's ELF header, which names the libraries it needs.
 */
#define DESKTOP_DIR "/usr/share/applications"

static bool prog_path(const char *prog, char *out, size_t len)
{
	const char *path = getenv("PATH"), *p;

	if (strchr(prog, '/')) {
		snprintf(out, len, "%s", prog);
		return access(out, X_OK) == 0;
	}
	if (!path || !*path)
		path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	for (p = path; *p; ) {
		size_t n = strcspn(p, ":");

		snprintf(out, len, "%.*s/%s", (int)n, p, prog);
		if (n && access(out, X_OK) == 0)
			return true;
		p += n;
		if (*p == ':')
			p++;
	}
	return false;
}

/* the program needs a desktop session: it links X11, Wayland, GTK or Qt */
static bool needs_desktop(const char *path)
{
	static const char *const libs[] = { "libX11.so", "libwayland-client.so",
		"libgtk-", "libQt5", "libQt6" };
	size_t len = 512 * 1024, got, i;	/* the dynamic section is near the start */
	char *buf = malloc(len);
	bool hit = false;
	FILE *f = fopen(path, "rb");

	if (!buf || !f) {
		free(buf);
		if (f)
			fclose(f);
		return false;
	}
	got = fread(buf, 1, len, f);
	fclose(f);
	for (i = 0; i < sizeof(libs) / sizeof(libs[0]) && !hit; i++)
		hit = memmem(buf, got, libs[i], strlen(libs[i])) != NULL;
	free(buf);
	return hit;
}

static void app_tiles_desktop(lv_obj_t *parent)
{
	DIR *d = opendir(DESKTOP_DIR);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[300], line[512], name[40] = "", cmd[400] = "", icon[24] = "";
		char prog[PATH_MAX], arg0[256];
		unsigned rgb = 0;
		bool have_rgb = false, skip = false, entry = false, app = false;
		size_t n = strlen(de->d_name);
		FILE *f;
		char *p, *q;

		if (n < 9 || strcmp(de->d_name + n - 8, ".desktop"))
			continue;
		snprintf(path, sizeof(path), DESKTOP_DIR "/%s", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0] == '[') {		/* only the main group */
				entry = !strcmp(line, "[Desktop Entry]");
				continue;
			}
			if (!entry)
				continue;
			if (!strncmp(line, "Name=", 5))
				snprintf(name, sizeof(name), "%s", line + 5);
			else if (!strncmp(line, "Exec=", 5))
				snprintf(cmd, sizeof(cmd), "%s", line + 5);
			else if (!strncmp(line, "Type=", 5))
				app = !strcmp(line + 5, "Application");
			else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true") ||
				 !strcmp(line, "Terminal=true"))
				skip = true;
			else if (!strncmp(line, "X-N72-Icon=", 11))
				snprintf(icon, sizeof(icon), "%s", line + 11);
			else if (!strncmp(line, "X-N72-Color=#", 13))
				have_rgb = sscanf(line + 13, "%x", &rgb) == 1;
			else if (!icon[0] && !strncmp(line, "Categories=", 11))
				snprintf(icon, sizeof(icon), "%s",
					 strstr(line, "Game") ? "game" : strstr(line, "WebBrowser") ? "web" :
					 strstr(line, "AudioVideo") ? "video" : strstr(line, "Graphics") ? "image" : "");
		}
		fclose(f);
		if (skip || !app || !name[0] || !cmd[0])
			continue;
		/* field codes (%f %U ...) out: started without a file */
		for (p = q = cmd; *p; p++) {
			if (*p == '%' && p[1]) {
				p++;
				if (*p == '%')
					*q++ = '%';
				continue;
			}
			*q++ = *p;
		}
		*q = 0;
		snprintf(arg0, sizeof(arg0), "%.*s", (int)strcspn(cmd, " \t"), cmd);
		if (!prog_path(arg0, prog, sizeof(prog)) || needs_desktop(prog))
			continue;
		app_tile_cmd(parent, name, cmd, icon, NULL, have_rgb, rgb);
	}
	closedir(d);
}

void app_tiles(lv_obj_t *parent)
{
	if (!mbx_gpu_on_shell_layer())
		return;
	app_tiles_n72(parent);
	app_tiles_desktop(parent);
}

/*
 * The folder of manifests changed (an application added, renamed or
 * removed; n72-ios-tiles rewrites iOS applications' manifests): the tiles
 * after the built-in ones are made again. Not while an application runs
 * or a card animates (the card's way back is a tile): then once home is
 * back (app_home_ui). A burst of changes is one rebuild, 300 ms after the
 * last.
 */
#include "paging.c"	/* home pages: layout, the swipe, its frames */
#include "dock.c"	/* the dock under them */

static lv_timer_t *apps_timer;

void apps_reload(void)
{
	int i;

	if (!home_apps || app_pid != 0 || za.phase) {
		apps_stale = true;
		return;
	}
	apps_stale = false;
	dock_before_reload();	/* (every tile back in the grid, in its order) */
	/* the manifests' tiles, wherever they were carried to (and their
	 * pictures with them: no tile is left pointing at a freed one) */
	for (i = (int)lv_obj_get_child_count(home_apps) - 1; i >= 1; i--) {
		lv_obj_t *t = lv_obj_get_child(home_apps, i);

		if (!lv_obj_has_flag(t, APP_TILE_MANIFEST))
			continue;
		if (za.tile == t)
			za.tile = NULL;
		lv_obj_delete(t);
	}
	app_icons_free();
	app_tiles(home_apps);
	dock_after_reload();
	pg_layout();
	bp_changed(home_scr);
	lv_obj_invalidate(home_scr);
}

static void apps_timer_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	apps_timer = NULL;
	apps_reload();
}

static void apps_ev(int fd)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	if (apps_timer) {
		lv_timer_reset(apps_timer);
		return;
	}
	apps_timer = lv_timer_create(apps_timer_cb, 300, NULL);
	lv_timer_set_repeat_count(apps_timer, 1);
}

void apps_watch(void)
{
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	if (fd < 0)
		return;
	/* our manifests, and the .desktop entries of installed programs (apk) */
	bool any = inotify_add_watch(fd, APPS_DIR, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
				     IN_MOVED_TO | IN_MOVED_FROM) >= 0;

	any |= inotify_add_watch(fd, DESKTOP_DIR, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
				 IN_MOVED_TO | IN_MOVED_FROM) >= 0;
	if (!any) {
		close(fd);
		return;
	}
	ui_watch(fd, apps_ev);
}
