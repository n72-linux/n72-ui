/* SPDX-License-Identifier: MIT */
/*
 * Build MBX 3D-blit commands (see <drm/apple_mbx_drm.h>) for ARGB8888
 * surfaces: no rotation, no scaling, no clipping. Blend words measured on
 * the hardware.
 */
#ifndef N72_MBX_CMD_H
#define N72_MBX_CMD_H

#include <stdint.h>
#include <string.h>

/*
 * Blend words measured on the iPod (result = src * a + dst * (1 - a)):
 * MBX2D's own "src alpha, one minus src alpha" encodings come out inverted
 * on this hardware, so these are the swapped forms. For copies, the texture
 * function word 0xa6887610 makes the vertex alpha multiply the texture alpha
 * (and not the colour), which gives a layer opacity on top of per-pixel alpha.
 */
#define MBX_BLEND_ALPHA		0xa1104620u	/* fills: alpha from the colour */
#define MBX_COPY_BLEND_W46	0xa6887610u	/* copies: src * a + dst * (1 - a) */
#define MBX_COPY_BLEND_W47	0xab504ea0u	/* (exact at full opacity) */

/*
 * With an opacity below 1 no single pass is exact for straight alpha (the
 * destination factor ignores the vertex alpha), so blend in two passes:
 * 1: dst * (1 - a * opa); 2: dst + src * a * opa. Found by sweeping MBX2D's
 * blend encodings on the iPod.
 */
enum mbx_copy_blend { MBX_COPY_ALPHA, MBX_COPY_PASS1, MBX_COPY_PASS2 };

static inline void mbx_cmd_copy_blend(uint32_t *p, enum mbx_copy_blend mode)
{
	static const uint32_t w[3][2] = {
		{ 0xa6887610, 0xab504ea0 },
		{ 0xa6884710, 0xae584eb0 },
		{ 0xa6887610, 0xd3407680 },
	};

	p[46] = w[mode][0];
	p[47] = w[mode][1];
}

