/* SPDX-License-Identifier: MIT */
#ifndef N72_MBX_GPU_H
#define N72_MBX_GPU_H

#include "lvgl/lvgl.h"

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
 * Create the display on the MBX GPU (/dev/mbx): GPU-mapped draw buffers, a
 * GPU fill unit and GPU flushes. Returns NULL if the GPU is not available;
 * call it after lv_init() and before creating any other display.
 */
lv_display_t *mbx_gpu_display_create(void);

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
int mbx_gpu_rotation(void);
/* a picture turned a quarter (90 or 270, as the screen), by the GPU */
lv_draw_buf_t *mbx_gpu_turn_buf(const lv_draw_buf_t *src, int deg);
/* straight alpha to premultiplied, in place; flags the picture so */
void mbx_gpu_premultiply_buf(lv_draw_buf_t *b);

#endif
