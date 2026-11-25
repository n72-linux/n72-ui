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
	uint32_t inv = 255u - m;
	uint32_t rb = ((n72_tab_rb[m] + (d & 0xff00ffu) * inv) >> 8) & 0xff00ffu;
	uint32_t g = (n72_tab_g[m] + ((d & 0xff00u) >> 8) * inv) & 0xff00u;

	return (d & 0xff000000u) | rb | g;
}

static inline uint32_t n72_mix_rgb_word(uint32_t s_rgb, uint32_t d, uint32_t m)
{
	uint32_t inv = 255u - m;
	uint32_t rb = (((s_rgb & 0x00ff00ffu) * m + (d & 0x00ff00ffu) * inv) >> 8) & 0x00ff00ffu;
	uint32_t g = ((((s_rgb & 0x0000ff00u) >> 8) * m + ((d & 0x0000ff00u) >> 8) * inv)) & 0x0000ff00u;

	return rb | g;
}

/*
 * Text on a flat background, the usual case: the finished pixel for the
 * 16 coverage levels of 4-bpp fonts (k * 17, 0x00..0xff), for one (text colour,
 * background pixel) pair. 8 slots fit in 576 bytes (~2 L1 cache lines per
 * slot on ARM1176) instead of 8.25 KiB; 8-bpp glyphs and non-matching pixels
 * use n72_mix_tab().
 */
#define N72_RAMPS	8
static struct n72_ramp {
	uint32_t fg, bg;	/* bg: the whole pixel, X byte too */
	uint32_t v[16];
} n72_ramps[N72_RAMPS];
static uint32_t n72_ramp_valid;	/* bit per slot */
static uint32_t n72_ramp_next;	/* slot to replace */
static uint32_t n72_want_fg = 0xffffffff, n72_want_bg;

static inline const uint32_t *n72_ramp_get(uint32_t c, uint32_t bg)
{
	struct n72_ramp *r;
	uint32_t i, k;

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
	for (k = 1; k < 16; k++) {
		uint32_t m = k * 17u;

		r->v[k] = m >= LV_OPA_MAX ? (bg & 0xff000000u) | c : n72_mix_tab(bg, m);
	}
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
				if (dv == bg && (m & 0x0fu) == (m >> 4))
					d[x] = ramp[m >> 4];
				else
					d[x] = m >= LV_OPA_MAX ? (dv & 0xff000000u) | c : n72_mix_tab(dv, m);
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

/* Colour with uniform opacity into 4-byte RGB888 (XRGB8888). */
static inline lv_result_t n72_blend_color_opa_xrgb(lv_draw_sw_blend_fill_dsc_t *dsc, uint32_t px)
{
	int32_t w = dsc->dest_w, h = dsc->dest_h, x, y;
	uint8_t *dest = dsc->dest_buf;
	uint32_t opa = dsc->opa, inv, c, s_rb, s_g;

	if (px != 4)
		return LV_RESULT_INVALID;
	if (opa == 0)
		return LV_RESULT_OK;
	c = lv_color_to_u32(dsc->color) & 0x00ffffffu;
	if (opa >= LV_OPA_MAX) {
		for (y = 0; y < h; y++) {
			uint32_t *d = (uint32_t *)dest;

			for (x = 0; x < w; x++)
				d[x] = (d[x] & 0xff000000u) | c;
			dest += dsc->dest_stride;
		}
		return LV_RESULT_OK;
	}
	inv = 255u - opa;
	s_rb = (c & 0x00ff00ffu) * opa;
	s_g = ((c & 0x0000ff00u) >> 8) * opa;
	for (y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)dest;

		for (x = 0; x + 1 < w; x += 2) {
			uint32_t d0 = d[x], d1 = d[x + 1];
			uint32_t rb0 = ((s_rb + (d0 & 0x00ff00ffu) * inv) >> 8) & 0x00ff00ffu;
			uint32_t g0 = (s_g + ((d0 & 0x0000ff00u) >> 8) * inv) & 0x0000ff00u;
			uint32_t rb1 = ((s_rb + (d1 & 0x00ff00ffu) * inv) >> 8) & 0x00ff00ffu;
			uint32_t g1 = (s_g + ((d1 & 0x0000ff00u) >> 8) * inv) & 0x0000ff00u;

			d[x] = (d0 & 0xff000000u) | rb0 | g0;
			d[x + 1] = (d1 & 0xff000000u) | rb1 | g1;
		}
		if (x < w) {
			uint32_t d0 = d[x];
			uint32_t rb0 = ((s_rb + (d0 & 0x00ff00ffu) * inv) >> 8) & 0x00ff00ffu;
			uint32_t g0 = (s_g + ((d0 & 0x0000ff00u) >> 8) * inv) & 0x0000ff00u;

			d[x] = (d0 & 0xff000000u) | rb0 | g0;
		}
		dest += dsc->dest_stride;
	}
	return LV_RESULT_OK;
}

