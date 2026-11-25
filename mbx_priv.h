// SPDX-License-Identifier: MIT
/* What mbx_gpu.c, mbx_draw.c and mbx_kms.c share. */
#ifndef N72_MBX_PRIV_H
#define N72_MBX_PRIV_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/apple_mbx_drm.h>
#include <n72mbxmem.h>			/* MBX device helpers (render node) */

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
#ifdef N72_DEBUG
void n72_trace(const char *what, long v);
#else
#define n72_trace(what, v) do { } while (0)
#endif

#define DRAW_UNIT_ID_MBX	5

#ifndef POOL_SIZE
#define POOL_SIZE		(6 * 1024 * 1024)	/* display, layers, snapshots */
#endif

#define POOL_BLOCKS		256

#ifdef N72_DEBUG
#define MBX_STAT_INC(field) (mbx_gpu_stats.field++)
#define MBX_STAT_ADD(field, v) (mbx_gpu_stats.field += (v))
/* (t0 is 0 when the flag came on between the two readings: nothing to add) */
#define STAT_US(field, t0) \
	do { if (mbx_gpu_timing && (t0) != 0) mbx_gpu_stats.field += gpu_now_us() - (t0); } while (0)
#else
#define MBX_STAT_INC(field) do { } while (0)
#define MBX_STAT_ADD(field, v) do { } while (0)
#define STAT_US(field, t0) do { (void)(t0); } while (0)
#endif

#define PM_SKIP_MAX 8

struct pool_block {
	uint32_t off, size;
	bool used;
};

/*
 * GPU-visible memory the CPU also maps: the pool, and with page flipping
 * the two scan-out buffers. Each region is one buffer object.
 */
struct region {
	uint8_t *cpu;
	uint32_t size, gpu, handle;
};

/* mbx_gpu.c */
extern int mbx;
extern struct mbx_fb_info fb;
extern struct mbx_buf pool_buf;
extern uint32_t pool_gpu;
extern uint8_t *pool;
extern struct pool_block blocks[POOL_BLOCKS];
extern int nblocks;
void *pool_alloc(size_t size);
void pool_free(void *p);
bool pool_is_empty(void);
void pool_release(void);
bool pool_reacquire(void);
extern struct region regions[4];
extern int nregions;
void region_remove(const void *cpu);
bool in_pool(const void *p);
uint32_t gpu_addr(const void *p);
double gpu_now_us(void);
void sync_range(const void *p, uint32_t size, uint32_t op);
int submit(const uint32_t *w, int n);
int put_fill(uint32_t *w, uint32_t dst, uint32_t stride, uint32_t color, int x, int y, int width, int height);
int put_copy(uint32_t *w, uint32_t dst, uint32_t dstride, uint32_t src, uint32_t sstride, int sx, int sy, int dx, int dy, int width, int height);
int blit3d(int type, uint32_t dst, uint32_t src, const uint32_t *w, int n);
void *buf_malloc(size_t size, lv_color_format_t cf);
uint32_t buf_width_to_stride(uint32_t w, lv_color_format_t cf);
void buf_free(void *p);
void buf_invalidate(const lv_draw_buf_t *b, const lv_area_t *a);
void buf_clean(const lv_draw_buf_t *b, const lv_area_t *a);

/* mbx_draw.c */
extern bool transparent;
extern bool frame_cpu_dirty;
extern int32_t cpu_dirty_y1, cpu_dirty_y2;
extern bool frame_drew_any;
extern uint32_t ta_active_seq;
int32_t mbx_evaluate(lv_draw_unit_t *u, lv_draw_task_t *t);
extern lv_area_t pm_skip[PM_SKIP_MAX];
extern int pm_skip_n;
int32_t mbx_dispatch(lv_draw_unit_t *u, lv_layer_t *layer);
void ta_wait_active(void);
void ta_ring_free(void);

/* mbx_kms.c */
extern bool kms_shell;
bool mbx_kms_direct_scan_buf(const void *p);
bool mbx_kms_can_async_ta(const lv_layer_t *layer, const lv_draw_task_t *last);
void mbx_kms_note_async_ta(uint32_t seq);
bool mbx_kms_buf_needs_invalidate(const void *p);
void mbx_kms_note_buf_invalidated(const void *p, const lv_area_t *a);
void mbx_kms_note_cpu_touch(const void *p);

#ifdef N72_DEBUG
static inline double stat_t0(void)
{
	return mbx_gpu_timing ? gpu_now_us() : 0;
}
#else
static inline double stat_t0(void)
{
	return 0;
}
#endif

#endif
