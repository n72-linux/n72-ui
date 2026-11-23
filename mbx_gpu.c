#define _GNU_SOURCE	/* pipe2 */
// SPDX-License-Identifier: MIT
/*
 * LVGL on the iPod touch 2G's PowerVR MBX Lite (2D engine; the apple-mbx
 * DRM render node).
 *
 * - All LVGL draw buffers come from one GPU-mapped, CPU-cached pool, so the
 *   GPU can target any of them; cache maintenance goes through GEM_SYNC
 *   (mbx_clean/mbx_flush).
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
#include "../n72-gles/src/mbxmem.h"	/* the MBX device: the render node */

#include "lvgl/lvgl.h"
#include "lvgl/src/lvgl_private.h"
#include "mbx_gpu.h"
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <sched.h>
#include "lvgl/src/display/lv_display_private.h"
#include "mbx_cmd.h"

void n72_frame_shown(void);
void n72_refr_start(void);
void n72_trace(const char *what, long v);

#define DRAW_UNIT_ID_MBX	5
#define POOL_SIZE		(6 * 1024 * 1024)	/* display, layers, snapshots */
#define POOL_ALIGN		128	/* the 3D core needs 128-byte aligned surfaces */
#define POOL_BLOCKS		256

static int mbx = -1;
static struct mbx_fb_info fb;
static uint32_t pool_gpu;
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

/*
 * GPU-visible memory the CPU also maps: the pool, and with page flipping
 * the two scan-out buffers. Each region is one buffer object.
 */
static struct region {
	uint8_t *cpu;
	uint32_t size, gpu, handle;
} regions[4];
static int nregions;

static const struct region *region_of(const void *p)
{
	int i;

	for (i = 0; p && i < nregions; i++)
		if ((const uint8_t *)p >= regions[i].cpu &&
		    (const uint8_t *)p < regions[i].cpu + regions[i].size)
			return &regions[i];
	return NULL;
}

/* (the name stays: "memory the GPU can reach") */
static bool in_pool(const void *p)
{
	return region_of(p) != NULL;
}

static uint32_t gpu_addr(const void *p)
{
	const struct region *r = region_of(p);

	return r->gpu + ((const uint8_t *)p - r->cpu);
}

/* ---------------------------------------------------------- the GPU */

static double gpu_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

/* the statistics' timings: only when somebody reads them (mbx_gpu_timing) */
bool mbx_gpu_timing;

static inline double stat_t0(void)
{
	return mbx_gpu_timing ? gpu_now_us() : 0;
}

/* (t0 is 0 when the flag came on between the two readings: nothing to add) */
#define STAT_US(field, t0) \
	do { if (mbx_gpu_timing && (t0) != 0) mbx_gpu_stats.field += gpu_now_us() - (t0); } while (0)

static void sync_range(const void *p, uint32_t size, uint32_t op)
{
	double t0 = stat_t0();
	const struct region *r = region_of(p);
	struct mbx_buf b = { .cpu = r->cpu, .handle = r->handle, .size = r->size, .cached = 1 };
	uint32_t off = (const uint8_t *)p - r->cpu;

	if (op == MBX_SYNC_FLUSH)
		mbx_flush(mbx, &b, off, size);
	else
		mbx_clean(mbx, &b, off, size);
	STAT_US(sync_us, t0);
	mbx_gpu_stats.sync_bytes += size;
}

/*
 * Jobs run synchronously (the kernel's default, no mbx_set_async); a
 * job's ioctl may return its sequence number, which is success.
 */
static int gpu_ret(int ret)
{
	return ret > 0 ? 0 : ret;
}

