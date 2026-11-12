// SPDX-License-Identifier: MIT
/*
 * n72-ui: a small LVGL launcher for Linux on the iPod touch 2G (N72AP).
 *
 * Display: /dev/fb0 (simplefb, 320x480 XRGB8888). Touch: the Zephyr2
 * evdev device. Buttons: gpio-keys (Home returns to the launcher, Power
 * puts the system to sleep (s2idle) and Power or Home wakes it, the volume
 * keys change the sound volume; audio.c).
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
#include <glob.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#ifndef SCHED_IDLE
#define SCHED_IDLE 5	/* (Linux; glibc shows it only with _GNU_SOURCE) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/indev/lv_indev_private.h"
#include "mbx_gpu.h"

#define W 320
#define H 480

static char *self_argv[4];
static lv_obj_t *home_scr;
static int keys_fd = -1;
static int touch_wait_fd = -1;
static lv_timer_t *refr_timer;		/* LVGL's display refresh */		/* own handle on the touch device, for poll() */
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
	struct mbx_gpu_stats g0 = mbx_gpu_stats;

	lat_input_frame = true;
	lv_refr_now(NULL);
	lat_input_frame = false;
	lat_event_us = 0;	/* nothing to show for this input: don't count it */
	d = mono_us() - t0;
	lat_render_max_us = LV_MAX(lat_render_max_us, d);
	/* N72_LATENCY: say what a slow frame was made of */
	if (lat_on && d > 50000) {
		struct mbx_gpu_stats *g = &mbx_gpu_stats;
		int i;

		fprintf(stderr, "slow frame %.1f ms: submits %lu fills %lu blends %lu layers %lu; "
			"CPU fills radius %lu grad %lu small %lu; sync %.1f ms (%llu KiB) submit %.1f ms "
			"blit3d %.1f ms; tasks",
			d / 1000.0, g->submits - g0.submits, g->fills - g0.fills,
			g->blends - g0.blends, g->layers - g0.layers,
			g->fill_radius - g0.fill_radius, g->fill_grad - g0.fill_grad,
			g->fill_small - g0.fill_small, (g->sync_us - g0.sync_us) / 1000,
			(g->sync_bytes - g0.sync_bytes) / 1024, (g->submit_us - g0.submit_us) / 1000,
			(g->blit3d_us - g0.blit3d_us) / 1000);
		for (i = 0; i < 16; i++)
			if (g->seen[i] != g0.seen[i])
				fprintf(stderr, " %d:%lu", i, g->seen[i] - g0.seen[i]);
		fprintf(stderr, "\n");
	}
}

/*
 * N72_FPS=1: frame pacing. Frames less than 100 ms apart form a burst (an
 * animation); at the end of one, print its frame count, intervals and the
 * time spent rendering (refresh start to flush).
 */
static bool fps_on;
static uint64_t fps_last_us, fps_first_us, fps_refr_us, fps_int_max, fps_rend_sum, fps_rend_max;
static unsigned int fps_n, fps_slow;
static struct mbx_gpu_stats fps_g0;

/*
 * At the start of every refresh: step the animations to now. They have
 * their own timer, which drifts against the refresh timer, so a frame
 * sometimes showed the previous step and the next one two steps (frames
 * landing 33 ms apart though each took 10 ms to draw).
 */
void n72_refr_start(void)
{
	fps_refr_us = mono_us();
	lv_anim_refr_now();
}

uint64_t n72_mark_us;	/* (debug) a point to time long frames from, e.g. a slide start */

