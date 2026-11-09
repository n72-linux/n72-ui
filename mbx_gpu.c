// SPDX-License-Identifier: MIT
/*
 * LVGL on the iPod touch 2G's PowerVR MBX Lite (2D engine, /dev/mbx).
 *
 * - All LVGL draw buffers come from one GPU-mapped, CPU-cached pool, so the
 *   GPU can target any of them; cache maintenance goes through MBX_IOC_SYNC.
 * - A draw unit takes plain fills (no radius, no gradient): opaque ones on
 *   the 2D engine, translucent ones blended by the 3D core; and layer
 *   composites (opacity layers) as 3D textured copies, blended per pixel.
 * - The display renders into a full-screen pool buffer ("direct" mode) and
 *   the GPU copies the refreshed areas to the framebuffer.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/apple-mbx.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/lvgl_private.h"
#include "mbx_gpu.h"
#include "mbx_cmd.h"

void n72_frame_shown(void);
void n72_refr_start(void);

#define DRAW_UNIT_ID_MBX	5
#define POOL_SIZE		(6 * 1024 * 1024)	/* display, layers, snapshots */
#define POOL_ALIGN		128	/* the 3D core needs 128-byte aligned surfaces */
#define POOL_BLOCKS		256

static int mbx = -1;
static struct mbx_fb_info fb;
static uint32_t pool_handle, pool_gpu;
static uint8_t *pool;

struct mbx_gpu_stats mbx_gpu_stats;

/* ---------------------------------------------------- pool allocator */

/* First-fit over a small table of blocks, kept sorted by offset. */
static struct {
	uint32_t off, size;
	bool used;
} blocks[POOL_BLOCKS];
static int nblocks;

static void *pool_alloc(size_t size)
{
	int i;

	size = (size + POOL_ALIGN - 1) & ~(size_t)(POOL_ALIGN - 1);
	for (i = 0; i < nblocks; i++) {
		if (blocks[i].used || blocks[i].size < size)
			continue;
		if (blocks[i].size > size && nblocks < POOL_BLOCKS) {
			memmove(&blocks[i + 2], &blocks[i + 1],
				(nblocks - i - 1) * sizeof(blocks[0]));
			blocks[i + 1].off = blocks[i].off + size;
			blocks[i + 1].size = blocks[i].size - size;
			blocks[i + 1].used = false;
			blocks[i].size = size;
			nblocks++;
		}
		blocks[i].used = true;
		return pool + blocks[i].off;
	}
	return NULL;
}

static void pool_free(void *p)
{
	uint32_t off = (uint8_t *)p - pool;
	int i;

	for (i = 0; i < nblocks; i++)
		if (blocks[i].off == off)
			break;
	if (i == nblocks)
		return;
	blocks[i].used = false;
	if (i + 1 < nblocks && !blocks[i + 1].used) {	/* merge right */
		blocks[i].size += blocks[i + 1].size;
		memmove(&blocks[i + 1], &blocks[i + 2], (nblocks - i - 2) * sizeof(blocks[0]));
		nblocks--;
	}
	if (i > 0 && !blocks[i - 1].used) {		/* merge left */
		blocks[i - 1].size += blocks[i].size;
		memmove(&blocks[i], &blocks[i + 1], (nblocks - i - 1) * sizeof(blocks[0]));
		nblocks--;
	}
}

static bool in_pool(const void *p)
{
	return p && (const uint8_t *)p >= pool && (const uint8_t *)p < pool + POOL_SIZE;
}

static uint32_t gpu_addr(const void *p)
{
	return pool_gpu + ((const uint8_t *)p - pool);
}

/* ---------------------------------------------------------- the GPU */

static double gpu_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void sync_range(const void *p, uint32_t size, uint32_t op)
{
	double t0 = gpu_now_us();
	struct mbx_sync s = {
		.handle = pool_handle, .op = op,
		.offset = (const uint8_t *)p - pool, .size = size,
	};

	ioctl(mbx, MBX_IOC_SYNC, &s);
	mbx_gpu_stats.sync_us += gpu_now_us() - t0;
	mbx_gpu_stats.sync_bytes += size;
}

/*
 * Asynchronous jobs (kernel MBX_IOC_SET_ASYNC): an async submit returns its
 * sequence number; the job's completion is polled with MBX_IOC_WAIT.
 */
