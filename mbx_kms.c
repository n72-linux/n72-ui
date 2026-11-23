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
static uint32_t scan_handle[NSCAN];
static uint32_t flush_words[16 * 64];
static int flush_n;
static bool flipping;

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
static bool kms_flip(int r);

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
		int r;

		while (st_ready < 0)
			pthread_cond_wait(&flip_cv, &flip_mx);
		r = st_ready;
		st_ready = -1;
		st_latching = r;
		pthread_mutex_unlock(&flip_mx);
		if (notify_pipe[1] >= 0) {	/* the main loop may draw again */
			char c = 1;

			if (write(notify_pipe[1], &c, 1) < 0)
				;
		}
		t0 = stat_t0();
		/* flipping implies KMS (flip_setup): waits for the flip's own event */
		bool shown = kms_flip(r);

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

/* GPU-copy @a from buffer @src into @dst (batched; flushed by sync_flush()) */
static void sync_rect(int dst, int src, const lv_area_t *a)
{
	lv_area_t c;
	lv_area_t all = { 0, 0, scan_w() - 1, scan_h() - 1 };
	uint32_t ss = scan_stride();

	if (!lv_area_intersect(&c, a, &all))
		return;
	sync_range(scan[dst] + c.y1 * ss, lv_area_get_height(&c) * ss, MBX_SYNC_FLUSH);
	if (flush_n > (int)(sizeof(flush_words) / 4) - 16) {
		submit(flush_words, flush_n);
		flush_n = 0;
	}
	flush_n += put_copy(flush_words + flush_n, gpu_addr(scan[dst]), ss,
			    gpu_addr(scan[src]), ss, c.x1, c.y1, c.x1, c.y1,
			    lv_area_get_width(&c), lv_area_get_height(&c));
}