static void fps_frame(void)
{
	uint64_t now = mono_us(), rend = now - fps_refr_us, iv = now - fps_last_us;

	if (fps_n && iv > 40000 && iv < 100000)
		fprintf(stderr, "long frame: %.1f ms (render %.1f), %.0f ms after the mark\n",
			iv / 1000.0, rend / 1000.0, (now - n72_mark_us) / 1000.0);

	if (fps_n && iv > 100000) {
		if (fps_n >= 4)
			fprintf(stderr, "burst: %u frames in %.0f ms (%.1f fps), interval max %.1f ms, "
				"%u over 20 ms; render avg %.1f max %.1f ms; pool misses %lu\n", fps_n,
				(fps_last_us - fps_first_us) / 1000.0,
				(fps_n - 1) * 1e6 / (double)(fps_last_us - fps_first_us + 1),
				fps_int_max / 1000.0, fps_slow, fps_rend_sum / 1000.0 / fps_n,
				fps_rend_max / 1000.0, mbx_gpu_stats.pool_misses);
		if (fps_n >= 4) {
			struct mbx_gpu_stats *g = &mbx_gpu_stats;
			int i;

			fprintf(stderr, "  per frame: GPU fills %.1f (rounded %.1f) blends %.1f layers %.1f; "
				"CPU fills radius %.1f small %.1f; GPU ioctl ms: sync %.1f submit %.1f 3d %.1f, vblank wait %.1f; fill skipped %.0f kpx; tasks",
				(g->fills - fps_g0.fills) / (double)fps_n,
				(g->fill_corners - fps_g0.fill_corners) / (double)fps_n,
				(g->blends - fps_g0.blends) / (double)fps_n,
				(g->layers - fps_g0.layers) / (double)fps_n,
				(g->fill_radius - fps_g0.fill_radius) / (double)fps_n,
				(g->fill_small - fps_g0.fill_small) / (double)fps_n,
				(g->sync_us - fps_g0.sync_us) / 1000 / fps_n,
				(g->submit_us - fps_g0.submit_us) / 1000 / fps_n,
				(g->blit3d_us - fps_g0.blit3d_us) / 1000 / fps_n,
				(g->flip_us - fps_g0.flip_us) / 1000 / fps_n,
				(double)(g->trim_px - fps_g0.trim_px) / 1000 / fps_n);
			for (i = 0; i < 16; i++)
				if (g->seen[i] != fps_g0.seen[i])
					fprintf(stderr, " %d:%.1f", i, (g->seen[i] - fps_g0.seen[i]) / (double)fps_n);
			fprintf(stderr, "\n");
		}
		fps_n = 0;
	}
	if (!fps_n) {
		fps_g0 = mbx_gpu_stats;
		fps_first_us = now;
		fps_int_max = fps_rend_sum = fps_rend_max = 0;
		fps_slow = 0;
	} else {
		fps_int_max = LV_MAX(fps_int_max, iv);
		fps_slow += iv > 20000;
	}
	fps_rend_sum += rend;
	fps_rend_max = LV_MAX(fps_rend_max, rend);
	fps_n++;
	fps_last_us = now;
}

/*
 * N72_DRAGS="wait:x0,y0,x1,y1,ms wait:..." replays drags through a virtual
 * pointer, for measuring scrolling without a finger: after @wait ms, press
 * at x0,y0, move to x1,y1 over @ms, release.
 */
static struct drag { uint32_t wait, ms; int32_t x0, y0, x1, y1; } drags[16];
static int ndrags, drag_i;
static uint32_t drag_t0;

static void drag_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct drag *d;
	uint32_t t;

	LV_UNUSED(indev);
	data->state = LV_INDEV_STATE_RELEASED;
	if (drag_i >= ndrags)
		return;
	d = &drags[drag_i];
	t = tick_ms() - drag_t0;
	if (t < d->wait) {
		data->point.x = d->x0;
		data->point.y = d->y0;
		return;
	}
	t -= d->wait;
	if (t > d->ms) {
		data->point.x = d->x1;
		data->point.y = d->y1;
		if (t > d->ms + 20) {		/* released: next drag */
			drag_i++;
			drag_t0 = tick_ms();
			fprintf(stderr, "drag %d done\n", drag_i);
		} else {
			data->state = LV_INDEV_STATE_PRESSED;
		}
		return;
	}
	data->point.x = d->x0 + (d->x1 - d->x0) * (int32_t)t / (int32_t)d->ms;
	data->point.y = d->y0 + (d->y1 - d->y0) * (int32_t)t / (int32_t)d->ms;
	data->state = LV_INDEV_STATE_PRESSED;
}

static void drags_init(void)
{
	const char *p = getenv("N72_DRAGS");
	lv_indev_t *in;

	if (!p)
		return;
	while (ndrags < 16) {
		struct drag *d = &drags[ndrags];
		int n = 0;

		if (sscanf(p, "%u:%d,%d,%d,%d,%u%n", &d->wait, &d->x0, &d->y0, &d->x1, &d->y1,
			   &d->ms, &n) != 6)
			break;
		ndrags++;
		p += n;
	}
	in = lv_indev_create();
	lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(in, drag_read);
	drag_t0 = tick_ms();
}