/* ARGB8888 normal blend (with optional uniform opacity) into 4-byte RGB888 (XRGB8888). */
static inline lv_result_t n72_blend_argb_to_xrgb(lv_draw_sw_blend_image_dsc_t *dsc, uint32_t px, lv_opa_t opa)
{
	int32_t w = dsc->dest_w, h = dsc->dest_h, x, y;
	uint8_t *dest = dsc->dest_buf;
	const uint8_t *src = dsc->src_buf;

	if (px != 4)
		return LV_RESULT_INVALID;
	if (opa == 0)
		return LV_RESULT_OK;
	for (y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)dest;
		const uint32_t *s = (const uint32_t *)src;

		if (opa >= LV_OPA_MAX) {
			for (x = 0; x < w; x++) {
				uint32_t sv = s[x], a = sv >> 24, dv;

				if (a == 0)
					continue;
				if (a >= LV_OPA_MAX) {
					d[x] = (d[x] & 0xff000000u) | (sv & 0x00ffffffu);
					continue;
				}
				dv = d[x];
				d[x] = (dv & 0xff000000u) | n72_mix_rgb_word(sv & 0x00ffffffu, dv, a);
			}
		} else {
			for (x = 0; x < w; x++) {
				uint32_t sv = s[x], a = (uint32_t)LV_OPA_MIX2(sv >> 24, opa), dv;

				if (a == 0)
					continue;
				if (a >= LV_OPA_MAX) {
					d[x] = (d[x] & 0xff000000u) | (sv & 0x00ffffffu);
					continue;
				}
				dv = d[x];
				d[x] = (dv & 0xff000000u) | n72_mix_rgb_word(sv & 0x00ffffffu, dv, a);
			}
		}
		dest += dsc->dest_stride;
		src += dsc->src_stride;
	}
	return LV_RESULT_OK;
}

/* Exact reciprocal table for (m * 255u) / ra without hardware division on ARM1176:
 * (m * n72_inv255[ra]) >> 16 == (m * 255u) / ra for all 1 <= ra <= 255, 0 <= m <= ra. */
