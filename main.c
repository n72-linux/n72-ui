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
#include <linux/netlink.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/indev/lv_indev_private.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/misc/lv_timer_private.h"
#include "mbx_gpu.h"
#include "lvgl/src/misc/lv_anim_private.h"	/* (ios_motion.h reads act_time) */
#include "ios_motion.h"

/* the screen as the UI lays it out: 320 x 480, or 480 x 320 turned */
static int32_t ui_w = 320, ui_h = 480;
#define W ui_w
#define H ui_h

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
void n72_trace(const char *what, long v);

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
	n72_trace("touch-render", 0);
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

/*
 * N72_TRACE=1 (with N72_FPS=1): what the main loop did between two frames
 * that came more than 20 ms apart, as "+ms what(value)" marks counted from
 * the frame before, and the GPU's part of that time.
 */
bool n72_trace_on;
static struct { uint64_t t; const char *what; long v; } trc[160];
static unsigned int trc_n;
static struct mbx_gpu_stats trc_g0;

void n72_trace(const char *what, long v)
{
	if (!n72_trace_on || trc_n >= sizeof(trc) / sizeof(trc[0]))
		return;
	trc[trc_n].t = mono_us();
	trc[trc_n].what = what;
	trc[trc_n].v = v;
	trc_n++;
}

static void trace_frame(uint64_t now, uint64_t iv, uint64_t rend)
{
	struct mbx_gpu_stats *g = &mbx_gpu_stats;
	unsigned int i;

	if (iv > 20000 && iv < 1000000) {
		fprintf(stderr, "trace: frame after %.1f ms (render %.1f):", iv / 1000.0, rend / 1000.0);
		for (i = 0; i < trc_n; i++) {
			fprintf(stderr, " +%.1f %s", ((double)trc[i].t - (double)(now - iv)) / 1000.0, trc[i].what);
			if (trc[i].v)
				fprintf(stderr, "(%ld)", trc[i].v);
		}
		fprintf(stderr, "; GPU ms: sync %.1f submit %.1f 3d %.1f; tasks",
			(g->sync_us - trc_g0.sync_us) / 1000, (g->submit_us - trc_g0.submit_us) / 1000,
			(g->blit3d_us - trc_g0.blit3d_us) / 1000);
		for (i = 0; i < 16; i++)
			if (g->seen[i] != trc_g0.seen[i])
				fprintf(stderr, " %u:%lu", i, g->seen[i] - trc_g0.seen[i]);
		fprintf(stderr, "\n");
	}
	trc_n = 0;
	trc_g0 = *g;
}

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
	n72_trace("refr", 0);
	lv_anim_refr_now();
	n72_trace("anims", 0);
	if (n72_trace_on) {		/* (LVGL does this next: timed apart) */
		lv_obj_update_layout(lv_screen_active());
		n72_trace("layout", 0);
	}
}

uint64_t n72_mark_us;	/* (debug) a point to time long frames from, e.g. a slide start */

static void fps_frame(void)
{
	uint64_t now = mono_us(), rend = now - fps_refr_us, iv = now - fps_last_us;
	static int frames_trace = -1;

	if (n72_trace_on)
		trace_frame(now, fps_n ? iv : 0, rend);
	if (frames_trace < 0)
		frames_trace = getenv("N72_FRAMES") != NULL;
	if (frames_trace)	/* (debug) every frame: when, how long since the last, render */
		fprintf(stderr, "F %.1f iv %.1f r %.1f\n", now / 1000.0, iv / 1000.0, rend / 1000.0);
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

/*
 * Brightness in percent: the user's setting (bl_user), which the fades
 * below leave alone while they move the hardware's level.
 */
static int bl_user = -1;
static int bl_fade_v;		/* the fade-in's animated variable */

/* the user's level in the hardware's steps (rounded: percent -> raw -> percent is stable) */
static int bl_user_raw(void);

static void backlight_raw(int raw)
{
	char p[160];

	if (!bl_dir[0])
		return;
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	write_int(p, LV_CLAMP(1, raw, bl_max));
}

static int backlight_get(void)
{
	char p[160];

	if (!bl_dir[0])
		return 100;
	if (bl_user >= 0)
		return bl_user;
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	bl_user = LV_CLAMP(5, (read_int(p, bl_max) * 100 + bl_max / 2) / bl_max, 100);
	return bl_user;
}

static int bl_user_raw(void)
{
	return (backlight_get() * bl_max + 50) / 100;
}

static void backlight_set(int pct)
{
	if (!bl_dir[0])
		return;
	lv_anim_delete(&bl_fade_v, NULL);	/* the slider wins over a fade */
	bl_user = LV_CLAMP(5, pct, 100);
	backlight_raw(bl_user_raw());
}

/*
 * Screen off and on with the backlight faded, not switched: down over
 * ~160 ms before the panel goes off (the frame on it stays), up from
 * nearly dark over BL_FADE_IN_MS once it is back. A level written while
 * the backlight is off does not light it (the PMU driver reads 0 then), so
 * the user's level goes back right after the panel is off. N72_FADE=0: no
 * fades.
 */
#define BL_FADE_OUT_STEPS	8	/* 20 ms apart */
#define BL_FADE_IN_MS		280

static bool bl_fades(void)
{
	const char *e = getenv("N72_FADE");

	return bl_dir[0] && !(e && !strcmp(e, "0"));
}

static void backlight_fade_out(void)
{
	int from = bl_user_raw(), i;

	lv_anim_delete(&bl_fade_v, NULL);
	for (i = 1; i <= BL_FADE_OUT_STEPS; i++) {
		float r = 1.0f - (float)i / BL_FADE_OUT_STEPS;

		backlight_raw((int)(from * r * r));	/* (the eye: about square law) */
		usleep(20000);
	}
}

static void bl_fade_exec(void *var, int32_t v)
{
	LV_UNUSED(var);
	backlight_raw(v);
}

static void backlight_fade_in_start(void)
{
	lv_anim_t a;
	int to = bl_user_raw();

	lv_anim_delete(&bl_fade_v, NULL);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &bl_fade_v);
	lv_anim_set_exec_cb(&a, bl_fade_exec);
	lv_anim_set_values(&a, 1, to);
	lv_anim_set_duration(&a, BL_FADE_IN_MS);
	lv_anim_set_path_cb(&a, ios_path_ease_in);	/* square law, as above */
	lv_anim_start(&a);
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

/*
 * Timers that only matter while the screen is lit (the clock, the shade's
 * picture, the accelerometer, the idle timeout, ...): paused while it is
 * dark, so a dark screen (sound playing, or on USB power) wakes nothing.
 */
#define NAWAKE 12
static lv_timer_t *awake_t[NAWAKE];
static int nawake;

static lv_timer_t *awake_timer(lv_timer_cb_t cb, uint32_t ms, void *ud)
{
	lv_timer_t *t = lv_timer_create(cb, ms, ud);

	if (t && nawake < NAWAKE)
		awake_t[nawake++] = t;
	else if (t)
		fprintf(stderr, "n72-ui: more than %d awake timers: one runs while dark\n", NAWAKE);
	return t;
}

/*
 * Delete an awake timer, and forget it here: deleted with lv_timer_delete
 * it stayed listed, and the next screen change paused or resumed freed
 * memory (the home screen's timers, after every turn or theme change),
 * while the list filled up and later timers ran in the dark.
 */
static void awake_timer_delete(lv_timer_t *t)
{
	for (int i = 0; i < nawake; i++)
		if (awake_t[i] == t) {
			awake_t[i] = awake_t[--nawake];
			break;
		}
	lv_timer_delete(t);
}

static void awake_set(bool on)
{
	for (int i = 0; i < nawake; i++) {
		if (on) {
			lv_timer_resume(awake_t[i]);
			lv_timer_ready(awake_t[i]);
		} else {
			lv_timer_pause(awake_t[i]);
		}
	}
}

static void rot_motion_off(void);
static void wp_apply_kick(void);

static void screen_set(bool on)
{
	bool fade;

	if (!on && screen_off)
		return;
	fade = bl_fades() && screen_off == on;	/* only on a change (on again: no fade) */
	if (screen_off == on)
		awake_set(on);		/* (on a change only) */
	screen_off = !on;
	if (!on)
		rot_motion_off();	/* the accelerometer may power down */
	if (fade && !on)
		backlight_fade_out();
	else if (fade)
		backlight_raw(1);	/* lit nearly dark, then faded up */
	if (access(PANEL_POWER, W_OK) == 0)
		write_int(PANEL_POWER, on);
	else
		backlight_power(on);
	if (fade && !on)
		backlight_raw(bl_user_raw());	/* (stays dark) */
	else if (fade)
		backlight_fade_in_start();
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
		wp_apply_kick();	/* (new wallpaper frames that waited for this) */
	}
}