/* Called when a frame has been flushed to the screen. */
void n72_frame_shown(void)
{
	uint64_t d;

	if (fps_on)
		fps_frame();

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
static int notify_fd = -1;		/* flip thread: a frame went to the display */
static bool render_pending;

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

/*
 * The screen off and on. The display driver (lcd-n72) powers the panel
 * down and gates the display controller's clocks, as iOS does, so the
 * system can idle at its slowest clock; it switches the backlight with
 * it. Without that driver only the backlight goes. Touch is ignored while
 * the screen is off.
 */
#define PANEL_POWER	"/sys/devices/platform/soc/38900000.lcd/panel_power"

static void drain(int fd);
static void home_wake(void);
static void screen_off_or_sleep(void);
/* the touch device's "inhibited" (sysfs): the driver stops the controller */
static char touch_inhibit[96];
static char rt_node[64];	/* the touch router's virtual device, for applications */
static void rt_reset(void);

static void ui_lock(void);

static void screen_set(bool on)
{
	if (!on && screen_off)
		return;
	screen_off = !on;
	if (access(PANEL_POWER, W_OK) == 0)
		write_int(PANEL_POWER, on);
	else
		backlight_power(on);
	/* the lock screen goes up in the dark: it is what waking shows */
	if (!on)
		ui_lock();
	if (touch_indev)
		lv_indev_enable(touch_indev, on);
	if (touch_inhibit[0])
		write_int(touch_inhibit, !on);
	drain(touch_wait_fd);
	rt_reset();
	if (on) {
		home_wake();
		lv_obj_invalidate(lv_screen_active());
	}
}

#include "audio.c"
#include "ui_m3.c"

/*
 * The touch router. With an application running, the touch screen serves
 * two programs, and each touch must reach only one. The launcher grabs the
 * device (EVIOCGRAB) and reads it alone; at the moment a finger comes down
 * it asks the UI (ui_wants_touch) whose touch it is. The launcher's go to
 * LVGL through rt_read_cb, the application's, every event of every frame
 * until the finger lifts, to a uinput device with the same axes, which the
 * application opens (N72_TOUCH). Without uinput, both read the device as
 * before.
 */
static int rt_uinput = -1;
static struct { int min_x, max_x, min_y, max_y; bool swap; } rt_cal;
static int rt_raw_x, rt_raw_y;
static bool rt_down;			/* the device's state */
static lv_point_t rt_pt;		/* what LVGL sees */
static bool rt_ui_down;
static enum { RT_NONE, RT_UI, RT_APP } rt_owner;
static struct input_event rt_frame[64];
static int rt_n;
static uint64_t rt_frame_us;		/* when LVGL last had a frame */
static bool rt_ui_touch;		/* a launcher touch is down */
static bool rt_claim;			/* ... and ui_drag() has it */

static void rt_reset(void)
{
	if (rt_owner == RT_APP && rt_uinput >= 0) {
		/* the application's touch ends here */
		struct input_event up[3] = {
			{ .type = EV_KEY, .code = BTN_TOUCH, .value = 0 },
			{ .type = EV_SYN, .code = SYN_MT_REPORT },
			{ .type = EV_SYN, .code = SYN_REPORT },
		};

		if (write(rt_uinput, up, sizeof(up)) < 0)
			;
	}
	rt_n = 0;
	rt_owner = RT_NONE;
	rt_down = rt_ui_down = false;
	rt_ui_touch = rt_claim = false;
}

static int rt_scale(int v, int in_min, int in_max, int out_max)
{
	if (in_min != in_max)
		v = (v - in_min) * out_max / (in_max - in_min);
	return LV_CLAMP(0, v, out_max);
}

static lv_point_t rt_map(int x, int y)
{
	lv_display_t *d = lv_display_get_default();
	lv_point_t p;

	if (rt_cal.swap) {
		int t = x;

		x = y;
		y = t;
	}
	p.x = rt_scale(x, rt_cal.min_x, rt_cal.max_x, lv_display_get_horizontal_resolution(d) - 1);
	p.y = rt_scale(y, rt_cal.min_y, rt_cal.max_y, lv_display_get_vertical_resolution(d) - 1);
	return p;
}

static void rt_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	LV_UNUSED(indev);
	data->point = rt_pt;
	data->state = rt_ui_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* A frame is complete (SYN_REPORT): route it. True if LVGL has news. */
static bool rt_frame_done(void)
{
	lv_point_t p = rt_map(rt_raw_x, rt_raw_y);
	bool news = false;

	if (rt_owner == RT_NONE && rt_down)
		rt_owner = app_pid > 0 && !ui_wants_touch(p.x, p.y) ? RT_APP : RT_UI;
	if (rt_owner == RT_APP) {
		if (write(rt_uinput, rt_frame, rt_n * sizeof(rt_frame[0])) < 0)
			;
	} else {
		if (rt_down || rt_ui_touch) {
			int ph = !rt_down ? 2 : rt_ui_touch ? 1 : 0;

			if (ui_drag(ph, p.x, p.y) && !rt_claim) {
				/* the launcher's drag now (ui_drag has ended LVGL's press) */
				rt_claim = true;
			}
		}
		rt_ui_touch = rt_down;
		if (rt_claim) {
			news = rt_ui_down;
			rt_ui_down = false;
			if (!rt_down)
				rt_claim = false;
		} else {
			news = rt_ui_down != rt_down || rt_pt.x != p.x || rt_pt.y != p.y;
			rt_pt = p;
			rt_ui_down = rt_down;
		}
	}
	if (!rt_down)
		rt_owner = RT_NONE;
	rt_n = 0;
	return news;
}

/* Read what the touch screen has; hand LVGL each frame of its touches. */
static void rt_feed(int fd, lv_indev_t *indev)
{
	struct input_event ev[32];
	ssize_t r;
	int i;

	while ((r = read(fd, ev, sizeof(ev))) > 0) {
		for (i = 0; i < (int)(r / sizeof(ev[0])); i++) {
			const struct input_event *e = &ev[i];

			if (e->type == EV_SYN && e->code == SYN_DROPPED) {
				rt_n = 0;
				continue;
			}
			if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_MT_POSITION_X))
				rt_raw_x = e->value;
			else if (e->type == EV_ABS && (e->code == ABS_Y || e->code == ABS_MT_POSITION_Y))
				rt_raw_y = e->value;
			else if (e->type == EV_KEY && e->code == BTN_TOUCH)
				rt_down = e->value != 0;
			if (rt_n < (int)(sizeof(rt_frame) / sizeof(rt_frame[0])))
				rt_frame[rt_n++] = *e;
			if (e->type == EV_SYN && e->code == SYN_REPORT) {
				/* every frame: a move and the lift in one read
				 * would lose the move (and the gesture) */
				if (rt_frame_done()) {
					rt_frame_us = mono_us();
					lv_indev_read(indev);
				}
				if (!lat_event_us && rt_owner != RT_APP)
					lat_event_us = (uint64_t)e->input_event_sec * 1000000 +
						       e->input_event_usec;
			}
		}
	}
}