static void render_start_cb(lv_event_t *e)
{
	lv_display_t *d = lv_event_get_target(e);
	lv_area_t all = { 0, 0, scan_w() - 1, scan_h() - 1 };
	int i, k, dr = -1, src;
	long have, f;

	pthread_mutex_lock(&flip_mx);
	for (i = 0; i < NSCAN; i++)
		if (i != st_front && i != st_latching && i != st_ready)
			break;
	if (i < NSCAN) {
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
	st_drawing = dr;
	src = st_latest;
	pthread_mutex_unlock(&flip_mx);

	/* bring the buffer up to date: frames buf_frame[dr]+1 .. frame_no */
	double dbg_t0 = stat_t0();
	have = buf_frame[dr];
	if (src >= 0 && src != dr && have < frame_no) {
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
		if (flush_n) {
			submit(flush_words, flush_n);
			flush_n = 0;
		}
	}
	d->buf_1->data = rot_deg && !hw_turn ? lbuf : scan[dr];
	cur_n = 0;
	cur_all = false;
	pm_skip_n = 0;
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

	n72_trace("flush", lv_area_get_height(area));
	if (transparent)
		premultiply(px_map, area);
	dbg_t1 = stat_t0();
	/* what the CPU drew in this area goes out to memory for the display */
	clean_rows(px_map, area);
#ifdef N72_DEBUG
	if (mbx_gpu_timing && gpu_now_us() - dbg_t0 > 8000 && getenv("N72_FPS"))
		fprintf(stderr, "flush: premultiply %.1f ms, clean %.1f ms (%d rows)\n",
			(dbg_t1 - dbg_t0) / 1000.0, (gpu_now_us() - dbg_t1) / 1000.0,
			(int)lv_area_get_height(area));
#else
	(void)dbg_t0;
	(void)dbg_t1;
#endif
	if (rot_deg && !hw_turn) {
		uint32_t cw[48];
		lv_area_t pa;
		int n = mbx_cmd_copy_turn(cw, fb.stride, lstride, area->x1, area->y1,
					  lv_area_get_width(area), lv_area_get_height(area),
					  rot_deg, fb.height, fb.width);

		to_panel(area, &pa);
		if (blit3d(8, gpu_addr(scan[st_drawing]), gpu_addr(lbuf), cw, n))
			fprintf(stderr, "n72-ui: turned copy failed\n");
		if (cur_n < HIST_AREAS)
			cur_areas[cur_n++] = pa;
		else
			cur_all = true;
	} else if (cur_n < HIST_AREAS)
		cur_areas[cur_n++] = *area;
	else
		cur_all = true;
	if (lv_display_flush_is_last(disp)) {
		long n = frame_no + 1;
		int k = n % HIST;
#ifdef N72_DEBUG
		static int alphacheck = -1;	/* N72_ALPHACHECK, read once */

		if (alphacheck < 0)
			alphacheck = getenv("N72_ALPHACHECK") != NULL;
		if (!transparent && alphacheck) {
			/* an opaque frame must have alpha 255 everywhere */
			uint32_t x, y, bad = 0, fx = 0, fy = 0;

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
		memcpy(hist[k].a, cur_areas, sizeof(cur_areas));
		hist[k].n = cur_n;
		hist[k].all = cur_all;
		hist[k].frame = n;
		pthread_mutex_lock(&flip_mx);
		frame_no = n;
		buf_frame[st_drawing] = n;
		buf_rot[st_drawing] = hw_turn ? rot_deg : 0;
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

/* Before the CPU draws the next frame into disp_buf. */
static void refr_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	n72_refr_start();
}

/* Map one more buffer object as a region. */
static uint8_t *region_add(uint32_t size, uint32_t flags, uint32_t *handle)
{
	struct mbx_buf b;

	if (nregions == 4 || mbx_alloc(mbx, size, flags, &b))
		return NULL;
	regions[nregions++] = (struct region){ b.cpu, size, b.gpu, b.handle };
	if (handle)
		*handle = b.handle;
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
static uint32_t kms_fb[NSCAN], kms_crtc;
static uint32_t kms_handle[NSCAN];	/* the scan buffers' GEM handles on kms_fd */
static bool kms_shell;

/*
 * A turned screen by the display (kernel n72-clcd: the plane's "rotation"):
 * the scan-out buffers hold the upright picture, landscape, as frame
 * buffers of their own (kms_fb_l), and a flip that changes the turn is an
 * atomic commit of the buffer and the rotation together.
 */
static uint32_t kms_fb_l[NSCAN];
static uint32_t kms_plane, kms_prop_rot, kms_prop_fb, kms_prop_sw, kms_prop_sh;
static uint64_t kms_rot_cur;		/* the plane's rotation (DRM_MODE_ROTATE_*) */

#define ROT_BITS(deg) ((deg) == 90 ? 2u : (deg) == 270 ? 8u : 1u)	/* DRM_MODE_ROTATE_* */

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
		uint32_t rot = 0, fbp = 0, sw = 0, sh = 0;
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
		kms_prop_sw = sw;
		kms_prop_sh = sh;
		kms_plane = planes[i];
		kms_prop_rot = rot;
		kms_prop_fb = fbp;
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
static bool kms_shell;		/* on the display's layer 1, over the applications */

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

/* The flip to buffer @r, waited for: true once it is on the screen. */
static bool kms_flip(int r)
{
	uint64_t want = ROT_BITS(buf_rot[r]);
	struct drm_mode_crtc_page_flip pf = {
		.crtc_id = kms_crtc, .fb_id = buf_rot[r] ? kms_fb_l[r] : kms_fb[r],
		.flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = ++kms_seq,
	};
	int tries;

	if (kms_can_turn && want != kms_rot_cur) {
		/* the turn changes: the buffer, its size and the rotation in one
		 * commit (the source rectangle must fit the new buffer) */
		uint32_t objs[1] = { kms_plane }, counts[1] = { 4 };
		uint32_t props[4] = { kms_prop_fb, kms_prop_rot, kms_prop_sw, kms_prop_sh };
		uint64_t vals[4] = {
			pf.fb_id, want,
			(uint64_t)(buf_rot[r] ? fb.height : fb.width) << 16,
			(uint64_t)(buf_rot[r] ? fb.width : fb.height) << 16,
		};
		struct drm_mode_atomic a = {
			.flags = DRM_MODE_PAGE_FLIP_EVENT, .count_objs = 1,
			.objs_ptr = (uintptr_t)objs, .count_props_ptr = (uintptr_t)counts,
			.props_ptr = (uintptr_t)props, .prop_values_ptr = (uintptr_t)vals,
			.user_data = pf.user_data,
		};

		if (ioctl(kms_fd, DRM_IOCTL_MODE_ATOMIC, &a)) {
			fprintf(stderr, "n72-ui: turning the layer: %s\n", strerror(errno));
			return false;
		}
		kms_rot_cur = want;
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
	for (i = 0; i < NSCAN; i++) {
		struct drm_prime_handle ph = { 0 };
		struct drm_mode_fb_cmd2 f = { 0 };
		int dfd = mbx_export(mbx, scan_handle[i], 1);	/* PRIME from the render node */

		if (dfd < 0)
			goto fail;
		ph.fd = dfd;
		if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph)) {
			close(dfd);
			goto fail;
		}
		close(dfd);
		f.width = fb.width;
		f.height = fb.height;
		f.pixel_format = kms_shell ? 0x34325241 : 0x34325258;	/* AR24 : XR24 */
		f.handles[0] = ph.handle;
		f.pitches[0] = fb.stride;
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f))
			goto fail;
		kms_fb[i] = f.fb_id;
		kms_handle[i] = ph.handle;
	}
	kms_crtc = crtcs[0];
	/* the same buffers as landscape pictures, for the display to turn */
	if (kms_find_plane(fd)) {
		uint32_t ls = buf_width_to_stride(fb.height, LV_COLOR_FORMAT_XRGB8888);

		kms_can_turn = ls * fb.width <= fb.stride * fb.height;
		for (i = 0; i < NSCAN && kms_can_turn; i++) {
			struct drm_mode_fb_cmd2 f = { 0 };

			f.width = fb.height;
			f.height = fb.width;
			f.pixel_format = kms_shell ? 0x34325241 : 0x34325258;
			f.handles[0] = kms_handle[i];
			f.pitches[0] = ls;
			if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f))
				kms_can_turn = false;
			kms_fb_l[i] = f.fb_id;
		}
		/* a start after a turned run: upright first (the layer is off) */
		if (kms_can_turn && !handed && kms_rot_cur != ROT_BITS(0))
			kms_set_rot(fd, ROT_BITS(0));
	}
	if (handed) {
		/* the layer stays on the old picture until the first flip */
		kms_fd = fd;
		return true;
	}
	set.crtc_id = kms_crtc;
	set.fb_id = kms_fb[0];
	set.mode = modes[0];
	set.mode_valid = 1;
	set.set_connectors_ptr = (uintptr_t)conns;
	set.count_connectors = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set))
		goto fail;
	kms_fd = fd;
	return true;