static int gpu_async;
static uint32_t last_seq;		/* of the last job started */

static int gpu_ret(int ret)
{
	if (ret > 0) {
		last_seq = ret;
		return 0;
	}
	return ret;
}

static void gpu_wait(uint32_t seq)
{
	struct mbx_wait w = { .wait_seq = seq };

	if (gpu_async && seq)
		ioctl(mbx, MBX_IOC_WAIT, &w);
}

static int gpu_done(uint32_t seq)
{
	struct mbx_wait w = { 0 };

	if (ioctl(mbx, MBX_IOC_WAIT, &w))
		return 1;
	return (int32_t)(w.seq_done - seq) >= 0;
}

static int submit(const uint32_t *w, int n)
{
	struct mbx_submit s = { .words = (uintptr_t)w, .count = n };

	double t0 = gpu_now_us();
	int ret;

	mbx_gpu_stats.submits++;
	ret = gpu_ret(ioctl(mbx, MBX_IOC_SUBMIT, &s));
	mbx_gpu_stats.submit_us += gpu_now_us() - t0;
	return ret;
}

static int put_tail(uint32_t *w, int n)
{
	int i;

	for (i = 0; i < 6; i++)
		w[n++] = 0x70000000;
	return n;
}

static int put_fill(uint32_t *w, uint32_t dst, uint32_t stride, uint32_t color,
		    int x, int y, int width, int height)
{
	int n = 0;

	w[n++] = 0xa0000000 | MBX_FMT_ARGB8888 | (stride & 0x7fff);
	w[n++] = dst;
	w[n++] = 0x94000000 | MBX_FMT_ARGB8888 | (stride & 0x7fff);
	w[n++] = 0;
	w[n++] = 0x30000000;
	w[n++] = 0x60800200;
	w[n++] = 0x8000f0f0;			/* ROP 0xf0: pattern (colour) copy */
	w[n++] = color;
	w[n++] = ((uint32_t)x << 16 & 0x1fff0000) | (y & 0x1fff);
	w[n++] = ((uint32_t)(x + width) << 16 & 0x1fff0000) | ((y + height) & 0x1fff);
	return put_tail(w, n);
}

static int put_copy(uint32_t *w, uint32_t dst, uint32_t dstride, uint32_t src,
		    uint32_t sstride, int sx, int sy, int dx, int dy, int width,
		    int height)
{
	int n = 0;

	w[n++] = 0xa0000000 | MBX_FMT_ARGB8888 | (dstride & 0x7fff);
	w[n++] = dst;
	w[n++] = 0x94000000 | MBX_FMT_ARGB8888 | (sstride & 0x7fff);
	w[n++] = src;
	w[n++] = 0x30000000 | ((uint32_t)sx << 14 & 0x7ffc000) | (sy & 0x1fff);
	w[n++] = 0x60800200;			/* 1:1 */
	w[n++] = 0x8000cccc;			/* ROP 0xcc: source copy */
	w[n++] = 0xffffffff;
	w[n++] = ((uint32_t)dx << 16 & 0x1fff0000) | (dy & 0x1fff);
	w[n++] = ((uint32_t)(dx + width) << 16 & 0x1fff0000) | ((dy + height) & 0x1fff);
	return put_tail(w, n);
}

static int blit3d(int type, uint32_t dst, uint32_t src, const uint32_t *w, int n)
{
	struct mbx_blit3d b = { .type = type, .dst_gpu = dst, .src_gpu = src, .count = n };

	double t0 = gpu_now_us();
	int ret;

	memcpy(b.words, w, n * 4);
	mbx_gpu_stats.submits++;
	ret = gpu_ret(ioctl(mbx, MBX_IOC_BLIT3D, &b));
	mbx_gpu_stats.blit3d_us += gpu_now_us() - t0;
	return ret;
}

/* ------------------------------------------------- LVGL draw buffers */

static void *buf_malloc(size_t size, lv_color_format_t cf)
{
	void *p;

	LV_UNUSED(cf);
	p = pool_alloc(size);
	if (!p) {
		mbx_gpu_stats.pool_misses++;
		p = lv_malloc(size);		/* CPU-only fallback */
	}
	return p;
}