/*
 * LVGL's own read timer, between frames: a read with no movement in it
 * resets LVGL's gesture count, so a swipe of small steps never became one.
 * Read only when the finger has rested a while (long press needs reads).
 */
static void rt_timer_cb(lv_timer_t *t)
{
	if (rt_ui_down && mono_us() - rt_frame_us < 40000)
		return;
	lv_indev_read(lv_timer_get_user_data(t));
}

/* Set up the router on the touch screen fd; false: no uinput. */
static bool rt_init(int fd, const char *cal, const char *swap)
{
	unsigned long absbits[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
	struct uinput_setup us;
	struct input_absinfo ai;
	char sys[32], path[96];
	struct dirent *de;
	DIR *d;
	int u, c;

	u = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (u < 0)
		return false;
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
		goto fail;
	ioctl(u, UI_SET_EVBIT, EV_SYN);
	ioctl(u, UI_SET_EVBIT, EV_KEY);
	ioctl(u, UI_SET_EVBIT, EV_ABS);
	ioctl(u, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(u, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	for (c = 0; c < ABS_CNT; c++) {
		struct uinput_abs_setup as;

		if (!(absbits[c / (8 * sizeof(long))] & (1UL << (c % (8 * sizeof(long))))))
			continue;
		if (ioctl(fd, EVIOCGABS(c), &ai) < 0)
			continue;
		memset(&as, 0, sizeof(as));
		as.code = c;
		as.absinfo = ai;
		ioctl(u, UI_SET_ABSBIT, c);
		if (ioctl(u, UI_ABS_SETUP, &as) < 0)
			goto fail;
	}
	memset(&us, 0, sizeof(us));
	ioctl(fd, EVIOCGID, &us.id);
	snprintf(us.name, sizeof(us.name), "n72-ui touch");
	if (ioctl(u, UI_DEV_SETUP, &us) < 0 || ioctl(u, UI_DEV_CREATE) < 0)
		goto fail;

	/* /dev/input/eventN of the new device, for the applications */
	rt_node[0] = 0;
	if (ioctl(u, UI_GET_SYSNAME(sizeof(sys)), sys) >= 0) {
		snprintf(path, sizeof(path), "/sys/devices/virtual/input/%s", sys);
		d = opendir(path);
		while (d && (de = readdir(d)))
			if (!strncmp(de->d_name, "event", 5))
				snprintf(rt_node, sizeof(rt_node), "/dev/input/%s", de->d_name);
		if (d)
			closedir(d);
	}
	if (ioctl(fd, EVIOCGRAB, 1) < 0) {
		ioctl(u, UI_DEV_DESTROY);
		goto fail;
	}

	if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) {
		rt_cal.min_x = ai.minimum;
		rt_cal.max_x = ai.maximum;
	}
	if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) {
		rt_cal.min_y = ai.minimum;
		rt_cal.max_y = ai.maximum;
	}
	if (cal)	/* as lv_evdev_set_calibration: min x, min y, max x, max y */
		sscanf(cal, "%d %d %d %d", &rt_cal.min_x, &rt_cal.min_y,
		       &rt_cal.max_x, &rt_cal.max_y);
	rt_cal.swap = swap && *swap == '1';
	rt_uinput = u;
	fprintf(stderr, "n72-ui: touch router on, applications get %s\n",
		rt_node[0] ? rt_node : "(no node)");
	return true;
fail:
	close(u);
	return false;
}

/*
 * A crash report on stderr (the log): signal, fault address, PC and LR, and
 * a few words of the stack (return addresses among them), for the
 * unstripped n72-ui-build/n72-ui.debug and addr2line.
 */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
	const ucontext_t *uc = ctx;
	const struct sigcontext *mc = &uc->uc_mcontext;
	const unsigned long *sp = (const unsigned long *)mc->arm_sp;
	char buf[512];
	int n, i;

	n = snprintf(buf, sizeof(buf), "n72-ui: signal %d at %p: pc %08lx lr %08lx sp %08lx\n  stack:",
		     sig, si->si_addr, mc->arm_pc, mc->arm_lr, mc->arm_sp);
	for (i = 0; i < 24 && n < (int)sizeof(buf) - 12; i++)
		n += snprintf(buf + n, sizeof(buf) - n, " %08lx", sp[i]);
	if (n < (int)sizeof(buf) - 1)
		buf[n++] = '\n';
	write(2, buf, n);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void crash_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_crash;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
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
/*
 * What can tell a wake by the network from one by a person: how often the
 * Wi-Fi chip's wake line and the two buttons have interrupted, and whether
 * the cable is in.
 */
struct wake_marks {
	long wlan, home, power;
	int wlan_irq, home_irq, power_irq;
	bool usb;
};

static void wake_marks(struct wake_marks *m)
{
	char line[256];
	FILE *f = fopen("/proc/interrupts", "r");

	memset(m, 0, sizeof(*m));
	m->wlan_irq = m->home_irq = m->power_irq = -1;
	m->usb = usb_powered();
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':'), *end;
		long n;
		int irq;

		if (!colon)
			continue;
		line[strcspn(line, "\n")] = 0;
		irq = atoi(line);
		n = strtol(colon + 1, &end, 10);
		if (end == colon + 1)
			continue;
		end = line + strlen(line);
		if (end - line > 15 && !strcmp(end - 15, "brcmf_host_wake")) {
			m->wlan = n;
			m->wlan_irq = irq;
		} else if (end - line > 5 && !strcmp(end - 5, " Home")) {
			m->home = n;
			m->home_irq = irq;
		} else if (end - line > 6 && !strcmp(end - 6, " Power")) {
			m->power = n;
			m->power_irq = irq;
		}
	}
	fclose(f);
}