#include "audio.c"
/*
 * More file descriptors for the main loop's poll(): events instead of
 * timers that look (the headphone jack, a file in /run changing, BlueZ's
 * D-Bus signals). Each has a callback, run when it is readable.
 */
#define NWATCH 32	/* (background commands take two each: ui_m3.c job_watch) */
static struct { int fd; short events; void (*cb)(int fd); } watch[NWATCH];
static int nwatch;

/* (events: POLLIN, or POLLPRI for a sysfs attribute the kernel notifies) */
static void ui_watch_ev(int fd, void (*cb)(int fd), short events)
{
	int i;

	for (i = 0; i < nwatch; i++)
		if (watch[i].fd == fd) {
			watch[i].cb = cb;
			watch[i].events = events;
			return;
		}
	if (fd >= 0 && nwatch < NWATCH) {
		watch[nwatch].fd = fd;
		watch[nwatch].events = events;
		watch[nwatch].cb = cb;
		nwatch++;
	}
}

static void ui_watch(int fd, void (*cb)(int fd))
{
	ui_watch_ev(fd, cb, POLLIN);
}

static lv_timer_t *idle_timer;
static void idle_kick(void);
static void rot_kick(void);

static void ui_unwatch(int fd)
{
	int i;

	for (i = 0; i < nwatch; i++)
		if (watch[i].fd == fd) {
			watch[i] = watch[--nwatch];
			return;
		}
}

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
/*
 * Multi-touch protocol B (the device has ABS_MT_SLOT): its state, as the
 * frames so far left it. Protocol B sends only what changed, so a touch
 * becoming the application's starts with the whole state of every contact
 * (rt_app_begin), and the application's touch ends with every slot lifted.
 */
#define RT_SLOTS	16
static bool rt_mt_b;
static int rt_nslots, rt_cur;
static int rt_frame_slot0;		/* the slot the frame being read began in */
/*
 * (lift-off hold, rt_lift: major = the contact's long axis as last
 * reported, was = the report before; hist = its last positions that were
 * passed on, newest last)
 */
static struct {
	int id, x, y;
	int major, was;
	bool major_new, hold, resync;
	int hist[3][2], nh;
} rt_sl[RT_SLOTS];
static int rt_rest_x, rt_rest_y;	/* "resting": no further than this in three frames */
static bool rt_lift_hold = true;	/* (N72_TOUCH_RAW=1: every frame as the panel gives it) */

static void rt_put(struct input_event *v, int *n, int type, int code, int value)
{
	v[*n].type = type;
	v[*n].code = code;
	v[*n].value = value;
	(*n)++;
}

/*
 * A finger that lifts takes its contact off the panel over a few frames,
 * and the panel's position for it, the middle of what still touches,
 * wanders as it goes: 2 to 4.5 pixels in the last one or two frames of a
 * tap (recorded on the device, 2026-11-18: eleven taps, the long axis
 * falling from ~125 to ~30 in two frames). An application that scrolls at
 * the first movement takes that for a drag, and the tap is lost (Spore's
 * buttons). So the application is not told where a lifting finger wanders
 * to: from a frame in which the contact's long axis has fallen to under
 * 85 % of the frame before, and for as long as it goes on falling, the
 * position stays where it was, if the finger was resting (within two
 * pixels over its last three frames). A finger that moves as it lifts (the
 * end of a drag, a flick: its last movements are its speed) is passed on
 * as it is; so is one whose contact shrinks slowly, or shrinks and grows
 * again (rolled, or half off the panel's edge), with its position sent
 * afresh. On the recording the taps' movement falls to under 2 pixels and
 * both drags are unchanged.
 *
 * Called at the end of every frame, before it is routed.
 */
static void rt_lift(void)
{
	int i;

	for (i = 0; i < rt_nslots; i++) {
		typeof(rt_sl[0]) *s = &rt_sl[i];

		if (s->id < 0) {
			s->hold = s->major_new = false;
			continue;
		}
		if (s->major_new && s->was >= 0) {
			if (s->hold) {
				if (s->major > s->was) {	/* not a lift after all */
					s->hold = false;
					s->resync = true;
				}
			} else if (rt_lift_hold && s->nh && s->major * 100 < s->was * 85) {
				int *ref = s->hist[0], *last = s->hist[s->nh - 1];

				if (abs(last[0] - ref[0]) <= rt_rest_x && abs(last[1] - ref[1]) <= rt_rest_y)
					s->hold = true;
			}
		}
		if (s->major_new)
			s->was = s->major;
		s->major_new = false;
		if (!s->hold) {
			if (s->nh == 3) {
				s->hist[0][0] = s->hist[1][0];
				s->hist[0][1] = s->hist[1][1];
				s->hist[1][0] = s->hist[2][0];
				s->hist[1][1] = s->hist[2][1];
				s->nh = 2;
			}
			s->hist[s->nh][0] = s->x;
			s->hist[s->nh][1] = s->y;
			s->nh++;
		}
	}
}