/* The 3D core needs strides in multiples of 8 pixels (32 bytes). */
static uint32_t buf_width_to_stride(uint32_t w, lv_color_format_t cf)
{
	return (w * lv_color_format_get_bpp(cf) / 8 + 31) & ~31u;
}

static void buf_free(void *p)
{
	if (in_pool(p))
		pool_free(p);
	else
		lv_free(p);
}

static void buf_sync_area(const lv_draw_buf_t *b, const lv_area_t *a, uint32_t op)
{
	uint32_t stride = b->header.stride;

	if (!in_pool(b->data))
		return;
	sync_range(b->data + a->y1 * stride, (lv_area_get_height(a)) * stride, op);
}

static void buf_invalidate(const lv_draw_buf_t *b, const lv_area_t *a)
{
	buf_sync_area(b, a, MBX_SYNC_FLUSH);
}

static void buf_clean(const lv_draw_buf_t *b, const lv_area_t *a)
{
	buf_sync_area(b, a, MBX_SYNC_CLEAN);
}

/* --------------------------------------------------- the draw unit */

static bool cf_ok(lv_color_format_t cf)
{
	return cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888;
}

/*
 * The 3D core blends into XRGB8888 targets (the display buffer); LVGL's own
 * formula for ARGB8888 layers keeps destination alpha, which it does not.
 */
static bool layer_ok(const lv_draw_task_t *t, const lv_layer_t *src)
{
	const lv_draw_image_dsc_t *d = t->draw_dsc;

	return cf_ok(src->color_format) && d->rotation == 0 &&
	       d->scale_x == LV_SCALE_NONE && d->scale_y == LV_SCALE_NONE &&
	       d->skew_x == 0 && d->skew_y == 0 && d->recolor_opa <= LV_OPA_MIN &&
	       d->blend_mode == LV_BLEND_MODE_NORMAL && d->clip_radius == 0 &&
	       !d->bitmap_mask_src;
}

/*
 * An image the GPU can copy: a variable-source image (an lv_draw_buf_t, as
 * lv_snapshot_take() makes) whose pixels live in the GPU pool, drawn 1:1.
 */
static bool image_ok(const lv_draw_task_t *t)
{
	const lv_draw_image_dsc_t *d = t->draw_dsc;
	const lv_image_dsc_t *img = d->src;

	if (lv_image_src_get_type(d->src) != LV_IMAGE_SRC_VARIABLE || !img->data ||
	    !in_pool(img->data) || ((uintptr_t)img->data - (uintptr_t)pool) & 127 ||
	    (img->header.stride & 31) || d->tile || !cf_ok(img->header.cf))
		return false;
	if (lv_area_get_width(&d->image_area) != img->header.w ||
	    lv_area_get_height(&d->image_area) != img->header.h)
		return false;
	return d->rotation == 0 && d->scale_x == LV_SCALE_NONE &&
	       d->scale_y == LV_SCALE_NONE && d->skew_x == 0 && d->skew_y == 0 &&
	       d->recolor_opa <= LV_OPA_MIN && d->blend_mode == LV_BLEND_MODE_NORMAL &&
	       d->clip_radius == 0 && !d->bitmap_mask_src;
}

static int32_t mbx_evaluate(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	const lv_draw_dsc_base_t *base = t->draw_dsc;
	lv_color_format_t dcf = base->layer->color_format;
	int32_t size = lv_area_get_size(&t->area);

	LV_UNUSED(u);
	if (t->type < 16)
		mbx_gpu_stats.seen[t->type]++;
	if (!cf_ok(dcf))
		return 0;

	if (t->type == LV_DRAW_TASK_TYPE_FILL) {
		const lv_draw_fill_dsc_t *d = t->draw_dsc;

		/*
		 * Rounded opaque fills: the GPU fills the body, the CPU only
		 * the corner squares (mbx_fill). Translucent rounded ones stay
		 * on the CPU.
		 */
		if (d->radius != 0 && d->opa < LV_OPA_MAX)
			mbx_gpu_stats.fill_radius++;
		else if (d->grad.dir != LV_GRAD_DIR_NONE)
			mbx_gpu_stats.fill_grad++;
		else if (size < 8192)
			mbx_gpu_stats.fill_small++;
		if ((d->radius != 0 && d->opa < LV_OPA_MAX) ||
		    d->grad.dir != LV_GRAD_DIR_NONE || d->opa <= LV_OPA_MIN)
			return 0;
		/*
		 * A GPU job costs ioctls and an interrupt (~0.5-0.7 ms): only large
		 * areas are worth it. Opaque fills use the 2D engine, translucent
		 * ones the 3D core (XRGB8888 targets only).
		 */
		if (d->opa >= LV_OPA_MAX ? size < 16384 : size < 8192 ||
		    dcf != LV_COLOR_FORMAT_XRGB8888)
			return 0;
	} else if (t->type == LV_DRAW_TASK_TYPE_LAYER) {
		const lv_draw_image_dsc_t *d = t->draw_dsc;

		if (dcf != LV_COLOR_FORMAT_XRGB8888 || size < 4096 ||
		    !layer_ok(t, (const lv_layer_t *)d->src))
			return 0;
	} else if (t->type == LV_DRAW_TASK_TYPE_IMAGE) {
		if (dcf != LV_COLOR_FORMAT_XRGB8888 || size < 4096 || !image_ok(t))
			return 0;
	} else {
		return 0;
	}
	if (t->preference_score > 50) {
		t->preference_score = 50;
		t->preferred_draw_unit_id = DRAW_UNIT_ID_MBX;
	}
	return 1;
}

