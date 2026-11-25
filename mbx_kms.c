#define _GNU_SOURCE	/* pipe2 */
// SPDX-License-Identifier: MIT
/* The display: scan-out buffers, page flipping through KMS, the flush callbacks, the turned screen. */
#include "mbx_priv.h"

/* -------------------------------------------------------- display */

/*
 * Two ways to the screen:
 *  - page flipping (through KMS, n72-clcd): LVGL draws straight into the
 *    scan-out buffers in turn; each frame is shown by switching the display
 *    controller to it at the next vertical blank. No copy, no tearing, and
 *    frames are paced by the display's 60 Hz.
 *  - otherwise one draw buffer in the pool, copied to the boot framebuffer
 *    by the GPU after each frame.
 */
static uint8_t *disp_buf;		/* copy mode: the draw buffer */
#define NSCAN 3
static uint8_t *scan[NSCAN];		/* flip mode: the scan-out buffers */
static struct mbx_buf scan_buf[NSCAN];
static uint32_t scan_handle[NSCAN];
static uint32_t scan_ta_seq[NSCAN];	/* pending async TA frame on scan[i] */
static bool scan_cpu_cached[NSCAN] = { true, true, true };
static int scan_count = NSCAN;
static uint32_t flush_words[16 * 64];
static int flush_n;
static bool flipping;
static int32_t clean_y1, clean_y2;
static bool clean_has_rows;

/*
 * A turned screen (mbx_gpu_set_rotation): LVGL draws the upright picture
 * (lw x lh) into lbuf, which keeps it from frame to frame as a scan-out
 * buffer would, and each flush GPU-copies the areas it drew, turned, into
 * the scan-out buffer of the frame. The scan-out buffers' bookkeeping
 * (history, catch-up) stays in panel coordinates.
 */
static int rot_deg;
static uint8_t *lbuf;
static uint32_t lstride;
/*
 * The display turns the layer itself (kms_can_turn): the scan-out buffers
 * hold the upright picture (lw x lh, stride lstride) and LVGL draws
 * straight into them, as when not turned; else (no KMS, or a kernel
 * without the plane's rotation) the GPU's turned copy from lbuf.
 */
static bool hw_turn;
static int buf_rot[NSCAN];		/* the turn each buffer's frame was drawn for */
static bool kms_can_turn;

static uint32_t scan_stride(void)
{
	return rot_deg && hw_turn ? lstride : fb.stride;
}

static int32_t scan_w(void)
{
	return rot_deg && hw_turn ? (int32_t)fb.height : (int32_t)fb.width;
}

static int32_t scan_h(void)
{
	return rot_deg && hw_turn ? (int32_t)fb.width : (int32_t)fb.height;
}

static uint32_t draw_stride(void)
{
	return rot_deg ? lstride : fb.stride;
}

/* An area of the upright picture to where it lies on the panel. */
static void to_panel(const lv_area_t *a, lv_area_t *o)
{
	int32_t lw = fb.height, lh = fb.width;	/* turned: the upright picture's size */

	if (hw_turn) {
		*o = *a;
	} else if (rot_deg == 90) {
		o->x1 = a->y1; o->x2 = a->y2;
		o->y1 = lw - 1 - a->x2; o->y2 = lw - 1 - a->x1;
	} else if (rot_deg == 270) {
		o->x1 = lh - 1 - a->y2; o->x2 = lh - 1 - a->y1;
		o->y1 = a->x1; o->y2 = a->x2;
	} else {
		*o = *a;
	}
}

static void clean_rows(uint8_t *buf, const lv_area_t *a)
{
	sync_range(buf + a->y1 * draw_stride(), lv_area_get_height(a) * draw_stride(), MBX_SYNC_CLEAN);
}

/*
 * Triple buffering. Three scan-out buffers: one on screen (front), one
 * whose address has been written and is latched by the display controller
 * at the next frame start (latching), and the one LVGL draws into
 * (drawing). A finished frame waits as "ready" until the flip thread hands
 * it to the controller; a newer finished frame replaces it. So drawing
 * never waits for the display, and each vblank shows the newest frame.
 *
 * LVGL is set up single-buffered: at the start of each rendered frame
 * (LV_EVENT_RENDER_START) we point its buffer at a free scan-out buffer and
 * bring that up to date by GPU-copying, from the newest frame, whatever
 * changed since the buffer was last drawn (minus what this frame redraws
 * anyway). The flip thread runs at real-time priority: it writes a ready
 * frame's address at once and then waits for the blank in which the
 * controller takes it (the KMS flip's event); only then is the old front
 * buffer free.
 */
static pthread_t flip_thread;
static pthread_mutex_t flip_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cv = PTHREAD_COND_INITIALIZER;
static int st_front = -1, st_latching = -1, st_ready = -1, st_drawing = -1;
static int st_latest = -1;		/* buffer with the newest finished frame */
static long buf_frame[NSCAN] = { -1, -1, -1 };	/* frame number each holds */
static long frame_no;			/* frames finished so far */

/* areas drawn in each of the last HIST frames */
#define HIST 4
#define HIST_AREAS 32
static struct {
	lv_area_t a[HIST_AREAS];
	int n;
	bool all;
	long frame;
} hist[HIST];
static lv_area_t cur_areas[HIST_AREAS];
static int cur_n;
static bool cur_all;

static int notify_pipe[2] = { -1, -1 };
static int kms_fd = -1;			/* /dev/dri/card0 when flipping through KMS */
static bool kms_layer_on = true;
static bool kms_layer_want = true;
static bool gpu_suspended;
static bool kms_can_window;
static uint32_t kms_src_y, kms_win_y, kms_win_h;
static uint32_t buf_src_y[NSCAN], buf_win_y[NSCAN], buf_win_h[NSCAN];
static bool kms_flip(int r, int rot, uint32_t sy, uint32_t wy, uint32_t wh);
static void shell_active_y_bounds(uint32_t *out_y, uint32_t *out_h);
static bool hw_slide_active;
static int hw_slide_fb = -1, hw_slide_blank_fb = -1;
static bool hw_slide_blank_zeroed;
static int32_t hw_slide_h, hw_slide_last_y;
static void *hw_slide_owner;
static mbx_hw_slide_abort_cb hw_slide_abort_fn;

/* a finished frame is waiting for the display (drawing another is wasted) */
bool mbx_gpu_frame_waiting(void)
{
	bool w;

	if (!flipping)
		return false;
	pthread_mutex_lock(&flip_mx);
	w = st_ready >= 0;
	pthread_mutex_unlock(&flip_mx);
	return w;
}

/* readable when the flip thread hands a frame to the display */
int mbx_gpu_notify_fd(void)
{
	char buf[16];

	if (notify_pipe[0] >= 0)
		while (read(notify_pipe[0], buf, sizeof(buf)) > 0)
			;
	return notify_pipe[0];
}

static void *flip_main(void *arg)
{
	struct sched_param sp = { .sched_priority = 20 };

	(void)arg;
	/* the switch must not be pushed past the frame by a busy main loop */
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	pthread_mutex_lock(&flip_mx);
	for (;;) {
		double t0;
		int r, rot;
		uint32_t sy, wy, wh;

		while (st_ready < 0)
			pthread_cond_wait(&flip_cv, &flip_mx);
		r = st_ready;
		st_ready = -1;
		st_latching = r;
		rot = buf_rot[r];
		sy = buf_src_y[r];
		wy = buf_win_y[r];
		wh = buf_win_h[r];
		pthread_mutex_unlock(&flip_mx);
		if (notify_pipe[1] >= 0) {	/* the main loop may draw again */
			char c = 1;

			if (write(notify_pipe[1], &c, 1) < 0)
				;
		}
		t0 = stat_t0();
		/* flipping implies KMS (flip_setup): waits for the flip's own event */
		bool shown = kms_flip(r, rot, sy, wy, wh);

		pthread_mutex_lock(&flip_mx);
		STAT_US(flip_us, t0);
		if (shown)		/* (a flip refused: the old front is still on screen) */
			st_front = r;
		st_latching = -1;
		pthread_cond_broadcast(&flip_cv);
	}
	return NULL;
}

/* all flips done (for tests and screenshots) */
static void flip_wait(void)
{
	pthread_mutex_lock(&flip_mx);
	while (st_ready >= 0 || st_latching >= 0)
		pthread_cond_wait(&flip_cv, &flip_mx);
	pthread_mutex_unlock(&flip_mx);
}

static bool covered(const lv_area_t *a, lv_display_t *d)
{
	uint32_t i;

	for (i = 0; i < d->inv_p; i++) {
		lv_area_t p;

		if (d->inv_area_joined[i])
			continue;
		to_panel(&d->inv_areas[i], &p);
		if (lv_area_is_in(a, &p, 0))
			return true;
	}
	return false;
}

/* Batch catch-up copy rects, coalescing cache sync across all queued rows. */
#define SYNC_BATCH 64
static lv_area_t sync_q[SYNC_BATCH];
static int sync_qn;
static int32_t sync_y1, sync_y2;

