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
#include "mbx_priv.h"

#define POOL_ALIGN		128	/* the 3D core needs 128-byte aligned surfaces */

int mbx = -1;
struct mbx_fb_info fb;
struct mbx_buf pool_buf;
uint32_t pool_gpu;
uint8_t *pool;

#ifdef N72_DEBUG
struct mbx_gpu_stats mbx_gpu_stats;
#endif

/* ---------------------------------------------------- pool allocator */

/* First-fit over a small table of blocks, kept sorted by offset. */
struct pool_block blocks[POOL_BLOCKS];
int nblocks;

void *pool_alloc(size_t size)
{
	int i;

	if (!pool)
		return NULL;
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

void pool_free(void *p)
{
	uint32_t off;
	int i;

	if (!pool || !p)
		return;
	off = (uint8_t *)p - pool;
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

bool pool_is_empty(void)
{
	return pool && nblocks == 1 && !blocks[0].used;
}

void region_remove(const void *cpu)
{
	int i;

	for (i = 0; i < nregions; i++) {
		if (regions[i].cpu == cpu) {
			memmove(&regions[i], &regions[i + 1],
				(nregions - i - 1) * sizeof(regions[0]));
			nregions--;
			return;
		}
	}
}

void pool_release(void)
{
	if (!pool)
		return;
	ta_wait_active();
	region_remove(pool);
	mbx_free(mbx, &pool_buf);
	memset(&pool_buf, 0, sizeof(pool_buf));
	pool = NULL;
	pool_gpu = 0;
	nblocks = 0;
}

bool pool_reacquire(void)
{
	if (pool)
		return true;
	if (mbx < 0 || nregions >= 4 || mbx_alloc(mbx, POOL_SIZE, MBX_ALLOC_CACHED, &pool_buf))
		return false;
	pool = pool_buf.cpu;
	pool_gpu = pool_buf.gpu;
	regions[nregions++] = (struct region){ pool, POOL_SIZE, pool_buf.gpu, pool_buf.handle };
	blocks[0].off = 0;
	blocks[0].size = POOL_SIZE;
	blocks[0].used = false;
	nblocks = 1;
	return true;
}

/*
 * GPU-visible memory the CPU also maps: the pool, and with page flipping
 * the two scan-out buffers. Each region is one buffer object.
 */
struct region regions[4];
int nregions;

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
bool in_pool(const void *p)
{
	return region_of(p) != NULL;
}

uint32_t gpu_addr(const void *p)
{
	const struct region *r = region_of(p);

	return r->gpu + ((const uint8_t *)p - r->cpu);
}

/* ---------------------------------------------------------- the GPU */

double gpu_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

#ifdef N72_DEBUG
/* the statistics' timings: only when somebody reads them (mbx_gpu_timing) */
bool mbx_gpu_timing;
#endif

void sync_range(const void *p, uint32_t size, uint32_t op)
{
	double t0 = stat_t0();
	const struct region *r = region_of(p);
	struct mbx_buf b;
	uint32_t off;

	if (!r)
		return;
	b = (struct mbx_buf){ .cpu = r->cpu, .handle = r->handle, .size = r->size, .cached = 1 };
	off = (const uint8_t *)p - r->cpu;

	if (op == MBX_SYNC_FLUSH)
		mbx_flush(mbx, &b, off, size);
	else
		mbx_clean(mbx, &b, off, size);
	STAT_US(sync_us, t0);
	MBX_STAT_ADD(sync_bytes, size);
}

/*
 * Jobs run synchronously (the kernel's default, no mbx_set_async); a
 * job's ioctl may return its sequence number, which is success.
 */
static int gpu_ret(int ret)
{
	return ret > 0 ? 0 : ret;
}

int submit(const uint32_t *w, int n)
{
	double t0 = stat_t0();
	int ret;

	MBX_STAT_INC(submits);
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

int put_fill(uint32_t *w, uint32_t dst, uint32_t stride, uint32_t color,
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

int put_copy(uint32_t *w, uint32_t dst, uint32_t dstride, uint32_t src,
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

int blit3d(int type, uint32_t dst, uint32_t src, const uint32_t *w, int n)
{
	struct mbx_blit3d b = { .type = type, .dst_gpu = dst, .src_gpu = src, .count = n };

	double t0 = stat_t0();
	int ret;

	memcpy(b.words, w, n * 4);
	MBX_STAT_INC(submits);
	ret = gpu_ret(mbx_blit_3d(mbx, &b));
	STAT_US(blit3d_us, t0);
	return ret;
}

/* ------------------------------------------------- LVGL draw buffers */

void *buf_malloc(size_t size, lv_color_format_t cf)
{
	void *p;

	if (pool &&
	    (cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888) &&
	    size >= 16384) {
		p = pool_alloc(size);
		if (p)
			return p;
		MBX_STAT_INC(pool_misses);
	}
	return lv_malloc(size);
}

/* The 3D core needs strides in multiples of 8 pixels (32 bytes). */
uint32_t buf_width_to_stride(uint32_t w, lv_color_format_t cf)
{
	return (w * lv_color_format_get_bpp(cf) / 8 + 31) & ~31u;
}

void buf_free(void *p)
{
	if (pool && p && (uint8_t *)p >= pool && (uint8_t *)p < pool + POOL_SIZE) {
		ta_wait_active();
		pool_free(p);
	} else {
		lv_free(p);
	}
}

static void buf_sync_area(const lv_draw_buf_t *b, const lv_area_t *a, uint32_t op)
{
	uint32_t stride = b->header.stride;

	if (!in_pool(b->data))
		return;
	sync_range(b->data + a->y1 * stride, (lv_area_get_height(a)) * stride, op);
}

void buf_invalidate(const lv_draw_buf_t *b, const lv_area_t *a)
{
	buf_sync_area(b, a, MBX_SYNC_FLUSH);
}

void buf_clean(const lv_draw_buf_t *b, const lv_area_t *a)
{
	buf_sync_area(b, a, MBX_SYNC_CLEAN);
}