/* The contact the single-touch axes follow (the kernel's: the oldest). */
static int rt_first_slot(void)
{
	int i, first = -1;

	for (i = 0; i < rt_nslots; i++)
		if (rt_sl[i].id >= 0 && (first < 0 || rt_sl[i].id < rt_sl[first].id))
			first = i;
	return first;
}

/*
 * The frame for the application: as it came, without the positions of
 * contacts that are held (rt_lift), and with the position of one whose
 * hold has ended.
 */
static void rt_app_frame(void)
{
	struct input_event v[sizeof(rt_frame) / sizeof(rt_frame[0]) + RT_SLOTS * 3 + 4] = { 0 };
	int n = 0, i, slot = rt_frame_slot0, first = rt_first_slot();
	bool any = false;

	for (i = 0; i < rt_nslots; i++)
		any |= rt_sl[i].id >= 0 && (rt_sl[i].hold || rt_sl[i].resync);
	if (!any) {
		if (write(rt_uinput, rt_frame, rt_n * sizeof(rt_frame[0])) < 0)
			;
		return;
	}
	for (i = 0; i < rt_n; i++) {
		const struct input_event *e = &rt_frame[i];

		if (e->type == EV_ABS && e->code == ABS_MT_SLOT)
			slot = LV_CLAMP(0, e->value, RT_SLOTS - 1);
		else if (e->type == EV_ABS && rt_sl[slot].hold &&
			 (e->code == ABS_MT_POSITION_X || e->code == ABS_MT_POSITION_Y))
			continue;
		else if (e->type == EV_ABS && first >= 0 && rt_sl[first].hold &&
			 (e->code == ABS_X || e->code == ABS_Y))
			continue;
		if (e->type == EV_SYN && e->code == SYN_REPORT) {
			int k, back = slot;

			for (k = 0; k < rt_nslots; k++) {
				if (rt_sl[k].id < 0 || !rt_sl[k].resync)
					continue;
				rt_put(v, &n, EV_ABS, ABS_MT_SLOT, k);
				rt_put(v, &n, EV_ABS, ABS_MT_POSITION_X, rt_sl[k].x);
				rt_put(v, &n, EV_ABS, ABS_MT_POSITION_Y, rt_sl[k].y);
				slot = k;
				if (k == first) {
					rt_put(v, &n, EV_ABS, ABS_X, rt_raw_x);
					rt_put(v, &n, EV_ABS, ABS_Y, rt_raw_y);
				}
			}
			if (slot != back)
				rt_put(v, &n, EV_ABS, ABS_MT_SLOT, back);
		}
		v[n++] = *e;
	}
	for (i = 0; i < rt_nslots; i++)
		rt_sl[i].resync = false;
	if (write(rt_uinput, v, n * sizeof(v[0])) < 0)
		;
}

/* A touch becomes the application's: one frame with everything down now. */
static void rt_app_begin(void)
{
	struct input_event v[RT_SLOTS * 4 + 8] = { 0 };
	int n = 0, i;

	for (i = 0; i < rt_nslots; i++) {
		if (rt_sl[i].id < 0)
			continue;
		rt_put(v, &n, EV_ABS, ABS_MT_SLOT, i);
		rt_put(v, &n, EV_ABS, ABS_MT_TRACKING_ID, rt_sl[i].id);
		rt_put(v, &n, EV_ABS, ABS_MT_POSITION_X, rt_sl[i].x);
		rt_put(v, &n, EV_ABS, ABS_MT_POSITION_Y, rt_sl[i].y);
	}
	rt_put(v, &n, EV_ABS, ABS_MT_SLOT, rt_cur);	/* (where the next frames continue) */
	rt_put(v, &n, EV_ABS, ABS_X, rt_raw_x);
	rt_put(v, &n, EV_ABS, ABS_Y, rt_raw_y);
	rt_put(v, &n, EV_KEY, BTN_TOUCH, 1);
	rt_put(v, &n, EV_SYN, SYN_REPORT, 0);
	if (write(rt_uinput, v, n * sizeof(v[0])) < 0)
		;
}