static int submit(const uint32_t *w, int n)
{
	double t0 = stat_t0();
	int ret;

	mbx_gpu_stats.submits++;
	ret = gpu_ret(mbx_submit_2d(mbx, w, n, 0));
	STAT_US(submit_us, t0);
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

	double t0 = stat_t0();
	int ret;

	memcpy(b.words, w, n * 4);
	mbx_gpu_stats.submits++;
	ret = gpu_ret(mbx_blit_3d(mbx, &b));
	STAT_US(blit3d_us, t0);
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
	if (p && (uint8_t *)p >= pool && (uint8_t *)p < pool + POOL_SIZE)
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
	    !in_pool(img->data) || (gpu_addr(img->data) & 127) ||
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

/*
 * The opt-in scaled copy (mbx_gpu_image_dsc_scaled): a marked draw of an
 * opaque XRGB8888 pool picture whose destination differs in size from it.
 * image_ok() is untouched: it still rejects every draw that is not 1:1.
 */
static int scaled_tag;

void mbx_gpu_image_dsc_scaled(lv_draw_image_dsc_t *d)
{
	d->base.user_data = &scaled_tag;
}

static bool image_scaled_ok(const lv_draw_task_t *t)
{
	const lv_draw_image_dsc_t *d = t->draw_dsc;
	const lv_image_dsc_t *img = d->src;

	if (d->base.user_data != &scaled_tag)
		return false;
	if (lv_image_src_get_type(d->src) != LV_IMAGE_SRC_VARIABLE || !img->data ||
	    !in_pool(img->data) || (gpu_addr(img->data) & 127) ||
	    (img->header.stride & 31) || d->tile || img->header.cf != LV_COLOR_FORMAT_XRGB8888 ||
	    !img->header.w || !img->header.h)
		return false;
	if (lv_area_get_width(&d->image_area) == img->header.w &&
	    lv_area_get_height(&d->image_area) == img->header.h)
		return false;	/* 1:1: image_ok()'s plain copy */
	return d->opa >= LV_OPA_MAX && d->rotation == 0 && d->scale_x == LV_SCALE_NONE &&
	       d->scale_y == LV_SCALE_NONE && d->skew_x == 0 && d->skew_y == 0 &&
	       d->recolor_opa <= LV_OPA_MIN && d->blend_mode == LV_BLEND_MODE_NORMAL &&
	       d->clip_radius == 0 && !d->bitmap_mask_src;
}

/* Is nothing drawn before @t in its layer where @t draws? */
static bool first_over(const lv_draw_task_t *t)
{
	const lv_draw_dsc_base_t *base = t->draw_dsc;
	const lv_draw_task_t *u;
	lv_area_t a, x;

	if (!lv_area_intersect(&a, &t->area, &t->clip_area))
		return false;
	for (u = base->layer->draw_task_head; u && u != t; u = u->next)
		if (lv_area_intersect(&x, &u->area, &a) &&
		    lv_area_intersect(&x, &x, &u->clip_area))
			return false;
	return u == t;
}

static bool transparent;		/* (mbx_gpu_set_transparent) */

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

	if (t->type == LV_DRAW_TASK_TYPE_FILL && dcf == LV_COLOR_FORMAT_ARGB8888) {
		const lv_draw_fill_dsc_t *d = t->draw_dsc;

		/*
		 * The launcher's transparent mode (alpha, premultiplied at the
		 * flush). A plain fill is one 2D fill of its word: opaque ones
		 * anywhere; translucent ones only where nothing is drawn under
		 * them this frame (LVGL cleared it), as the card's fades: the
		 * straight (alpha, colour) is then exactly what LVGL's blend
		 * would leave, ~1.3 ms instead of ~15 ms of CPU blending.
		 */
		if (d->radius != 0 || d->grad.dir != LV_GRAD_DIR_NONE || d->opa <= LV_OPA_MIN ||
		    size < 16384 || (d->opa < LV_OPA_MAX && !first_over(t)))
			return 0;
	} else if (t->type == LV_DRAW_TASK_TYPE_FILL) {
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
		const lv_draw_image_dsc_t *d = t->draw_dsc;

		if (image_scaled_ok(t)) {
			/* (opt-in scaled copy: into the XRGB8888 display buffer only) */
			if (dcf != LV_COLOR_FORMAT_XRGB8888)
				return 0;
		} else if (size < 4096 || !image_ok(t))
			return 0;
		/*
		 * Transparent mode: a premultiplied picture at full opacity
		 * (the shade over an application) is copied as it is, and the
		 * flush leaves its area alone (mbx_image_premul).
		 */
		if (dcf == LV_COLOR_FORMAT_ARGB8888 &&
		    (!transparent || d->opa < LV_OPA_MAX ||
		     !(((const lv_image_dsc_t *)d->src)->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED)))
			return 0;
		if (dcf != LV_COLOR_FORMAT_XRGB8888 && dcf != LV_COLOR_FORMAT_ARGB8888)
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

/*
 * Opaque rectangles (in screen coordinates) on the 2D engine, as one job:
 * rect_add() queues them, rect_flush() drops the CPU's lines over their
 * rows and submits the lot. A job is an ioctl, an interrupt and a wake-up,
 * so three bands of a rounded fill or the pieces of a trimmed background
 * are much cheaper together.
 */
#define RECT_BATCH	16
static uint32_t rect_words[RECT_BATCH * 16];
static int rect_n;
static lv_area_t rect_rows;		/* union, in buffer coordinates */
static lv_draw_buf_t *rect_buf;

static void rect_flush(void)
{
	if (!rect_n)
		return;
	buf_invalidate(rect_buf, &rect_rows);	/* drop CPU lines over the area */
	submit(rect_words, rect_n);
	rect_n = 0;
}

static void rect_add(lv_layer_t *layer, const lv_area_t *clip, const lv_area_t *r,
		     lv_color_t color)
{
	lv_draw_buf_t *b = layer->draw_buf;
	lv_area_t a;

	if (!lv_area_intersect(&a, r, clip))
		return;
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	if (rect_n && (rect_buf != b || rect_n > (int)(sizeof(rect_words) / 4) - 16))
		rect_flush();
	if (!rect_n) {
		rect_buf = b;
		rect_rows = a;
	} else {
		rect_rows.y1 = LV_MIN(rect_rows.y1, a.y1);
		rect_rows.y2 = LV_MAX(rect_rows.y2, a.y2);
	}
	rect_n += put_fill(rect_words + rect_n, gpu_addr(b->data), b->header.stride,
			   0xff000000 | lv_color_to_u32(color),
			   a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a));
	mbx_gpu_stats.fills++;
}

/*
 * What a later task in the same layer is certain to overwrite, every pixel:
 * the body of an opaque fill (between its rounded corners), or an opaque
 * picture copied 1:1, each within its own clip area. False: nothing certain.
 */
static bool image_ok(const lv_draw_task_t *t);

static bool task_covers(const lv_draw_task_t *u, lv_area_t *out)
{
	lv_area_t core = u->area;

	if (u->type == LV_DRAW_TASK_TYPE_FILL) {
		const lv_draw_fill_dsc_t *d = u->draw_dsc;
		int32_t r = LV_MIN(d->radius, LV_MIN(lv_area_get_width(&core), lv_area_get_height(&core)) / 2);

		if (d->opa < LV_OPA_MAX || d->grad.dir != LV_GRAD_DIR_NONE)
			return false;
		if (r > 0) {		/* the full-width band between the corners */
			core.y1 += r;
			core.y2 -= r;
		}
	} else if (u->type == LV_DRAW_TASK_TYPE_IMAGE) {
		const lv_draw_image_dsc_t *d = u->draw_dsc;
		const lv_image_dsc_t *img = d->src;

		if (d->opa < LV_OPA_MAX || !image_ok(u) || img->header.cf != LV_COLOR_FORMAT_XRGB8888 ||
		    !lv_area_intersect(&core, &u->area, &d->image_area))
			return false;
	} else {
		return false;
	}
	if (core.y2 < core.y1 || core.x2 < core.x1)
		return false;
	return lv_area_intersect(out, &core, &u->clip_area);
}

/*
 * A plain opaque fill, less what later tasks cover anyway: a page's
 * background under its cards or their scroll pictures is most of the
 * screen, filled only to be overwritten (the GPU is memory bound: ~1.3 ms
 * for a screen). The pieces left go out as one job.
 */
#define TRIM_RECTS	12
#define TRIM_MIN_PX	(64 * 32)	/* smaller covers are not worth a split */

static void gpu_fill_trimmed(lv_draw_unit_t *unit, lv_layer_t *layer, const lv_draw_task_t *t,
			     const lv_area_t *clip, lv_color_t color)
{
	lv_area_t rects[TRIM_RECTS], next[TRIM_RECTS], cov, x;
	const lv_draw_task_t *u;
	int n = 1, i, m;
	int32_t px = 0;

	if (!lv_area_intersect(&rects[0], &t->area, clip))
		return;
	for (u = t->next; u; u = u->next) {
		if (!task_covers(u, &cov) || lv_area_get_size(&cov) < TRIM_MIN_PX)
			continue;
		m = 0;
		for (i = 0; i < n; i++) {
			const lv_area_t *r = &rects[i];

			if (!lv_area_intersect(&x, r, &cov)) {
				if (m >= TRIM_RECTS)
					goto full;
				next[m++] = *r;
				continue;
			}
			/* r minus x: bands above and below, strips left and right */
			if (m + 4 > TRIM_RECTS)
				goto full;
			if (x.y1 > r->y1)
				next[m++] = (lv_area_t){ r->x1, r->y1, r->x2, x.y1 - 1 };
			if (x.y2 < r->y2)
				next[m++] = (lv_area_t){ r->x1, x.y2 + 1, r->x2, r->y2 };
			if (x.x1 > r->x1)
				next[m++] = (lv_area_t){ r->x1, x.y1, x.x1 - 1, x.y2 };
			if (x.x2 < r->x2)
				next[m++] = (lv_area_t){ x.x2 + 1, x.y1, r->x2, x.y2 };
		}
		memcpy(rects, next, m * sizeof(rects[0]));
		n = m;
		continue;
full:		/* no room to split further: keep what there is */
		break;
	}
	for (i = 0; i < n; i++)
		px += lv_area_get_size(&rects[i]);
	mbx_gpu_stats.trim_px += lv_area_get_size(&t->area) - px;
	if (px < 16384) {
		/* little left (margins round a card): not worth a GPU job */
		for (i = 0; i < n; i++) {
			unit->clip_area = &rects[i];
			lv_draw_sw_fill(unit, t->draw_dsc, &t->area);
		}
		unit->clip_area = clip;
		return;
	}
	for (i = 0; i < n; i++)
		rect_add(layer, clip, &rects[i], color);
	rect_flush();
}

/* Is @t a fill worth holding back for look-ahead (see mbx_dispatch)? */
static bool trim_candidate(const lv_draw_task_t *t)
{
	const lv_draw_fill_dsc_t *d = t->draw_dsc;
	lv_area_t a;

	return t->type == LV_DRAW_TASK_TYPE_FILL && d->opa >= LV_OPA_MAX && d->radius == 0 &&
	       d->grad.dir == LV_GRAD_DIR_NONE && lv_area_intersect(&a, &t->area, &t->clip_area) &&
	       lv_area_get_size(&a) >= 320 * 120;
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
		gpu_fill_trimmed(u, layer, t, clip, d->color);
		return;
	}
	/*
	 * Rounded: the GPU fills a cross (full-width middle band, and the
	 * top and bottom bands between the corners); then, once it is done,
	 * the CPU draws the r x r corner squares, clipped to each corner.
	 */
	band = (lv_area_t){ c->x1, c->y1 + r, c->x2, c->y2 - r };
	if (band.y2 >= band.y1)
		rect_add(layer, clip, &band, d->color);
	band = (lv_area_t){ c->x1 + r, c->y1, c->x2 - r, c->y1 + r - 1 };
	if (band.x2 >= band.x1) {
		rect_add(layer, clip, &band, d->color);
		band.y1 = c->y2 - r + 1;
		band.y2 = c->y2;
		rect_add(layer, clip, &band, d->color);
	}
	rect_flush();
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

/*
 * Areas of the display buffer this frame whose pixels are premultiplied
 * already (mbx_fill_straight): the flush leaves them alone. Reading them
 * back to convert them cost 13-19 ms a full screen (all cache misses).
 */
static bool transparent;
#define PM_SKIP_MAX 4
static lv_area_t pm_skip[PM_SKIP_MAX];
static int pm_skip_n;

static bool later_over(const lv_draw_task_t *t, const lv_area_t *a)
{
	const lv_draw_task_t *u;
	lv_area_t x;

	for (u = t->next; u; u = u->next)
		if (lv_area_intersect(&x, &u->area, a) && lv_area_intersect(&x, &x, &u->clip_area))
			return true;
	return false;
}

static uint32_t premul_word(lv_opa_t a, lv_color_t c)
{
	uint32_t r = c.red * a + 128, g = c.green * a + 128, bl = c.blue * a + 128;

	r = (r + (r >> 8)) >> 8;
	g = (g + (g >> 8)) >> 8;
	bl = (bl + (bl >> 8)) >> 8;
	return (uint32_t)a << 24 | r << 16 | g << 8 | bl;
}

/*
 * A translucent plain fill into an ARGB8888 layer with nothing under it
 * (mbx_evaluate checked): a 2D fill of its straight (alpha, colour) word;
 * of the premultiplied word, and the flush told to skip it, when it is
 * the display's buffer and nothing is drawn over it afterwards.
 */
static void mbx_fill_straight(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_draw_fill_dsc_t *d = t->draw_dsc;
	lv_area_t a, scr;
	uint32_t word;

	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return;
	if (!in_pool(b->data)) {
		lv_draw_sw_fill(u, d, &t->area);
		return;
	}
	scr = a;
	word = (uint32_t)d->opa << 24 | (lv_color_to_u32(d->color) & 0xffffff);
	if (transparent && b->data == lv_display_get_default()->buf_1->data &&
	    pm_skip_n < PM_SKIP_MAX && !later_over(t, &scr)) {
		word = premul_word(d->opa, d->color);
		pm_skip[pm_skip_n++] = scr;	/* (screen = buffer coordinates here) */
	}
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	if (rect_n)
		rect_flush();
	rect_buf = b;
	rect_rows = a;
	rect_n = put_fill(rect_words, gpu_addr(b->data), b->header.stride, word,
			  a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a));
	rect_flush();
	mbx_gpu_stats.fills++;
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
	    (sb->header.stride & 31) || (gpu_addr(sb->data) & 127))
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
/*
 * A premultiplied picture onto the transparent display: the 2D engine's
 * exact copy (alpha and all), and the flush told not to premultiply it
 * again. What it covers is replaced, not blended (fine for the shade:
 * only its rounded corners' edge pixels lose what was under them).
 */