/* One opaque rectangle (in screen coordinates) on the 2D engine. */
static void gpu_rect(lv_layer_t *layer, const lv_area_t *clip, const lv_area_t *r,
		     lv_color_t color)
{
	lv_draw_buf_t *b = layer->draw_buf;
	uint32_t w[16];
	lv_area_t a;

	if (!lv_area_intersect(&a, r, clip))
		return;
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	buf_invalidate(b, &a);			/* drop CPU lines over the area */
	submit(w, put_fill(w, gpu_addr(b->data), b->header.stride,
			   0xff000000 | lv_color_to_u32(color),
			   a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a)));
	mbx_gpu_stats.fills++;
}

static void mbx_fill(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_draw_fill_dsc_t *d = t->draw_dsc;
	const lv_area_t *clip = u->clip_area, *c = &t->area;
	int32_t wd = lv_area_get_width(c), ht = lv_area_get_height(c);
	int32_t r = LV_MIN(d->radius, LV_MIN(wd, ht) / 2);
	lv_area_t a, band, corner, cclip;
	int i;

	if (!lv_area_intersect(&a, c, clip))
		return;
	if (!in_pool(b->data)) {
		lv_draw_sw_fill(u, d, c);
		return;
	}
	if (r <= 0) {
		gpu_rect(layer, clip, c, d->color);
		return;
	}
	/*
	 * Rounded: the GPU fills a cross (full-width middle band, and the
	 * top and bottom bands between the corners); then, once it is done,
	 * the CPU draws the r x r corner squares, clipped to each corner.
	 */
	band = (lv_area_t){ c->x1, c->y1 + r, c->x2, c->y2 - r };
	if (band.y2 >= band.y1)
		gpu_rect(layer, clip, &band, d->color);
	band = (lv_area_t){ c->x1 + r, c->y1, c->x2 - r, c->y1 + r - 1 };
	if (band.x2 >= band.x1) {
		gpu_rect(layer, clip, &band, d->color);
		band.y1 = c->y2 - r + 1;
		band.y2 = c->y2;
		gpu_rect(layer, clip, &band, d->color);
	}
	gpu_wait(last_seq);
	last_seq = 0;
	for (i = 0; i < 4; i++) {
		corner.x1 = i & 1 ? c->x2 - r + 1 : c->x1;
		corner.x2 = corner.x1 + r - 1;
		corner.y1 = i & 2 ? c->y2 - r + 1 : c->y1;
		corner.y2 = corner.y1 + r - 1;
		if (!lv_area_intersect(&cclip, &corner, clip))
			continue;
		u->clip_area = &cclip;
		lv_draw_sw_fill(u, d, c);
	}
	u->clip_area = clip;
	mbx_gpu_stats.fill_corners++;
}