static void rt_reset(void)
{
	if (rt_owner == RT_APP && rt_uinput >= 0) {
		/* the application's touch ends here */
		struct input_event up[RT_SLOTS * 2 + 4] = { 0 };
		int n = 0, i;

		if (rt_mt_b) {
			for (i = 0; i < rt_nslots; i++) {
				rt_put(up, &n, EV_ABS, ABS_MT_SLOT, i);
				rt_put(up, &n, EV_ABS, ABS_MT_TRACKING_ID, -1);
			}
		}
		rt_put(up, &n, EV_KEY, BTN_TOUCH, 0);
		if (!rt_mt_b)
			rt_put(up, &n, EV_SYN, SYN_MT_REPORT, 0);
		rt_put(up, &n, EV_SYN, SYN_REPORT, 0);
		if (write(rt_uinput, up, n * sizeof(up[0])) < 0)
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
	/* the panel's pixels: LVGL turns them itself for a turned screen */
	p.x = rt_scale(x, rt_cal.min_x, rt_cal.max_x, d->hor_res - 1);
	p.y = rt_scale(y, rt_cal.min_y, rt_cal.max_y, d->ver_res - 1);
	return p;
}

/* A panel point where LVGL sees it (its rotation, as lv_indev turns it). */
static lv_point_t rt_logical(lv_point_t p)
{
	lv_display_t *d = lv_display_get_default();
	lv_point_t l = p;

	if (d->rotation == LV_DISPLAY_ROTATION_90) {
		l.x = d->ver_res - 1 - p.y;
		l.y = p.x;
	} else if (d->rotation == LV_DISPLAY_ROTATION_270) {
		l.x = p.y;
		l.y = d->hor_res - 1 - p.x;
	}
	return l;
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
	lv_point_t p = rt_map(rt_raw_x, rt_raw_y), l = rt_logical(p);
	bool news = false;

	if (rt_mt_b)
		rt_lift();
	if (rt_owner == RT_NONE && rt_down) {
		rt_owner = app_pid > 0 && !ui_wants_touch(l.x, l.y) ? RT_APP : RT_UI;
		if (rt_owner == RT_APP && rt_mt_b) {
			rt_app_begin();		/* (this frame's news included) */
			rt_n = 0;
			return false;
		}
	}
	if (rt_owner == RT_APP) {
		if (rt_mt_b)
			rt_app_frame();
		else if (write(rt_uinput, rt_frame, rt_n * sizeof(rt_frame[0])) < 0)
			;
	} else {
		if (rt_down || rt_ui_touch) {
			int ph = !rt_down ? 2 : rt_ui_touch ? 1 : 0;

			if (ui_drag(ph, l.x, l.y) && !rt_claim) {
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
			/*
			 * The launcher follows the single-touch axes (the first
			 * contact; with slots, another finger's MT position would
			 * move it); the slots are kept for the application.
			 */
			if (rt_n == 0)
				rt_frame_slot0 = rt_cur;
			if (e->type == EV_ABS && e->code == ABS_X)
				rt_raw_x = e->value;
			else if (e->type == EV_ABS && e->code == ABS_Y)
				rt_raw_y = e->value;
			else if (e->type == EV_KEY && e->code == BTN_TOUCH)
				rt_down = e->value != 0;
			else if (e->type == EV_ABS && e->code == ABS_MT_SLOT)
				rt_cur = LV_CLAMP(0, e->value, RT_SLOTS - 1);
			else if (e->type == EV_ABS && e->code == ABS_MT_TRACKING_ID) {
				if (e->value >= 0 && rt_sl[rt_cur].id < 0) {
					/* a new contact: nothing known of it yet */
					rt_sl[rt_cur].was = -1;
					rt_sl[rt_cur].nh = 0;
					rt_sl[rt_cur].hold = rt_sl[rt_cur].resync = false;
					rt_sl[rt_cur].major_new = false;
				}
				rt_sl[rt_cur].id = e->value;
			} else if (e->type == EV_ABS && e->code == ABS_MT_TOUCH_MAJOR) {
				rt_sl[rt_cur].major = e->value;
				rt_sl[rt_cur].major_new = true;
			}
			else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_X)
				rt_sl[rt_cur].x = e->value;
			else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_Y)
				rt_sl[rt_cur].y = e->value;
			if (rt_n < (int)(sizeof(rt_frame) / sizeof(rt_frame[0])))
				rt_frame[rt_n++] = *e;
			if (e->type == EV_SYN && e->code == SYN_REPORT) {
				/* every frame: a move and the lift in one read
				 * would lose the move (and the gesture) */
				if (rt_frame_done()) {
					rt_frame_us = mono_us();
					lv_indev_read(indev);
				}
				if (rt_down)	/* (long presses need reads between frames) */
					lv_timer_resume(lv_indev_get_read_timer(indev));
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
	/* no finger: no reads until one comes (rt_feed resumes it) */
	if (!rt_ui_down && !rt_down)
		lv_timer_pause(t);
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
	rt_mt_b = absbits[ABS_MT_SLOT / (8 * sizeof(long))] & (1UL << (ABS_MT_SLOT % (8 * sizeof(long))));
	if (rt_mt_b && ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0)
		rt_nslots = LV_MIN(ai.maximum + 1, RT_SLOTS);
	/* resting: within two of the panel's 320 x 480 pixels (rt_lift) */
	if (rt_mt_b && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0)
		rt_rest_x = (ai.maximum - ai.minimum) / 160;
	if (rt_mt_b && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0)
		rt_rest_y = (ai.maximum - ai.minimum) / 240;
	rt_lift_hold = !getenv("N72_TOUCH_RAW") && rt_rest_x > 0 && rt_rest_y > 0;
	for (c = 0; c < RT_SLOTS; c++)
		rt_sl[c].id = -1;
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
	char buf[1024];
	int n, i, fd;

	n = snprintf(buf, sizeof(buf), "n72-ui: signal %d at %p: pc %08lx lr %08lx sp %08lx r0 %08lx\n  stack:",
		     sig, si->si_addr, mc->arm_pc, mc->arm_lr, mc->arm_sp, mc->arm_r0);
	for (i = 0; i < 80 && n < (int)sizeof(buf) - 12; i++)
		n += snprintf(buf + n, sizeof(buf) - n, "%s%08lx", i % 8 ? " " : "\n   ", sp[i]);
	if (n < (int)sizeof(buf) - 1)
		buf[n++] = '\n';
	write(2, buf, n);
	/* kept on the flash too: /tmp (the log) is gone after a reboot */
	fd = open("/var/log/n72-ui-crash.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd >= 0) {
		write(fd, buf, n);
		fsync(fd);
		close(fd);
	}
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

/*
 * Suspend to RAM ("deep" in /sys/power/mem_sleep). Which state "mem" means
 * is chosen there: n72-base leaves it at s2idle, and n72-ui picks deep only
 * for its own deep sleeps (deep_sleep).
 */
#define MEM_SLEEP	"/sys/power/mem_sleep"

static bool deep_offered(void)
{
	char buf[64] = "";
	FILE *f = fopen(MEM_SLEEP, "r");

	if (!f)
		return false;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return strstr(buf, "deep") != NULL;
}

static bool mem_sleep_set(const char *mode)
{
	int fd = open(MEM_SLEEP, O_WRONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = write(fd, mode, strlen(mode)) == (ssize_t)strlen(mode);
	if (!ok)
		fprintf(stderr, "n72-ui: mem_sleep %s: %s\n", mode, strerror(errno));
	close(fd);
	return ok;
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
 * What tells a wake that wants the screen from any other: how often the
 * two buttons have interrupted, the cable, and the alarm that was set.
 */
struct wake_marks {
	long wlan, home, power;
	int wlan_irq, home_irq, power_irq;
	bool usb;		/* the cable is in */
	long alarm;		/* the RTC's wake alarm, seconds since 1970, or 0 */
	bool jack;		/* earphones in the jack */
	unsigned pmu_int[3];	/* the PMU's last interrupt (INT1..INT3) ... */
	unsigned long pmu_n;	/* ... and how many it has had (apple-n72-pmu last_irq) */
};

/*
 * (the PMU's last interrupt, logged at each deep wake: N72_WAKE_LOG=1)
 */
#define PMU_LAST_IRQ	"/sys/bus/i2c/devices/0-0073/last_irq"

static void wake_marks(struct wake_marks *m)
{
	char line[256];
	FILE *f;

	memset(m, 0, sizeof(*m));
	m->wlan_irq = m->home_irq = m->power_irq = -1;
	m->usb = usb_powered();
	{	/* (read without changing jack_in: jack_poll notices the change itself) */
		unsigned long sw[1] = { 0 };

		m->jack = jack_fd >= 0 && ioctl(jack_fd, EVIOCGSW(sizeof(sw)), sw) >= 0 &&
			  (sw[0] & (1UL << SW_HEADPHONE_INSERT));
	}
	f = fopen(PMU_LAST_IRQ, "r");
	if (f) {
		if (fscanf(f, "%x %x %x %lu", &m->pmu_int[0], &m->pmu_int[1], &m->pmu_int[2],
			   &m->pmu_n) != 4)
			m->pmu_n = 0;
		fclose(f);
	}
	f = fopen("/sys/class/rtc/rtc0/wakealarm", "r");
	if (f) {
		if (fscanf(f, "%ld", &m->alarm) != 1)
			m->alarm = 0;
		fclose(f);
	}
	f = fopen("/proc/interrupts", "r");
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
 * Is the screen wanted after this wake? For Power and Home, for the cable
 * going in or out, and for an alarm somebody set (the RTC's wakealarm).
 * Anything else that wakes the system (a frame for this station over
 * Wi-Fi, which stays associated in suspend to idle so that an SSH login
 * gets in; a firmware event) is served in the dark, and the system sleeps
 * on. The signs of a button: the kernel names its interrupt as the one
 * that woke it (suspend to idle), or its interrupt has counted up. A press
 * seen a moment later lights the screen as well (dark_linger): after the
 * deep sleep, where the wake comes through the boot loader, that is the
 * only sign of a button there is.
 */
static bool woke_for_screen(const struct wake_marks *before)
{
	struct wake_marks now;
	int irq = wake_irq();

	wake_marks(&now);
	if (now.home != before->home || now.power != before->power)
		return true;
	if (irq >= 0 && (irq == now.home_irq || irq == now.power_irq))
		return true;
	if (now.usb != before->usb && conf.wake_plug)	/* Settings > Display */
		return true;
	return before->alarm && time(NULL) >= before->alarm;
}

/*
 * After a suspend to RAM the SoC was off: Power or Home wakes the PMU in
 * hardware, the boot loader brings Linux back, and the SoC's own button
 * interrupts never see that press (2026-11-20: the screen stayed dark until
 * a second press, inside the dark linger). The PMU leaves no event for it
 * either. What else can end a deep sleep is known: the PMU's unmasked
 * events (the cable in or out, the headphone jack; read on the device:
 * masks f7 bf bb), and an alarm (ours is cleared before a deep sleep). So
 * a deep wake with the cable and the jack as they were and no alarm due
 * was a button. (Wi-Fi is off in deep sleep; its host wake is masked.)
 */
static bool deep_woke_by_button(const struct wake_marks *before)
{
	struct wake_marks now;

	wake_marks(&now);
	if (now.usb != before->usb)
		return false;		/* (woke_for_screen has decided that one) */
	if (before->alarm && time(NULL) >= before->alarm)
		return false;
	if (now.jack != before->jack)
		return false;		/* the headphone jack: the screen stays dark */
	return true;
}

/*
 * Is somebody logged in over SSH, running something? dropbear forks one
 * process per connection; a connection counts while that process has a
 * child (the shell or the command). One left behind with nothing under it
 * does not keep the system awake.
 */
static bool ssh_sessions(void)
{
	struct { int pid, ppid; bool dropbear; } pr[256];
	int n = 0;
	glob_t g;
	bool found = false;

	if (glob("/proc/[0-9]*/stat", 0, NULL, &g))
		return false;
	for (size_t i = 0; i < g.gl_pathc && n < 256; i++) {
		char buf[256] = "", *comm, *end;
		FILE *f = fopen(g.gl_pathv[i], "r");

		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && (comm = strchr(buf, '(')) &&
		    (end = strrchr(buf, ')'))) {
			pr[n].pid = atoi(buf);
			pr[n].dropbear = end - comm == 9 && !strncmp(comm + 1, "dropbear", 8);
			pr[n].ppid = 0;
			sscanf(end + 1, " %*c %d", &pr[n].ppid);
			n++;
		}
		fclose(f);
	}
	globfree(&g);
	for (int i = 0; i < n && !found; i++) {
		bool session = false;

		if (!pr[i].dropbear)
			continue;
		for (int k = 0; k < n; k++)	/* its parent is the listener */
			if (pr[k].pid == pr[i].ppid && pr[k].dropbear)
				session = true;
		for (int k = 0; k < n && session && !found; k++)
			found = pr[k].ppid == pr[i].pid;
	}
	return found;
}

/*
 * Awake but not for a person, screen dark: stay up for a few seconds, and
 * for as long as an SSH session lasts, then sleep on. True: Power or Home
 * was pressed (or is reported now, as the press that woke us): the screen
 * is wanted.
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

/*
 * Before a sleep: tell the kernel the wakeup events seen so far
 * (/sys/power/wakeup_count). False when one came in meanwhile (an alarm
 * that rang, a key): the sleep would miss it, so do not sleep now. Without
 * the file, sleep anyway.
 */
static bool wakeup_count_arm(void)
{
	char buf[32];
	ssize_t n;
	int fd = open("/sys/power/wakeup_count", O_RDWR | O_CLOEXEC);
	bool ok = true;

	if (fd < 0)
		return true;
	n = read(fd, buf, sizeof(buf) - 1);	/* (waits while an event is being handled) */
	if (n > 0) {
		buf[n] = 0;
		ok = lseek(fd, 0, SEEK_SET) == 0 && write(fd, buf, strlen(buf)) > 0;
	}
	close(fd);
	return ok;
}

/* the RTC's wake alarm: "+N" seconds from now, or 0 to clear it */
static void wake_alarm(const char *v)
{
	int fd = open("/sys/class/rtc/rtc0/wakealarm", O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, "0", 1) < 0 || (strcmp(v, "0") && write(fd, v, strlen(v)) < 0))
		fprintf(stderr, "n72-ui: wakealarm %s: %s\n", v, strerror(errno));
	close(fd);
}

/* seconds since boot, counting the time asleep */
static time_t boot_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec;
}

static bool audio_playing(void);

/*
 * Suspend to RAM (the SoC powers off) once the system has slept this long
 * in suspend to idle, which wakes quickly and keeps Wi-Fi associated.
 */
#define DEEP_AFTER_S	180
/* (deep_sleep: a refused suspend to RAM backs off) */
#define DEEP_RETRY_MIN_S	180
#define DEEP_RETRY_MAX_S	1800
static time_t deep_retry_at, deep_retry_gap = DEEP_RETRY_MIN_S;

/*
 * May the sleep go deep now? Not on USB power, where the charger's events
 * wake the PMU straight out of hibernate; not while sound plays (that keeps
 * the system up anyway) or an application is open; not with N72_SLEEP=freeze;
 * and only where the kernel offers it.
 */
static bool deep_allowed(void)
{
	const char *e = getenv("N72_SLEEP");

	if ((e && !strcmp(e, "freeze")) || usb_powered() || audio_playing() || app_pid > 0)
		return false;
	if (deep_retry_at && boot_s() < deep_retry_at)
		return false;		/* refused lately: deep_sleep */
	return deep_offered();
}

/*
 * A suspend to RAM the kernel refused (a driver's suspend failed: the
 * attempt costs seconds of the CPU and the radio each time) is not tried
 * again at once: the next one waits 3 min, then twice as long after each
 * further refusal, up to 30 min, meanwhile suspend to idle. A deep sleep
 * that works starts over. (Before, a refusing kernel was asked again
 * every ~9 s all night: #57 on 2026-11-16, hci's resume timing out.)
 */

/* One suspend to RAM: "mem" as deep for this sleep, s2idle again after. */
static bool deep_sleep(void)
{
	bool ok;

	if (!mem_sleep_set("deep"))
		return false;
	ok = write_sleep_state("mem");
	mem_sleep_set("s2idle");
	if (ok) {
		deep_retry_gap = DEEP_RETRY_MIN_S;
	} else {
		deep_retry_at = boot_s() + deep_retry_gap;
		fprintf(stderr, "n72-ui: suspend to RAM refused: suspend to idle, again in %ld s\n",
			(long)deep_retry_gap);
		deep_retry_gap = deep_retry_gap * 2 > DEEP_RETRY_MAX_S ? DEEP_RETRY_MAX_S :
					 deep_retry_gap * 2;
	}
	return ok;
}

/*
 * Sleep until Power or Home is pressed, the cable goes in or out, or a set
 * alarm rings: suspend to idle first, and after DEEP_AFTER_S suspend to RAM
 * when deep_allowed (an RTC alarm of our own wakes the system for the
 * change, and again every DEEP_AFTER_S while deep sleep is not allowed;
 * it is left alone when somebody else has set one). Whatever else wakes
 * the system is served with the screen dark, and the sleep goes on
 * (woke_for_screen, dark_linger). N72_SLEEP=0 turns sleep off,
 * N72_SLEEP=freeze always uses suspend to idle. Returns false when sleep
 * is unavailable or refused, so the caller can fall back to blanking the
 * screen.
 */
static bool system_sleep(void)
{
	const char *e = getenv("N72_SLEEP");
	time_t start = boot_s();
	bool ok, own = false;	/* the alarm set is ours */

	if (e && !strcmp(e, "0"))
		return false;
	for (;;) {
		struct wake_marks before;
		bool deep = boot_s() - start >= DEEP_AFTER_S && deep_allowed();

		if (deep && own) {	/* nothing to wake for in deep sleep */
			wake_alarm("0");
			own = false;
		}
		wake_marks(&before);
		if (!deep && !before.alarm) {
			char at[16];
			time_t in = DEEP_AFTER_S - (boot_s() - start);

			if (in <= 0 && deep_retry_at > boot_s())
				in = deep_retry_at - boot_s();	/* (deep held back: wake for the retry) */
			snprintf(at, sizeof(at), "+%ld", (long)(in > 0 ? in : DEEP_AFTER_S));
			wake_alarm(at);
			own = true;
		}
		if (own)
			before.alarm = 0;	/* ours: not a wake for the screen */
		/* (the caller has switched the panel off: screen_set(false)) */
		if (!wakeup_count_arm()) {
			ok = true;	/* an event came first: look at it, as after a wake */
		} else {
			ok = deep && deep_sleep();
			if (!ok)	/* not deep, or the PMU refused: suspend to idle */
				ok = write_sleep_state("freeze");
		}
		woke_us = mono_us();
		if (deep && ok && getenv("N72_WAKE_LOG")) {
			struct wake_marks w;

			wake_marks(&w);
			fprintf(stderr, "n72-ui: deep wake: PMU %02x %02x %02x (%lu, was %lu), Home %ld->%ld, Power %ld->%ld, cable %d->%d\n",
				w.pmu_int[0], w.pmu_int[1], w.pmu_int[2], w.pmu_n, before.pmu_n,
				before.home, w.home, before.power, w.power, before.usb, w.usb);
		}
		if (!ok || woke_for_screen(&before) || (deep && deep_woke_by_button(&before)))
			break;
		if (own) {
			struct wake_marks now;

			wake_marks(&now);
			if (!now.alarm) {	/* ours rang (the RTC disarms it): sleep on */
				own = false;
				continue;
			}
		}
		if (dark_linger())
			break;
	}
	if (own)
		wake_alarm("0");

	/* the wake press and anything touched meanwhile are not input */
	drain(keys_fd);
	screen_set(true);
	/* the idle and lock timeouts count from the wake, not from the press
	 * that put it to sleep (which a long sleep has long outlasted) */
	lv_display_trigger_activity(NULL);
	return ok;
}

/*
 * Is the Bluetooth output playing? bluealsa has no kernel sound card, so
 * the cards' status says nothing about it: while n72-audio-route names a
 * Bluetooth output (/run/n72-audio: pcm=), bluealsa is asked whether that
 * PCM is running (bluealsa-cli info: "Running: true"). Without this a
 * screen turned off during music into earbuds on battery put the system
 * to sleep, and the kernel's Bluetooth suspend dropped the earbuds' link
 * (2026-11-20, found by the Hardware gaps session from the logs). Only
 * asked while such a route exists: a few times at a screen-off and every
 * 2 s while the screen is off for sound (playback_check).
 */
static bool bt_audio_playing(void)
{
	char line[192], pcm[160] = "", cmd[224];
	bool on = false;
	FILE *f = fopen("/run/n72-audio", "r");

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "pcm=", 4))
			snprintf(pcm, sizeof(pcm), "%s", line + 4);
	fclose(f);
	pcm[strcspn(pcm, "\n")] = 0;
	if (!pcm[0] || strchr(pcm, '\'') || !have_tool("bluealsa-cli"))
		return false;
	snprintf(cmd, sizeof(cmd), "bluealsa-cli info '%s' 2>/dev/null", pcm);
	f = popen(cmd, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "Running: true", 13))
			on = true;
	pclose(f);
	return on;
}

/* Is sound playing? (any ALSA playback stream running, or Bluetooth's) */
static bool audio_playing(void)
{
	glob_t g;
	bool on = false;

	if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g))
		return bt_audio_playing();
	for (size_t i = 0; i < g.gl_pathc && !on; i++) {
		char buf[64] = "";
		FILE *f = fopen(g.gl_pathv[i], "r");

		if (!f)
			continue;
		on = fgets(buf, sizeof(buf), f) && strstr(buf, "RUNNING");
		fclose(f);
	}
	globfree(&g);
	return on || bt_audio_playing();
}

