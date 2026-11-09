// SPDX-License-Identifier: MIT
/*
 * n72-ui: a small LVGL launcher for Linux on the iPod touch 2G (N72AP).
 *
 * Display: /dev/fb0 (simplefb, 320x480 XRGB8888). Touch: the Zephyr2
 * evdev device. Buttons: gpio-keys (Home returns to the launcher, Power
 * puts the system to sleep (s2idle) and Power or Home wakes it, the volume
 * keys change the brightness).
 *
 * Environment:
 *   N72_FB         framebuffer device (default /dev/fb0)
 *   N72_TOUCH      touch event device (default: found by name)
 *   N72_KEYS       button event device (default: found by name)
 *   N72_TOUCH_CAL  "min_x min_y max_x max_y" raw touch range; swap min and
 *                  max to flip an axis (default: the device's own range)
 *   N72_TOUCH_SWAP set to 1 to swap the touch axes
 *   N72_GPU        set to 0 to draw with the CPU only (default: MBX GPU
 *                  through /dev/mbx when present)
 *   N72_SLEEP      set to 0 for Power to only blank the screen instead of
 *                  suspending the system
 *   N72_IDLE_SLEEP seconds without input before sleeping (default 120, 0
 *                  never); not while USB power is present
 *
 * n72-ui --bench times full-screen redraws and exits.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include "mbx_gpu.h"

#define W 320
#define H 480

static char *self_argv[4];
static lv_obj_t *home_scr;
static lv_obj_t *clock_lbl, *date_lbl, *batt_lbl;
static bool in_demo;
static int keys_fd = -1;
static int touch_wait_fd = -1;		/* own handle on the touch device, for poll() */
static lv_indev_t *touch_indev;

/* Touch-to-screen latency (N72_LATENCY=1): newest touch event -> flush end. */
static bool lat_on;
static uint64_t lat_event_us;		/* newest touch event not yet on screen */
static uint64_t lat_sum_us, lat_max_us;
static unsigned int lat_n;
static uint32_t lat_report_ms;

static uint32_t tick_ms(void);

static uint64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t lat_render_max_us, lat_wake_sum_us, lat_read_us;
static bool lat_input_frame;

/* Render pending changes immediately (after input), timing the render. */
static void render_now(void)
{
	uint64_t t0 = mono_us(), d;

	lat_input_frame = true;
	lv_refr_now(NULL);
	lat_input_frame = false;
	lat_event_us = 0;	/* nothing to show for this input: don't count it */
	d = mono_us() - t0;
	lat_render_max_us = LV_MAX(lat_render_max_us, d);
}

/* Called when a frame has been flushed to the screen. */
void n72_frame_shown(void)
{
	uint64_t d;

	/* only frames drawn because of touch input count */
	if (!lat_on || !lat_event_us || !lat_input_frame)
		return;
	d = mono_us() - lat_event_us;
	lat_wake_sum_us += lat_read_us - lat_event_us;
	lat_event_us = 0;
	lat_sum_us += d;
	lat_max_us = LV_MAX(lat_max_us, d);
	lat_n++;
	if (tick_ms() - lat_report_ms >= 2000 && lat_n) {
		fprintf(stderr, "t=%llu touch->screen: %u input frames, average %.1f ms "
			"(wake %.1f), max %.1f ms; slowest touch render %.1f ms\n",
			(unsigned long long)(mono_us() / 1000000), lat_n,
			lat_sum_us / 1000.0 / lat_n, lat_wake_sum_us / 1000.0 / lat_n,
			lat_max_us / 1000.0, lat_render_max_us / 1000.0);
		lat_sum_us = lat_max_us = lat_render_max_us = lat_wake_sum_us = 0;
		lat_n = 0;
		lat_report_ms = tick_ms();
	}
}
static int tty_fd = -1;

/* ---------------------------------------------------------------- utils */

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_int(const char *path, int def)
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

static void write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

/* Find /dev/input/eventN whose device name contains @name. */
static const char *find_input(const char *name)
{
	static char found[2][64];
	static int slot;
	char path[128], devname[128];
	int i;

	for (i = 0; i < 16; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		devname[0] = 0;
		ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);
		close(fd);
		if (strstr(devname, name)) {
			char *out = found[slot++ & 1];

			snprintf(out, 64, "%s", path);
			return out;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------ backlight */

static char bl_dir[128];
static int bl_max = 1;
static bool screen_off;

static void backlight_init(void)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(bl_dir, sizeof(bl_dir), "/sys/class/backlight/%s",
			 e->d_name);
		break;
	}
	closedir(d);
	if (bl_dir[0]) {
		char p[160];

		snprintf(p, sizeof(p), "%s/max_brightness", bl_dir);
		bl_max = read_int(p, 1);
	}
}