static inline uint32_t mbx_f(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

static inline int mbx_log2_ceil8(uint32_t n)
{
	int l = 3;

	while ((1u << l) < n)
		l++;
	return l;
}

/* Common destination words 0-9 and the quad's vertices (10-17 / 15-22). */
static inline void mbx_cmd_dst(uint32_t *p, uint32_t dstride, int x0, int y0, int x1, int y1)
{
	p[0] = mbx_f(x0); p[1] = mbx_f(y0); p[2] = mbx_f(x1); p[3] = mbx_f(y1);
	p[4] = 0x18000; p[5] = dstride / 4; p[6] = 6; p[7] = dstride / 4;
	p[8] = (x0 & ~7) | (uint32_t)((x1 + 7) & ~7) << 16;
	p[9] = (y0 & ~15) | (uint32_t)((y1 + 15) & ~15) << 16;
}

/* Type 6 (fill) or 7 (fill blended with MBX_BLEND_ALPHA). Returns words. */
static inline int mbx_cmd_fill(uint32_t *p, int blend, uint32_t dstride, int x0, int y0,
			       int x1, int y1, uint32_t argb)
{
	const float s = 1.0f / 1024;

	mbx_cmd_dst(p, dstride, x0, y0, x1, y1);
	p[10] = mbx_f(x0); p[11] = mbx_f(y1); p[12] = mbx_f(x0); p[13] = mbx_f(y0);
	p[14] = mbx_f(x1); p[15] = mbx_f(y1); p[16] = mbx_f(x1); p[17] = mbx_f(y0);
	p[18] = argb;
	if (!blend)
		return 19;
	p[19] = mbx_f(x0 * s); p[20] = mbx_f(y1 * s); p[21] = mbx_f(x0 * s); p[22] = mbx_f(y0 * s);
	p[23] = mbx_f(x1 * s); p[24] = mbx_f(y1 * s); p[25] = mbx_f(x1 * s); p[26] = mbx_f(y0 * s);
	p[27] = MBX_BLEND_ALPHA;
	return 28;
}

/*
 * Type 8 (copy) or 9 (copy blended: src alpha * opa over the destination).
 * Copies w x h pixels from (sx, sy) of the source to (dx, dy).
 */
static inline int mbx_cmd_copy(uint32_t *p, int blend, uint32_t dstride, uint32_t sstride,
			       int sx, int sy, int dx, int dy, int w, int h, uint8_t opa)
{
	uint32_t spx = sstride / 4, right = sx + w, bottom = sy + h;
	float tw, th;
	int lw, lh, i;

	if (spx > 8 && !(spx & (spx - 1)))
		right = spx;	/* power-of-two stride: texture as wide as a row */
	lw = mbx_log2_ceil8(right);
	lh = mbx_log2_ceil8(bottom);
	tw = (float)(1u << lw);
	th = (float)(1u << lh);

	mbx_cmd_dst(p, dstride, dx, dy, dx + w, dy + h);
	p[10] = 0;
	p[11] = lw - 3;
	p[12] = lh - 3;
	p[13] = 0x18000;			/* source ARGB8888 */
	p[14] = spx;
	p[15] = mbx_f(dx); p[16] = mbx_f(dy + h); p[17] = mbx_f(dx); p[18] = mbx_f(dy);
	p[19] = mbx_f(dx + w); p[20] = mbx_f(dy + h); p[21] = mbx_f(dx + w); p[22] = mbx_f(dy);
	p[23] = mbx_f(sx / tw); p[24] = mbx_f((sy + h) / th);
	p[25] = mbx_f(sx / tw); p[26] = mbx_f(sy / th);
	p[27] = mbx_f((sx + w) / tw); p[28] = mbx_f((sy + h) / th);
	p[29] = mbx_f((sx + w) / tw); p[30] = mbx_f(sy / th);
	p[31] = 0;				/* no filtering */
	p[32] = blend ? (uint32_t)opa << 24 | 0xffffff : 0xffffffff;
	p[33] = 0;
	for (i = 34; i < 38; i++)
		p[i] = 0x3f800000;
	if (!blend)
		return 38;
	{
		const float s = 1.0f / 1024;

		p[38] = mbx_f(dx * s); p[39] = mbx_f((dy + h) * s);
		p[40] = mbx_f(dx * s); p[41] = mbx_f(dy * s);
		p[42] = mbx_f((dx + w) * s); p[43] = mbx_f((dy + h) * s);
		p[44] = mbx_f((dx + w) * s); p[45] = mbx_f(dy * s);
	}
	p[46] = MBX_COPY_BLEND_W46;
	p[47] = MBX_COPY_BLEND_W47;
	return 48;
}

/*
 * Type 8 copy, scaled: the source rectangle (sx0, sy0)-(sx1, sy1), in
 * fractional pixels, stretched over the destination (dx0, dy0)-(dx1, dy1)
 * (ends exclusive). Opaque, point sampled (the filtering word, p[31], stays
 * 0: its encoding for bilinear is not known yet). For pinch-zoom previews.
 */
static inline int mbx_cmd_copy_scaled(uint32_t *p, uint32_t dstride, uint32_t sstride,
				      float sx0, float sy0, float sx1, float sy1,
				      int dx0, int dy0, int dx1, int dy1)
{
	uint32_t spx = sstride / 4, right = (uint32_t)(sx1 + 0.999f), bottom = (uint32_t)(sy1 + 0.999f);
	float tw, th;
	int lw, lh, i;

	if (spx > 8 && !(spx & (spx - 1)))
		right = spx;	/* power-of-two stride: texture as wide as a row */
	lw = mbx_log2_ceil8(right);
	lh = mbx_log2_ceil8(bottom);
	tw = (float)(1u << lw);
	th = (float)(1u << lh);

	mbx_cmd_dst(p, dstride, dx0, dy0, dx1, dy1);
	p[10] = 0;
	p[11] = lw - 3;
	p[12] = lh - 3;
	p[13] = 0x18000;			/* source ARGB8888 */
	p[14] = spx;
	p[15] = mbx_f(dx0); p[16] = mbx_f(dy1); p[17] = mbx_f(dx0); p[18] = mbx_f(dy0);
	p[19] = mbx_f(dx1); p[20] = mbx_f(dy1); p[21] = mbx_f(dx1); p[22] = mbx_f(dy0);
	p[23] = mbx_f(sx0 / tw); p[24] = mbx_f(sy1 / th);
	p[25] = mbx_f(sx0 / tw); p[26] = mbx_f(sy0 / th);
	p[27] = mbx_f(sx1 / tw); p[28] = mbx_f(sy1 / th);
	p[29] = mbx_f(sx1 / tw); p[30] = mbx_f(sy0 / th);
	p[31] = 0;				/* no filtering */
	p[32] = 0xffffffff;
	p[33] = 0;
	for (i = 34; i < 38; i++)
		p[i] = 0x3f800000;
	return 38;
}

/*
 * Type 8 copy with a quarter turn, for a turned screen: the area (lx, ly,
 * w x h) of the upright picture LVGL draws (lw x lh) to where it lies on
 * the panel, as LVGL turns its pointer input. In continuous coordinates,
 * 90: panel (X, Y) = (ly, lw - lx); 270: (lh - ly, lx). Point sampling at
 * pixel centres, so the copy is exact.
 */
static inline int mbx_cmd_copy_turn(uint32_t *p, uint32_t dstride, uint32_t sstride,
				    int lx, int ly, int w, int h, int rot, int lw, int lh)
{
	uint32_t spx = sstride / 4, right = lx + w, bottom = ly + h;
	int tlw, tlh, i, X0, Y0, X1, Y1, k;
	float tw, th, vx[4], vy[4];

	if (spx > 8 && !(spx & (spx - 1)))
		right = spx;
	tlw = mbx_log2_ceil8(right);
	tlh = mbx_log2_ceil8(bottom);
	tw = (float)(1u << tlw);
	th = (float)(1u << tlh);
	if (rot == 90) {
		X0 = ly; X1 = ly + h; Y0 = lw - (lx + w); Y1 = lw - lx;
	} else {
		X0 = lh - (ly + h); X1 = lh - ly; Y0 = lx; Y1 = lx + w;
	}
	mbx_cmd_dst(p, dstride, X0, Y0, X1, Y1);
	p[10] = 0;
	p[11] = tlw - 3;
	p[12] = tlh - 3;
	p[13] = 0x18000;
	p[14] = spx;
	/* the quad's corners as mbx_cmd_copy orders them */
	vx[0] = X0; vy[0] = Y1; vx[1] = X0; vy[1] = Y0;
	vx[2] = X1; vy[2] = Y1; vx[3] = X1; vy[3] = Y0;
	for (k = 0; k < 4; k++) {
		float u, v;

		if (rot == 90) {
			u = lw - vy[k]; v = vx[k];
		} else {
			u = vy[k]; v = lh - vx[k];
		}
		p[15 + 2 * k] = mbx_f(vx[k]);
		p[16 + 2 * k] = mbx_f(vy[k]);
		p[23 + 2 * k] = mbx_f(u / tw);
		p[24 + 2 * k] = mbx_f(v / th);
	}
	p[31] = 0;
	p[32] = 0xffffffff;
	p[33] = 0;
	for (i = 34; i < 38; i++)
		p[i] = 0x3f800000;
	return 38;
}

#endif