/*
 * Power, the idle timeout and Settings > Power > Sleep: sleep, or with
 * sound playing only turn the screen off, and sleep once it stops
 * (playback_check). Screen off with the cable out lets the kernel gate
 * the display and USB and idle at the system clock's slowest divider.
 */
static bool screen_off_for_sound;
static lv_timer_t *playback_timer;	/* runs only while screen_off_for_sound */

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
		if (playback_timer)
			lv_timer_resume(playback_timer);
		return;
	}
	if (!system_sleep())
		screen_set(false);
}

static void playback_check(lv_timer_t *t)
{
	static int quiet;

	if (!screen_off || !screen_off_for_sound) {
		quiet = 0;
		lv_timer_pause(t);
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
/*
 * The screen goes dark (locked): on USB power only the screen, on battery
 * the system sleeps as well.
 */
static void screen_dark(void)
{
	if (usb_powered())
		screen_set(false);
	else
		screen_off_or_sleep();
	lv_display_trigger_activity(NULL);
}

#define LOCK_IDLE_MS	8000	/* the lock screen, not unlocked: dark again */

/*
 * USB power in or out. The PMU interrupts on both and the kernel sends a
 * power_supply uevent ("usb"), read here from a netlink socket in the main
 * loop's poll(); only without one does idle_check look every second.
 */
static int uevent_fd = -1;

static void plug_changed(void)
{
	static int last_plug = -1;
	int plug = usb_powered();

	if (last_plug < 0 || plug == last_plug) {
		last_plug = plug;
		return;
	}
	last_plug = plug;
	if (screen_off && conf.wake_plug) {	/* Settings > Display */
		screen_off_for_sound = false;
		screen_set(true);
		lv_display_trigger_activity(NULL);
		return;
	}
	if (screen_off && !plug && !screen_off_for_sound) {
		screen_off_or_sleep();	/* dark on USB, now on battery: sleep */
		return;
	}
	lv_display_trigger_activity(NULL);
}

static void uevent_ev(int fd)
{
	char buf[2048];
	bool usb = false, psy = false;
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (!strcmp(p, "POWER_SUPPLY_NAME=usb"))
				usb = true;
			if (!strcmp(p, "SUBSYSTEM=power_supply"))
				psy = true;
		}
	}
	if (usb)
		plug_changed();
	if (psy)
		status_kick(true, false);	/* the battery text: bolt, level */
}