/* The interrupt that ended the last suspend to idle, or -1. */
static int wake_irq(void)
{
	char buf[32] = "";
	FILE *f = fopen("/sys/power/pm_wakeup_irq", "r");

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return buf[0] ? atoi(buf) : -1;
}

/*
 * Was it only the network? Wi-Fi stays associated in suspend to idle, and
 * a frame for this station wakes the system (so that it answers, and an
 * SSH login gets in). That is no reason to light the screen. Anything not
 * clearly the network counts as a person: a button's interrupt, the cable,
 * a wake the kernel cannot name.
 */
static bool woke_for_network(const struct wake_marks *before)
{
	struct wake_marks now;
	int irq = wake_irq();

	wake_marks(&now);
	if (now.home != before->home || now.power != before->power || now.usb != before->usb)
		return false;
	if (irq < 0 || irq == now.home_irq || irq == now.power_irq)
		return false;
	return now.wlan > before->wlan;
}

/* Is somebody logged in over SSH? (dropbear: the listener plus one per session) */
static bool ssh_sessions(void)
{
	glob_t g;
	int n = 0;

	if (glob("/proc/[0-9]*/comm", 0, NULL, &g))
		return false;
	for (size_t i = 0; i < g.gl_pathc && n < 2; i++) {
		char buf[32] = "";
		FILE *f = fopen(g.gl_pathv[i], "r");

		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && !strncmp(buf, "dropbear", 8))
			n++;
		fclose(f);
	}
	globfree(&g);
	return n >= 2;
}

/*
 * Awake for the network, screen dark: stay up for a few seconds, and for
 * as long as an SSH session lasts, then sleep on. True: Power or Home was
 * pressed meanwhile, the screen is wanted.
 */
static bool dark_linger(void)
{
	uint64_t until = mono_us() + 4000000, checked = 0;

	for (;;) {
		struct pollfd p = { .fd = keys_fd, .events = POLLIN };
		struct input_event ev;

		poll(&p, 1, 250);
		while (keys_fd >= 0 && read(keys_fd, &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && ev.value == 1 &&
			    (ev.code == KEY_POWER || ev.code == KEY_HOMEPAGE))
				return true;
		if (mono_us() - checked > 1000000) {
			checked = mono_us();
			if (ssh_sessions())
				until = checked + 4000000;
		}
		if (mono_us() > until)
			return false;
	}
}

static bool system_sleep(void)
{
	const char *e = getenv("N72_SLEEP");
	bool deep, ok;

	if (e && !strcmp(e, "0"))
		return false;
	for (;;) {
		struct wake_marks before;

		deep = !(e && !strcmp(e, "freeze")) && !usb_powered() && sleep_offered("mem");
		wake_marks(&before);
		/* (the caller has switched the panel off: screen_set(false)) */
		ok = deep && write_sleep_state("mem");
		if (ok) {
			deep = true;
		} else {	/* not deep, or the PMU refused: suspend to idle */
			deep = false;
			ok = write_sleep_state("freeze");
		}
		woke_us = mono_us();
		/* out of the deep sleep nothing says what woke us: the screen */
		if (!ok || deep || !woke_for_network(&before))
			break;
		if (dark_linger())
			break;
	}

	/* the wake press and anything touched meanwhile are not input */
	drain(keys_fd);
	screen_set(true);
	return ok;
}

/* Is sound playing? (any ALSA playback stream running) */
static bool audio_playing(void)
{
	glob_t g;
	bool on = false;

	if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g))
		return false;
	for (size_t i = 0; i < g.gl_pathc && !on; i++) {
		char buf[64] = "";
		FILE *f = fopen(g.gl_pathv[i], "r");

		if (!f)
			continue;
		on = fgets(buf, sizeof(buf), f) && strstr(buf, "RUNNING");
		fclose(f);
	}
	globfree(&g);
	return on;
}