/* Blend a translucent plain fill with the 3D core. */
static void mbx_fill_blend(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_draw_fill_dsc_t *d = t->draw_dsc;
	uint32_t w[48];
	lv_area_t a;

	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return;
	if (!in_pool(b->data) || (b->header.stride & 31)) {
		lv_draw_sw_fill(u, d, &t->area);
		return;
	}
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	buf_invalidate(b, &a);
	if (blit3d(7, gpu_addr(b->data), 0, w,
		   mbx_cmd_fill(w, 1, b->header.stride, a.x1, a.y1, a.x2 + 1, a.y2 + 1,
				(uint32_t)d->opa << 24 | (lv_color_to_u32(d->color) & 0xffffff))))
		lv_draw_sw_fill(u, d, &t->area);	/* GPU refused: draw it here */
	mbx_gpu_stats.blends++;
}

/*
 * Copy @sb (whose pixel (0,0) is at screen @sx,@sy) onto the target layer
 * over t->area: a 3D textured copy, blended per pixel. False: not possible
 * here, the caller draws it in software.
 */
static bool gpu_copy(lv_draw_unit_t *u, lv_draw_task_t *t, lv_draw_buf_t *sb,
		     int32_t sx, int32_t sy, lv_opa_t opa, bool clean_src)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_area_t a, sa, da;
	uint32_t w[48];
	int blend, n, ret;

	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return true;
	if (!in_pool(b->data) || !in_pool(sb->data) || (b->header.stride & 31) ||
	    (sb->header.stride & 31) || ((uintptr_t)sb->data - (uintptr_t)pool) & 127)
		return false;
	sa = a;
	lv_area_move(&sa, -sx, -sy);
	da = a;
	lv_area_move(&da, -layer->buf_area.x1, -layer->buf_area.y1);
	if (clean_src)
		buf_clean(sb, &sa);		/* the CPU drew the layer */
	buf_invalidate(b, &da);
	/*
	 * Opaque XRGB layer: plain copy. Full opacity: one blended pass with
	 * per-pixel alpha. Lower opacity: two passes (the hardware's destination
	 * factor ignores the opacity in a single pass).
	 */
	blend = !(opa >= LV_OPA_MAX && sb->header.cf == LV_COLOR_FORMAT_XRGB8888);
	n = mbx_cmd_copy(w, blend, b->header.stride, sb->header.stride, sa.x1, sa.y1,
			 da.x1, da.y1, lv_area_get_width(&a), lv_area_get_height(&a), opa);
	if (!blend) {
		/* (the 3D copy: ~3 ms a screen, the 2D engine's takes ~8) */
		ret = blit3d(8, gpu_addr(b->data), gpu_addr(sb->data), w, n);
	} else if (opa >= LV_OPA_MAX) {
		mbx_cmd_copy_blend(w, MBX_COPY_ALPHA);
		ret = blit3d(9, gpu_addr(b->data), gpu_addr(sb->data), w, n);
	} else {
		mbx_cmd_copy_blend(w, MBX_COPY_PASS1);
		ret = blit3d(9, gpu_addr(b->data), gpu_addr(sb->data), w, n);
		mbx_cmd_copy_blend(w, MBX_COPY_PASS2);
		ret = ret ?: blit3d(9, gpu_addr(b->data), gpu_addr(sb->data), w, n);
	}
	mbx_gpu_stats.layers++;
	return !ret;
}

/* Composite a layer onto its parent. */
static void mbx_layer(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_draw_image_dsc_t *d = t->draw_dsc;
	lv_layer_t *src = (lv_layer_t *)d->src;

	if (!src->draw_buf)
		return;
	if (!gpu_copy(u, t, src->draw_buf, src->buf_area.x1, src->buf_area.y1, d->opa, true))
		lv_draw_sw_layer(u, d, &t->area);	/* software fallback */
}

/* Draw a pool-resident image 1:1 (a snapshot, during transitions). */
static void mbx_image(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_draw_image_dsc_t *d = t->draw_dsc;

	/* the CPU does not write snapshots after taking them: no clean */
	if (!gpu_copy(u, t, (lv_draw_buf_t *)d->src, d->image_area.x1, d->image_area.y1,
		      d->opa, false))
		lv_draw_sw_image(u, d, &t->area);
	mbx_gpu_stats.images++;
}

/*
 * A task may finish asynchronously only if the CPU cannot share a cache line
 * with its target area while the GPU writes it: the area's left and right
 * edges on 32-byte boundaries (8 pixels at 32 bpp).
 */
static bool async_ok(const lv_draw_task_t *t)
{
	return gpu_async && !(t->area.x1 & 7) && !((t->area.x2 + 1) & 7);
}