static void uevent_init(void)
{
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			NETLINK_KOBJECT_UEVENT);

	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		fd = -1;
	}
	uevent_fd = fd;
	if (fd >= 0)
		ui_watch(fd, uevent_ev);
	plug_changed();		/* (the state now) */
}

/*
 * The idle timeout: the timer runs when the next deadline is due (the
 * lock screen's or Settings > Display's), not every second; input in the
 * meantime only moves the deadline later, which the run then finds.
 * Paused while the screen is dark (an awake timer).
 */
static void idle_check(lv_timer_t *t)
{
	long idle_ms = conf.idle_s * 1000L;	/* Settings > Display */
	uint32_t inactive = lv_display_get_inactive_time(NULL), next = 60000;

	if (uevent_fd < 0) {
		plug_changed();
		next = 1000;
	}
	if (screen_off)
		return;
	if (video_playing() || game_playing()) {	/* a video watched, a game played */
		lv_display_trigger_activity(NULL);
		lv_timer_set_period(t, LV_MIN(next, 5000));
		return;
	}
	if (locked) {
		if (inactive >= LOCK_IDLE_MS) {
			screen_dark();
			return;
		}
		next = LV_MIN(next, LOCK_IDLE_MS - inactive);
	}
	if (idle_ms) {
		if (inactive >= (uint32_t)idle_ms) {
			screen_dark();
			return;
		}
		next = LV_MIN(next, (uint32_t)idle_ms - inactive);
	}
	lv_timer_set_period(t, LV_MAX(next + 20, 250));
}