fail:
	if (fd >= 0)
		close(fd);
	return false;
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
		scan[i] = region_add(size, MBX_ALLOC_CACHED | MBX_ALLOC_CONTIG, &scan_handle[i]);
		if (!scan[i]) {
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
			scan[i] = region_add(size, MBX_ALLOC_CACHED | MBX_ALLOC_CONTIG,
					     &scan_handle[i]);
			if (scan[i])
				fprintf(stderr, "n72-ui: contiguous memory after compaction\n");
		}
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
	struct mbx_buf a;
	lv_draw_buf_handlers_t *h;
	lv_draw_unit_t *u;
	lv_display_t *disp;
	uint32_t size;

	mbx = mbx_open();		/* the render node */
	if (mbx < 0)
		return NULL;
	if (mbx_fb_info(mbx, &fb) || mbx_alloc(mbx, POOL_SIZE, MBX_ALLOC_CACHED, &a))
		goto fail;
	pool = a.cpu;
	pool_gpu = a.gpu;
	regions[nregions++] = (struct region){ pool, POOL_SIZE, a.gpu, a.handle };
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
	if (on == transparent || !mbx_gpu_on_shell_layer())
		return;
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
 * the last result; c * a / 255 rounded without a division. */
static void premultiply_span(uint32_t *p, int32_t w)
{
#define MUL255(c, al) ({ uint32_t t_ = (c) * (al) + 128; (t_ + (t_ >> 8)) >> 8; })
	uint32_t last_in = 0, last_out = 0;
	int32_t x;

	for (x = 0; x < w; x++) {
		uint32_t v = p[x], al = v >> 24;

		if (al == 255)
			continue;
		if (!al) {
			if (v)
				p[x] = 0;
			continue;
		}
		if (v != last_in) {
			last_in = v;
			last_out = al << 24 | MUL255(v >> 16 & 255, al) << 16 |
				   MUL255(v >> 8 & 255, al) << 8 | MUL255(v & 255, al);
		}
		p[x] = last_out;
	}
#undef MUL255
}

