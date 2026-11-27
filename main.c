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
 *                  through its DRM render node when present)
 *   N72_SLEEP      set to 0 for Power to only blank the screen instead of
 *                  suspending the system
 *   N72_IDLE_SLEEP seconds without input before sleeping (default 120, 0
 *                  never); not while USB power is present
 *
 * n72-ui --bench times full-screen redraws and exits.
 */
#include "main_priv.h"
#include <math.h>
#include "lvgl/src/misc/lv_anim_private.h"	/* (ios_motion.h reads act_time) */
#include "ios_motion.h"

/* the screen as the UI lays it out: 320 x 480, or 480 x 320 turned */
int32_t ui_w = 320, ui_h = 480;

char *self_argv[4];
lv_obj_t *home_scr;
int keys_fd = -1;
static int touch_wait_fd = -1;
static lv_timer_t *refr_timer;		/* LVGL's display refresh */		/* own handle on the touch device, for poll() */
lv_indev_t *touch_indev;

static uint32_t tick_ms(void);

uint64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef N72_DEBUG
/* Touch-to-screen latency (N72_LATENCY=1): newest touch event -> flush end. */
static bool lat_on;
static uint64_t lat_event_us;		/* newest touch event not yet on screen */
static uint64_t lat_sum_us, lat_max_us;
static unsigned int lat_n;
static uint32_t lat_report_ms;

static uint64_t lat_render_max_us, lat_wake_sum_us, lat_read_us;
static bool lat_input_frame;