static bool mbx_image_premul(lv_draw_unit_t *u, lv_draw_task_t *t, lv_draw_buf_t *sb)
{
	lv_draw_image_dsc_t *d = t->draw_dsc;
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_area_t a, sa, da;
	uint32_t w[16];

	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return true;
	if (!in_pool(b->data) || !in_pool(sb->data))
		return false;
	sa = a;
	lv_area_move(&sa, -d->image_area.x1, -d->image_area.y1);
	da = a;
	lv_area_move(&da, -layer->buf_area.x1, -layer->buf_area.y1);
	buf_invalidate(b, &da);
	if (rect_n)
		rect_flush();
	submit(w, put_copy(w, gpu_addr(b->data), b->header.stride, gpu_addr(sb->data),
			   sb->header.stride, sa.x1, sa.y1, da.x1, da.y1,
			   lv_area_get_width(&a), lv_area_get_height(&a)));
	if (b->data == lv_display_get_default()->buf_1->data && pm_skip_n < PM_SKIP_MAX &&
	    !later_over(t, &a))
		pm_skip[pm_skip_n++] = a;	/* (screen = buffer coordinates here) */
	mbx_gpu_stats.layers++;
	return true;
}

/*
 * The opt-in scaled copy: the whole picture over d->image_area, the part in
 * the clip area only (its source rectangle in fractional pixels).
 */