/* The pixels on screen now (stride fb.stride), for screenshots. */
const uint8_t *mbx_gpu_front(uint32_t *stride)
{
	*stride = fb.stride;
	if (flipping)
		flip_wait();
	return flipping ? scan[st_latest] : disp_buf;
}

/* Debugging the flip buffers: both scan-out buffers (NULL if not flipping). */
const uint8_t *mbx_gpu_scan(int i, int *is_front)
{
	if (!flipping || i < 0 || i >= NSCAN)
		return NULL;
	flip_wait();
	*is_front = i == st_front;
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
	if (deg && !lbuf) {
		lstride = buf_width_to_stride(fb.height, LV_COLOR_FORMAT_XRGB8888);
		lbuf = pool_alloc(lstride * fb.width);
		if (!lbuf)
			return false;
	}
	flip_wait();
	/* the GPU writes the scan-out buffers now (or the CPU again): no
	 * cache lines of the other way may be written back over them */
	for (i = 0; i < NSCAN; i++)
		sync_range(scan[i], size, MBX_SYNC_FLUSH);
	if (lbuf)
		sync_range(lbuf, lstride * fb.width, MBX_SYNC_FLUSH);
	hw_turn = kms_can_turn;		/* the display turns it, else the GPU */
	rot_deg = deg;
	lv_display_set_rotation(d, deg == 90 ? LV_DISPLAY_ROTATION_90 :
			        deg == 270 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0);
	lv_display_set_buffers(d, deg && !hw_turn ? lbuf : scan[st_latest >= 0 ? st_latest : 0], NULL,
			       deg ? lstride * fb.width : size, LV_DISPLAY_RENDER_MODE_DIRECT);
	pthread_mutex_lock(&flip_mx);
	for (i = 0; i < NSCAN; i++)
		buf_frame[i] = -1;	/* every buffer: the whole screen anew */
	pthread_mutex_unlock(&flip_mx);
	lv_obj_invalidate(lv_screen_active());
	return true;
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
 * @b (ARGB8888, straight alpha) premultiplied in place, written out for
 * the GPU, and flagged so: the transparent display copies it as it is.
 */
void mbx_gpu_premultiply_buf(lv_draw_buf_t *b)
{
	lv_area_t all;
	uint32_t y;

	if (!b || b->header.cf != LV_COLOR_FORMAT_ARGB8888)
		return;
	for (y = 0; y < b->header.h; y++)
		premultiply_span((uint32_t *)(b->data + y * b->header.stride), b->header.w);
	lv_area_set(&all, 0, 0, b->header.w - 1, b->header.h - 1);
	if (in_pool(b->data))
		buf_clean(b, &all);
	b->header.flags |= LV_IMAGE_FLAGS_PREMULTIPLIED;
}