static int async_dbg = -1;
#define ADBG(...) do { if (async_dbg) fprintf(stderr, __VA_ARGS__); } while (0)
static lv_draw_task_t *inflight;
static uint32_t inflight_seq;

static int32_t mbx_dispatch(lv_draw_unit_t *u, lv_layer_t *layer)
{
	lv_draw_task_t *t;

	if (async_dbg < 0)
		async_dbg = getenv("N72_GPU_ADBG") != NULL;
	if (inflight) {
		static unsigned long spins;

		if (!gpu_done(inflight_seq)) {
			if (spins < 3)
				ADBG("mbx: poll %lu, seq %u still running\n", spins, inflight_seq);
			if (++spins % 200000 == 0) {
				struct mbx_wait w = { 0 };

				ioctl(mbx, MBX_IOC_WAIT, &w);
				fprintf(stderr, "mbx: waiting on seq %u: submitted %u done %u (task type %d state %d)\n",
					inflight_seq, w.seq_submitted, w.seq_done,
					inflight->type, inflight->state);
			}
			/*
			 * Still running; the CPU draws on. Ask for another
			 * dispatch: without an OS, LVGL waits for one to poll again.
			 */
			lv_draw_dispatch_request();
			return 0;
		}
		spins = 0;
		ADBG("mbx: seq %u done, task %p ready\n", inflight_seq, (void *)inflight);
		inflight->state = LV_DRAW_TASK_STATE_READY;
		inflight = NULL;
		mbx_gpu_stats.async_done++;
		lv_draw_dispatch_request();
	}
	t = lv_draw_get_next_available_task(layer, NULL, DRAW_UNIT_ID_MBX);
	if (!t || t->preferred_draw_unit_id != DRAW_UNIT_ID_MBX)
		return LV_DRAW_UNIT_IDLE;
	if (!lv_draw_layer_alloc_buf(layer))
		return LV_DRAW_UNIT_IDLE;

	t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
	u->target_layer = layer;
	u->clip_area = &t->clip_area;
	if (t->type == LV_DRAW_TASK_TYPE_LAYER)
		mbx_layer(u, t);
	else if (t->type == LV_DRAW_TASK_TYPE_IMAGE)
		mbx_image(u, t);
	else if (((lv_draw_fill_dsc_t *)t->draw_dsc)->opa >= LV_OPA_MAX)
		mbx_fill(u, t);
	else
		mbx_fill_blend(u, t);
	ADBG("mbx: dispatch on layer %p\n", (void *)layer);
	ADBG("mbx: task %p type %d area %d,%d-%d,%d seq %u %s\n", (void *)t, t->type,
	     (int)t->area.x1, (int)t->area.y1, (int)t->area.x2, (int)t->area.y2, last_seq,
	     last_seq && async_ok(t) ? "async" : "sync");
	if (last_seq && async_ok(t)) {
		inflight = t;			/* finished in a later dispatch */
		inflight_seq = last_seq;
		last_seq = 0;
		lv_draw_dispatch_request();	/* so LVGL comes back to poll it */
		return 1;
	}
	gpu_wait(last_seq);			/* synchronous after all */
	last_seq = 0;
	t->state = LV_DRAW_TASK_STATE_READY;
	lv_draw_dispatch_request();
	return 1;
}

/* -------------------------------------------------------- display */

static uint8_t *disp_buf;
static uint32_t flush_seq;		/* the last display copy */
static uint32_t flush_words[16 * 64];
static int flush_n;

static int flush3d = -1;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	uint32_t stride = fb.stride;

	LV_UNUSED(px_map);
	if (flush3d < 0)
		flush3d = getenv("N72_FLUSH3D") != NULL;	/* no faster: memory bound */
	sync_range(disp_buf + area->y1 * stride, lv_area_get_height(area) * stride,
		   MBX_SYNC_CLEAN);
	if (flush_n > (int)(sizeof(flush_words) / 4) - 16) {
		submit(flush_words, flush_n);
		gpu_wait(last_seq);
		last_seq = 0;
		flush_n = 0;
	}
	if (flush3d) {
		/*
		 * The 3D core copies a full screen in ~3 ms, the 2D engine in
		 * ~8: large areas go through it (one job per area).
		 */
		uint32_t cw[48];
		int n = mbx_cmd_copy(cw, 0, fb.stride, stride, area->x1, area->y1, area->x1,
				     area->y1, lv_area_get_width(area),
				     lv_area_get_height(area), LV_OPA_COVER);

		if (flush_n) {
			submit(flush_words, flush_n);
			gpu_wait(last_seq);
			last_seq = 0;
			flush_n = 0;
		}
		if (!blit3d(8, fb.gpu_addr, gpu_addr(disp_buf), cw, n)) {
			gpu_wait(last_seq);
			last_seq = 0;
			goto copied;
		}
	}
	flush_n += put_copy(flush_words + flush_n, fb.gpu_addr, fb.stride,
			    gpu_addr(disp_buf), stride, area->x1, area->y1,
			    area->x1, area->y1, lv_area_get_width(area),
			    lv_area_get_height(area));