/* Brightness in percent. */
static int backlight_get(void)
{
	char p[160];

	if (!bl_dir[0])
		return 100;
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	return read_int(p, bl_max) * 100 / bl_max;
}

static void backlight_set(int pct)
{
	char p[160];

	if (!bl_dir[0])
		return;
	pct = LV_CLAMP(5, pct, 100);
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	write_int(p, pct * bl_max / 100);
}

static void backlight_power(bool on)
{
	char p[160];

	if (!bl_dir[0])
		return;
	snprintf(p, sizeof(p), "%s/bl_power", bl_dir);
	write_int(p, on ? 0 : 4);
}

/* -------------------------------------------------------------- helpers */

static void go_home(void)
{
	if (in_demo) {
		/* The demos keep global state and timers: start afresh. */
		if (tty_fd >= 0)
			close(tty_fd);
		execv("/proc/self/exe", self_argv);
		return;
	}
	if (lv_screen_active() != home_scr)
		lv_screen_load_anim(home_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0,
				    true);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	go_home();
}

static void style_screen(lv_obj_t *scr)
{
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1021), 0);
	lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x2a1747), 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);
}

/* A screen with a header (back button and title); returns the content area. */
static lv_obj_t *app_screen(const char *title, lv_obj_t **scr_out)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_t *hdr, *btn, *lbl, *body;

	style_screen(scr);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	hdr = lv_obj_create(scr);
	lv_obj_set_size(hdr, W, 48);
	lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(hdr, LV_OPA_40, 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_radius(hdr, 0, 0);
	lv_obj_set_style_pad_all(hdr, 4, 0);
	lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

	btn = lv_button_create(hdr);
	lv_obj_set_height(btn, 38);
	lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
	lv_obj_center(lbl);

	lbl = lv_label_create(hdr);
	lv_label_set_text(lbl, title);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
	lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

	body = lv_obj_create(scr);
	lv_obj_set_size(body, W, H - 48);
	lv_obj_set_pos(body, 0, 48);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);

	*scr_out = scr;
	return body;
}