static bool gpu_copy_scaled(lv_draw_unit_t *u, lv_draw_task_t *t, const lv_draw_buf_t *sb)
{
	const lv_draw_image_dsc_t *d = t->draw_dsc;
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	const lv_area_t *ia = &d->image_area;
	float fx = (float)sb->header.w / lv_area_get_width(ia);
	float fy = (float)sb->header.h / lv_area_get_height(ia);
	lv_area_t a, da;
	uint32_t w[48];
	int n;

	if (!lv_area_intersect(&a, ia, u->clip_area))
		return true;
	if (!in_pool(b->data) || (b->header.stride & 31))
		return false;
	da = a;
	lv_area_move(&da, -layer->buf_area.x1, -layer->buf_area.y1);
	buf_invalidate(b, &da);
	n = mbx_cmd_copy_scaled(w, b->header.stride, sb->header.stride,
				(a.x1 - ia->x1) * fx, (a.y1 - ia->y1) * fy,
				(a.x2 + 1 - ia->x1) * fx, (a.y2 + 1 - ia->y1) * fy,
				da.x1, da.y1, da.x2 + 1, da.y2 + 1);
	mbx_gpu_stats.images++;
	return !blit3d(8, gpu_addr(b->data), gpu_addr(sb->data), w, n);
}