static bool sync_flush(int dst, int src)
{
	uint32_t ss = scan_stride();
	int i;

	if (!sync_qn || !scan[dst] || !scan[src]) {
		sync_qn = 0;
		return false;
	}
	if (gpu_suspended || transparent) {
		mbx_kms_note_cpu_touch(scan[dst]);
		mbx_kms_note_cpu_touch(scan[src]);
		for (i = 0; i < sync_qn; i++) {
			lv_area_t *c = &sync_q[i];
			size_t bytes = (size_t)lv_area_get_width(c) * 4;
			int32_t y, h = lv_area_get_height(c);
			uint8_t *dp = scan[dst] + c->y1 * (int32_t)ss + c->x1 * 4;
			const uint8_t *sp = scan[src] + c->y1 * (int32_t)ss + c->x1 * 4;

			for (y = 0; y < h; y++) {
				memcpy(dp, sp, bytes);
				dp += ss;
				sp += ss;
			}
		}
		sync_qn = 0;
		return true;
	}
	sync_range(scan[dst] + sync_y1 * ss, (sync_y2 - sync_y1 + 1) * ss, MBX_SYNC_FLUSH);
	if (sync_qn == 1 && !transparent &&
	    lv_area_get_size(&sync_q[0]) >= 320 * 120 && !(ss & 31)) {
		lv_area_t *c = &sync_q[0];
		uint32_t w3[48];
		int n = mbx_cmd_copy(w3, 0, ss, ss, c->x1, c->y1, c->x1, c->y1,
				     lv_area_get_width(c), lv_area_get_height(c), LV_OPA_COVER);

		if (!blit3d(8, gpu_addr(scan[dst]), gpu_addr(scan[src]), w3, n)) {
			sync_qn = 0;
			return false;
		}
	}
	for (i = 0; i < sync_qn; i++) {
		lv_area_t *c = &sync_q[i];

		if (flush_n > (int)(sizeof(flush_words) / 4) - 16) {
			submit(flush_words, flush_n);
			flush_n = 0;
		}
		flush_n += put_copy(flush_words + flush_n, gpu_addr(scan[dst]), ss,
				    gpu_addr(scan[src]), ss, c->x1, c->y1, c->x1, c->y1,
				    lv_area_get_width(c), lv_area_get_height(c));
	}
	if (flush_n) {
		submit(flush_words, flush_n);
		flush_n = 0;
	}
	sync_qn = 0;
	return false;
}

static void sync_rect(int dst, int src, const lv_area_t *a)
{
	lv_area_t c;
	lv_area_t all = { 0, 0, scan_w() - 1, scan_h() - 1 };

	if (!lv_area_intersect(&c, a, &all))
		return;
	if (sync_qn >= SYNC_BATCH)
		sync_flush(dst, src);
	if (!sync_qn) {
		sync_y1 = c.y1;
		sync_y2 = c.y2;
	} else {
		sync_y1 = LV_MIN(sync_y1, c.y1);
		sync_y2 = LV_MAX(sync_y2, c.y2);
	}
	sync_q[sync_qn++] = c;
}

bool mbx_kms_direct_scan_buf(const void *p)
{
	return flipping && (!rot_deg || hw_turn) && st_drawing >= 0 && p == scan[st_drawing];
}

bool mbx_kms_can_async_ta(const lv_layer_t *layer, const lv_draw_task_t *last)
{
	lv_display_t *d = lv_refr_get_disp_refreshing();

	return flipping && (!rot_deg || hw_turn) && st_drawing >= 0 &&
	       d && d->last_area && layer == d->layer_head &&
	       layer->draw_buf && layer->draw_buf->data == scan[st_drawing] &&
	       last && !last->next;
}

void mbx_kms_note_async_ta(uint32_t seq)
{
	if (st_drawing >= 0)
		scan_ta_seq[st_drawing] = seq;
	ta_active_seq = seq;
}

bool mbx_kms_buf_needs_invalidate(const void *p)
{
	if (mbx_kms_direct_scan_buf(p))
		return scan_cpu_cached[st_drawing] || frame_cpu_dirty;
	return true;
}

void mbx_kms_note_buf_invalidated(const void *p, const lv_area_t *a)
{
	if (mbx_kms_direct_scan_buf(p) && a && a->y1 <= 0 && a->y2 >= scan_h() - 1)
		scan_cpu_cached[st_drawing] = false;
}

void mbx_kms_note_cpu_touch(const void *p)
{
	int i;

	for (i = 0; i < NSCAN; i++) {
		if (scan[i] == p) {
			scan_cpu_cached[i] = true;
			return;
		}
	}
}

static void scan_zero_transparent(void)
{
	uint32_t size = (uint32_t)scan_h() * scan_stride();
	int i;

	if (!transparent || (rot_deg && !hw_turn))
		return;
	for (i = 0; i < scan_count; i++) {
		if (!scan[i])
			continue;
		memset(scan[i], 0, size);
		sync_range(scan[i], size, MBX_SYNC_CLEAN);
		scan_cpu_cached[i] = true;
		buf_frame[i] = frame_no;
	}
}

static void render_start_cb(lv_event_t *e)
{
	lv_display_t *d = lv_event_get_target(e);
	lv_area_t all = { 0, 0, scan_w() - 1, scan_h() - 1 };
	int i, k, dr = -1, src;
	long have, f;

	if (hw_slide_active)
		mbx_gpu_hw_slide_end(true);

	if (scan_count <= 2)
		flip_wait();	/* double-buffered on scan[0..1] (or single fallback) while GPU-suspended */

	pthread_mutex_lock(&flip_mx);
	if (scan_count == 1) {
		dr = 0;
	} else if (scan_count == 2) {
		dr = st_front == 0 ? 1 : 0;
	} else {
		for (i = 0; i < scan_count; i++)
			if (i != st_front && i != st_latching && i != st_ready)
				break;
		if (i < scan_count) {
			dr = i;
		} else {
			/*
			 * Drawing faster than 60 Hz: the finished frame still waiting
			 * has not been handed to the display; draw the newer one over
			 * it (it holds the newest content, so nothing to bring over).
			 */
			dr = st_ready;
			st_ready = -1;
		}
	}
	st_drawing = dr;
	src = st_latest;
	pthread_mutex_unlock(&flip_mx);

	if (scan_ta_seq[dr]) {
		ta_wait_active();
		scan_ta_seq[dr] = 0;
	}

	/* bring the buffer up to date: frames buf_frame[dr]+1 .. frame_no */
	double dbg_t0 = stat_t0();
	bool synced_cpu = false;
	int32_t sy1 = 0, sy2 = -1;
	have = buf_frame[dr];
	sync_qn = 0;
	if (scan_count > 1 && src >= 0 && src != dr && scan[src] && have < frame_no) {
		if (have < 0 || frame_no - have > HIST - 1) {
			if (!covered(&all, d))
				sync_rect(dr, src, &all);
		} else {
			for (f = have + 1; f <= frame_no; f++) {
				k = f % HIST;
				if (hist[k].frame != f || hist[k].all) {
					if (!covered(&all, d))
						sync_rect(dr, src, &all);
					break;
				}
				for (i = 0; i < hist[k].n; i++)
					if (!covered(&hist[k].a[i], d))
						sync_rect(dr, src, &hist[k].a[i]);
			}
		}
		sy1 = sync_y1;
		sy2 = sync_y2;
		synced_cpu = sync_flush(dr, src);
	}
	d->buf_1->data = rot_deg && !hw_turn ? lbuf : scan[dr];
	cur_n = 0;
	cur_all = false;
	pm_skip_n = 0;
	frame_cpu_dirty = synced_cpu;
	cpu_dirty_y1 = synced_cpu ? sy1 : 0;
	cpu_dirty_y2 = synced_cpu ? sy2 : -1;
	frame_drew_any = false;
	clean_has_rows = synced_cpu;
	clean_y1 = sy1;
	clean_y2 = sy2;
	n72_trace("draw", 0);
#ifdef N72_DEBUG
	if (mbx_gpu_timing && gpu_now_us() - dbg_t0 > 8000 && getenv("N72_FPS"))
		fprintf(stderr, "render start: catch-up %.1f ms\n", (gpu_now_us() - dbg_t0) / 1000.0);
#else
	(void)dbg_t0;
#endif
}

extern bool transparent;
static void premultiply(uint8_t *px_map, const lv_area_t *a);