copied:
	mbx_gpu_stats.flushes++;
	if (lv_display_flush_is_last(disp)) {
		if (flush_n) {
			submit(flush_words, flush_n);
			/* the next frame waits for it before drawing (refr_start_cb) */
			flush_seq = last_seq;
			last_seq = 0;
			flush_n = 0;
		}
		n72_frame_shown();
	}
	lv_display_flush_ready(disp);
}

/* Before the CPU draws the next frame into disp_buf: the copy must be done. */
static void refr_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ADBG("mbx: refresh start, waiting for flush seq %u\n", flush_seq);
	gpu_wait(flush_seq);
	flush_seq = 0;
	n72_refr_start();
}

lv_display_t *mbx_gpu_display_create(void)
{
	struct mbx_alloc a = { .size = POOL_SIZE, .flags = MBX_ALLOC_CACHED };
	lv_draw_buf_handlers_t *h;
	lv_draw_unit_t *u;
	lv_display_t *disp;
	uint32_t size;

	mbx = open("/dev/mbx", O_RDWR | O_CLOEXEC);
	if (mbx < 0)
		return NULL;
	{
		const char *e = getenv("N72_GPU_ASYNC");
		uint32_t on = 1;

		/*
		 * Opt-in (N72_GPU_ASYNC=1): correct (the home screen renders
		 * pixel-identical) but not faster yet, since most UI tasks
		 * overlap and the CPU has little to draw meanwhile. Older
		 * kernels have no MBX_IOC_SET_ASYNC: stay synchronous.
		 */
		if (e && !strcmp(e, "1") && !ioctl(mbx, MBX_IOC_SET_ASYNC, &on))
			gpu_async = 1;
	}
	if (ioctl(mbx, MBX_IOC_FB_INFO, &fb) || ioctl(mbx, MBX_IOC_ALLOC, &a))
		goto fail;
	pool = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mbx,
		    a.mmap_offset);
	if (pool == MAP_FAILED)
		goto fail;
	pool_handle = a.handle;
	pool_gpu = a.gpu_addr;
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
	disp_buf = pool_alloc(size);
	if (!disp_buf)
		goto fail;
	disp = lv_display_create(fb.width, fb.height);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, disp_buf, NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	lv_display_add_event_cb(disp, refr_start_cb, LV_EVENT_REFR_START, NULL);
	printf("n72-ui: MBX GPU: framebuffer at GPU %08x, %u KiB draw pool at %08x, %s jobs\n",
	       fb.gpu_addr, POOL_SIZE / 1024, pool_gpu, gpu_async ? "async" : "sync");
	return disp;

fail:
	perror("n72-ui: /dev/mbx");
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
	b = lv_draw_buf_create(fb.width, fb.height, LV_COLOR_FORMAT_XRGB8888, 0);
	if (!b)
		return NULL;
	if (!in_pool(b->data) || (b->header.stride & 31) ||
	    ((uintptr_t)b->data - (uintptr_t)pool) & 127) {
		lv_draw_buf_destroy(b);
		return NULL;
	}
	lv_area_set(&all, 0, 0, fb.width - 1, fb.height - 1);
	gpu_wait(flush_seq);		/* the last frame is complete */
	flush_seq = 0;
	buf_invalidate(b, &all);
	submit(w, put_copy(w, gpu_addr(b->data), b->header.stride, gpu_addr(disp_buf),
			   fb.stride, 0, 0, 0, 0, fb.width, fb.height));
	gpu_wait(last_seq);
	last_seq = 0;
	return b;
}