/*
 * Power, the idle timeout and Settings > Power > Sleep: sleep, or with
 * sound playing only turn the screen off, and sleep once it stops
 * (playback_check). Screen off with the cable out lets the kernel gate
 * the display and USB and idle at the system clock's slowest divider.
 */
static bool screen_off_for_sound;

static void screen_off_or_sleep(void)
{
	/*
	 * The panel off first, then the lock screen drawn into it, then the
	 * system sleep: drawing it lit showed it for the second the sleep
	 * takes, and the kernel's own panel bring-up on wake showed whatever
	 * was in the controller before the layers were back.
	 */
	screen_set(false);
	if (audio_playing()) {
		screen_off_for_sound = true;
		screen_set(false);
		return;
	}
	if (!system_sleep())
		screen_set(false);
}

static void playback_check(lv_timer_t *t)
{
	static int quiet;

	LV_UNUSED(t);
	if (!screen_off || !screen_off_for_sound) {
		quiet = 0;
		return;
	}
	if (audio_playing()) {
		quiet = 0;
		return;
	}
	/* a gap between tracks is not the end: two checks in a row */
	if (++quiet < 2)
		return;
	quiet = 0;
	screen_off_for_sound = false;
	if (!system_sleep())
		screen_set(false);
}

/* Sleep after a while without input, unless running from USB power. */
static void idle_check(lv_timer_t *t)
{
	long idle_ms = conf.idle_s * 1000L;	/* Settings > Display */

	LV_UNUSED(t);
	if (!idle_ms || screen_off || lv_display_get_inactive_time(NULL) < (uint32_t)idle_ms)
		return;
	if (usb_powered())
		return;
	screen_off_or_sleep();
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
				screen_off_for_sound = false;
				screen_set(true);
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
			if (screen_off) {
				screen_off_for_sound = false;
				screen_set(true);
			} else {
				screen_off_or_sleep();
			}
			break;
		/* the volume works with the screen off too (sound playing) */
		case KEY_VOLUMEUP:
			if (screen_off || !logs_scroll(1))
				volume_step(1);
			break;
		case KEY_VOLUMEDOWN:
			if (screen_off || !logs_scroll(-1))
				volume_step(-1);
			break;
		}
	}
}

/* ---------------------------------------------------------------- bench */

static double redraw_ms(int n)
{
	uint64_t t0;
	int i;

	lv_refr_now(NULL);
	t0 = mono_us();
	for (i = 0; i < n; i++) {
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
	}
	return (double)(mono_us() - t0) / 1000.0 / n;
}

/*
 * Text: one screen of log-like lines in a single label (Logs), then many
 * short labels (Settings-like rows). "--bench text [n]" prints both; the
 * text is fixed, so runs compare.
 */