static void flush_flip(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	double dbg_t0 = stat_t0(), dbg_t1;
	bool last = lv_display_flush_is_last(disp);

	n72_trace("flush", lv_area_get_height(area));
	if (transparent) {
		mbx_kms_note_cpu_touch(px_map);
		premultiply(px_map, area);
		if (!frame_cpu_dirty || cpu_dirty_y1 > cpu_dirty_y2) {
			cpu_dirty_y1 = area->y1;
			cpu_dirty_y2 = area->y2;
		} else {
			if (area->y1 < cpu_dirty_y1)
				cpu_dirty_y1 = area->y1;
			if (area->y2 > cpu_dirty_y2)
				cpu_dirty_y2 = area->y2;
		}
		frame_cpu_dirty = true;
	}
	dbg_t1 = stat_t0();
	if (rot_deg && !hw_turn) {
		uint32_t cw[48];
		lv_area_t pa;
		int n = mbx_cmd_copy_turn(cw, fb.stride, lstride, area->x1, area->y1,
					  lv_area_get_width(area), lv_area_get_height(area),
					  rot_deg, fb.height, fb.width);

		if (frame_cpu_dirty)
			clean_rows(px_map, area);
		to_panel(area, &pa);
		if (blit3d(8, gpu_addr(scan[st_drawing]), gpu_addr(lbuf), cw, n))
			fprintf(stderr, "n72-ui: turned copy failed\n");
		if (cur_n < HIST_AREAS)
			cur_areas[cur_n++] = pa;
		else
			cur_all = true;
	} else {
		if (!clean_has_rows) {
			clean_y1 = area->y1;
			clean_y2 = area->y2;
			clean_has_rows = true;
		} else {
			clean_y1 = LV_MIN(clean_y1, area->y1);
			clean_y2 = LV_MAX(clean_y2, area->y2);
		}
		if (cur_n < HIST_AREAS)
			cur_areas[cur_n++] = *area;
		else
			cur_all = true;
		if (last && frame_cpu_dirty && clean_has_rows) {
			lv_area_t ca = {
				0,
				cpu_dirty_y1 <= cpu_dirty_y2 ? cpu_dirty_y1 : clean_y1,
				0,
				cpu_dirty_y1 <= cpu_dirty_y2 ? cpu_dirty_y2 : clean_y2,
			};

			clean_rows(px_map, &ca);
		}
	}
#ifdef N72_DEBUG
	if (mbx_gpu_timing && gpu_now_us() - dbg_t0 > 8000 && getenv("N72_FPS"))
		fprintf(stderr, "flush: premultiply %.1f ms, clean %.1f ms (%d rows)\n",
			(dbg_t1 - dbg_t0) / 1000.0, (gpu_now_us() - dbg_t1) / 1000.0,
			(int)lv_area_get_height(area));
#else
	(void)dbg_t0;
	(void)dbg_t1;
#endif
	if (last) {
		long n = frame_no + 1;
		int k = n % HIST;
#ifdef N72_DEBUG
		static int alphacheck = -1;	/* N72_ALPHACHECK, read once */

		if (alphacheck < 0)
			alphacheck = getenv("N72_ALPHACHECK") != NULL;
		if (!transparent && alphacheck) {
			/* an opaque frame must have alpha 255 everywhere */
			uint32_t x, y, bad = 0, fx = 0, fy = 0;

			ta_wait_active();
			mbx_kms_note_cpu_touch(px_map);
			for (y = 0; y < fb.height; y++)
				for (x = 0; x < fb.width; x++)
					if ((((uint32_t *)(px_map + y * fb.stride))[x] >> 24) != 255 && !bad++) {
						fx = x;
						fy = y;
					}
			if (bad)
				fprintf(stderr, "alpha: frame %ld has %u non-opaque pixels, first at %u,%u\n",
					n, bad, fx, fy);
		}
#endif

		n72_trace("cleaned", 0);
		if (kms_shell && !kms_layer_want) {
			/* Layer 1 intentionally disabled (dormant): drop flip so we don't re-enable CRTC. */
			pthread_mutex_lock(&flip_mx);
			st_drawing = -1;
			pthread_mutex_unlock(&flip_mx);
			MBX_STAT_INC(flushes);
			lv_display_flush_ready(disp);
			return;
		}
		uint32_t wy = 0, wh = fb.height;

		shell_active_y_bounds(&wy, &wh);
		memcpy(hist[k].a, cur_areas, sizeof(cur_areas));
		hist[k].n = cur_n;
		hist[k].all = cur_all;
		hist[k].frame = n;
		pthread_mutex_lock(&flip_mx);
		frame_no = n;
		buf_frame[st_drawing] = n;
		buf_rot[st_drawing] = hw_turn ? rot_deg : 0;
		buf_src_y[st_drawing] = (hw_turn && rot_deg) ? 0 : wy;
		buf_win_y[st_drawing] = wy;
		buf_win_h[st_drawing] = wh;
		st_latest = st_drawing;
		st_ready = st_drawing;		/* (replaces an older ready frame) */
		st_drawing = -1;
		pthread_cond_signal(&flip_cv);
		pthread_mutex_unlock(&flip_mx);
		n72_frame_shown();		/* (queued: on screen at the next vblank) */
	}
	MBX_STAT_INC(flushes);
	lv_display_flush_ready(disp);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	uint32_t stride = fb.stride;

	if (flipping) {
		flush_flip(disp, area, px_map);
		return;
	}
	if (frame_cpu_dirty)
		sync_range(disp_buf + area->y1 * stride, lv_area_get_height(area) * stride,
			   MBX_SYNC_CLEAN);
	if (flush_n > (int)(sizeof(flush_words) / 4) - 16) {
		submit(flush_words, flush_n);
		flush_n = 0;
	}
	flush_n += put_copy(flush_words + flush_n, fb.gpu_addr, fb.stride,
			    gpu_addr(disp_buf), stride, area->x1, area->y1,
			    area->x1, area->y1, lv_area_get_width(area),
			    lv_area_get_height(area));
	MBX_STAT_INC(flushes);
	if (lv_display_flush_is_last(disp)) {
		if (flush_n) {
			submit(flush_words, flush_n);
			flush_n = 0;
		}
		n72_frame_shown();
	}
	lv_display_flush_ready(disp);
}

/* Before LVGL lays out and draws the next frame. */
static void refr_start_cb(lv_event_t *e)
{
	lv_display_t *d = lv_event_get_target(e);

	if (hw_slide_active && d && d->inv_p > 0)
		mbx_gpu_hw_slide_abort(NULL);
	if (!flipping) {
		frame_cpu_dirty = false;
		cpu_dirty_y1 = 0;
		cpu_dirty_y2 = -1;
		frame_drew_any = false;
	}
	n72_refr_start();
}

/* Map one more buffer object as a region. */
static uint8_t *region_add(uint32_t size, uint32_t flags, uint32_t *handle, struct mbx_buf *out)
{
	struct mbx_buf b;

	if (nregions == 4 || mbx_alloc(mbx, size, flags, &b))
		return NULL;
	regions[nregions++] = (struct region){ b.cpu, size, b.gpu, b.handle };
	if (handle)
		*handle = b.handle;
	if (out)
		*out = b;
	return b.cpu;
}

/* Page flipping: contiguous scan-out buffers, if the display has KMS. */
/*
 * KMS (kms_open() below): the scan-out buffers are exported from
 * the MBX driver as dma-bufs, imported as DRM framebuffers, and flipped
 * with DRM_IOCTL_MODE_PAGE_FLIP; the flip's event comes at the vblank that
 * shows it. When n72-ui exits, DRM gives the screen back to the console.
 * Without KMS there is no flipping: copy to the framebuffer.
 */
static uint32_t kms_fb[NSCAN], kms_crtc, kms_conn;
static struct drm_mode_modeinfo kms_mode;
static uint32_t kms_handle[NSCAN];	/* the scan buffers' GEM handles on kms_fd */
bool kms_shell;				/* on the display's layer 1, over the applications */

/*
 * A turned screen by the display (kernel n72-clcd: the plane's "rotation"):
 * the scan-out buffers hold the upright picture, landscape, as frame
 * buffers of their own (kms_fb_l), and a flip that changes the turn is an
 * atomic commit of the buffer and the rotation together.
 */
static uint32_t kms_fb_l[NSCAN];
static uint32_t kms_plane, kms_prop_rot, kms_prop_fb, kms_prop_sw, kms_prop_sh;
static uint32_t kms_prop_cx, kms_prop_cy, kms_prop_cw, kms_prop_ch, kms_prop_sx, kms_prop_sy;
static uint32_t kms_prop_plane_crtc, kms_prop_active, kms_prop_mode_id, kms_prop_conn_crtc;
static uint32_t kms_mode_blob;
static uint64_t kms_rot_cur;		/* the plane's rotation (DRM_MODE_ROTATE_*) */

#define ROT_BITS(deg) ((deg) == 90 ? 2u : (deg) == 270 ? 8u : 1u)	/* DRM_MODE_ROTATE_* */

static uint32_t kms_obj_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
	struct drm_mode_obj_get_properties op = {
		.obj_id = obj_id, .obj_type = obj_type,
	};
	uint32_t ids[24];
	uint64_t vals[24];
	unsigned int k;

	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return 0;
	op.props_ptr = (uintptr_t)ids;
	op.prop_values_ptr = (uintptr_t)vals;
	if (op.count_props > 24)
		op.count_props = 24;
	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return 0;
	for (k = 0; k < op.count_props; k++) {
		struct drm_mode_get_property gp = { .prop_id = ids[k] };

		if (!ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) && !strcmp(gp.name, name))
			return ids[k];
	}
	return 0;
}

/* The layer's plane and its properties; false if it cannot turn. */
static bool kms_find_plane(int fd)
{
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	struct drm_set_client_cap acap = { DRM_CLIENT_CAP_ATOMIC, 1 };
	struct drm_mode_get_plane_res pr = { 0 };
	uint32_t planes[4];
	unsigned int i, k;

	if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) || ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &acap))
		return false;
	pr.plane_id_ptr = (uintptr_t)planes;
	pr.count_planes = 4;
	if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr))
		return false;
	for (i = 0; i < pr.count_planes && i < 4; i++) {
		struct drm_mode_obj_get_properties op = {
			.obj_id = planes[i], .obj_type = DRM_MODE_OBJECT_PLANE,
		};
		uint32_t ids[24];
		uint64_t vals[24];
		uint32_t rot = 0, fbp = 0, crtcp = 0, sw = 0, sh = 0;
		uint32_t cx = 0, cy = 0, cw = 0, ch = 0, sx = 0, sy = 0;
		uint64_t rotv = 1, type = 0;

		if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
			continue;
		op.props_ptr = (uintptr_t)ids;
		op.prop_values_ptr = (uintptr_t)vals;
		if (op.count_props > 24)
			op.count_props = 24;
		if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
			continue;
		for (k = 0; k < op.count_props; k++) {
			struct drm_mode_get_property gp = { .prop_id = ids[k] };

			if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp))
				continue;
			if (!strcmp(gp.name, "rotation")) {
				rot = ids[k];
				rotv = vals[k];
			} else if (!strcmp(gp.name, "FB_ID")) {
				fbp = ids[k];
			} else if (!strcmp(gp.name, "CRTC_ID")) {
				crtcp = ids[k];
			} else if (!strcmp(gp.name, "CRTC_X")) {
				cx = ids[k];
			} else if (!strcmp(gp.name, "CRTC_Y")) {
				cy = ids[k];
			} else if (!strcmp(gp.name, "CRTC_W")) {
				cw = ids[k];
			} else if (!strcmp(gp.name, "CRTC_H")) {
				ch = ids[k];
			} else if (!strcmp(gp.name, "SRC_X")) {
				sx = ids[k];
			} else if (!strcmp(gp.name, "SRC_Y")) {
				sy = ids[k];
			} else if (!strcmp(gp.name, "SRC_W")) {
				sw = ids[k];
			} else if (!strcmp(gp.name, "SRC_H")) {
				sh = ids[k];
			} else if (!strcmp(gp.name, "type")) {
				type = vals[k];
			}
		}
		if (type != 1 /* primary */ || !rot || !fbp || !sw || !sh)
			continue;
		kms_prop_cx = cx;
		kms_prop_cy = cy;
		kms_prop_cw = cw;
		kms_prop_ch = ch;
		kms_prop_sx = sx;
		kms_prop_sy = sy;
		kms_prop_sw = sw;
		kms_prop_sh = sh;
		kms_plane = planes[i];
		kms_prop_rot = rot;
		kms_prop_fb = fbp;
		kms_prop_plane_crtc = crtcp;
		kms_rot_cur = rotv;
		return true;
	}
	return false;
}

