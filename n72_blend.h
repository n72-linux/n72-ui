/* SPDX-License-Identifier: MIT */
/*
 * n72-ui: faster software blends for LVGL (LV_USE_DRAW_SW_ASM = CUSTOM).
 *
 * LVGL's generic loops mix 32-bit pixels one byte at a time through
 * lv_color_24_24_mix(). Text is mostly "colour through an A8 mask into an
 * XRGB8888 buffer"; these do that a word at a time, two channels per
 * multiply (the colour's products from a table), with the same arithmetic (exact: (s * m + d * (255 - m)) >> 8
 * per channel, copy at m >= LV_OPA_MAX, skip at 0), so the result is
 * pixel-identical; LVGL leaves the X byte alone, so do these. Anything else returns LV_RESULT_INVALID and LVGL's own
 * code runs.
 *
 * Included by every lv_draw_sw_blend_to_*.c after lv_draw_sw_blend_private.h.
 */
#ifndef N72_BLEND_H
#if defined(LV_DRAW_SW_BLEND_PRIVATE_H)	/* also included by lv_draw_sw.c / _img.c: nothing for them */
#define N72_BLEND_H

#include <stdint.h>

/*
 * The colour's products with every coverage value, for the current colour:
 * text keeps one colour for many glyphs, so a pixel then needs two
 * multiplies (the destination's) instead of four.
 */
static uint32_t n72_tab_color = 0xffffffff;
static uint32_t n72_tab_rb[256], n72_tab_g[256];

static inline void n72_tab_set(uint32_t c)
{
	uint32_t m;

	if (c == n72_tab_color)
		return;
	for (m = 0; m < 256; m++) {
		n72_tab_rb[m] = (c & 0xff00ff) * m;
		n72_tab_g[m] = ((c & 0xff00) >> 8) * m;
	}
	n72_tab_color = c;
}

static inline uint32_t n72_mix_tab(uint32_t d, uint32_t m)
{
	uint32_t inv = 255 - m;
	uint32_t rb = ((n72_tab_rb[m] + (d & 0xff00ff) * inv) >> 8) & 0xff00ff;
	uint32_t g = (n72_tab_g[m] + ((d & 0xff00) >> 8) * inv) & 0xff00;

	return (d & 0xff000000) | rb | g;
}

/*
 * Text on a flat background, the usual case: the finished pixel for every
 * coverage value, for one (text colour, background pixel) pair. A pixel is
 * then a compare and a table load instead of two multiplies; any pixel
 * that is not that background takes the mix above, so the result is the
 * same. A few pairs are kept (a page has a handful of text colours and
 * surfaces); a pair gets its table when a second glyph in a row asks for
 * it, so pictures and gradients under text do not rebuild tables for every
 * glyph.
 */
#define N72_RAMPS	8
static struct n72_ramp {
	uint32_t fg, bg;	/* bg: the whole pixel, X byte too */
	uint32_t v[256];
} n72_ramps[N72_RAMPS];
static uint32_t n72_ramp_valid;	/* bit per slot */
static uint32_t n72_ramp_next;	/* slot to replace */
static uint32_t n72_want_fg = 0xffffffff, n72_want_bg;

static inline const uint32_t *n72_ramp_get(uint32_t c, uint32_t bg)
{
	struct n72_ramp *r;
	uint32_t i, m;

	for (i = 0; i < N72_RAMPS; i++)
		if ((n72_ramp_valid >> i & 1) && n72_ramps[i].fg == c && n72_ramps[i].bg == bg)
			return n72_ramps[i].v;
	if (n72_want_fg != c || n72_want_bg != bg) {	/* first time in a row: not yet */
		n72_want_fg = c;
		n72_want_bg = bg;
		return NULL;
	}
	r = &n72_ramps[n72_ramp_next];
	n72_ramp_valid |= 1u << n72_ramp_next;
	n72_ramp_next = (n72_ramp_next + 1) % N72_RAMPS;
	r->fg = c;
	r->bg = bg;
	r->v[0] = bg;
	for (m = 1; m < 256; m++)
		r->v[m] = m >= LV_OPA_MAX ? (bg & 0xff000000) | c : n72_mix_tab(bg, m);
	return r->v;
}

/* Colour through a mask (and an overall opacity) into 4-byte RGB888 (XRGB8888). */
static inline lv_result_t n72_blend_color_mask_xrgb(lv_draw_sw_blend_fill_dsc_t *dsc, uint32_t px, lv_opa_t opa)
{
	int32_t w = dsc->dest_w, h = dsc->dest_h, x, y;
	const lv_opa_t *mask = dsc->mask_buf;
	uint8_t *dest = dsc->dest_buf;
	uint32_t c = lv_color_to_u32(dsc->color) & 0xffffff;
	const uint32_t *ramp = NULL;
	uint32_t bg = 0;

	if (px != 4)
		return LV_RESULT_INVALID;
	n72_tab_set(c);
	if (opa >= LV_OPA_MAX) {
		/* the box's first pixel is background nearly always (drawn before the glyph) */
		bg = *(const uint32_t *)dest;
		ramp = n72_ramp_get(c, bg);
	}
	for (y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)dest;

		if (ramp) {
			for (x = 0; x < w; x++) {
				uint32_t m = mask[x], dv;

				if (m == 0)
					continue;
				dv = d[x];
				if (dv == bg)
					d[x] = ramp[m];
				else
					d[x] = m >= LV_OPA_MAX ? (dv & 0xff000000) | c : n72_mix_tab(dv, m);
			}
		} else if (opa >= LV_OPA_MAX) {
			for (x = 0; x < w; x++) {
				uint32_t m = mask[x];

				if (m == 0)
					continue;
				d[x] = m >= LV_OPA_MAX ? (d[x] & 0xff000000) | c : n72_mix_tab(d[x], m);
			}
		} else {
			for (x = 0; x < w; x++) {
				uint32_t m = LV_OPA_MIX2(opa, mask[x]);

				if (m == 0)
					continue;
				d[x] = m >= LV_OPA_MAX ? (d[x] & 0xff000000) | c : n72_mix_tab(d[x], m);
			}
		}
		dest += dsc->dest_stride;
		mask += dsc->mask_stride;
	}
	return LV_RESULT_OK;
}

#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_WITH_MASK(dsc, px)	n72_blend_color_mask_xrgb(dsc, px, LV_OPA_COVER)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_MIX_MASK_OPA(dsc, px)	n72_blend_color_mask_xrgb(dsc, px, (dsc)->opa)

#endif
#endif