static const uint32_t n72_inv255[256] = {
	0u, 16711680u, 8355840u, 5570560u, 4177920u, 3342336u, 2785280u, 2387383u,
	2088960u, 1856854u, 1671168u, 1519244u, 1392640u, 1285514u, 1193692u, 1114112u,
	1044480u, 983040u, 928427u, 879563u, 835584u, 795795u, 759622u, 726595u,
	696320u, 668468u, 642757u, 618952u, 596846u, 576265u, 557056u, 539087u,
	522240u, 506415u, 491520u, 477477u, 464214u, 451668u, 439782u, 428505u,
	417792u, 407602u, 397898u, 388644u, 379811u, 371371u, 363298u, 355568u,
	348160u, 341055u, 334234u, 327680u, 321379u, 315315u, 309476u, 303849u,
	298423u, 293188u, 288133u, 283249u, 278528u, 273962u, 269544u, 265265u,
	261120u, 257103u, 253208u, 249429u, 245760u, 242199u, 238739u, 235376u,
	232107u, 228928u, 225834u, 222823u, 219891u, 217035u, 214253u, 211541u,
	208896u, 206318u, 203801u, 201346u, 198949u, 196608u, 194322u, 192089u,
	189906u, 187772u, 185686u, 183645u, 181649u, 179696u, 177784u, 175913u,
	174080u, 172286u, 170528u, 168805u, 167117u, 165463u, 163840u, 162250u,
	160690u, 159159u, 157658u, 156184u, 154738u, 153319u, 151925u, 150556u,
	149212u, 147891u, 146594u, 145319u, 144067u, 142835u, 141625u, 140435u,
	139264u, 138114u, 136981u, 135868u, 134772u, 133694u, 132633u, 131589u,
	130560u, 129548u, 128552u, 127571u, 126604u, 125652u, 124715u, 123791u,
	122880u, 121984u, 121100u, 120228u, 119370u, 118523u, 117688u, 116865u,
	116054u, 115253u, 114464u, 113685u, 112917u, 112159u, 111412u, 110674u,
	109946u, 109227u, 108518u, 107818u, 107127u, 106444u, 105771u, 105105u,
	104448u, 103800u, 103159u, 102526u, 101901u, 101283u, 100673u, 100070u,
	99475u, 98886u, 98304u, 97730u, 97161u, 96600u, 96045u, 95496u,
	94953u, 94417u, 93886u, 93362u, 92843u, 92330u, 91823u, 91321u,
	90825u, 90334u, 89848u, 89368u, 88892u, 88422u, 87957u, 87496u,
	87040u, 86590u, 86143u, 85701u, 85264u, 84831u, 84403u, 83979u,
	83559u, 83143u, 82732u, 82324u, 81920u, 81521u, 81125u, 80733u,
	80345u, 79961u, 79580u, 79203u, 78829u, 78459u, 78092u, 77729u,
	77369u, 77013u, 76660u, 76310u, 75963u, 75619u, 75278u, 74941u,
	74606u, 74275u, 73946u, 73620u, 73297u, 72977u, 72660u, 72345u,
	72034u, 71724u, 71418u, 71114u, 70813u, 70514u, 70218u, 69924u,
	69632u, 69344u, 69057u, 68773u, 68491u, 68211u, 67934u, 67659u,
	67386u, 67116u, 66847u, 66581u, 66317u, 66055u, 65795u, 65536u,
};

/* Colour (optionally through a mask and/or uniform opacity) into ARGB8888. */
static inline lv_result_t n72_blend_color_to_argb(lv_draw_sw_blend_fill_dsc_t *dsc,
						  const lv_opa_t *mask, lv_opa_t opa)
{
	int32_t w = dsc->dest_w, h = dsc->dest_h, x, y;
	uint8_t *dest = dsc->dest_buf;
	uint32_t c = lv_color_to_u32(dsc->color) & 0x00ffffffu;
	uint32_t last_m = 256, last_ba = 256, ra = 255, ratio = 255;

	if (opa == 0)
		return LV_RESULT_OK;
	n72_tab_set(c);
	for (y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)dest;

		for (x = 0; x < w; x++) {
			uint32_t m = mask ? (opa >= LV_OPA_MAX ? (uint32_t)mask[x] : (uint32_t)LV_OPA_MIX2(mask[x], opa))
					  : (uint32_t)opa;
			uint32_t dv, ba;

			if (m == 0)
				continue;
			if (m >= LV_OPA_MAX) {
				d[x] = (m << 24) | c;
				continue;
			}
			dv = d[x];
			ba = dv >> 24;
			if (ba <= LV_OPA_MIN) {
				d[x] = (m << 24) | c;
				continue;
			}
			if (m <= LV_OPA_MIN)
				continue;
			if (ba == 255u) {
				d[x] = 0xff000000u | (n72_mix_tab(dv, m) & 0x00ffffffu);
				continue;
			}
			if (m != last_m || ba != last_ba) {
				last_m = m;
				last_ba = ba;
				ra = 255u - (uint32_t)LV_OPA_MIX2(255u - m, 255u - ba);
				ratio = (m * n72_inv255[ra]) >> 16;
			}
			if (ratio >= LV_OPA_MAX)
				d[x] = (ra << 24) | c;
			else if (ratio <= LV_OPA_MIN)
				d[x] = (ra << 24) | (dv & 0x00ffffffu);
			else
				d[x] = (ra << 24) | (n72_mix_tab(dv, ratio) & 0x00ffffffu);
		}
		dest += dsc->dest_stride;
		if (mask)
			mask += dsc->mask_stride;
	}
	return LV_RESULT_OK;
}