/* Set the plane's rotation alone (the layer off, or the same size). */
static bool kms_set_rot(int fd, uint64_t v)
{
	struct drm_mode_obj_set_property sp = {
		.value = v, .prop_id = kms_prop_rot, .obj_id = kms_plane,
		.obj_type = DRM_MODE_OBJECT_PLANE,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp))
		return false;
	kms_rot_cur = v;
	return true;
}

/*
 * A restart (a theme change execs n72-ui again) hands the KMS file over:
 * N72_KMS_HANDOFF="fd fb handle fb handle ..." names it and the old
 * process's frame buffers. The new process keeps the layer as it is, so
 * the old picture stays until its own first flip (the screen never shows
 * layer 0 underneath), then frees the old buffers.
 */
#define HANDOFF_ENV	"N72_KMS_HANDOFF"
static uint32_t handoff_fb[NSCAN], handoff_handle[NSCAN];
static int handoff_n;

void mbx_gpu_handoff(void)
{
	char buf[128];
	int i, n;

	if (kms_fd < 0)
		return;
	fcntl(kms_fd, F_SETFD, 0);	/* kept across exec */
	n = snprintf(buf, sizeof(buf), "%d", kms_fd);
	for (i = 0; i < NSCAN && n < (int)sizeof(buf) - 24; i++)
		if (kms_fb[i] && kms_handle[i])
			n += snprintf(buf + n, sizeof(buf) - n, " %u %u", kms_fb[i], kms_handle[i]);
	setenv(HANDOFF_ENV, buf, 1);
}

/* the handed-over KMS file, if there is one and it is the layer we want */
static int kms_take_handoff(void)
{
	const char *e = getenv(HANDOFF_ENV);
	char name[32] = "";
	struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
	int fd, i, off = 0, k;

	if (!e)
		return -1;
	if (sscanf(e, "%d%n", &fd, &off) != 1 || fd < 0)
		goto bad;
	for (i = 0; i < NSCAN; i++) {
		if (sscanf(e + off, "%u %u%n", &handoff_fb[i], &handoff_handle[i], &k) != 2)
			break;
		off += k;
	}
	handoff_n = i;
	if (ioctl(fd, DRM_IOCTL_VERSION, &v) || strcmp(name, "n72-clcd-shell"))
		goto bad;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	unsetenv(HANDOFF_ENV);
	kms_shell = true;
	return fd;
bad:
	handoff_n = 0;
	unsetenv(HANDOFF_ENV);
	return -1;
}

/* after the new process's first flip: the old buffers go */
static void kms_drop_handoff(void)
{
	int i;

	for (i = 0; i < handoff_n; i++) {
		struct drm_gem_close gc = { .handle = handoff_handle[i] };

		ioctl(kms_fd, DRM_IOCTL_MODE_RMFB, &handoff_fb[i]);
		ioctl(kms_fd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	handoff_n = 0;
}

/*
 * The launcher's own layer if the kernel has it: n72-clcd-shell, the
 * display controller's second RGB layer as a device of its own, blended
 * over layer 0 with its alpha (premultiplied; 0 lets layer 0 through).
 * Applications are then the masters of n72-clcd and the launcher never has
 * to give the screen away. When @shell_layer is false (or on older kernels),
 * open layer 0 (n72-clcd).
 */
static int kms_open(bool shell_layer)
{
	static const char *const want[] = { "n72-clcd-shell", "n72-clcd" };
	char path[32], name[32];
	int w, i, fd;

	for (w = shell_layer ? 0 : 1; w < 2; w++) {
		for (i = 0; i < 8; i++) {
			struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };

			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd < 0)
				continue;
			memset(name, 0, sizeof(name));
			if (!ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, want[w])) {
				kms_shell = !w;
				return fd;
			}
			close(fd);
		}
	}
	return -1;
}

/*
 * Wait for the completion event of flip @seq (its user_data), up to @ms;
 * events of earlier flips are dropped. Each flip used to wait 200 ms and
 * read one event, whatever it was: after one wait that ran out (a stall
 * of a few hundred milliseconds), every later wait read the previous
 * flip's event at once, the launcher took a buffer still being scanned
 * out for free and drew into it, and its layer flickered until two events
 * happened to arrive in one read (the vblank wait statistic dropped to
 * 0.2-0.5 ms instead of the normal 14-16 ms). True when the event arrived.
 */
static bool kms_wait_flip(uint64_t seq, int ms)
{
	struct pollfd p = { .fd = kms_fd, .events = POLLIN };
	char buf[512] __attribute__((aligned(8)));
	double until = gpu_now_us() + ms * 1000.0;

	for (;;) {
		int left = (int)((until - gpu_now_us()) / 1000);
		ssize_t n, o;

		if (left <= 0 || poll(&p, 1, left) <= 0)
			return false;
		n = read(kms_fd, buf, sizeof(buf));
		for (o = 0; n > 0 && o + (ssize_t)sizeof(struct drm_event) <= n;) {
			struct drm_event *e = (struct drm_event *)(buf + o);

			if (e->length < sizeof(*e))
				break;
			if (e->type == DRM_EVENT_FLIP_COMPLETE &&
			    ((struct drm_event_vblank *)e)->user_data == seq)
				return true;
			o += e->length;
		}
	}
}

static uint64_t kms_seq;

static void shell_active_y_bounds(uint32_t *out_y, uint32_t *out_h)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *roots[3];
	int32_t min_y = (int32_t)fb.height, max_y = -1;
	int r;

	if (!kms_can_window || !transparent || rot_deg) {
		*out_y = 0;
		*out_h = fb.height;
		return;
	}
	if (scr && lv_obj_get_style_bg_opa(scr, LV_PART_MAIN) > LV_OPA_TRANSP) {
		*out_y = 0;
		*out_h = fb.height;
		return;
	}
	roots[0] = scr;
	roots[1] = lv_layer_top();
	roots[2] = lv_layer_sys();
	for (r = 0; r < 3; r++) {
		uint32_t n, i;

		if (!roots[r])
			continue;
		n = lv_obj_get_child_count(roots[r]);
		for (i = 0; i < n; i++) {
			lv_obj_t *c = lv_obj_get_child(roots[r], (int32_t)i);
			lv_area_t a;
			int32_t ext, y1, y2;

			if (!c || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
				continue;
			lv_obj_get_coords(c, &a);
			ext = lv_obj_get_ext_draw_size(c);
			y1 = a.y1 - ext;
			y2 = a.y2 + ext;
			if (y2 < 0 || y1 >= (int32_t)fb.height)
				continue;
			if (y1 < 0)
				y1 = 0;
			if (y2 >= (int32_t)fb.height)
				y2 = (int32_t)fb.height - 1;
			if (y1 < min_y)
				min_y = y1;
			if (y2 > max_y)
				max_y = y2;
		}
	}
	if (min_y > max_y) {
		*out_y = 0;
		*out_h = 16;
		return;
	}
	{
		uint32_t h = (uint32_t)(max_y - min_y + 1);

		if (h < 16) {
			h = 16;
			if ((uint32_t)min_y + h > fb.height)
				min_y = (int32_t)fb.height - 16;
		}
		*out_y = (uint32_t)min_y;
		*out_h = h;
	}
}