static void show(lv_obj_t *scr)
{
	lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static lv_obj_t *card(lv_obj_t *parent)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(c, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_10, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_radius(c, 16, 0);
	lv_obj_set_style_pad_all(c, 14, 0);
	lv_obj_set_style_text_color(c, lv_color_white(), 0);
	return c;
}

/* ---------------------------------------------------------------- paint */

#define PAINT_H (H - 48)
static lv_obj_t *paint_canvas;
static void *paint_buf;
static lv_point_t paint_last;
static bool paint_down;
static uint32_t paint_hue;

static void paint_clear(void)
{
	lv_canvas_fill_bg(paint_canvas, lv_color_hex(0x101018), LV_OPA_COVER);
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
	d.color = lv_color_hsv_to_rgb(paint_hue, 75, 100);
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
	lv_obj_t *scr, *body, *btn, *lbl;

	LV_UNUSED(e);
	body = app_screen("Paint", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	paint_buf = malloc(LV_CANVAS_BUF_SIZE(W, PAINT_H, 32,
					      LV_DRAW_BUF_STRIDE_ALIGN));
	if (!paint_buf)
		return;
	paint_canvas = lv_canvas_create(body);
	lv_canvas_set_buffer(paint_canvas, paint_buf, W, PAINT_H,
			     LV_COLOR_FORMAT_XRGB8888);
	lv_obj_add_flag(paint_canvas, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(paint_canvas, paint_event, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(scr, paint_delete_cb, LV_EVENT_DELETE, NULL);
	paint_clear();

	btn = lv_button_create(lv_obj_get_child(scr, 0));
	lv_obj_set_height(btn, 36);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(btn, paint_clear_cb, LV_EVENT_CLICKED, NULL);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, LV_SYMBOL_TRASH);
	lv_obj_center(lbl);

	show(scr);
}

/* ------------------------------------------------------------- settings */

static lv_obj_t *bright_lbl;

static void bright_cb(lv_event_t *e)
{
	lv_obj_t *s = lv_event_get_target(e);
	int v = lv_slider_get_value(s);

	backlight_set(v);
	lv_label_set_text_fmt(bright_lbl, "Brightness  %d%%", v);
}

static void open_settings(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *s, *lbl;

	LV_UNUSED(e);
	body = app_screen("Settings", &scr);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(body, 12, 0);
	lv_obj_set_style_pad_row(body, 12, 0);

	c = card(body);
	bright_lbl = lv_label_create(c);
	lv_label_set_text_fmt(bright_lbl, "Brightness  %d%%", backlight_get());
	s = lv_slider_create(c);
	lv_obj_set_width(s, lv_pct(95));
	lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 34);
	lv_slider_set_range(s, 5, 100);
	lv_slider_set_value(s, backlight_get(), LV_ANIM_OFF);
	lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_set_height(c, 80);

	c = card(body);
	lbl = lv_label_create(c);
	lv_obj_set_width(lbl, lv_pct(100));
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
	lv_label_set_text(lbl,
		LV_SYMBOL_HOME "  Home: back to the launcher\n"
		LV_SYMBOL_POWER "  Power: screen off / on\n"
		LV_SYMBOL_PLUS " " LV_SYMBOL_MINUS "  Volume: brightness");

	show(scr);
}

/* --------------------------------------------------------------- system */

static lv_obj_t *sys_val[5];
static const char *const sys_titles[5] = { "Device", "Kernel", "Uptime", "Memory", "Load" };
static lv_timer_t *sys_timer;

static void sys_update(lv_timer_t *t)
{
	char kernel[160] = "?", line[128];
	long mem_total = 0, mem_avail = 0;
	struct utsname u;
	struct sysinfo si;
	double load = 0;
	FILE *f;

	LV_UNUSED(t);
	if (!uname(&u))
		snprintf(kernel, sizeof(kernel), "%s %s", u.sysname, u.release);
	sysinfo(&si);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %ld", &mem_total);
			sscanf(line, "MemAvailable: %ld", &mem_avail);
		}
		fclose(f);
	}
	f = fopen("/proc/loadavg", "r");
	if (f) {
		if (fscanf(f, "%lf", &load) != 1)
			load = 0;
		fclose(f);
	}
	lv_label_set_text(sys_val[0], "iPod touch 2G (N72AP)\nS5L8720, ARM1176 @ 532 MHz");
	lv_label_set_text(sys_val[1], kernel);
	lv_label_set_text_fmt(sys_val[2], "%ldh %02ldm %02lds", si.uptime / 3600,
			      si.uptime / 60 % 60, si.uptime % 60);
	lv_label_set_text_fmt(sys_val[3], "%ld MiB free of %ld MiB",
			      mem_avail / 1024, mem_total / 1024);
	lv_label_set_text_fmt(sys_val[4], "%d.%02d    %d processes", (int)load,
			      (int)(load * 100) % 100, si.procs);
}

static void sys_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (sys_timer)
		lv_timer_delete(sys_timer);
	sys_timer = NULL;
}

static void open_system(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *t;
	int i;

	LV_UNUSED(e);
	body = app_screen("System", &scr);
	lv_obj_set_style_pad_all(body, 12, 0);
	c = card(body);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 4, 0);
	for (i = 0; i < 5; i++) {
		t = lv_label_create(c);
		lv_label_set_text(t, sys_titles[i]);
		lv_obj_set_style_text_color(t, lv_color_hex(0x8ab4ff), 0);
		if (i)
			lv_obj_set_style_pad_top(t, 10, 0);
		sys_val[i] = lv_label_create(c);
		lv_obj_set_width(sys_val[i], lv_pct(100));
		lv_label_set_long_mode(sys_val[i], LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_font(sys_val[i], &lv_font_montserrat_16, 0);
	}
	sys_update(NULL);
	sys_timer = lv_timer_create(sys_update, 1000, NULL);
	lv_obj_add_event_cb(scr, sys_delete_cb, LV_EVENT_DELETE, NULL);
	show(scr);
}

/* ---------------------------------------------------------------- logs */

/*
 * The kernel log on the screen, for when there is no USB or Wi-Fi to read it
 * over (e.g. after a suspend to RAM): the network addresses, then the tail of
 * the ring buffer, refreshed every 2 s and kept scrolled to the end.
 */
#define LOGS_TAIL 6144

static lv_obj_t *logs_net, *logs_text, *logs_box;
static lv_timer_t *logs_timer;
static char *logs_buf;
static int logs_len = -1;
static bool logs_follow = true;		/* keep the newest lines in view */