/* ARGB8888 normal blend (with optional uniform opacity) into ARGB8888. */
static inline lv_result_t n72_blend_argb_to_argb(lv_draw_sw_blend_image_dsc_t *dsc, lv_opa_t opa)
{
	int32_t w = dsc->dest_w, h = dsc->dest_h, x, y;
	uint8_t *dest = dsc->dest_buf;
	const uint8_t *src = dsc->src_buf;
	uint32_t last_a = 256, last_ba = 256, ra = 255, ratio = 255;

	if (opa <= LV_OPA_MIN)
		return LV_RESULT_OK;
	for (y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)dest;
		const uint32_t *s = (const uint32_t *)src;

		for (x = 0; x < w; x++) {
			uint32_t sv = s[x];
			uint32_t a = opa >= LV_OPA_MAX ? (sv >> 24) : (uint32_t)LV_OPA_MIX2(sv >> 24, opa);
			uint32_t dv, ba;

			if (a == 0)
				continue;
			if (a >= LV_OPA_MAX) {
				d[x] = (a << 24) | (sv & 0x00ffffffu);
				continue;
			}
			dv = d[x];
			ba = dv >> 24;
			if (ba <= LV_OPA_MIN) {
				d[x] = (a << 24) | (sv & 0x00ffffffu);
				continue;
			}
			if (a <= LV_OPA_MIN)
				continue;
			if (ba == 255u) {
				d[x] = 0xff000000u | n72_mix_rgb_word(sv & 0x00ffffffu, dv, a);
				continue;
			}
			if (a != last_a || ba != last_ba) {
				last_a = a;
				last_ba = ba;
				ra = 255u - (uint32_t)LV_OPA_MIX2(255u - a, 255u - ba);
				ratio = (a * n72_inv255[ra]) >> 16;
			}
			if (ratio >= LV_OPA_MAX)
				d[x] = (ra << 24) | (sv & 0x00ffffffu);
			else if (ratio <= LV_OPA_MIN)
				d[x] = (ra << 24) | (dv & 0x00ffffffu);
			else
				d[x] = (ra << 24) | n72_mix_rgb_word(sv & 0x00ffffffu, dv, ratio);
		}
		dest += dsc->dest_stride;
		src += dsc->src_stride;
	}
	return LV_RESULT_OK;
}

#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_WITH_OPA(dsc, px)		n72_blend_color_opa_xrgb(dsc, px)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_WITH_MASK(dsc, px)		n72_blend_color_mask_xrgb(dsc, px, LV_OPA_COVER)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_MIX_MASK_OPA(dsc, px)		n72_blend_color_mask_xrgb(dsc, px, (dsc)->opa)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB888(dsc, px)		n72_blend_argb_to_xrgb(dsc, px, LV_OPA_COVER)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB888_WITH_OPA(dsc, px)	n72_blend_argb_to_xrgb(dsc, px, (dsc)->opa)

#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_WITH_OPA(dsc)		n72_blend_color_to_argb(dsc, NULL, (dsc)->opa)
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_WITH_MASK(dsc)		n72_blend_color_to_argb(dsc, (dsc)->mask_buf, LV_OPA_COVER)
#define LV_DRAW_SW_COLOR_BLEND_TO_ARGB8888_MIX_MASK_OPA(dsc)		n72_blend_color_to_argb(dsc, (dsc)->mask_buf, (dsc)->opa)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_ARGB8888(dsc)		n72_blend_argb_to_argb(dsc, LV_OPA_COVER)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_ARGB8888_WITH_OPA(dsc)	n72_blend_argb_to_argb(dsc, (dsc)->opa)

#endif
#endif