/* The flip to buffer @r, waited for: true once it is on the screen. */
static bool kms_flip(int r, int rot, uint32_t sy, uint32_t wy, uint32_t wh)
{
	uint64_t want = ROT_BITS(rot);
	uint32_t fsy = rot ? 0 : sy;
	uint32_t fwy = (rot || !wh) ? 0 : wy;
	uint32_t fwh = (rot || !wh) ? fb.height : wh;
	struct drm_mode_crtc_page_flip pf = {
		.crtc_id = kms_crtc, .fb_id = rot ? kms_fb_l[r] : kms_fb[r],
		.flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = ++kms_seq,
	};
	bool need_rot, need_win;
	int tries;

	if (!kms_layer_on) {
		if (kms_can_window && kms_mode_blob && kms_prop_active &&
		    kms_prop_mode_id && kms_prop_conn_crtc && kms_prop_plane_crtc) {
			uint32_t objs[3] = { kms_crtc, kms_conn, kms_plane };
			uint32_t counts[3] = { 2, 1, 0 };
			uint32_t props[16];
			uint64_t vals[16];
			struct drm_mode_atomic a = { 0 };
			int n = 0;

			props[n] = kms_prop_active;
			vals[n++] = 1;
			props[n] = kms_prop_mode_id;
			vals[n++] = kms_mode_blob;
			props[n] = kms_prop_conn_crtc;
			vals[n++] = kms_crtc;
			props[n] = kms_prop_plane_crtc;
			vals[n++] = kms_crtc;
			props[n] = kms_prop_fb;
			vals[n++] = pf.fb_id;
			if (kms_can_turn) {
				props[n] = kms_prop_rot;
				vals[n++] = want;
			}
			props[n] = kms_prop_cx;
			vals[n++] = 0;
			props[n] = kms_prop_cy;
			vals[n++] = fwy;
			props[n] = kms_prop_cw;
			vals[n++] = fb.width;
			props[n] = kms_prop_ch;
			vals[n++] = fwh;
			props[n] = kms_prop_sx;
			vals[n++] = 0;
			props[n] = kms_prop_sy;
			vals[n++] = (uint64_t)fsy << 16;
			props[n] = kms_prop_sw;
			vals[n++] = (uint64_t)(rot ? fb.height : fb.width) << 16;
			props[n] = kms_prop_sh;
			vals[n++] = (uint64_t)(rot ? fb.width : fwh) << 16;
			counts[2] = (uint32_t)(n - 3);
			a.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
			a.count_objs = 3;
			a.objs_ptr = (uintptr_t)objs;
			a.count_props_ptr = (uintptr_t)counts;
			a.props_ptr = (uintptr_t)props;
			a.prop_values_ptr = (uintptr_t)vals;
			if (!ioctl(kms_fd, DRM_IOCTL_MODE_ATOMIC, &a)) {
				kms_layer_on = true;
				if (kms_can_turn)
					kms_rot_cur = want;
				kms_src_y = fsy;
				kms_win_y = fwy;
				kms_win_h = fwh;
				if (handoff_n)
					kms_drop_handoff();
				return true;
			}
			fprintf(stderr, "n72-ui: atomic enable layer 1: %s\n", strerror(errno));
		}
		{
			struct drm_mode_crtc set = {
				.crtc_id = kms_crtc,
				.fb_id = pf.fb_id,
				.mode = kms_mode,
				.mode_valid = 1,
				.set_connectors_ptr = (uintptr_t)&kms_conn,
				.count_connectors = 1,
			};

			if (kms_can_turn && want != kms_rot_cur)
				kms_set_rot(kms_fd, want);
			if (ioctl(kms_fd, DRM_IOCTL_MODE_SETCRTC, &set)) {
				fprintf(stderr, "n72-ui: re-enabling layer 1: %s\n", strerror(errno));
				return false;
			}
		}
		kms_layer_on = true;
		kms_src_y = 0;
		kms_win_y = 0;
		kms_win_h = fb.height;
		if (handoff_n)
			kms_drop_handoff();
		if (!kms_can_window || rot ||
		    (fsy == kms_src_y && fwy == kms_win_y && fwh == kms_win_h))
			return true;
	}
	need_rot = kms_can_turn && want != kms_rot_cur;
	need_win = kms_can_window &&
		   (fsy != kms_src_y || fwy != kms_win_y || fwh != kms_win_h);
	if (need_rot || need_win) {
		uint32_t objs[1] = { kms_plane }, counts[1] = { 0 };
		uint32_t props[10];
		uint64_t vals[10];
		int n = 0;

		props[n] = kms_prop_fb;
		vals[n++] = pf.fb_id;
		if (need_rot) {
			props[n] = kms_prop_rot;
			vals[n++] = want;
		}
		if (kms_can_window) {
			props[n] = kms_prop_cx;
			vals[n++] = 0;
			props[n] = kms_prop_cy;
			vals[n++] = fwy;
			props[n] = kms_prop_cw;
			vals[n++] = fb.width;
			props[n] = kms_prop_ch;
			vals[n++] = fwh;
			props[n] = kms_prop_sx;
			vals[n++] = 0;
			props[n] = kms_prop_sy;
			vals[n++] = (uint64_t)fsy << 16;
			props[n] = kms_prop_sw;
			vals[n++] = (uint64_t)(rot ? fb.height : fb.width) << 16;
			props[n] = kms_prop_sh;
			vals[n++] = (uint64_t)(rot ? fb.width : fwh) << 16;
		} else {
			props[n] = kms_prop_sw;
			vals[n++] = (uint64_t)(rot ? fb.height : fb.width) << 16;
			props[n] = kms_prop_sh;
			vals[n++] = (uint64_t)(rot ? fb.width : fb.height) << 16;
		}
		counts[0] = n;
		{
			struct drm_mode_atomic a = {
				.flags = DRM_MODE_PAGE_FLIP_EVENT, .count_objs = 1,
				.objs_ptr = (uintptr_t)objs, .count_props_ptr = (uintptr_t)counts,
				.props_ptr = (uintptr_t)props, .prop_values_ptr = (uintptr_t)vals,
				.user_data = pf.user_data,
			};

			if (ioctl(kms_fd, DRM_IOCTL_MODE_ATOMIC, &a)) {
				fprintf(stderr, "n72-ui: atomic flip: %s\n", strerror(errno));
				return false;
			}
		}
		kms_rot_cur = want;
		if (kms_can_window) {
			kms_src_y = fsy;
			kms_win_y = fwy;
			kms_win_h = fwh;
		}
		goto wait;
	}
	for (tries = 0; tries < 50; tries++) {
		if (!ioctl(kms_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf))
			break;
		if (errno != EBUSY)
			return false;
		usleep(2000);		/* a flip still pending (not expected) */
	}
	if (tries == 50)
		return false;
wait:
	/*
	 * Its own event, for up to a second (only this thread waits; the
	 * renderer has its third buffer). Lost after that: taken as shown,
	 * and its event, if it ever comes, is dropped by the next wait.
	 */
	if (!kms_wait_flip(pf.user_data, 1000))
		fprintf(stderr, "n72-ui: flip %llu: no completion in 1 s\n",
			(unsigned long long)pf.user_data);
	if (handoff_n)
		kms_drop_handoff();	/* (the old picture is off the layer now) */
	return true;
}

static bool kms_import_scan(int fd, int i)
{
	struct drm_prime_handle ph = { 0 };
	struct drm_mode_fb_cmd2 f = { 0 };
	int dfd = mbx_export(mbx, scan_handle[i], 1);	/* PRIME from the render node */

	if (dfd < 0)
		return false;
	ph.fd = dfd;
	if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph)) {
		close(dfd);
		return false;
	}
	close(dfd);
	f.width = fb.width;
	f.height = fb.height;
	f.pixel_format = kms_shell ? 0x34325241 : 0x34325258;	/* AR24 : XR24 */
	f.handles[0] = ph.handle;
	f.pitches[0] = fb.stride;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f)) {
		struct drm_gem_close gc = { .handle = ph.handle };

		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		return false;
	}
	kms_fb[i] = f.fb_id;
	kms_handle[i] = ph.handle;
	if (kms_can_turn) {
		uint32_t ls = buf_width_to_stride(fb.height, LV_COLOR_FORMAT_XRGB8888);
		struct drm_mode_fb_cmd2 fl = { 0 };

		fl.width = fb.height;
		fl.height = fb.width;
		fl.pixel_format = kms_shell ? 0x34325241 : 0x34325258;
		fl.handles[0] = kms_handle[i];
		fl.pitches[0] = ls;
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fl))
			kms_can_turn = false;
		else
			kms_fb_l[i] = fl.fb_id;
	}
	return true;
}

static void kms_release_scan(int i)
{
	if (i <= 0 || i >= NSCAN)
		return;
	if (kms_fd >= 0) {
		if (kms_fb_l[i]) {
			ioctl(kms_fd, DRM_IOCTL_MODE_RMFB, &kms_fb_l[i]);
			kms_fb_l[i] = 0;
		}
		if (kms_fb[i]) {
			ioctl(kms_fd, DRM_IOCTL_MODE_RMFB, &kms_fb[i]);
			kms_fb[i] = 0;
		}
		if (kms_handle[i]) {
			struct drm_gem_close gc = { .handle = kms_handle[i] };

			ioctl(kms_fd, DRM_IOCTL_GEM_CLOSE, &gc);
			kms_handle[i] = 0;
		}
	}
	if (scan[i]) {
		region_remove(scan[i]);
		mbx_free(mbx, &scan_buf[i]);
		memset(&scan_buf[i], 0, sizeof(scan_buf[i]));
		scan[i] = NULL;
	}
	scan_handle[i] = 0;
	scan_ta_seq[i] = 0;
	buf_frame[i] = -1;
}

static bool kms_setup(bool shell_layer)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[2], conns[2], encs[2];
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo modes[4];
	struct drm_mode_crtc set = { 0 };
	int i, fd = kms_take_handoff();
	bool handed = fd >= 0;

	if (!handed)
		fd = kms_open(shell_layer);
	if (fd < 0)
		goto fail;
	ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_crtcs = res.count_connectors = res.count_encoders = 2;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs ||
	    !res.count_connectors)
		goto fail;
	/* asked without room for modes first: only that makes the kernel probe
	 * a connector nobody has looked at yet (layer 1 has no console) */
	conn.connector_id = conns[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes)
		goto fail;
	memset(&conn, 0, sizeof(conn));
	conn.connector_id = conns[0];
	conn.modes_ptr = (uintptr_t)modes;
	conn.count_modes = 4;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes)
		goto fail;
	kms_crtc = crtcs[0];
	kms_conn = conns[0];
	kms_mode = modes[0];
	/* probe plane & rotation before importing framebuffers so kms_import_scan creates kms_fb_l[] */
	if (kms_find_plane(fd)) {
		uint32_t ls = buf_width_to_stride(fb.height, LV_COLOR_FORMAT_XRGB8888);

		kms_can_turn = ls * fb.width <= fb.stride * fb.height;
		/* a start after a turned run: upright first (the layer is off) */
		if (kms_can_turn && !handed && kms_rot_cur != ROT_BITS(0))
			kms_set_rot(fd, ROT_BITS(0));
	}
	for (i = 0; i < NSCAN; i++) {
		if (!kms_import_scan(fd, i))
			goto fail;
	}
	if (handed) {
		/* the layer stays on the old picture until the first flip */
		kms_fd = fd;
		kms_layer_on = true;
	} else {
		set.crtc_id = kms_crtc;
		set.fb_id = kms_fb[0];
		set.mode = kms_mode;
		set.mode_valid = 1;
		set.set_connectors_ptr = (uintptr_t)&kms_conn;
		set.count_connectors = 1;
		if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set))
			goto fail;
		kms_fd = fd;
		kms_layer_on = true;
		kms_win_y = 0;
		kms_win_h = fb.height;
		kms_src_y = 0;
	}
	if (kms_shell && kms_prop_cx && kms_prop_cy && kms_prop_cw && kms_prop_ch &&
	    kms_prop_sx && kms_prop_sy && kms_prop_fb && kms_prop_sw && kms_prop_sh) {
		uint32_t objs[1] = { kms_plane }, counts[1] = { 9 };
		uint32_t props[9] = {
			kms_prop_fb, kms_prop_cx, kms_prop_cy, kms_prop_cw, kms_prop_ch,
			kms_prop_sx, kms_prop_sy, kms_prop_sw, kms_prop_sh,
		};
		uint64_t vals[9] = {
			kms_fb[0], 0, 16, fb.width, 4,
			0, (uint64_t)32 << 16, (uint64_t)fb.width << 16, (uint64_t)4 << 16,
		};
		struct drm_mode_atomic a = {
			.flags = DRM_MODE_ATOMIC_TEST_ONLY, .count_objs = 1,
			.objs_ptr = (uintptr_t)objs, .count_props_ptr = (uintptr_t)counts,
			.props_ptr = (uintptr_t)props, .prop_values_ptr = (uintptr_t)vals,
		};

		kms_can_window = !ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &a);
		if (kms_can_window) {
			struct drm_mode_create_blob cb = {
				.data = (uintptr_t)&kms_mode,
				.length = sizeof(kms_mode),
			};

			kms_prop_active = kms_obj_prop(fd, kms_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
			kms_prop_mode_id = kms_obj_prop(fd, kms_crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
			kms_prop_conn_crtc = kms_obj_prop(fd, kms_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
			if (!ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb))
				kms_mode_blob = cb.blob_id;
		}
	}
	return true;
fail:
	if (fd >= 0)
		close(fd);
	return false;
}

