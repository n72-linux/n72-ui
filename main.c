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
#include "mbx_gpu.h"

#define W 320
#define H 480

static char *self_argv[4];
static lv_obj_t *home_scr;
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

static void fps_frame(void)
{
	uint64_t now = mono_us(), rend = now - fps_refr_us, iv = now - fps_last_us;

	if (fps_n && iv > 100000) {
		if (fps_n >= 4)
			fprintf(stderr, "burst: %u frames in %.0f ms (%.1f fps), interval max %.1f ms, "
				"%u over 20 ms; render avg %.1f max %.1f ms\n", fps_n,
				(fps_last_us - fps_first_us) / 1000.0,
				(fps_n - 1) * 1e6 / (double)(fps_last_us - fps_first_us + 1),
				fps_int_max / 1000.0, fps_slow, fps_rend_sum / 1000.0 / fps_n,
				fps_rend_max / 1000.0);
		if (fps_n >= 4) {
			struct mbx_gpu_stats *g = &mbx_gpu_stats;
			int i;

			fprintf(stderr, "  per frame: GPU fills %.1f (rounded %.1f) blends %.1f layers %.1f; "
				"CPU fills radius %.1f small %.1f; GPU ioctl ms: sync %.1f submit %.1f 3d %.1f, vblank wait %.1f; tasks",
				(g->fills - fps_g0.fills) / (double)fps_n,
				(g->fill_corners - fps_g0.fill_corners) / (double)fps_n,
				(g->blends - fps_g0.blends) / (double)fps_n,
				(g->layers - fps_g0.layers) / (double)fps_n,
				(g->fill_radius - fps_g0.fill_radius) / (double)fps_n,
				(g->fill_small - fps_g0.fill_small) / (double)fps_n,
				(g->sync_us - fps_g0.sync_us) / 1000 / fps_n,
				(g->submit_us - fps_g0.submit_us) / 1000 / fps_n,
				(g->blit3d_us - fps_g0.blit3d_us) / 1000 / fps_n,
				(g->flip_us - fps_g0.flip_us) / 1000 / fps_n);
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

#include "ui_m3.c"

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
	home_wake();
	lv_obj_invalidate(lv_screen_active());
	return ok;
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
				home_wake();
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
			if (!screen_off)
				home_wake();
			break;
		case KEY_VOLUMEUP:
			if (!logs_scroll(1))
				backlight_set(backlight_get() + 10);
			lv_slider_set_value(home_bright, backlight_get(), LV_ANIM_OFF);
			break;
		case KEY_VOLUMEDOWN:
			if (!logs_scroll(-1))
				backlight_set(backlight_get() - 10);
			lv_slider_set_value(home_bright, backlight_get(), LV_ANIM_OFF);
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

/* SIGUSR1: write what is on screen to /tmp/n72-shot.raw (XRGB8888, 320x480). */
static volatile sig_atomic_t shot_wanted;

static void on_usr1(int sig)
{
	LV_UNUSED(sig);
	shot_wanted = 1;
}

static void shot_poll(lv_timer_t *t)
{
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
			touch_indev = indev;
			touch_wait_fd = open(touch, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (touch_wait_fd >= 0) {
				int clk = CLOCK_MONOTONIC;

				ioctl(touch_wait_fd, EVIOCSCLOCKID, &clk);
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

	build_home();
	drags_init();
	signal(SIGUSR1, on_usr1);
	lv_timer_create(shot_poll, 100, NULL);
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