/* Render pending changes immediately (after input), timing the render. */
static void render_now(void)
{
	uint64_t t0 = mono_us(), d;
	static struct mbx_gpu_stats g0;	/* (only the N72_LATENCY log reads it) */

	if (lat_on)
		g0 = mbx_gpu_stats;
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
#else
static inline void render_now(void)
{
	lv_refr_now(NULL);
}

void n72_refr_start(void)
{
	lv_anim_refr_now();
}

static inline void drags_init(void)
{
}

void n72_frame_shown(void)
{
}
#endif
int tty_fd = -1;
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
bool screen_off;

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
static int bl_hw_raw = -1;
static int bl_fade_v;		/* the fade-in's animated variable */

/* the user's level in the hardware's steps (rounded: percent -> raw -> percent is stable) */
static int bl_user_raw(void);

static void backlight_raw(int raw)
{
	char p[160];
	int clamped;

	if (!bl_dir[0])
		return;
	clamped = LV_CLAMP(1, raw, bl_max);
	bl_hw_raw = clamped;
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	write_int(p, clamped);
}

int backlight_get(void)
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

void backlight_set(int pct)
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

void drain(int fd);
void home_wake(void);
void screen_off_or_sleep(void);
/* the touch device's "inhibited" (sysfs): the driver stops the controller */
static char touch_inhibit[96];
char rt_node[64];	/* the touch router's virtual device, for applications */
static void rt_reset(void);

void ui_lock(void);

/*
 * Timers that only matter while the screen is lit (the clock, the shade's
 * picture, the accelerometer, the idle timeout, ...): paused while it is
 * dark, so a dark screen (sound playing, or on USB power) wakes nothing.
 */
#define NAWAKE 16
static lv_timer_t *awake_t[NAWAKE];
static int nawake;

lv_timer_t *awake_timer(lv_timer_cb_t cb, uint32_t ms, void *ud)
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
void awake_timer_delete(lv_timer_t *t)
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

void rot_motion_off(void);
void wp_apply_kick(void);

void screen_set(bool on)
{
	bool fade;

	if (!on && screen_off)
		return;
	fade = bl_fades() && screen_off == on;	/* only on a change (on again: no fade) */
	if (screen_off == on)
		awake_set(on);		/* (on a change only) */
	screen_off = !on;
	auto_bright_sync();
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
static struct { int fd; short events; void (*cb)(int fd); } watch[NWATCH];
int nwatch;

/* (events: POLLIN, or POLLPRI for a sysfs attribute the kernel notifies) */
void ui_watch_ev(int fd, void (*cb)(int fd), short events)
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

void ui_watch(int fd, void (*cb)(int fd))
{
	ui_watch_ev(fd, cb, POLLIN);
}

lv_timer_t *idle_timer;
void idle_kick(void);
void rot_kick(void);

static int ext_ovl_sock = -1;

static void ext_ovl_cb(int fd)
{
	char buf[64];

	while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
		;
	if (app_dormant)
		mbx_gpu_sync_ext_overlay();
}

static void ext_ovl_sock_cleanup(void)
{
	if (ext_ovl_sock >= 0) {
		close(ext_ovl_sock);
		ext_ovl_sock = -1;
	}
	unlink(N72_EXT_OVL_SOCK);
}

static void ext_ovl_sock_init(void)
{
	struct sockaddr_un sa;
	int fd;

	if (!mbx_gpu_on_shell_layer())
		return;
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", N72_EXT_OVL_SOCK);
	unlink(N72_EXT_OVL_SOCK);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		ext_ovl_sock = fd;
		ui_watch(ext_ovl_sock, ext_ovl_cb);
		atexit(ext_ovl_sock_cleanup);
	} else {
		close(fd);
	}
}

void ui_unwatch(int fd)
{
	int i;

	for (i = 0; i < nwatch; i++)
		if (watch[i].fd == fd) {
			watch[i] = watch[--nwatch];
			return;
		}
}

#include "ui_m3_priv.h"

/* ------------------------------------------------------------ auto-brightness */

#define ALS_DIR "/sys/bus/i2c/devices/1-0044"

static lv_timer_t *als_timer;
static float als_lux = -1.0f;
static int als_dir, als_dir_cnt;

bool als_present(void)
{
	return access(ALS_DIR "/lux", R_OK) == 0 &&
	       access(ALS_DIR "/power_state", W_OK) == 0;
}

static void als_tick(lv_timer_t *t)
{
	/* 88.0f / log1pf(3000.0f) */
	static const float als_log_scale = 88.0f / 8.00670085f;
	int lux, target, cur, diff, dir, from, to;
	float alpha, flux;

	LV_UNUSED(t);
	if (!conf.auto_bright || screen_off)
		return;
	lux = read_int(ALS_DIR "/lux", -1);
	if (lux < 0)
		return;
	flux = (float)lux;
	if (als_lux < 0.0f) {
		als_lux = flux;
	} else {
		alpha = flux > als_lux ? 0.25f : 0.12f;
		als_lux += alpha * (flux - als_lux);
	}
	target = 12 + (int)(als_log_scale * log1pf(als_lux) + 0.5f);
	target = LV_CLAMP(12, target, 100);
	cur = backlight_get();
	diff = target - cur;
	if ((diff > 0 && diff < 6) || (diff <= 0 && -diff < 8)) {
		als_dir = 0;
		als_dir_cnt = 0;
		return;
	}
	dir = diff > 0 ? 1 : -1;
	if (dir != als_dir) {
		als_dir = dir;
		als_dir_cnt = 1;
		return;
	}
	if (++als_dir_cnt < (dir > 0 ? 2 : 4))
		return;
	als_dir_cnt = 0;
	from = bl_hw_raw > 0 ? bl_hw_raw : bl_user_raw();
	bl_user = target;
	to = bl_user_raw();
	if (from != to) {
		lv_anim_t a;

		lv_anim_delete(&bl_fade_v, NULL);
		lv_anim_init(&a);
		lv_anim_set_var(&a, &bl_fade_v);
		lv_anim_set_exec_cb(&a, bl_fade_exec);
		lv_anim_set_values(&a, from, to);
		lv_anim_set_duration(&a, LV_CLAMP(500, abs(to - from) * 18, 1800));
		lv_anim_set_path_cb(&a, ios_path_ease_in_out);
		lv_anim_start(&a);
	}
	ui_bright_sync_widgets(target);
}

void auto_bright_sync(void)
{
	bool want = conf.auto_bright && !screen_off && als_present();

	if (want) {
		write_int(ALS_DIR "/power_state", 1);
		if (!als_timer)
			als_timer = awake_timer(als_tick, 500, NULL);
	} else {
		if (als_timer) {
			awake_timer_delete(als_timer);
			als_timer = NULL;
		}
		als_lux = -1.0f;
		als_dir = 0;
		als_dir_cnt = 0;
		if (als_present())
			write_int(ALS_DIR "/power_state", 0);
	}
}

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
 * tap (measured over eleven taps, with the long axis falling from ~125 to
 * ~30 in two frames). An application that scrolls at the first movement takes that for a drag, and the tap is lost (Spore's
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
lv_point_t rt_logical(lv_point_t p)
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

/* A frame is complete (SYN_REPORT): route it. Set *@ui_touch if it went to the UI. */
static bool rt_frame_done(bool *ui_touch)
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
			*ui_touch = false;
			return false;
		}
	}
	if (rt_owner == RT_APP) {
		*ui_touch = false;
		if (rt_mt_b)
			rt_app_frame();
		else if (write(rt_uinput, rt_frame, rt_n * sizeof(rt_frame[0])) < 0)
			;
	} else {
		*ui_touch = true;
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

/* Read what the touch screen has; hand LVGL each frame of its touches. True if UI received news. */
static bool rt_feed(int fd, lv_indev_t *indev)
{
	struct input_event ev[32];
	bool ui_news = false;
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
				bool ui_touch = false;

				/* every frame: a move and the lift in one read
				 * would lose the move (and the gesture) */
				if (rt_frame_done(&ui_touch)) {
					rt_frame_us = mono_us();
					lv_indev_read(indev);
					ui_news = true;
				} else if (ui_touch) {
					ui_news = true;
				}
				if (rt_ui_down)	/* (long presses need reads between frames) */
					lv_timer_resume(lv_indev_get_read_timer(indev));
#ifdef N72_DEBUG
				if (!lat_event_us && rt_owner != RT_APP)
					lat_event_us = (uint64_t)e->input_event_sec * 1000000 +
						       e->input_event_usec;
#endif
			}
		}
	}
	return ui_news;
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

