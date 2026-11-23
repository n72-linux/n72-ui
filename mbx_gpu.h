/* SPDX-License-Identifier: MIT */
#ifndef N72_MBX_GPU_H
#define N72_MBX_GPU_H

#include "lvgl/lvgl.h"

#ifdef N72_DEBUG
struct mbx_gpu_stats {
	unsigned long submits, fills, blends, layers, images, flushes, pool_misses;
	unsigned long fill_radius, fill_grad, fill_small;	/* fills left to the CPU */
	unsigned long fill_corners;	/* rounded fills split GPU body + CPU corners */
	double sync_us, submit_us, blit3d_us;	/* time spent in each ioctl */
	double flip_us;		/* waiting for the vertical blank */
	unsigned long async_done;	/* tasks finished asynchronously */
	unsigned long long sync_bytes;
	unsigned long seen[16];	/* draw tasks evaluated, by type */
	unsigned long long trim_px;	/* fill pixels skipped: covered by later tasks */
};
extern struct mbx_gpu_stats mbx_gpu_stats;
/*
 * Set by whoever reads the *_us totals above (N72_LATENCY, N72_FPS, --bench):
 * every cache sync, submit and flip is then timed. Off, those cost no
 * clock_gettime (a system call on this CPU, twice a job).
 */
extern bool mbx_gpu_timing;
#endif

/*
 * Create the display on the MBX GPU (its DRM render node): GPU-mapped draw
 * buffers, a GPU fill unit and GPU flushes. @shell_layer=true opens the
 * launcher's layer 1 (n72-clcd-shell, falling back to n72-clcd); false opens
 * layer 0 (n72-clcd) and clears it to black. Returns NULL if the GPU is not
 * available; call after lv_init() and before creating any other display.
 */
lv_display_t *mbx_gpu_display_create(bool shell_layer);

/* A GPU copy of the screen as shown now, or NULL. */
lv_draw_buf_t *mbx_gpu_grab_screen(void);

/* The pixels on screen now, for screenshots (SIGUSR1 -> /tmp/n72-shot.raw). */
const uint8_t *mbx_gpu_front(uint32_t *stride);
const uint8_t *mbx_gpu_scan(int i, int *is_front);

/* Triple buffering: a finished frame is still waiting for the display. */
bool mbx_gpu_frame_waiting(void);
/* Readable (then drained by the next call) when a frame goes to the display; -1 if none. */
int mbx_gpu_notify_fd(void);

/* An opaque (XRGB8888) copy of an ARGB8888 picture over @bg, or NULL. */
lv_draw_buf_t *mbx_gpu_flatten(const lv_draw_buf_t *src, lv_color_t bg);

/*
 * Transparent mode, for what the launcher shows over an application: the
 * frame keeps the alpha of what is drawn and is zeros, which the display's
 * layer 1 shows as the application underneath, everywhere else. Drawing is
 * the CPU's while it is on. Without layer 1 it does nothing.
 */
void mbx_gpu_set_transparent(bool on);
/* before exec'ing a new n72-ui: the display is handed over (no flash) */
void mbx_gpu_handoff(void);
/* True when the launcher has the display's layer 1 (KMS n72-clcd-shell). */
bool mbx_gpu_on_shell_layer(void);
/* turn the screen (LVGL's rotation 0, 90, 270); false if it cannot */
bool mbx_gpu_set_rotation(int deg);
/* true: no turned copy after drawing (upright, or the display turns the layer) */
bool mbx_gpu_draws_direct(void);
/* a picture turned a quarter (90 or 270, as the screen), by the GPU */
lv_draw_buf_t *mbx_gpu_turn_buf(const lv_draw_buf_t *src, int deg);
/* straight alpha to premultiplied, in place; flags the picture so */
void mbx_gpu_premultiply_buf(lv_draw_buf_t *b);

/*
 * Opt in, for one image draw: the GPU may stretch the whole picture (d->src,
 * an XRGB8888 lv_draw_buf_t in the GPU pool, drawn opaque) over d->image_area
 * when the two differ in size (the area is the destination; leave LVGL's
 * scale_x/scale_y at LV_SCALE_NONE). Point sampled. Draws without the mark
 * are unchanged. Without the GPU (or a picture outside the pool) LVGL draws
 * it 1:1, unscaled: callers check that the GPU display is in use.
 * (The browser's pinch-zoom: page tiles drawn bigger or smaller.)
 */
void mbx_gpu_image_dsc_scaled(lv_draw_image_dsc_t *d);

#endif