static uint32_t lv_color_32_checksum(const lv_draw_buf_t *db)
{
	const uint32_t *p = (const uint32_t *)db->data;
	uint32_t h = 2166136261u, i, nw = db->header.stride / 4 * db->header.h;

	for (i = 0; i < nw; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* FNV-1a over the frame just shown (the active scan-out buffer's pixels) */
static uint32_t screen_checksum(void)
{
	lv_display_t *disp = lv_display_get_default();
	lv_draw_buf_t *db = lv_display_get_buf_active(disp);
	uint32_t h = 2166136261u, x, y;

	if (!db)
		return 0;
	for (y = 0; y < db->header.h; y++) {
		const uint32_t *p = (const uint32_t *)(db->data + y * db->header.stride);

		for (x = 0; x < db->header.w; x++)
			h = (h ^ (p[x] & 0xffffff)) * 16777619u;
	}
	return h;
}

static int bench_text(int n, bool label_only)
{
	uint32_t scr_sum[2] = { 0, 0 };
	static char buf[8192];
	static const char *const words[] = {
		"wlan0:", "brcmfmac", "mmc0:", "new", "SDIO", "card", "at", "address", "0001",
		"apple-mbx", "3b000000.gpu:", "VGP", "program", "255", "words", "PM:", "suspend",
		"entry", "(s2idle)", "exit", "dwc2", "38400000.usb:", "resuming", "gadget", "g_ether",
	};
	lv_obj_t *scr, *l;
	double big, rows;
	size_t o = 0;
	unsigned seed = 1;
	int i, line = 0;

	while (o < sizeof(buf) - 64 && line < 60) {
		int w, nw = 4 + line % 5;

		o += snprintf(buf + o, sizeof(buf) - o, "[%5d.%06d] ", 3 + line * 7, (line * 7919) % 1000000);
		for (w = 0; w < nw; w++) {
			seed = seed * 1103515245u + 12345u;
			o += snprintf(buf + o, sizeof(buf) - o, "%s ", words[(seed >> 16) % (sizeof(words) / sizeof(words[0]))]);
		}
		buf[o++] = '\n';
		line++;
	}
	buf[o] = 0;

	if (label_only)
		goto canvas;
	scr = lv_obj_create(NULL);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	l = lv_label_create(scr);
	lv_obj_set_width(l, 312);
	lv_obj_set_pos(l, 4, 4);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
	lv_label_set_text_static(l, buf);
	lv_screen_load(scr);
	big = redraw_ms(n);
	scr_sum[0] = screen_checksum();

	scr = lv_obj_create(NULL);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	for (i = 0; i < 16; i++) {
		lv_obj_t *t = lv_label_create(scr), *v = lv_label_create(scr);

		lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
		lv_label_set_text_fmt(t, "Setting row %d", i);
		lv_obj_set_pos(t, 12, 8 + i * 29);
		lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
		lv_label_set_text(v, i & 1 ? "On" : "Automatic");
		lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -12, 10 + i * 29);
	}
	lv_screen_load(scr);
	rows = redraw_ms(n);
	scr_sum[1] = screen_checksum();
	printf("n72-ui text bench: log label (%zu chars, 60 lines) %.2f ms, 32 short labels %.2f ms; screen checksums %08x %08x\n",
	       o, big, rows, (unsigned)scr_sum[0], (unsigned)scr_sum[1]);

canvas:
	/* the label alone, drawn by the CPU into an off-screen XRGB8888 canvas (the display format): no display pacing */
	{
		lv_draw_buf_t *db = lv_draw_buf_create(320, 480, LV_COLOR_FORMAT_XRGB8888, 0);
		lv_obj_t *cv = lv_canvas_create(lv_screen_active());
		lv_draw_label_dsc_t d;
		lv_area_t a = { 4, 4, 315, 479 };
		lv_layer_t layer;
		uint64_t t0, t;
		double best = 1e9, sum = 0;

		lv_canvas_set_draw_buf(cv, db);
		lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
		lv_draw_label_dsc_init(&d);
		d.text = buf;
		d.font = &lv_font_montserrat_12;
		d.color = lv_color_white();
		for (i = 0; i < n; i++) {
			lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_COVER);
			t0 = mono_us();
			lv_canvas_init_layer(cv, &layer);
			lv_draw_label(&layer, &d, &a);
			lv_canvas_finish_layer(cv, &layer);
			t = mono_us() - t0;
			sum += t;
			if (t < best)
				best = t;
		}
		printf("  log label into a canvas: avg %.2f ms, best %.2f ms; checksum %08x\n",
		       sum / n / 1000.0, best / 1000.0, (unsigned)lv_color_32_checksum(db));
	}
	return 0;
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

/* SIGUSR1: write what is on screen to /tmp/n72-shot.raw (XRGB8888, 320x480). */
static volatile sig_atomic_t shot_wanted;

static void on_usr1(int sig)
{
	LV_UNUSED(sig);
	shot_wanted = 1;
}

static volatile sig_atomic_t scans_wanted;

static void on_usr2(int sig)
{
	LV_UNUSED(sig);
	scans_wanted = 1;
}

static void shot_poll(lv_timer_t *t)
{
	if (scans_wanted) {		/* SIGUSR2: both flip buffers */
		int i, fr;

		scans_wanted = 0;
		for (i = 0; i < 3; i++) {
			const uint8_t *p = mbx_gpu_scan(i, &fr);
			char name[40];
			FILE *f;

			snprintf(name, sizeof(name), "/tmp/n72-scan%d%s.raw", i, fr ? "-front" : "");
			f = p ? fopen(name, "w") : NULL;
			if (f) {
				fwrite(p, 4 * W, H, f);
				fclose(f);
			}
		}
	}
	uint32_t stride;
	const uint8_t *p;
	FILE *f;
	int y;

	LV_UNUSED(t);
	if (!shot_wanted)
		return;
	shot_wanted = 0;
	p = mbx_gpu_front(&stride);
	f = p ? fopen("/tmp/n72-shot.raw.tmp", "w") : NULL;
	if (!f)
		return;
	for (y = 0; y < H; y++)
		fwrite(p + y * stride, 4, W, f);
	fclose(f);
	rename("/tmp/n72-shot.raw.tmp", "/tmp/n72-shot.raw");
}

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
			const char *ev = strrchr(touch, '/');

			snprintf(touch_inhibit, sizeof(touch_inhibit),
				 "/sys/class/input/%s/device/inhibited", ev ? ev + 1 : touch);
			if (access(touch_inhibit, W_OK))
				touch_inhibit[0] = 0;
			touch_indev = indev;
			touch_wait_fd = open(touch, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (touch_wait_fd >= 0) {
				int clk = CLOCK_MONOTONIC;

				ioctl(touch_wait_fd, EVIOCSCLOCKID, &clk);
				if (mbx_gpu_on_shell_layer() &&
				    rt_init(touch_wait_fd, cal, swap)) {
					lv_indev_set_read_cb(indev, rt_read_cb);
					/* a page follows the finger after 6 px, not 10 */
					lv_indev_set_scroll_limit(indev, 6);
					lv_timer_set_cb(lv_indev_get_read_timer(indev), rt_timer_cb);
				}
			}
		}
		lat_on = getenv("N72_LATENCY") && *getenv("N72_LATENCY") == '1';
		fps_on = getenv("N72_FPS") && *getenv("N72_FPS") == '1';
	}

	if (!keys)
		keys = find_input("gpio-keys");
	if (keys)
		keys_fd = open(keys, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	lv_timer_create(keys_poll, 30, NULL);
	lv_timer_create(idle_check, 1000, NULL);
	lv_timer_create(playback_check, 2000, NULL);

	build_home();
	drags_init();
	sound_ui_init();
	crash_handlers();
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	lv_timer_create(shot_poll, 100, NULL);
	if (argc > 2 && !strcmp(argv[1], "--bench") && !strcmp(argv[2], "text"))
		return bench_text(argc > 3 ? atoi(argv[3]) : 30, false);
	if (argc > 2 && !strcmp(argv[1], "--bench") && !strcmp(argv[2], "label"))
		return bench_text(argc > 3 ? atoi(argv[3]) : 30, true);
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		return bench();

	/*
	 * Sleep until the next LVGL timer or until the touch screen or a button
	 * has something, and hand touch input to LVGL straight away instead of
	 * waiting for its read timer.
	 */
	notify_fd = mbx_gpu_notify_fd();
	refr_timer = lv_display_get_refr_timer(lv_display_get_default());
	for (;;) {
		struct pollfd pf[3] = {
			{ .fd = touch_wait_fd, .events = POLLIN },
			{ .fd = keys_fd, .events = POLLIN },
			{ .fd = notify_fd, .events = POLLIN },
		};
		uint64_t th0;
		uint32_t idle;

		/*
		 * Animation frames start when the display takes the last one,
		 * not on LVGL's 16 ms timer, which drifts against the 16.7 ms
		 * refresh: a frame drawn while one is still waiting replaces it
		 * (drawn for nothing), and the one shown then stays two
		 * refreshes. While a frame waits, the refresh timer rests;
		 * the hand-over (notify_fd) wakes it.
		 */
		if (refr_timer && notify_fd >= 0) {
			if (mbx_gpu_frame_waiting())
				lv_timer_pause(refr_timer);
			else
				lv_timer_resume(refr_timer);
		}
		th0 = mono_us();
		idle = lv_timer_handler();

		if (fps_on && mono_us() - th0 > 30000)
			fprintf(stderr, "timers: %.1f ms\n", (mono_us() - th0) / 1000.0);

		if (poll(pf, 3, LV_MIN(idle, LV_DEF_REFR_PERIOD)) > 0) {
			if ((pf[0].revents & POLLIN) && rt_uinput >= 0) {
				lat_read_us = mono_us();
				rt_feed(touch_wait_fd, touch_indev);	/* reads LVGL's side */
				if (mbx_gpu_frame_waiting())
					render_pending = true;
				else
					render_now();
			} else if (pf[0].revents & POLLIN) {
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
				/*
				 * Draw the result now, unless a finished frame is
				 * still waiting for the display: then drawing again
				 * would only be replaced. Draw when the flip thread
				 * hands that frame over (notify_fd), with the
				 * newest input by then.
				 */
				if (mbx_gpu_frame_waiting())
					render_pending = true;
				else
					render_now();
			}
			if (pf[2].revents & POLLIN) {
				mbx_gpu_notify_fd();	/* drain */
				if (render_pending) {
					render_pending = false;
					render_now();
				} else if (refr_timer && lv_anim_count_running()) {
					/* an animation: its next frame now */
					lv_timer_resume(refr_timer);
					lv_timer_ready(refr_timer);
				}
			}
			if (pf[1].revents & POLLIN)
				keys_poll(NULL);
		}
	}
	return 0;
}