static void mbx_image(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_draw_image_dsc_t *d = t->draw_dsc;

	if (image_scaled_ok(t)) {
		if (!gpu_copy_scaled(u, t, (lv_draw_buf_t *)d->src))
			lv_draw_sw_image(u, d, &t->area);	/* (1:1, unscaled) */
		return;
	}

	if (u->target_layer->draw_buf->header.cf == LV_COLOR_FORMAT_ARGB8888) {
		if (!mbx_image_premul(u, t, (lv_draw_buf_t *)d->src))
			lv_draw_sw_image(u, d, &t->area);
		mbx_gpu_stats.images++;
		return;
	}

	/* the CPU does not write snapshots after taking them: no clean */
	if (!gpu_copy(u, t, (lv_draw_buf_t *)d->src, d->image_area.x1, d->image_area.y1,
		      d->opa, false))
		lv_draw_sw_image(u, d, &t->area);
	mbx_gpu_stats.images++;
}

/* (debug) N72_GPU_ADBG: every task the GPU draws, on stderr */
static int async_dbg = -1;
#define ADBG(...) do { if (async_dbg) fprintf(stderr, __VA_ARGS__); } while (0)

static lv_layer_t *defer_layer;		/* a fill is held back in this layer */
static lv_draw_task_t *defer_t, *defer_tail;
static int trim_on = -1;