/* -------------------------------------------------------------- buttons */

/*
 * The screen as the launcher believes it, against the panel and the
 * backlight as they are. Another program may have switched one off under
 * a launcher that holds the screen lit (leaving the backlight or panel
 * disabled under a freshly started launcher): the launcher drew and took
 * touches unseen, Home went home unseen, and Power put the dark screen "to
 * sleep". Looked at when Home or Power is pressed, the keys one reaches
 * for then: a dark screen that should be lit is lit, and that press does
 * no more.
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
	while (read(keys_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.value != 1)
			continue;
		lv_display_trigger_activity(NULL);
		if ((ev.code == KEY_HOMEPAGE || ev.code == KEY_POWER) && screen_lit_check())
			continue;	/* (the press that brought the picture back) */
		switch (ev.code) {
		case KEY_HOMEPAGE:
			if (screen_off) {
				screen_off_for_sound = false;
				screen_set(true);
			} else if (!gp_home()) {	/* (a game: its pause panel) */
				go_home();
			}
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
			volume_step(1);
			break;
		case KEY_VOLUMEDOWN:
			volume_step(-1);
			break;
		}
	}
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
#ifdef __clang__
		extern int __llvm_profile_write_file(void);

		__llvm_profile_write_file();
#else
		extern void __gcov_dump(void);	/* libgcov: into PGO_DIR's .gcda files */

		__gcov_dump();
#endif
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
#ifdef N72_DEBUG
	if (!getenv("N72_GPU") || strcmp(getenv("N72_GPU"), "0"))
#endif
		disp = mbx_gpu_display_create(true);
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
#ifdef N72_DEBUG
		lat_on = getenv("N72_LATENCY") && *getenv("N72_LATENCY") == '1';
		fps_on = getenv("N72_FPS") && *getenv("N72_FPS") == '1';
		n72_trace_on = fps_on && getenv("N72_TRACE") != NULL;
#endif
	}
#ifdef N72_DEBUG
	/* the logs that print the GPU's timings (N72_FPS in any spelling:
	 * mbx_gpu.c's own two lines ask only whether it is set, touch or not) */
	mbx_gpu_timing = lat_on || getenv("N72_FPS") != NULL;