/* Settings changed the timeout, or the lock screen went up: look now. */
static void idle_kick(void)
{
	if (idle_timer && !screen_off)
		lv_timer_ready(idle_timer);
}

/* -------------------------------------------------------------- buttons */

/*
 * The screen as the launcher believes it, against the panel and the
 * backlight as they are. Another program may have switched one off under
 * a launcher that holds the screen lit (a test script putting back "what
 * it found", 2026-11-18: the backlight off under a freshly started
 * launcher, then the panel too): the launcher drew and took touches
 * unseen, Home went home unseen, and Power put the dark screen "to sleep".
 * Looked at when Home or Power is pressed, the keys one reaches for then:
 * a dark screen that should be lit is lit, and that press does no more.
 */
static bool screen_lit_check(void)
{
	char p[160];
	bool fixed = false;

	if (screen_off)
		return false;
	if (access(PANEL_POWER, W_OK) == 0 && read_int(PANEL_POWER, 1) == 0) {
		write_int(PANEL_POWER, 1);
		fixed = true;
	}
	if (bl_dir[0]) {
		snprintf(p, sizeof(p), "%s/bl_power", bl_dir);
		if (read_int(p, 0) != 0) {
			backlight_power(true);
			fixed = true;
		}
	}
	if (fixed) {
		fprintf(stderr, "n72-ui: the screen was dark under a lit launcher: lit again\n");
		lv_obj_invalidate(lv_screen_active());
	}
	return fixed;
}

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
		if ((ev.code == KEY_HOMEPAGE || ev.code == KEY_POWER) && screen_lit_check()) {
			home_down_us = 0;
			continue;	/* (the press that brought the picture back) */
		}
		switch (ev.code) {
		case KEY_HOMEPAGE:
			if (screen_off) {
				screen_off_for_sound = false;
				screen_set(true);
			} else if (home_down_us && mono_us() - home_down_us >= 1000000) {
				go_home();
				open_logs(NULL);
			} else if (!gp_home()) {	/* (a game: its pause panel) */
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

/* SIGUSR1: write what is on screen to /tmp/n72-shot.raw (32-bit pixels B,G,R,A, upright W x H; the size in /tmp/n72-shot.size). */
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
				fwrite(p, 4 * 320, 480, f);
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
	{
		/*
		 * What the UI shows, upright as it lays it out (W x H, also
		 * written to n72-shot.size): over an application, LVGL's own
		 * rendering of its screen (the layer holds only the launcher's
		 * things, premultiplied: the application and video layers are
		 * not in it); otherwise a GPU copy of the newest frame.
		 */
		bool over_app = app_pid > 0 && lv_screen_active() == app_scr;
		lv_draw_buf_t *b = over_app ? lv_snapshot_take(app_scr, LV_COLOR_FORMAT_ARGB8888)
					    : mbx_gpu_grab_screen();

		if (b) {
			f = fopen("/tmp/n72-shot.raw.tmp", "w");
			if (f) {
				for (y = 0; y < (int)b->header.h; y++)
					fwrite(b->data + y * b->header.stride, 4, b->header.w, f);
				fclose(f);
				f = fopen("/tmp/n72-shot.size", "w");
				if (f) {
					fprintf(f, "%u %u\n", (unsigned)b->header.w, (unsigned)b->header.h);
					fclose(f);
				}
				rename("/tmp/n72-shot.raw.tmp", "/tmp/n72-shot.raw");
			}
			lv_draw_buf_destroy(b);
			return;
		}
	}
	p = mbx_gpu_front(&stride);	/* (no GPU copy: the panel's pixels) */
	f = p ? fopen("/tmp/n72-shot.raw.tmp", "w") : NULL;
	if (!f)
		return;
	for (y = 0; y < 480; y++)
		fwrite(p + y * stride, 4, 320, f);
	fclose(f);
	f = fopen("/tmp/n72-shot.size", "w");
	if (f) {
		fprintf(f, "320 480\n");
		fclose(f);
	}
	rename("/tmp/n72-shot.raw.tmp", "/tmp/n72-shot.raw");
}

static void restore_tty(void)
{
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
}

static void on_signal(int sig)
{
	/* the application started goes with the launcher (else a video kept
	 * playing, with nothing to stop it, after a restart) */
	if (app_pid > 0)
		kill(-app_pid, SIGTERM);
	restore_tty();
#ifdef N72_PGO_GEN
	{	/* (instrumented build: the counters, as it is stopped) */
		extern int __llvm_profile_write_file(void);

		__llvm_profile_write_file();
	}
#endif
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
	/* (a background command: a picture made into wallpaper frames, no UI) */
	if (argc > 1 && !strcmp(argv[1], "--wallpaper"))
		return wp_convert_main(argc, argv);
	/* (a Games box art made small, into its cache: ui_games.c) */
	if (argc > 1 && !strcmp(argv[1], "--thumb"))
		return games_thumb_main(argc, argv);

	/* Keep the text console from drawing over the UI. */
	tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	/*
	 * Layer 0 (/dev/fb0) still holds what the console drew while no
	 * launcher ran (boot, or a restart: the kernel's messages). It shows
	 * wherever layer 1 is transparent before an application or the
	 * wallpaper paints it (an app's card growing over home): black now.
	 */
	{
		int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
		struct fb_var_screeninfo vi;

		if (fd >= 0) {
			if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 && vi.bits_per_pixel == 32) {
				size_t len = (size_t)vi.xres_virtual * vi.yres_virtual * 4;
				uint32_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

				if (p != MAP_FAILED) {
					for (size_t i = 0; i < len / 4; i++)
						p[i] = 0xff000000u;
					munmap(p, len);
				}
			}
			close(fd);
		}
	}
	atexit(restore_tty);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	lv_init();
	lv_tick_set_cb(tick_ms);
	backlight_init();
	/*
	 * Lit, as screen_off (false) says: whatever ran before may have left
	 * the panel off (a launcher killed while dark), and the screen would
	 * then stay dark with Home going home unseen and Power putting it
	 * to sleep.
	 */
	if (access(PANEL_POWER, W_OK) == 0)
		write_int(PANEL_POWER, 1);
	/* (the driver lights the backlight when the panel goes on; a panel
	 * already on with the backlight switched off stayed dark) */
	backlight_power(true);

	disp = NULL;
	if (!getenv("N72_GPU") || strcmp(getenv("N72_GPU"), "0"))
		disp = mbx_gpu_display_create();
	if (!disp) {
		disp = lv_linux_fbdev_create();
		lv_linux_fbdev_set_file(disp, fb ? fb : "/dev/fb0");
	}
	{
		/* (N72_ROT=90|270: start turned, for tests) */
		const char *r = getenv("N72_ROT");

		if (r && (atoi(r) == 90 || atoi(r) == 270) && mbx_gpu_set_rotation(atoi(r))) {
			ui_rot = atoi(r);
			ui_w = lv_display_get_horizontal_resolution(disp);
			ui_h = lv_display_get_vertical_resolution(disp);
		}
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
		n72_trace_on = fps_on && getenv("N72_TRACE") != NULL;
	}

	if (!keys)
		keys = find_input("gpio-keys");
	if (keys)
		keys_fd = open(keys, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	/* (the buttons: their fd in the main loop's poll, no timer) */
	idle_timer = awake_timer(idle_check, 1000, NULL);
	playback_timer = lv_timer_create(playback_check, 2000, NULL);
	lv_timer_pause(playback_timer);		/* (screen_off_or_sleep starts it) */
	uevent_init();

	build_home();
	kbd_init();
	rot_init();
	drags_init();
	sound_ui_init();
	ui_apply_saved();	/* brightness, Wi-Fi and Bluetooth as left */
	ui_reopen();		/* a page asked for at start (after a theme change) */
	crash_handlers();
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	/* (screenshots: the signal breaks the main loop's poll; shot_poll there) */
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
		struct pollfd pf[3 + NWATCH] = {
			{ .fd = touch_wait_fd, .events = POLLIN },
			{ .fd = keys_fd, .events = POLLIN },
			{ .fd = notify_fd, .events = POLLIN },
		};
		int nw, w;
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
		/*
		 * (Only what was paused here is resumed: LVGL pauses the timer
		 * itself when there is nothing to draw and resumes it on an
		 * invalidation; resuming it every turn woke this loop 60 times
		 * a second for nothing, screen on or off.)
		 */
		if (refr_timer && notify_fd >= 0) {
			static bool held;

			if (mbx_gpu_frame_waiting()) {
				if (!refr_timer->paused) {
					lv_timer_pause(refr_timer);
					held = true;
				}
			} else if (held) {
				lv_timer_resume(refr_timer);
				held = false;
			}
		}
		th0 = mono_us();
		idle = lv_timer_handler();

		if (fps_on && mono_us() - th0 > 30000)
			fprintf(stderr, "timers: %.1f ms\n", (mono_us() - th0) / 1000.0);
		if (n72_trace_on && mono_us() - th0 > 1500)
			n72_trace("timers-us", (long)(mono_us() - th0));

		nw = nwatch;	/* (after the timers, which may have changed it) */
		for (w = 0; w < nw; w++) {
			pf[3 + w].fd = watch[w].fd;
			pf[3 + w].events = watch[w].events;
		}
		/* until LVGL's next timer, or an event: no fixed tick */
		if (poll(pf, 3 + nw, idle == LV_NO_TIMER_READY ? -1 : (int)LV_MIN(idle, 60000)) > 0) {
			n72_trace("poll", (pf[0].revents & POLLIN ? 1 : 0) | (pf[2].revents & POLLIN ? 4 : 0) | 8);
			for (w = 0; w < nw; w++)
				if ((pf[3 + w].revents & (POLLIN | POLLPRI | POLLHUP | POLLERR)) &&
				    w < nwatch && watch[w].fd == pf[3 + w].fd)
					watch[w].cb(pf[3 + w].fd);
			if ((pf[0].revents & POLLIN) && rt_uinput >= 0) {
				lat_read_us = mono_us();
				rt_feed(touch_wait_fd, touch_indev);	/* reads LVGL's side */
				n72_trace("indev<", 0);
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
		if (shot_wanted || scans_wanted)
			shot_poll(NULL);	/* (asked by SIGUSR1/2) */
		{
		}
	}
	return 0;
}