static int32_t mbx_dispatch(lv_draw_unit_t *u, lv_layer_t *layer)
{
	if (trim_on < 0)
		trim_on = !getenv("N72_NO_TRIM");
	lv_draw_task_t *t;

	if (async_dbg < 0)
		async_dbg = getenv("N72_GPU_ADBG") != NULL;
	t = lv_draw_get_next_available_task(layer, NULL, DRAW_UNIT_ID_MBX);
	if (!t || t->preferred_draw_unit_id != DRAW_UNIT_ID_MBX)
		return LV_DRAW_UNIT_IDLE;
	if (!lv_draw_layer_alloc_buf(layer))
		return LV_DRAW_UNIT_IDLE;

	/*
	 * LVGL (without an OS) dispatches after every task it creates, so a
	 * page's background fill would run before the cards over it exist as
	 * tasks. A large plain fill is held back instead: everything over it
	 * waits for it and queues up, and it runs, trimmed by what follows
	 * (gpu_fill_trimmed), once a dispatch comes with no new task behind
	 * it, i.e. from LVGL's wait loop at the end of the area. One at a time.
	 */
	if (trim_on && trim_candidate(t) && (!defer_layer || defer_layer == layer)) {
		lv_draw_task_t *tail = t;

		while (tail->next)
			tail = tail->next;
		if (defer_layer != layer || defer_t != t || defer_tail != tail) {
			defer_layer = layer;
			defer_t = t;
			defer_tail = tail;
			lv_draw_dispatch_request();	/* so the wait loop comes back */
			return LV_DRAW_UNIT_IDLE;
		}
	}
	if (defer_layer == layer)
		defer_layer = NULL;

	t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
	u->target_layer = layer;
	u->clip_area = &t->clip_area;
	if (t->type == LV_DRAW_TASK_TYPE_LAYER)
		mbx_layer(u, t);
	else if (t->type == LV_DRAW_TASK_TYPE_IMAGE)
		mbx_image(u, t);
	else if (((lv_draw_fill_dsc_t *)t->draw_dsc)->opa >= LV_OPA_MAX)
		mbx_fill(u, t);
	else if (layer->color_format == LV_COLOR_FORMAT_ARGB8888)
		mbx_fill_straight(u, t);
	else
		mbx_fill_blend(u, t);
	ADBG("mbx: dispatch on layer %p\n", (void *)layer);
	ADBG("mbx: task %p type %d area %d,%d-%d,%d\n", (void *)t, t->type,
	     (int)t->area.x1, (int)t->area.y1, (int)t->area.x2, (int)t->area.y2);
	t->state = LV_DRAW_TASK_STATE_READY;
	lv_draw_dispatch_request();
	return 1;
}

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
	if (mbx_gpu_timing && gpu_now_us() - dbg_t0 > 8000 && getenv("N72_FPS"))
		fprintf(stderr, "render start: catch-up %.1f ms\n", (gpu_now_us() - dbg_t0) / 1000.0);
}