static uint8_t *alloc_scan_buf(uint32_t size, int i)
{
	uint8_t *p = region_add(size, MBX_ALLOC_CACHED | MBX_ALLOC_CONTIG,
				&scan_handle[i], &scan_buf[i]);
	if (!p) {
		/*
		 * Contiguous memory can run out by fragmentation after a
		 * long uptime: have the kernel compact memory, once, and
		 * try again (without flipping the screen is wrong).
		 */
		int fd = open("/proc/sys/vm/compact_memory", O_WRONLY | O_CLOEXEC);

		if (fd >= 0) {
			if (write(fd, "1", 1) < 0)
				;
			close(fd);
		}
		p = region_add(size, MBX_ALLOC_CACHED | MBX_ALLOC_CONTIG,
			       &scan_handle[i], &scan_buf[i]);
		if (p)
			fprintf(stderr, "n72-ui: contiguous memory after compaction\n");
	}
	if (p)
		scan_cpu_cached[i] = true;
	return p;
}

static bool flip_setup(uint32_t size, bool shell_layer)
{
	int i;

	/* the render node flips through KMS only (kms_setup below) */
#ifdef N72_DEBUG
	if (getenv("N72_NOFLIP"))
		return false;
#endif
	for (i = 0; i < NSCAN; i++) {
		scan[i] = alloc_scan_buf(size, i);
		if (!scan[i])
			return false;	/* (the regions stay mapped; unused) */
	}
	/*
	 * Start from what is on the screen, then show buffer 0. An application
	 * on layer 0 (!shell_layer) starts from black instead: fb.gpu_addr is
	 * the boot frame buffer, which holds stale text and would flash before
	 * the application's first frame.
	 */
	{
		uint32_t w[16];

		if (!shell_layer)
			submit(w, put_fill(w, gpu_addr(scan[0]), fb.stride, 0xff000000,
					   0, 0, fb.width, fb.height));
		else
			submit(w, put_copy(w, gpu_addr(scan[0]), fb.stride, fb.gpu_addr, fb.stride,
					   0, 0, 0, 0, fb.width, fb.height));
	}
	if (!kms_setup(shell_layer)) {
		fprintf(stderr, "n72-ui: no KMS for the render node: copying to the framebuffer\n");
		return false;
	}
	st_front = st_latest = 0;		/* frame 0: what was on the screen */
	buf_frame[0] = 0;
	return true;
}