static void logs_addr(const char *ifname, char *out, size_t len)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	snprintf(out, len, "%s: -", ifname);
	if (fd < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (!ioctl(fd, SIOCGIFADDR, &ifr))
		snprintf(out, len, "%s: %s", ifname,
			 inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	close(fd);
}

static void logs_update(lv_timer_t *t)
{
	char usb[48], wlan[48];
	int size, n, i, o, start;
	char *raw;

	LV_UNUSED(t);
	logs_addr("usb0", usb, sizeof(usb));
	logs_addr("wlan0", wlan, sizeof(wlan));
	lv_label_set_text_fmt(logs_net, "%s    %s", usb, wlan);

	size = klogctl(10, NULL, 0);		/* SYSLOG_ACTION_SIZE_BUFFER */
	if (size <= 0)
		size = 1 << 17;
	raw = malloc(size + 1);
	if (!raw)
		return;
	n = klogctl(3, raw, size);		/* SYSLOG_ACTION_READ_ALL */
	if (n < 0)
		n = 0;
	if (n == logs_len) {			/* nothing new */
		free(raw);
		return;
	}
	logs_len = n;
	/* the last LOGS_TAIL bytes, from a line start, without "<level>" */
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

/* Volume up/down scroll the log (no touch needed); back at the end, follow. */
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
	logs_buf = malloc(LOGS_TAIL + 1);
	if (!logs_buf)
		return;
	logs_buf[0] = 0;
	logs_len = -1;
	logs_follow = true;
	body = app_screen("Logs", &scr);
	lv_obj_set_style_pad_all(body, 6, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(body, 4, 0);

	logs_net = lv_label_create(body);
	lv_obj_set_width(logs_net, lv_pct(100));
	lv_obj_set_style_text_font(logs_net, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_color(logs_net, lv_color_hex(0x8ab4ff), 0);

	logs_box = lv_obj_create(body);
	lv_obj_set_width(logs_box, lv_pct(100));
	lv_obj_set_flex_grow(logs_box, 1);
	lv_obj_set_style_pad_all(logs_box, 4, 0);
	lv_obj_set_style_bg_color(logs_box, lv_color_hex(0x05070b), 0);
	lv_obj_set_style_border_width(logs_box, 0, 0);
	lv_obj_set_scroll_dir(logs_box, LV_DIR_VER);

	logs_text = lv_label_create(logs_box);
	lv_obj_set_width(logs_text, lv_pct(100));
	lv_label_set_long_mode(logs_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(logs_text, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_color(logs_text, lv_color_hex(0xd0d6e0), 0);

	logs_update(NULL);
	logs_timer = lv_timer_create(logs_update, 2000, NULL);
	lv_obj_add_event_cb(scr, logs_delete_cb, LV_EVENT_DELETE, NULL);
	show(scr);
}

/* ---------------------------------------------------------------- power */

static void power_do(lv_event_t *e)
{
	intptr_t what = (intptr_t)lv_event_get_user_data(e);
	lv_obj_t *mbox;

	if (!what) {
		mbox = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
		lv_msgbox_close_async(mbox);
		return;
	}
	sync();
	reboot(what == 1 ? RB_POWER_OFF : RB_AUTOBOOT);
}

static void open_power(lv_event_t *e)
{
	lv_obj_t *mbox, *btn;

	LV_UNUSED(e);
	mbox = lv_msgbox_create(NULL);
	lv_msgbox_add_title(mbox, "Power");
	lv_msgbox_add_text(mbox, "Power off or restart the iPod?\n"
			   "Restarting goes back to the bootloader.");
	btn = lv_msgbox_add_footer_button(mbox, "Off");
	lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
	lv_obj_add_event_cb(btn, power_do, LV_EVENT_CLICKED, (void *)1);
	btn = lv_msgbox_add_footer_button(mbox, "Restart");
	lv_obj_add_event_cb(btn, power_do, LV_EVENT_CLICKED, (void *)2);
	btn = lv_msgbox_add_footer_button(mbox, "Cancel");
	lv_obj_add_event_cb(btn, power_do, LV_EVENT_CLICKED, (void *)0);
}

/* ---------------------------------------------------------------- demos */

static void open_demo(lv_event_t *e)
{
	intptr_t which = (intptr_t)lv_event_get_user_data(e);
	lv_obj_t *scr = lv_obj_create(NULL);

	lv_screen_load(scr);
	in_demo = true;
	if (which == 1)
		lv_demo_widgets();
	else
		lv_demo_music();
}

/* ----------------------------------------------------------------- home */

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

/* Battery icon and percentage from the PMU's power supply. */
static void battery_update(void)
{
	char st[16] = "";
	const char *icon;
	FILE *f;
	int cap;

	if (!batt_lbl)
		return;
	cap = read_sysfs_int("/sys/class/power_supply/battery/capacity", -1);
	if (cap < 0) {
		lv_label_set_text(batt_lbl, "");
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
	lv_label_set_text_fmt(batt_lbl, "%s%d%% %s", strncmp(st, "Charging", 8) ? "" :
			      LV_SYMBOL_CHARGE " ", cap, icon);
}

static void clock_update(lv_timer_t *t)
{
	static const char *const days[] = { "Sunday", "Monday", "Tuesday",
		"Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September",
		"October", "November", "December" };
	time_t now = time(NULL);
	struct tm tm;

	static unsigned int ticks;

	LV_UNUSED(t);
	if (ticks++ % 10 == 0)
		battery_update();
	localtime_r(&now, &tm);
	lv_label_set_text_fmt(clock_lbl, "%02d:%02d", tm.tm_hour, tm.tm_min);
	if (tm.tm_year + 1900 < 2024)
		lv_label_set_text(date_lbl, "Clock not set");
	else
		lv_label_set_text_fmt(date_lbl, "%s, %d %s", days[tm.tm_wday],
				      tm.tm_mday, months[tm.tm_mon]);
}

struct app {
	const char *name;
	const char *icon;
	uint32_t c1, c2;
	lv_event_cb_t cb;
	intptr_t arg;
};

static const struct app apps[] = {
	{ "Widgets",  LV_SYMBOL_LIST,     0x4f8cff, 0x2b54d6, open_demo, 1 },
	{ "Music",    LV_SYMBOL_AUDIO,    0xff5f8f, 0xc2185b, open_demo, 2 },
	{ "Paint",    LV_SYMBOL_EDIT,     0xffb84d, 0xe67e00, open_paint, 0 },
	{ "Settings", LV_SYMBOL_SETTINGS, 0x9aa5b8, 0x5b6578, open_settings, 0 },
	{ "System",   LV_SYMBOL_EYE_OPEN, 0x45d6a8, 0x14946b, open_system, 0 },
	{ "Logs",     LV_SYMBOL_FILE,     0x6c7a96, 0x3a4560, open_logs, 0 },
	{ "Power",    LV_SYMBOL_POWER,    0xff6b5b, 0xc23021, open_power, 0 },
};

static void build_home(void)
{
	lv_obj_t *grid, *lbl;
	unsigned int i;

	home_scr = lv_obj_create(NULL);
	style_screen(home_scr);
	lv_obj_remove_flag(home_scr, LV_OBJ_FLAG_SCROLLABLE);

	lbl = lv_label_create(home_scr);
	lv_label_set_text(lbl, "iPod touch  " LV_SYMBOL_USB);
	lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
	lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 12, 8);

	batt_lbl = lv_label_create(home_scr);
	lv_label_set_text(batt_lbl, "");
	lv_obj_set_style_text_opa(batt_lbl, LV_OPA_70, 0);
	lv_obj_align(batt_lbl, LV_ALIGN_TOP_RIGHT, -12, 8);

	clock_lbl = lv_label_create(home_scr);
	lv_obj_set_style_text_font(clock_lbl, &lv_font_montserrat_48, 0);
	lv_obj_align(clock_lbl, LV_ALIGN_TOP_MID, 0, 44);
	date_lbl = lv_label_create(home_scr);
	lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_opa(date_lbl, LV_OPA_70, 0);
	lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 104);
	clock_update(NULL);
	lv_timer_create(clock_update, 1000, NULL);

	grid = lv_obj_create(home_scr);
	lv_obj_set_size(grid, W, 300);
	lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -24);
	lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(grid, 18, 0);
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

	for (i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
		const struct app *a = &apps[i];
		lv_obj_t *cell, *btn, *icon, *name;

		cell = lv_obj_create(grid);
		lv_obj_set_size(cell, 88, 116);
		lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(cell, 0, 0);
		lv_obj_set_style_pad_all(cell, 0, 0);
		lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

		btn = lv_button_create(cell);
		lv_obj_set_size(btn, 72, 72);
		lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 4);
		lv_obj_set_style_radius(btn, 20, 0);
		lv_obj_set_style_bg_color(btn, lv_color_hex(a->c1), 0);
		lv_obj_set_style_bg_grad_color(btn, lv_color_hex(a->c2), 0);
		lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_shadow_width(btn, 18, 0);
		lv_obj_set_style_shadow_color(btn, lv_color_hex(a->c2), 0);
		lv_obj_set_style_shadow_opa(btn, LV_OPA_50, 0);
		lv_obj_set_style_transform_scale(btn, 230, LV_STATE_PRESSED);
		lv_obj_set_style_transform_pivot_x(btn, 36, 0);
		lv_obj_set_style_transform_pivot_y(btn, 36, 0);
		lv_obj_add_event_cb(btn, a->cb, LV_EVENT_CLICKED, (void *)a->arg);

		icon = lv_label_create(btn);
		lv_label_set_text(icon, a->icon);
		lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
		lv_obj_center(icon);

		name = lv_label_create(cell);
		lv_label_set_text(name, a->name);
		lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
		lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
	}

	lv_screen_load(home_scr);
}

/* ---------------------------------------------------------------- sleep */

static uint64_t woke_us;	/* when the last system sleep ended */

static void drain(int fd)
{
	struct input_event ev[16];

	if (fd >= 0)
		while (read(fd, ev, sizeof(ev)) > 0)
			;
}

/* n72_lcd.panel_sleep reads "Y" or "N". */
static bool panel_sleep_param_yes(void)
{
	char c = 0;
	FILE *f = fopen("/sys/module/n72_lcd/parameters/panel_sleep", "r");

	if (!f)
		return false;
	if (fread(&c, 1, 1, f) != 1)
		c = 0;
	fclose(f);
	return c == 'Y';
}

/* Does the kernel offer this sleep state ("mem" only with n72_sleep.deep=1)? */
static bool sleep_offered(const char *state)
{
	char buf[64] = "";
	FILE *f = fopen("/sys/power/state", "r");

	if (!f)
		return false;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return strstr(buf, state) != NULL;
}

static bool write_sleep_state(const char *state)
{
	int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return false;
	r = write(fd, state, strlen(state));
	close(fd);
	if (r != (ssize_t)strlen(state))
		fprintf(stderr, "n72-ui: %s failed: %s\n", state, strerror(errno));
	return r == (ssize_t)strlen(state);
}

/*
 * Sleep until Power or Home wakes the system. On battery, suspend to RAM
 * when the kernel offers it (the SoC powers off); on USB power suspend to
 * idle, since the charger's events wake the PMU straight out of
 * hibernate (and plugging in should wake us anyway). N72_SLEEP=0 turns
 * sleep off, N72_SLEEP=freeze always uses suspend to idle. Returns false
 * when sleep is unavailable or refused, so the caller can fall back to
 * blanking the screen.
 */
static bool system_sleep(void)
{
	const char *e = getenv("N72_SLEEP");
	bool deep, ok;

	if (e && !strcmp(e, "0"))
		return false;
	deep = !(e && !strcmp(e, "freeze")) && !usb_powered() && sleep_offered("mem");
	/*
	 * The display driver switches the backlight and the panel off
	 * together and back on once the picture is back, unless it leaves the
	 * panel on in s2idle: blanking here first only showed a dim, unlit
	 * picture for a moment on the way down and up.
	 */
	if (!deep && !panel_sleep_param_yes())
		backlight_power(false);
	ok = deep && write_sleep_state("mem");
	if (!ok)	/* not deep, or the PMU refused: suspend to idle */
		ok = write_sleep_state("freeze");
	woke_us = mono_us();

	/* the wake press and anything touched meanwhile are not input */
	drain(keys_fd);
	drain(touch_wait_fd);
	screen_off = false;
	backlight_power(true);
	lv_obj_invalidate(lv_screen_active());
	return ok;
}

/* Sleep after a while without input, unless running from USB power. */
static void idle_check(lv_timer_t *t)
{
	static long idle_ms = -1;

	LV_UNUSED(t);
	if (idle_ms < 0) {
		const char *e = getenv("N72_IDLE_SLEEP");

		idle_ms = (e ? atol(e) : 120) * 1000;
	}
	if (!idle_ms || screen_off || lv_display_get_inactive_time(NULL) < (uint32_t)idle_ms)
		return;
	if (usb_powered())
		return;
	if (!system_sleep()) {
		screen_off = true;
		backlight_power(false);
	}
	lv_display_trigger_activity(NULL);
}

/* -------------------------------------------------------------- buttons */

static void keys_poll(lv_timer_t *t)
{
	struct input_event ev;

	LV_UNUSED(t);
	if (keys_fd < 0)
		return;
	static uint64_t home_down_us;

	while (read(keys_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		/*
		 * Home acts on release: held for a second or more it opens the
		 * log view, which needs no touch (touch may be what is broken).
		 */
		if (ev.code == KEY_HOMEPAGE) {
			if (ev.value == 1)
				home_down_us = mono_us();
			if (ev.value != 0)
				continue;
		} else if (ev.value != 1) {
			continue;
		}
		lv_display_trigger_activity(NULL);
		switch (ev.code) {
		case KEY_HOMEPAGE:
			if (screen_off) {
				screen_off = false;
				backlight_power(true);
			} else if (home_down_us && mono_us() - home_down_us >= 1000000) {
				go_home();
				open_logs(NULL);
			} else {
				go_home();
			}
			home_down_us = 0;
			break;
		case KEY_POWER:
			/* a late report of the press that woke us */
			if (mono_us() - woke_us < 500000)
				break;
			if (!screen_off && system_sleep())
				break;
			screen_off = !screen_off;
			backlight_power(!screen_off);
			break;
		case KEY_VOLUMEUP:
			if (!logs_scroll(1))
				backlight_set(backlight_get() + 10);
			break;
		case KEY_VOLUMEDOWN:
			if (!logs_scroll(-1))
				backlight_set(backlight_get() - 10);
			break;
		}
	}
}

/* ---------------------------------------------------------------- bench */

static double redraw_ms(int n)
{
	uint32_t t0;
	int i;

	lv_refr_now(NULL);
	t0 = tick_ms();
	for (i = 0; i < n; i++) {
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
	}
	return (double)(tick_ms() - t0) / n;
}

/* Redraw the home screen, then a fill-heavy screen of plain rectangles. */
static int bench(void)
{
	lv_obj_t *scr;
	double home, rects, overlay;
	int i;

	home = redraw_ms(20);
	fprintf(stderr, "bench: home %.1f ms\n", home);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x203040), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	for (i = 0; i < 24; i++) {
		lv_obj_t *r = lv_obj_create(scr);

		lv_obj_set_size(r, 96, 72);
		lv_obj_set_pos(r, 8 + (i % 3) * 104, 8 + (i / 3) * 58);
		lv_obj_set_style_radius(r, 0, 0);
		lv_obj_set_style_border_width(r, 0, 0);
		lv_obj_set_style_bg_color(r, lv_color_hsv_to_rgb(i * 15, 70, 90), 0);
	}
	lv_screen_load(scr);
	rects = redraw_ms(20);
	fprintf(stderr, "bench: rectangles %.1f ms\n", rects);

	/* Translucent plain panels and an opacity layer over the rectangles. */
	for (i = 0; i < 3; i++) {
		lv_obj_t *p = lv_obj_create(scr);

		lv_obj_set_size(p, 280, 110);
		lv_obj_set_pos(p, 20, 20 + i * 150);
		lv_obj_set_style_radius(p, 0, 0);
		lv_obj_set_style_border_width(p, 0, 0);
		lv_obj_set_style_bg_color(p, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(p, LV_OPA_30, 0);
	}
	{
		lv_obj_t *grp = lv_obj_create(scr), *l;

		lv_obj_set_size(grp, 240, 300);
		lv_obj_center(grp);
		lv_obj_set_style_opa_layered(grp, LV_OPA_70, 0);	/* drawn via a layer */
		lv_obj_set_style_radius(grp, 0, 0);
		lv_obj_set_style_bg_color(grp, lv_color_hex(0x2a9d8f), 0);
		l = lv_label_create(grp);
		lv_label_set_text(l, "Layer with 70% opacity");
		lv_obj_center(l);
	}
	overlay = redraw_ms(20);

	printf("n72-ui bench (%s): home %.1f ms, 24 rectangles %.1f ms, + translucent panels and layer %.1f ms\n",
	       mbx_gpu_stats.submits ? "GPU" : "CPU", home, rects, overlay);
	printf("  GPU submits %lu: fills %lu, blended fills %lu, layers %lu, flushes %lu; pool misses %lu\n",
	       mbx_gpu_stats.submits, mbx_gpu_stats.fills, mbx_gpu_stats.blends,
	       mbx_gpu_stats.layers, mbx_gpu_stats.flushes, mbx_gpu_stats.pool_misses);
	printf("  time in GPU ioctls: sync %.1f ms (%llu KiB), 2D submit %.1f ms, 3D blit %.1f ms; %lu tasks async\n",
	       mbx_gpu_stats.sync_us / 1000, mbx_gpu_stats.sync_bytes / 1024,
	       mbx_gpu_stats.submit_us / 1000, mbx_gpu_stats.blit3d_us / 1000,
	       mbx_gpu_stats.async_done);
	printf("  fills left to the CPU: radius %lu, gradient %lu, small %lu\n",
	       mbx_gpu_stats.fill_radius, mbx_gpu_stats.fill_grad, mbx_gpu_stats.fill_small);
	printf("  tasks seen by type:");
	for (i = 0; i < 16; i++)
		if (mbx_gpu_stats.seen[i])
			printf(" %d:%lu", i, mbx_gpu_stats.seen[i]);
	printf("\n");
	if (getenv("N72_BENCH_SHOT"))
		sleep(2);	/* leave the last frame up for a screenshot */
	return 0;
}

/* ----------------------------------------------------------------- main */

static void restore_tty(void)
{
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
}

static void on_signal(int sig)
{
	restore_tty();
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv)
{
	const char *fb = getenv("N72_FB");
	const char *touch = getenv("N72_TOUCH");
	const char *keys = getenv("N72_KEYS");
	const char *cal = getenv("N72_TOUCH_CAL");
	const char *swap = getenv("N72_TOUCH_SWAP");
	lv_display_t *disp;
	lv_indev_t *indev;
	int i;

	for (i = 0; i < argc && i < 3; i++)
		self_argv[i] = argv[i];

	/* Keep the text console from drawing over the UI. */
	tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	atexit(restore_tty);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	lv_init();
	lv_tick_set_cb(tick_ms);
	backlight_init();

	disp = NULL;
	if (!getenv("N72_GPU") || strcmp(getenv("N72_GPU"), "0"))
		disp = mbx_gpu_display_create();
	if (!disp) {
		disp = lv_linux_fbdev_create();
		lv_linux_fbdev_set_file(disp, fb ? fb : "/dev/fb0");
	}
	lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
			      lv_palette_main(LV_PALETTE_PINK), true,
			      &lv_font_montserrat_14);

	if (!touch)
		touch = find_input("Zephyr2");
	if (touch) {
		indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
		if (indev && cal) {
			int c[4];

			if (sscanf(cal, "%d %d %d %d", &c[0], &c[1], &c[2],
				   &c[3]) == 4)
				lv_evdev_set_calibration(indev, c[0], c[1],
							 c[2], c[3]);
		}
		if (indev && swap && *swap == '1')
			lv_evdev_set_swap_axes(indev, true);
		if (indev) {
			touch_indev = indev;
			touch_wait_fd = open(touch, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (touch_wait_fd >= 0) {
				int clk = CLOCK_MONOTONIC;

				ioctl(touch_wait_fd, EVIOCSCLOCKID, &clk);
			}
		}
		lat_on = getenv("N72_LATENCY") && *getenv("N72_LATENCY") == '1';
	}

	if (!keys)
		keys = find_input("gpio-keys");
	if (keys)
		keys_fd = open(keys, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	lv_timer_create(keys_poll, 30, NULL);
	lv_timer_create(idle_check, 1000, NULL);

	build_home();
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		return bench();

	/*
	 * Sleep until the next LVGL timer or until the touch screen or a button
	 * has something, and hand touch input to LVGL straight away instead of
	 * waiting for its read timer.
	 */
	for (;;) {
		struct pollfd pf[2] = {
			{ .fd = touch_wait_fd, .events = POLLIN },
			{ .fd = keys_fd, .events = POLLIN },
		};
		uint32_t idle = lv_timer_handler();

		if (poll(pf, 2, LV_MIN(idle, LV_DEF_REFR_PERIOD)) > 0) {
			if (pf[0].revents & POLLIN) {
				struct input_event ev[16];
				ssize_t r;

				while ((r = read(touch_wait_fd, ev, sizeof(ev))) > 0) {
					const struct input_event *e = &ev[r / sizeof(ev[0]) - 1];

					/* time the newest event still waiting for a frame */
					if (!lat_event_us)
						lat_event_us = (uint64_t)e->input_event_sec * 1000000 +
							       e->input_event_usec;
				}
				lat_read_us = mono_us();
				lv_indev_read(touch_indev);
				/* draw the result now, not at the next refresh tick */
				render_now();
			}
			if (pf[1].revents & POLLIN)
				keys_poll(NULL);
		}
	}
	return 0;
}
