#define _GNU_SOURCE	/* pipe2 */
// SPDX-License-Identifier: MIT
/* The LVGL draw unit on the MBX: fills, layers and images on the 2D engine and the 3D core. */
#include "mbx_priv.h"

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

bool transparent;		/* (mbx_gpu_set_transparent) */

int32_t mbx_evaluate(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	const lv_draw_dsc_base_t *base = t->draw_dsc;
	lv_color_format_t dcf = base->layer->color_format;
	int32_t size = lv_area_get_size(&t->area);

	LV_UNUSED(u);
	if (t->type < 16)
		MBX_STAT_INC(seen[t->type]);
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
			MBX_STAT_INC(fill_radius);
		else if (d->grad.dir != LV_GRAD_DIR_NONE)
			MBX_STAT_INC(fill_grad);
		else if (size < 8192)
			MBX_STAT_INC(fill_small);
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
	MBX_STAT_INC(fills);
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
	MBX_STAT_ADD(trim_px, lv_area_get_size(&t->area) - px);
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
	MBX_STAT_INC(fill_corners);
}

/*
 * Areas of the display buffer this frame whose pixels are premultiplied
 * already (mbx_fill_straight): the flush leaves them alone. Reading them
 * back to convert them cost 13-19 ms a full screen (all cache misses).
 */
bool transparent;
lv_area_t pm_skip[PM_SKIP_MAX];
int pm_skip_n;

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
	MBX_STAT_INC(fills);
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
	MBX_STAT_INC(blends);
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
	MBX_STAT_INC(layers);
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
	MBX_STAT_INC(layers);
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
	MBX_STAT_INC(images);
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
		MBX_STAT_INC(images);
		return;
	}

	/* the CPU does not write snapshots after taking them: no clean */
	if (!gpu_copy(u, t, (lv_draw_buf_t *)d->src, d->image_area.x1, d->image_area.y1,
		      d->opa, false))
		lv_draw_sw_image(u, d, &t->area);
	MBX_STAT_INC(images);
}

#ifdef N72_DEBUG
/* N72_GPU_ADBG: every task the GPU draws, on stderr */
static int async_dbg = -1;
#define ADBG(...) do { if (async_dbg) fprintf(stderr, __VA_ARGS__); } while (0)
static int trim_on = -1;
#else
#define ADBG(...) do { } while (0)
#endif

static lv_layer_t *defer_layer;		/* a fill is held back in this layer */
static lv_draw_task_t *defer_t, *defer_tail;

int32_t mbx_dispatch(lv_draw_unit_t *u, lv_layer_t *layer)
{
	lv_draw_task_t *t;
#ifdef N72_DEBUG
	if (trim_on < 0)
		trim_on = !getenv("N72_NO_TRIM");
	if (async_dbg < 0)
		async_dbg = getenv("N72_GPU_ADBG") != NULL;
#else
	const bool trim_on = true;
#endif
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