lv_display_t *mbx_gpu_display_create(bool shell_layer)
{
	lv_draw_buf_handlers_t *h;
	lv_draw_unit_t *u;
	lv_display_t *disp;
	uint32_t size;

	mbx = mbx_open();		/* the render node */
	if (mbx < 0)
		return NULL;
	if (mbx_fb_info(mbx, &fb) || mbx_alloc(mbx, POOL_SIZE, MBX_ALLOC_CACHED, &pool_buf))
		goto fail;
	pool = pool_buf.cpu;
	pool_gpu = pool_buf.gpu;
	regions[nregions++] = (struct region){ pool, POOL_SIZE, pool_buf.gpu, pool_buf.handle };
	blocks[0].off = 0;
	blocks[0].size = POOL_SIZE;
	nblocks = 1;

	h = lv_draw_buf_get_handlers();
	h->buf_malloc_cb = buf_malloc;
	h->buf_free_cb = buf_free;
	h->invalidate_cache_cb = buf_invalidate;
	h->flush_cache_cb = buf_clean;
	h->width_to_stride_cb = buf_width_to_stride;

	u = lv_draw_create_unit(sizeof(lv_draw_unit_t));
	u->evaluate_cb = mbx_evaluate;
	u->dispatch_cb = mbx_dispatch;

	size = fb.stride * fb.height;
	disp = lv_display_create(fb.width, fb.height);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	flipping = flip_setup(size, shell_layer);
	if (flipping && (pipe2(notify_pipe, O_NONBLOCK | O_CLOEXEC) ||
			 pthread_create(&flip_thread, NULL, flip_main, NULL)))
		flipping = false;
	if (flipping) {
		lv_display_set_buffers(disp, scan[1], NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
		lv_display_add_event_cb(disp, render_start_cb, LV_EVENT_RENDER_START, NULL);
	} else {
		disp_buf = pool_alloc(size);
		if (!disp_buf)
			goto fail;
		lv_display_set_buffers(disp, disp_buf, NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
	}
	lv_display_set_flush_cb(disp, flush_cb);
	lv_display_add_event_cb(disp, refr_start_cb, LV_EVENT_REFR_START, NULL);
	fprintf(stderr, "n72-ui: %s; MBX GPU (%s):", kms_fd < 0 ? "no KMS" :
		kms_shell ? "KMS (n72-clcd-shell, layer 1)" : "KMS (n72-clcd)",
		"DRM render node");
	fprintf(stderr, "  framebuffer at GPU %08x, %u KiB draw pool at %08x, sync jobs, %s\n",
	       fb.gpu_addr, POOL_SIZE / 1024, pool_gpu,
	       flipping ? "page flipping at vblank" : "copy to the framebuffer");
	return disp;

fail:
	perror("n72-ui: MBX GPU");
	close(mbx);
	mbx = -1;
	return NULL;
}

/*
 * A copy of what is on the screen now (the display buffer), made by the
 * GPU: the "before" picture of a transition without rendering it again.
 */
lv_draw_buf_t *mbx_gpu_grab_screen(void)
{
	lv_draw_buf_t *b;
	lv_area_t all;
	uint32_t w[16];

	if (mbx < 0)
		return NULL;
	{
		uint32_t gw = rot_deg ? fb.height : fb.width, gh = rot_deg ? fb.width : fb.height;

		b = lv_draw_buf_create(gw, gh, LV_COLOR_FORMAT_XRGB8888, 0);
		if (!b)
			return NULL;
		if (!in_pool(b->data) || (b->header.stride & 31) || (gpu_addr(b->data) & 127)) {
			lv_draw_buf_destroy(b);
			return NULL;
		}
		lv_area_set(&all, 0, 0, gw - 1, gh - 1);
	}
	/*
	 * No wait for the display: the newest finished frame (st_latest) is
	 * complete once its flush is (flush_flip waits for the GPU), and only
	 * this thread draws, so nothing writes it meanwhile. Waiting for it to
	 * reach the screen cost up to a frame at every slide's start.
	 */
	buf_invalidate(b, &all);
	{
		/* the 3D copy: ~3 ms a screen against the 2D engine's ~8 */
		uint32_t w3[48];
		/* turned: lbuf has the newest frame, upright */
		uint32_t src = gpu_addr(rot_deg && !hw_turn ? lbuf : flipping ? scan[st_latest] : disp_buf);
		uint32_t ss = draw_stride();
		int n = mbx_cmd_copy(w3, 0, b->header.stride, ss, 0, 0, 0, 0,
				     b->header.w, b->header.h, LV_OPA_COVER);

		if (blit3d(8, gpu_addr(b->data), src, w3, n)) {
			submit(w, put_copy(w, gpu_addr(b->data), b->header.stride, src,
					   ss, 0, 0, 0, 0, b->header.w, b->header.h));
		}
	}
	return b;
}

/*
 * An opaque copy of an ARGB8888 picture over a plain colour, made by the
 * GPU: fill, then a blended 3D copy. NULL if it cannot be done.
 */
lv_draw_buf_t *mbx_gpu_flatten(const lv_draw_buf_t *src, lv_color_t bg)
{
	uint32_t w[48];
	lv_draw_buf_t *b;
	lv_area_t all;
	int n;

	if (mbx < 0 || !in_pool(src->data) || (src->header.stride & 31) ||
	    (gpu_addr(src->data) & 127))
		return NULL;
	b = lv_draw_buf_create(src->header.w, src->header.h, LV_COLOR_FORMAT_XRGB8888, 0);
	if (!b)
		return NULL;
	if (!in_pool(b->data) || (b->header.stride & 31) || (gpu_addr(b->data) & 127)) {
		lv_draw_buf_destroy(b);
		return NULL;
	}
	lv_area_set(&all, 0, 0, b->header.w - 1, b->header.h - 1);
	buf_invalidate(b, &all);
	submit(w, put_fill(w, gpu_addr(b->data), b->header.stride,
			   0xff000000 | lv_color_to_u32(bg), 0, 0, b->header.w, b->header.h));
	n = mbx_cmd_copy(w, 1, b->header.stride, src->header.stride, 0, 0, 0, 0,
			 b->header.w, b->header.h, LV_OPA_COVER);
	mbx_cmd_copy_blend(w, MBX_COPY_ALPHA);
	if (blit3d(9, gpu_addr(b->data), gpu_addr(src->data), w, n)) {
		lv_draw_buf_destroy(b);
		return NULL;
	}
	return b;
}

bool mbx_gpu_on_shell_layer(void)
{
	return kms_fd >= 0 && kms_shell;
}

/*
 * Transparent mode, for what the launcher shows over an application. The
 * display is rendered as ARGB8888: LVGL then clears every area it redraws
 * and its software renderer keeps the alpha of what it draws (edges,
 * shadows, fading), where the usual XRGB8888 rendering would leave an
 * opaque box around every rounded or shadowed thing. The GPU paths want an
 * opaque target and step aside, so everything is the CPU's: fine for a bar
 * or a pop-up. Each finished area is then turned from LVGL's straight
 * alpha into the premultiplied alpha the display's layer 1 blends
 * (flush_flip).
 */
extern bool transparent;

void mbx_gpu_set_transparent(bool on)
{
	int i;

	if (on == transparent || !mbx_gpu_on_shell_layer())
		return;
	if (on && flipping) {
		uint32_t size = fb.stride * fb.height;

		ta_wait_active();
		flip_wait();
		for (i = 0; i < scan_count; i++)
			if (scan[i])
				sync_range(scan[i], size, MBX_SYNC_FLUSH);
	}
	transparent = on;
	lv_display_set_color_format(lv_display_get_default(),
				    on ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_XRGB8888);
}

/* Straight alpha to premultiplied; nothing but zeros where alpha is 0. */
static void premultiply_span(uint32_t *p, int32_t w);

static void premultiply(uint8_t *px_map, const lv_area_t *a)
{
	int32_t y;

	for (y = a->y1; y <= a->y2; y++) {
		uint32_t *row = (uint32_t *)(px_map + y * draw_stride());
		int32_t x = a->x1;

		while (x <= a->x2) {
			int32_t end = a->x2, i;
			bool skip = false;

			/* the next span: inside one skip area, or up to the next */
			for (i = 0; i < pm_skip_n; i++) {
				const lv_area_t *k = &pm_skip[i];

				if (y < k->y1 || y > k->y2 || k->x2 < x)
					continue;
				if (k->x1 <= x) {
					skip = true;
					end = LV_MIN(a->x2, k->x2);
					break;
				}
				end = LV_MIN(end, k->x1 - 1);
			}
			if (!skip)
				premultiply_span(row + x, end - x + 1);
			x = end + 1;
		}
	}
}

/* Straight alpha to premultiplied over @w pixels; runs of one colour reuse
 * the last result; SWAR 2-multiply rb + g channel rounding without division. */
static void premultiply_span(uint32_t *p, int32_t w)
{
	uint32_t last_in = 0, last_out = 0;
	int32_t x;

	for (x = 0; x < w; x++) {
		uint32_t v = p[x], al;

		if (v == 0 || v >= 0xff000000u)
			continue;
		al = v >> 24;
		if (!al) {
			p[x] = 0;
			continue;
		}
		if (v != last_in) {
			uint32_t rb = (v & 0x00ff00ffu) * al + 0x00800080u;
			uint32_t g = ((v & 0x0000ff00u) >> 8) * al + 128u;

			rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
			g = (g + (g >> 8)) & 0x0000ff00u;
			last_in = v;
			last_out = (al << 24) | rb | g;
		}
		p[x] = last_out;
	}
}

/* The pixels on screen now (stride fb.stride), for screenshots. */
const uint8_t *mbx_gpu_front(uint32_t *stride)
{
	const uint8_t *p;

	*stride = fb.stride;
	ta_wait_active();
	if (flipping)
		flip_wait();
	p = flipping ? scan[st_latest >= 0 && scan[st_latest] ? st_latest : 0] : disp_buf;
	if (flipping)
		mbx_kms_note_cpu_touch(p);
	return p;
}

/* Debugging the flip buffers: both scan-out buffers (NULL if not flipping). */
const uint8_t *mbx_gpu_scan(int i, int *is_front)
{
	if (!flipping || i < 0 || i >= NSCAN || !scan[i])
		return NULL;
	ta_wait_active();
	flip_wait();
	*is_front = i == st_front;
	scan_cpu_cached[i] = true;
	return scan[i];
}

/*
 * Turn the screen: 0, 90 or 270 (LVGL's rotation; LVGL turns the pointer).
 * Only with page flipping. The screen must be redrawn whole afterwards
 * (the caller's new layout does that). False if it cannot turn.
 */
bool mbx_gpu_set_rotation(int deg)
{
	lv_display_t *d = lv_display_get_default();
	uint32_t size = fb.stride * fb.height;
	int i;

	if (deg == rot_deg)
		return true;
	if (!flipping || mbx < 0 || (deg != 0 && deg != 90 && deg != 270))
		return false;
	if (hw_slide_active)
		mbx_gpu_hw_slide_abort(NULL);
	if (deg)
		lstride = buf_width_to_stride(fb.height, LV_COLOR_FORMAT_XRGB8888);
	if (deg && !kms_can_turn && !lbuf) {
		lbuf = pool_alloc(lstride * fb.width);
		if (!lbuf)
			return false;
	}
	ta_wait_active();
	flip_wait();
	for (i = 0; i < NSCAN; i++)
		scan_ta_seq[i] = 0;
	/* the GPU writes the scan-out buffers now (or the CPU again): no
	 * cache lines of the other way may be written back over them */
	for (i = 0; i < NSCAN; i++)
		if (scan[i])
			sync_range(scan[i], size, MBX_SYNC_FLUSH);
	if (lbuf)
		sync_range(lbuf, lstride * fb.width, MBX_SYNC_FLUSH);
	hw_turn = kms_can_turn;		/* the display turns it, else the GPU */
	rot_deg = deg;
	lv_display_set_rotation(d, deg == 90 ? LV_DISPLAY_ROTATION_90 :
			        deg == 270 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0);
	lv_display_set_buffers(d, deg && !hw_turn ? lbuf :
			       scan[st_latest >= 0 && scan[st_latest] ? st_latest : 0], NULL,
			       deg ? lstride * fb.width : size, LV_DISPLAY_RENDER_MODE_DIRECT);
	pthread_mutex_lock(&flip_mx);
	for (i = 0; i < NSCAN; i++)
		buf_frame[i] = -1;	/* every buffer: the whole screen anew */
	pthread_mutex_unlock(&flip_mx);
	lv_obj_invalidate(lv_screen_active());
	return true;
}

void mbx_gpu_set_layer_enabled(bool on)
{
	int i;

	if (!flipping || !kms_shell || kms_fd < 0)
		return;
	if (on) {
		kms_layer_want = true;
		return;
	}
	kms_layer_want = false;
	if (hw_slide_active)
		mbx_gpu_hw_slide_abort(NULL);
	ta_wait_active();
	flip_wait();
	if (kms_layer_on) {
		struct drm_mode_crtc set = { .crtc_id = kms_crtc };

		if (!ioctl(kms_fd, DRM_IOCTL_MODE_SETCRTC, &set))
			kms_layer_on = false;
	}
	pthread_mutex_lock(&flip_mx);
	st_front = -1;
	st_latest = -1;
	for (i = 0; i < NSCAN; i++)
		buf_frame[i] = -1;
	scan_zero_transparent();
	pthread_mutex_unlock(&flip_mx);
}

void mbx_gpu_suspend(void)
{
	lv_display_t *d;
	int i;

	if (mbx < 0 || !flipping || !kms_shell || gpu_suspended)
		return;
	mbx_gpu_set_layer_enabled(false);
	ta_ring_free();
	for (i = 2; i < NSCAN; i++)
		kms_release_scan(i);
	pthread_mutex_lock(&flip_mx);
	scan_count = scan[1] ? 2 : 1;
	st_front = -1;
	st_latest = -1;
	for (i = 0; i < NSCAN; i++)
		if (!transparent || i >= scan_count || (rot_deg && !hw_turn))
			buf_frame[i] = -1;
	pthread_mutex_unlock(&flip_mx);
	if (lbuf && (!rot_deg || hw_turn)) {
		pool_free(lbuf);
		lbuf = NULL;
	}
	if (pool_is_empty())
		pool_release();
	d = lv_display_get_default();
	if (d && d->buf_1)
		d->buf_1->data = (rot_deg && !hw_turn && lbuf) ? lbuf : scan[0];
	gpu_suspended = true;
}

void mbx_gpu_resume(void)
{
	uint32_t size = fb.stride * fb.height;
	bool had[NSCAN] = { false };
	bool ok = true;
	int i;

	if (!gpu_suspended) {
		mbx_gpu_set_layer_enabled(true);
		return;
	}
	if (hw_slide_active)
		mbx_gpu_hw_slide_abort(NULL);
	ta_wait_active();
	flip_wait();
	if (!pool_reacquire())
		fprintf(stderr, "n72-ui: pool reacquire failed; continuing with CPU fallback\n");
	for (i = 1; i < NSCAN; i++) {
		had[i] = scan[i] != NULL;
		if (had[i])
			continue;
		scan[i] = alloc_scan_buf(size, i);
		if (!scan[i] || !kms_import_scan(kms_fd, i)) {
			ok = false;
			break;
		}
	}
	if (!ok) {
		for (i = 1; i < NSCAN; i++)
			if (!had[i])
				kms_release_scan(i);
		pthread_mutex_lock(&flip_mx);
		scan_count = scan[1] ? 2 : 1;
		pthread_mutex_unlock(&flip_mx);
		fprintf(stderr, "n72-ui: scan[2] reacquire failed; double-buffered\n");
	} else {
		pthread_mutex_lock(&flip_mx);
		scan_count = NSCAN;
		pthread_mutex_unlock(&flip_mx);
	}
	pthread_mutex_lock(&flip_mx);
	for (i = 0; i < NSCAN; i++)
		buf_frame[i] = -1;
	pthread_mutex_unlock(&flip_mx);
	gpu_suspended = false;
	mbx_gpu_set_layer_enabled(true);
}

bool mbx_gpu_is_suspended(void)
{
	return gpu_suspended;
}

/* LVGL draws into the scan-out buffers themselves: upright, or turned by the display */
bool mbx_gpu_draws_direct(void)
{
	return !rot_deg || hw_turn;
}

/*
 * @src (XRGB8888, in GPU memory) turned a quarter, 90 or 270 as LVGL turns
 * the screen, into a new buffer of the swapped size: one 3D copy. NULL if
 * it cannot be done.
 */
lv_draw_buf_t *mbx_gpu_turn_buf(const lv_draw_buf_t *src, int deg)
{
	lv_draw_buf_t *b;
	uint32_t cw[48];
	int n;

	if (mbx < 0 || !src || (deg != 90 && deg != 270) || !in_pool(src->data))
		return NULL;
	b = lv_draw_buf_create(src->header.h, src->header.w, LV_COLOR_FORMAT_XRGB8888, 0);
	if (!b)
		return NULL;
	if (!in_pool(b->data) || (b->header.stride & 31) || (gpu_addr(b->data) & 127)) {
		lv_draw_buf_destroy(b);
		return NULL;
	}
	n = mbx_cmd_copy_turn(cw, b->header.stride, src->header.stride, 0, 0,
			      src->header.w, src->header.h, deg, src->header.w, src->header.h);
	sync_range(b->data, b->header.stride * b->header.h, MBX_SYNC_FLUSH);
	if (blit3d(8, gpu_addr(b->data), gpu_addr(src->data), cw, n)) {
		lv_draw_buf_destroy(b);
		return NULL;
	}
	return b;
}

/*
 * @b (ARGB8888, straight alpha) premultiplied in place, cleaned if in GPU
 * memory, and flagged so: the transparent display copies it as it is.
 */
void mbx_gpu_premultiply_buf(lv_draw_buf_t *b)
{
	lv_area_t all;
	uint32_t y;

	if (!b || b->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
	    (b->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED))
		return;
	for (y = 0; y < b->header.h; y++)
		premultiply_span((uint32_t *)(b->data + y * b->header.stride), b->header.w);
	if (in_pool(b->data)) {
		lv_area_set(&all, 0, 0, b->header.w - 1, b->header.h - 1);
		buf_clean(b, &all);
	}
	b->header.flags |= LV_IMAGE_FLAGS_PREMULTIPLIED;
}

static bool hw_slide_solo_check(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *roots[3];
	int r;

	if (scr && lv_obj_get_style_bg_opa(scr, LV_PART_MAIN) > LV_OPA_TRANSP)
		return false;
	roots[0] = scr;
	roots[1] = lv_layer_top();
	roots[2] = lv_layer_sys();
	for (r = 0; r < 3; r++) {
		uint32_t n, i;

		if (!roots[r])
			continue;
		n = lv_obj_get_child_count(roots[r]);
		for (i = 0; i < n; i++) {
			lv_obj_t *c = lv_obj_get_child(roots[r], (int32_t)i);

			if (c && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
				return false;
		}
	}
	return true;
}

bool mbx_gpu_hw_slide_begin(const lv_draw_buf_t *pic, int32_t x, int32_t init_y,
			    void *owner, mbx_hw_slide_abort_cb abort_cb)
{
	lv_display_t *d = lv_display_get_default();
	uint32_t size = fb.stride * fb.height;
	int32_t pw, ph, sx0, dx0, cw, y;
	int sfb, bfb;

	if (!flipping || !kms_shell || !kms_can_window || !transparent || rot_deg ||
	    scan_count < 2 || !scan[0] || !scan[1] || hw_slide_active)
		return false;
	if (!pic || pic->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
	    !(pic->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) ||
	    pic->header.h == 0 || pic->header.h > fb.height)
		return false;
	if (!hw_slide_solo_check())
		return false;

	ta_wait_active();
	flip_wait();

	sfb = (st_front == 0) ? 1 : 0;
	bfb = 1 - sfb;

	pw = (int32_t)pic->header.w;
	ph = (int32_t)pic->header.h;
	sx0 = x < 0 ? -x : 0;
	dx0 = x < 0 ? 0 : x;
	cw = pw - sx0;
	if (dx0 + cw > (int32_t)fb.width)
		cw = (int32_t)fb.width - dx0;
	if (dx0 != 0 || cw != (int32_t)fb.width)
		memset(scan[sfb], 0, size);
	else if (ph < (int32_t)fb.height)
		memset(scan[sfb] + (size_t)ph * fb.stride, 0,
		       (size_t)(fb.height - ph) * fb.stride);
	if (cw > 0) {
		size_t bytes = (size_t)cw * 4;
		uint8_t *dp = scan[sfb] + dx0 * 4;
		const uint8_t *sp = pic->data + sx0 * 4;

		for (y = 0; y < ph; y++) {
			memcpy(dp, sp, bytes);
			dp += fb.stride;
			sp += pic->header.stride;
		}
	}
	sync_range(scan[sfb], size, MBX_SYNC_CLEAN);

	hw_slide_blank_zeroed = false;
	if (st_front != bfb) {
		memset(scan[bfb], 0, size);
		sync_range(scan[bfb], size, MBX_SYNC_CLEAN);
		hw_slide_blank_zeroed = true;
	}

	pthread_mutex_lock(&flip_mx);
	buf_frame[sfb] = -1;
	if (hw_slide_blank_zeroed)
		buf_frame[bfb] = -1;
	pthread_mutex_unlock(&flip_mx);

	if (d) {
		d->inv_p = 0;
		memset(d->inv_area_joined, 0, sizeof(d->inv_area_joined));
	}

	hw_slide_fb = sfb;
	hw_slide_blank_fb = bfb;
	hw_slide_active = true;
	hw_slide_h = ph;
	hw_slide_last_y = 0x7fffffff;
	hw_slide_owner = owner;
	hw_slide_abort_fn = abort_cb;
	mbx_gpu_hw_slide_move(init_y);
	return true;
}

void mbx_gpu_hw_slide_move(int32_t py)
{
	int32_t dst_y1, dst_y2;
	int fb_idx;
	uint32_t sy, wy, wh;

	if (!hw_slide_active || py == hw_slide_last_y)
		return;
	hw_slide_last_y = py;
	dst_y1 = py > 0 ? py : 0;
	dst_y2 = py + hw_slide_h - 1;
	if (dst_y2 >= (int32_t)fb.height)
		dst_y2 = (int32_t)fb.height - 1;
	if (dst_y1 > dst_y2) {
		int bfb = hw_slide_blank_fb;

		if (!hw_slide_blank_zeroed) {
			bool busy;

			pthread_mutex_lock(&flip_mx);
			busy = (st_front == bfb || st_latching == bfb || st_ready == bfb);
			pthread_mutex_unlock(&flip_mx);
			if (busy)
				return;
			memset(scan[bfb], 0, fb.stride * fb.height);
			sync_range(scan[bfb], fb.stride * fb.height, MBX_SYNC_CLEAN);
			hw_slide_blank_zeroed = true;
		}
		fb_idx = bfb;
		sy = 0;
		wy = 0;
		wh = 16;
	} else {
		fb_idx = hw_slide_fb;
		sy = (uint32_t)(dst_y1 - py);
		wy = (uint32_t)dst_y1;
		wh = (uint32_t)(dst_y2 - dst_y1 + 1);
	}
	pthread_mutex_lock(&flip_mx);
	buf_rot[fb_idx] = 0;
	buf_src_y[fb_idx] = sy;
	buf_win_y[fb_idx] = wy;
	buf_win_h[fb_idx] = wh;
	st_ready = fb_idx;
	pthread_cond_signal(&flip_cv);
	pthread_mutex_unlock(&flip_mx);
	n72_frame_shown();
}

void mbx_gpu_hw_slide_end(bool keep_visible)
{
	uint32_t size = fb.stride * fb.height;
	int sfb, bfb;
	bool bzero;

	if (!hw_slide_active)
		return;
	flip_wait();
	sfb = hw_slide_fb;
	bfb = hw_slide_blank_fb;
	bzero = hw_slide_blank_zeroed;
	hw_slide_active = false;
	hw_slide_owner = NULL;
	hw_slide_abort_fn = NULL;
	hw_slide_fb = -1;
	hw_slide_blank_fb = -1;
	hw_slide_blank_zeroed = false;

	if (keep_visible && sfb >= 0 && st_front == sfb) {
		if (bfb >= 0 && scan[bfb] && !bzero) {
			memset(scan[bfb], 0, size);
			sync_range(scan[bfb], size, MBX_SYNC_CLEAN);
		}
	} else {
		if (!keep_visible && sfb >= 0 && st_front == sfb && bfb >= 0 && scan[bfb]) {
			if (!bzero) {
				memset(scan[bfb], 0, size);
				sync_range(scan[bfb], size, MBX_SYNC_CLEAN);
				bzero = true;
			}
			pthread_mutex_lock(&flip_mx);
			buf_rot[bfb] = 0;
			buf_src_y[bfb] = 0;
			buf_win_y[bfb] = 0;
			buf_win_h[bfb] = 16;
			st_ready = bfb;
			pthread_cond_signal(&flip_cv);
			pthread_mutex_unlock(&flip_mx);
			flip_wait();
		} else if (!keep_visible && bfb >= 0 && st_front == bfb && !bzero &&
			   sfb >= 0 && scan[sfb]) {
			memset(scan[sfb], 0, size);
			sync_range(scan[sfb], size, MBX_SYNC_CLEAN);
			pthread_mutex_lock(&flip_mx);
			buf_rot[sfb] = 0;
			buf_src_y[sfb] = 0;
			buf_win_y[sfb] = 0;
			buf_win_h[sfb] = 16;
			st_ready = sfb;
			pthread_cond_signal(&flip_cv);
			pthread_mutex_unlock(&flip_mx);
			flip_wait();
		}
		if (sfb >= 0 && scan[sfb] && st_front != sfb) {
			memset(scan[sfb], 0, size);
			sync_range(scan[sfb], size, MBX_SYNC_CLEAN);
		}
		if (bfb >= 0 && scan[bfb] && st_front != bfb && !bzero) {
			memset(scan[bfb], 0, size);
			sync_range(scan[bfb], size, MBX_SYNC_CLEAN);
		}
	}
	pthread_mutex_lock(&flip_mx);
	st_latest = -1;
	if (sfb >= 0)
		buf_frame[sfb] = -1;
	if (bfb >= 0)
		buf_frame[bfb] = -1;
	pthread_mutex_unlock(&flip_mx);
}

void mbx_gpu_hw_slide_abort(void *except_owner)
{
	void *owner;
	mbx_hw_slide_abort_cb cb;

	if (!hw_slide_active || hw_slide_owner == except_owner)
		return;
	owner = hw_slide_owner;
	cb = hw_slide_abort_fn;
	if (cb)
		cb(owner);
	else
		mbx_gpu_hw_slide_end(true);
}