#endif

	if (!keys)
		keys = find_input("gpio-keys");
	if (keys)
		keys_fd = open(keys, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	/* (the buttons: their fd in the main loop's poll, no timer) */
	idle_timer = awake_timer(idle_check, 1000, NULL);
	playback_timer = lv_timer_create(playback_check, 2000, NULL);
	lv_timer_pause(playback_timer);		/* (screen_off_or_sleep starts it) */
	uevent_init();
	ext_ovl_sock_init();

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
#ifdef N72_DEBUG
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		mbx_gpu_timing = true;	/* the bench prints the totals */
	if (argc > 2 && !strcmp(argv[1], "--bench") && !strcmp(argv[2], "text"))
		return bench_text(argc > 3 ? atoi(argv[3]) : 30, false);
	if (argc > 2 && !strcmp(argv[1], "--bench") && !strcmp(argv[2], "label"))
		return bench_text(argc > 3 ? atoi(argv[3]) : 30, true);
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		return bench();
#endif

	/*
	 * Sleep until the next LVGL timer or until the touch screen or a button
	 * has something, and hand touch input to LVGL straight away instead of
	 * waiting for its read timer.
	 */
	notify_fd = mbx_gpu_notify_fd();
	refr_timer = lv_display_get_refr_timer(lv_display_get_default());
	{
		uint32_t next_timer_ms = 0;
		bool skip_timers = false;

		for (;;) {
			struct pollfd pf[3 + NWATCH] = {
				{ .fd = touch_wait_fd, .events = POLLIN },
				{ .fd = keys_fd, .events = POLLIN },
				{ .fd = notify_fd, .events = POLLIN },
			};
			int nw, w, wait_ms;
#ifdef N72_DEBUG
			uint64_t th0;
#endif
			uint32_t idle;

			if (!skip_timers) {
				/*
				 * Animation frames start when the display takes the last one,
				 * not on LVGL's 16 ms timer, which drifts against the 16.7 ms
				 * refresh: a frame drawn while one is still waiting replaces it
				 * (drawn for nothing), and the one shown then stays two
				 * refreshes. While a frame waits, the refresh timer rests;
				 * the hand-over (notify_fd) wakes it.
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
#ifdef N72_DEBUG
				th0 = mono_us();
#endif
				idle = lv_timer_handler();
				next_timer_ms = idle == LV_NO_TIMER_READY ? UINT32_MAX : tick_ms() + idle;

#ifdef N72_DEBUG
				if (fps_on && mono_us() - th0 > 30000)
					fprintf(stderr, "timers: %.1f ms\n", (mono_us() - th0) / 1000.0);
				if (n72_trace_on && mono_us() - th0 > 1500)
					n72_trace("timers-us", (long)(mono_us() - th0));
#endif
			}
			skip_timers = false;

			nw = nwatch;	/* (after the timers, which may have changed it) */
			for (w = 0; w < nw; w++) {
				pf[3 + w].fd = watch[w].fd;
				pf[3 + w].events = watch[w].events;
			}
			if (next_timer_ms == UINT32_MAX) {
				wait_ms = -1;
			} else {
				int32_t rem = (int32_t)(next_timer_ms - tick_ms());

				wait_ms = rem <= 0 ? 0 : (int)LV_MIN((uint32_t)rem, 60000u);
			}
			/* until LVGL's next timer, or an event: no fixed tick */
			if (poll(pf, 3 + nw, wait_ms) > 0) {
				bool other_ev = (pf[1].revents & POLLIN) || (pf[2].revents & POLLIN);
				bool touch_ui = false;

				n72_trace("poll", (pf[0].revents & POLLIN ? 1 : 0) | (pf[2].revents & POLLIN ? 4 : 0) | 8);
				for (w = 0; w < nw; w++) {
					if ((pf[3 + w].revents & (POLLIN | POLLPRI | POLLHUP | POLLERR)) &&
					    w < nwatch && watch[w].fd == pf[3 + w].fd) {
						other_ev = true;
						watch[w].cb(pf[3 + w].fd);
					}
				}
				if ((pf[0].revents & POLLIN) && rt_uinput >= 0) {
#ifdef N72_DEBUG
					lat_read_us = mono_us();
#endif
					touch_ui = rt_feed(touch_wait_fd, touch_indev);	/* reads LVGL's side */
					if (touch_ui) {
						n72_trace("indev<", 0);
						if (mbx_gpu_frame_waiting())
							render_pending = true;
						else
							render_now();
					}
				} else if (pf[0].revents & POLLIN) {
					struct input_event ev[16];
					ssize_t r;

					touch_ui = true;
					while ((r = read(touch_wait_fd, ev, sizeof(ev))) > 0) {
#ifdef N72_DEBUG
						const struct input_event *e = &ev[r / sizeof(ev[0]) - 1];

						/* time the newest event still waiting for a frame */
						if (!lat_event_us)
							lat_event_us = (uint64_t)e->input_event_sec * 1000000 +
								       e->input_event_usec;
#else
						(void)r;
#endif
					}
#ifdef N72_DEBUG
					lat_read_us = mono_us();
#endif
					lv_indev_read(touch_indev);
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
				if (app_dormant && !other_ev && !touch_ui && !render_pending &&
				    !shot_wanted && !scans_wanted &&
				    (next_timer_ms == UINT32_MAX || (int32_t)(next_timer_ms - tick_ms()) > 0))
					skip_timers = true;
			}
			if (shot_wanted || scans_wanted)
				shot_poll(NULL);	/* (asked by SIGUSR1/2) */
		}
	}
	return 0;
}