static bool transparent;
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
	if (mbx_gpu_timing && gpu_now_us() - dbg_t0 > 8000 && getenv("N72_FPS"))
		fprintf(stderr, "flush: premultiply %.1f ms, clean %.1f ms (%d rows)\n",
			(dbg_t1 - dbg_t0) / 1000.0, (gpu_now_us() - dbg_t1) / 1000.0,
			(int)lv_area_get_height(area));
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
		static int alphacheck = -1;	/* (debug) N72_ALPHACHECK, read once */

		if (alphacheck < 0)
			alphacheck = getenv("N72_ALPHACHECK") != NULL;
		if (!transparent && alphacheck) {
			/* (debug) an opaque frame must be alpha 255 everywhere */
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
	mbx_gpu_stats.flushes++;
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
	mbx_gpu_stats.flushes++;
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
 * to give the screen away. Older kernels (or N72_LAYER0=1): n72-clcd.
 */
static int kms_open(void)
{
	static const char *const want[] = { "n72-clcd-shell", "n72-clcd" };
	char path[32], name[32];
	int w, i, fd;

	for (w = getenv("N72_LAYER0") ? 1 : 0; w < 2; w++) {
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
 * happened to arrive in one read (2026-11-20, after a slow wake; found by
 * the Hardware gaps session in the vblank-wait statistic, 0.2-0.5 ms where
 * 14-16 is normal). True when it came.
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

static bool kms_setup(void)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[2], conns[2], encs[2];
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo modes[4];
	struct drm_mode_crtc set = { 0 };
	int i, fd = kms_take_handoff();
	bool handed = fd >= 0;

	if (!handed)
		fd = kms_open();
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

static bool flip_setup(uint32_t size)
{
	int i;

	/* the render node flips through KMS only (kms_setup below) */
	if (getenv("N72_NOFLIP"))
		return false;
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
	 * on layer 0 (N72_LAYER0) starts from black instead: fb.gpu_addr is the
	 * boot frame buffer, which still holds the boot console's text, and it
	 * would show until the application's first frame (the "console flash"
	 * when the browser opens).
	 */
	{
		uint32_t w[16];

		if (getenv("N72_LAYER0"))
			submit(w, put_fill(w, gpu_addr(scan[0]), fb.stride, 0xff000000,
					   0, 0, fb.width, fb.height));
		else
			submit(w, put_copy(w, gpu_addr(scan[0]), fb.stride, fb.gpu_addr, fb.stride,
					   0, 0, 0, 0, fb.width, fb.height));
	}
	if (!kms_setup()) {
		fprintf(stderr, "n72-ui: no KMS for the render node: copying to the framebuffer\n");
		return false;
	}
	st_front = st_latest = 0;		/* frame 0: what was on the screen */
	buf_frame[0] = 0;
	return true;
}

lv_display_t *mbx_gpu_display_create(void)
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
	flipping = flip_setup(size);
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
static bool transparent;

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
