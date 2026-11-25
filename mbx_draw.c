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

/*
 * Premultiplied 1:1 ARGB8888 variable image (can live in GPU pool or heap).
 */
static bool image_premul_ok(const lv_draw_task_t *t)
{
	const lv_draw_image_dsc_t *d = t->draw_dsc;
	const lv_image_dsc_t *img = d->src;

	if (lv_image_src_get_type(d->src) != LV_IMAGE_SRC_VARIABLE || !img->data ||
	    img->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
	    !(img->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) || d->tile)
		return false;
	if (lv_area_get_width(&d->image_area) != img->header.w ||
	    lv_area_get_height(&d->image_area) != img->header.h)
		return false;
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
bool frame_cpu_dirty;
int32_t cpu_dirty_y1, cpu_dirty_y2 = -1;
bool frame_drew_any;

static inline void note_cpu_draw(const lv_layer_t *layer, const lv_area_t *area)
{
	lv_display_t *d = lv_display_get_default();

	if (d && d->buf_1 && layer->draw_buf && layer->draw_buf->data == d->buf_1->data) {
		int32_t h = (int32_t)layer->draw_buf->header.h;
		int32_t y1 = 0, y2 = h - 1;

		mbx_kms_note_cpu_touch(layer->draw_buf->data);
		if (area) {
			y1 = area->y1 - layer->buf_area.y1;
			y2 = area->y2 - layer->buf_area.y1;
			if (y1 < 0)
				y1 = 0;
			if (y2 >= h)
				y2 = h - 1;
		}
		if (y1 <= y2) {
			if (!frame_cpu_dirty || cpu_dirty_y1 > cpu_dirty_y2) {
				cpu_dirty_y1 = y1;
				cpu_dirty_y2 = y2;
			} else {
				if (y1 < cpu_dirty_y1)
					cpu_dirty_y1 = y1;
				if (y2 > cpu_dirty_y2)
					cpu_dirty_y2 = y2;
			}
		}
		frame_cpu_dirty = true;
		frame_drew_any = true;
	}
}

static inline void note_gpu_draw(const lv_layer_t *layer)
{
	lv_display_t *d = lv_display_get_default();

	if (d && d->buf_1 && layer->draw_buf && layer->draw_buf->data == d->buf_1->data)
		frame_drew_any = true;
}

static bool ta_target_ok(const lv_layer_t *layer);

int32_t mbx_evaluate(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	const lv_draw_dsc_base_t *base = t->draw_dsc;
	lv_color_format_t dcf = base->layer->color_format;
	lv_area_t ca;
	int32_t size = lv_area_intersect(&ca, &t->area, &t->clip_area) ? lv_area_get_size(&ca) : 0;

	LV_UNUSED(u);
	if (t->type < 16)
		MBX_STAT_INC(seen[t->type]);
	if (!size || !cf_ok(dcf)) {
		if (size)
			note_cpu_draw(base->layer, &ca);
		return 0;
	}

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
		    size < 16384 || (d->opa < LV_OPA_MAX && !first_over(t))) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
	} else if (t->type == LV_DRAW_TASK_TYPE_FILL) {
		const lv_draw_fill_dsc_t *d = t->draw_dsc;
		bool ta_small = d->grad.dir == LV_GRAD_DIR_NONE && d->radius == 0 &&
				d->opa >= LV_OPA_MAX &&
				base->layer->draw_task_head &&
				base->layer->draw_task_head != t &&
				base->layer->draw_task_head->preferred_draw_unit_id == DRAW_UNIT_ID_MBX &&
				ta_target_ok(base->layer);
		int32_t min_sz = (d->opa >= LV_OPA_MAX || d->radius != 0) ? (ta_small ? 1 : 16384) : 8192;

		/*
		 * Rounded fills (opaque or large translucent on XRGB8888): the
		 * GPU fills the body cross, the CPU only the corner squares.
		 */
		if (d->grad.dir != LV_GRAD_DIR_NONE)
			MBX_STAT_INC(fill_grad);
		else if (d->radius != 0 && d->opa < LV_OPA_MAX && size < 16384)
			MBX_STAT_INC(fill_radius);
		else if (size < 8192)
			MBX_STAT_INC(fill_small);
		if (d->grad.dir != LV_GRAD_DIR_NONE || d->opa <= LV_OPA_MIN ||
		    size < min_sz || dcf != LV_COLOR_FORMAT_XRGB8888) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
	} else if (t->type == LV_DRAW_TASK_TYPE_LAYER) {
		const lv_draw_image_dsc_t *d = t->draw_dsc;
		const lv_layer_t *src = (const lv_layer_t *)d->src;
		int32_t min_sz;

		if (dcf != LV_COLOR_FORMAT_XRGB8888 || !layer_ok(t, src)) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
		min_sz = (src->color_format == LV_COLOR_FORMAT_ARGB8888 || d->opa < LV_OPA_MAX) ? 8192 : 4096;
		if (size < min_sz) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
	} else if (t->type == LV_DRAW_TASK_TYPE_IMAGE) {
		const lv_draw_image_dsc_t *d = t->draw_dsc;
		const lv_image_dsc_t *img = d->src;
		bool scaled = image_scaled_ok(t);
		bool premul = !scaled && dcf == LV_COLOR_FORMAT_ARGB8888 && transparent &&
			      image_premul_ok(t);

		if (scaled) {
			if (dcf != LV_COLOR_FORMAT_XRGB8888) {
				note_cpu_draw(base->layer, &ca);
				return 0;
			}
		} else if (!premul && !image_ok(t)) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
		if (dcf == LV_COLOR_FORMAT_ARGB8888 && !premul) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
		if (dcf != LV_COLOR_FORMAT_XRGB8888 && dcf != LV_COLOR_FORMAT_ARGB8888) {
			note_cpu_draw(base->layer, &ca);
			return 0;
		}
		if (!scaled && !premul) {
			int32_t min_sz = (img->header.cf == LV_COLOR_FORMAT_ARGB8888 || d->opa < LV_OPA_MAX) ? 8192 : 4096;

			if (size < min_sz) {
				note_cpu_draw(base->layer, &ca);
				return 0;
			}
		}
	} else {
		note_cpu_draw(base->layer, &ca);
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

static bool next_same_2d_fill(const lv_draw_task_t *n, const lv_layer_t *layer)
{
	const lv_draw_fill_dsc_t *d;

	if (!n || n->state != LV_DRAW_TASK_STATE_QUEUED ||
	    n->preferred_draw_unit_id != DRAW_UNIT_ID_MBX ||
	    n->type != LV_DRAW_TASK_TYPE_FILL ||
	    ((const lv_draw_dsc_base_t *)n->draw_dsc)->layer != layer)
		return false;
	d = n->draw_dsc;
	return d->opa >= LV_OPA_MAX && d->radius == 0 && d->grad.dir == LV_GRAD_DIR_NONE;
}

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
		rect_flush();
		for (i = 0; i < n; i++) {
			note_cpu_draw(layer, &rects[i]);
			unit->clip_area = &rects[i];
			lv_draw_sw_fill(unit, t->draw_dsc, &t->area);
		}
		unit->clip_area = clip;
		return;
	}
	note_gpu_draw(layer);
	for (i = 0; i < n; i++)
		rect_add(layer, clip, &rects[i], color);
	if (!next_same_2d_fill(t->next, layer))
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
		rect_flush();
		note_cpu_draw(layer, &a);
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
	note_gpu_draw(layer);
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
		note_cpu_draw(layer, &cclip);
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

	rect_flush();
	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return;
	scr = a;
	word = (uint32_t)d->opa << 24 | (lv_color_to_u32(d->color) & 0xffffff);
	if (transparent && b->data == lv_display_get_default()->buf_1->data &&
	    pm_skip_n < PM_SKIP_MAX && !later_over(t, &scr)) {
		word = premul_word(d->opa, d->color);
		pm_skip[pm_skip_n++] = scr;	/* (screen = buffer coordinates here) */
	}
	if (transparent || mbx_gpu_is_suspended() || !in_pool(b->data)) {
		int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a), x, y;
		uint32_t stride = b->header.stride;
		uint8_t *dst = b->data + (a.y1 - layer->buf_area.y1) * (int32_t)stride +
			       (a.x1 - layer->buf_area.x1) * 4;

		for (y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)dst;

			for (x = 0; x + 3 < w; x += 4) {
				row[x] = word;
				row[x + 1] = word;
				row[x + 2] = word;
				row[x + 3] = word;
			}
			for (; x < w; x++)
				row[x] = word;
			dst += stride;
		}
		note_cpu_draw(layer, &scr);
		MBX_STAT_INC(fills);
		return;
	}
	note_gpu_draw(layer);
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	rect_buf = b;
	rect_rows = a;
	rect_n = put_fill(rect_words, gpu_addr(b->data), b->header.stride, word,
			  a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a));
	rect_flush();
	MBX_STAT_INC(fills);
}

static bool blit_blend_band(lv_draw_unit_t *u, const lv_area_t *band, uint32_t argb)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_area_t a;
	uint32_t w[48];

	if (!lv_area_intersect(&a, band, u->clip_area))
		return true;
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	return !blit3d(7, gpu_addr(b->data), 0, w,
		       mbx_cmd_fill(w, 1, b->header.stride, a.x1, a.y1, a.x2 + 1, a.y2 + 1, argb));
}

/* Blend a translucent fill (plain or rounded body + CPU corners) with the 3D core. */
static void mbx_fill_blend(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_draw_fill_dsc_t *d = t->draw_dsc;
	const lv_area_t *clip = u->clip_area, *c = &t->area;
	int32_t wd = lv_area_get_width(c), ht = lv_area_get_height(c);
	int32_t r = LV_MIN(d->radius, LV_MIN(wd, ht) / 2);
	uint32_t argb = (uint32_t)d->opa << 24 | (lv_color_to_u32(d->color) & 0xffffff);
	lv_area_t a, da;

	rect_flush();
	if (!lv_area_intersect(&a, c, clip))
		return;
	if (!in_pool(b->data) || (b->header.stride & 31)) {
		note_cpu_draw(layer, &a);
		lv_draw_sw_fill(u, d, c);
		return;
	}
	da = a;
	lv_area_move(&da, -layer->buf_area.x1, -layer->buf_area.y1);
	if (r <= 0) {
		uint32_t w[48];

		buf_invalidate(b, &da);
		note_gpu_draw(layer);
		if (blit3d(7, gpu_addr(b->data), 0, w,
			   mbx_cmd_fill(w, 1, b->header.stride, da.x1, da.y1,
					da.x2 + 1, da.y2 + 1, argb))) {
			note_cpu_draw(layer, &a);
			lv_draw_sw_fill(u, d, c);
		}
		MBX_STAT_INC(blends);
		return;
	}
	{
		lv_area_t mid = { c->x1, c->y1 + r, c->x2, c->y2 - r };
		lv_area_t top = { c->x1 + r, c->y1, c->x2 - r, c->y1 + r - 1 };
		lv_area_t bot = { c->x1 + r, c->y2 - r + 1, c->x2 - r, c->y2 };
		lv_area_t ca, corner, cclip;
		int32_t body_px = 0;
		int i;

		if (mid.y2 >= mid.y1 && lv_area_intersect(&ca, &mid, clip))
			body_px += lv_area_get_size(&ca);
		if (top.x2 >= top.x1 && lv_area_intersect(&ca, &top, clip))
			body_px += lv_area_get_size(&ca);
		if (bot.x2 >= bot.x1 && lv_area_intersect(&ca, &bot, clip))
			body_px += lv_area_get_size(&ca);
		if (body_px < 4096) {
			note_cpu_draw(layer, &a);
			lv_draw_sw_fill(u, d, c);
			return;
		}
		buf_invalidate(b, &da);
		note_gpu_draw(layer);
		if (mid.y2 >= mid.y1 && !blit_blend_band(u, &mid, argb)) {
			if (lv_area_intersect(&cclip, &mid, clip)) {
				note_cpu_draw(layer, &cclip);
				u->clip_area = &cclip;
				lv_draw_sw_fill(u, d, c);
			}
		}
		if (top.x2 >= top.x1 && !blit_blend_band(u, &top, argb)) {
			if (lv_area_intersect(&cclip, &top, clip)) {
				note_cpu_draw(layer, &cclip);
				u->clip_area = &cclip;
				lv_draw_sw_fill(u, d, c);
			}
		}
		if (bot.x2 >= bot.x1 && !blit_blend_band(u, &bot, argb)) {
			if (lv_area_intersect(&cclip, &bot, clip)) {
				note_cpu_draw(layer, &cclip);
				u->clip_area = &cclip;
				lv_draw_sw_fill(u, d, c);
			}
		}
		for (i = 0; i < 4; i++) {
			corner.x1 = i & 1 ? c->x2 - r + 1 : c->x1;
			corner.x2 = corner.x1 + r - 1;
			corner.y1 = i & 2 ? c->y2 - r + 1 : c->y1;
			corner.y2 = corner.y1 + r - 1;
			if (!lv_area_intersect(&cclip, &corner, clip))
				continue;
			note_cpu_draw(layer, &cclip);
			u->clip_area = &cclip;
			lv_draw_sw_fill(u, d, c);
		}
		u->clip_area = clip;
		MBX_STAT_INC(blends);
		MBX_STAT_INC(fill_corners);
	}
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

	rect_flush();
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
	note_gpu_draw(layer);
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
	if (!gpu_copy(u, t, src->draw_buf, src->buf_area.x1, src->buf_area.y1, d->opa, true)) {
		lv_area_t a;

		if (lv_area_intersect(&a, &t->area, u->clip_area))
			note_cpu_draw(u->target_layer, &a);
		lv_draw_sw_layer(u, d, &t->area);	/* software fallback */
	}
}

/* Draw a pool-resident image 1:1 (a snapshot, during transitions). */
/*
 * A premultiplied picture onto the transparent display: CPU row memcpy when
 * GPU-suspended or off-pool (or in transparent mode to avoid mbx->lock stalls
 * behind games), else 2D engine's exact copy; flush told not to premultiply
 * it again. What it covers is replaced, not blended.
 */
static bool mbx_image_premul(lv_draw_unit_t *u, lv_draw_task_t *t, lv_draw_buf_t *sb)
{
	lv_draw_image_dsc_t *d = t->draw_dsc;
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	lv_area_t a, sa, da;
	uint32_t w[16];

	rect_flush();
	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return true;
	sa = a;
	lv_area_move(&sa, -d->image_area.x1, -d->image_area.y1);
	da = a;
	lv_area_move(&da, -layer->buf_area.x1, -layer->buf_area.y1);
	if (transparent || mbx_gpu_is_suspended() || !in_pool(b->data) || !in_pool(sb->data)) {
		int32_t rw = lv_area_get_width(&a), rh = lv_area_get_height(&a), y;
		size_t bytes = (size_t)rw * 4;
		uint8_t *dst = b->data + da.y1 * (int32_t)b->header.stride + da.x1 * 4;
		const uint8_t *src = sb->data + sa.y1 * (int32_t)sb->header.stride + sa.x1 * 4;

		for (y = 0; y < rh; y++) {
			memcpy(dst, src, bytes);
			dst += b->header.stride;
			src += sb->header.stride;
		}
		note_cpu_draw(layer, &a);
		if (b->data == lv_display_get_default()->buf_1->data && pm_skip_n < PM_SKIP_MAX &&
		    !later_over(t, &a))
			pm_skip[pm_skip_n++] = a;
		MBX_STAT_INC(layers);
		return true;
	}
	buf_invalidate(b, &da);
	note_gpu_draw(layer);
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

	rect_flush();
	if (!lv_area_intersect(&a, ia, u->clip_area))
		return true;
	if (!in_pool(b->data) || (b->header.stride & 31))
		return false;
	da = a;
	lv_area_move(&da, -layer->buf_area.x1, -layer->buf_area.y1);
	buf_invalidate(b, &da);
	note_gpu_draw(layer);
	n = mbx_cmd_copy_scaled(w, b->header.stride, sb->header.stride,
				sb->header.w, sb->header.h,
				(a.x1 - ia->x1) * fx, (a.y1 - ia->y1) * fy,
				(a.x2 + 1 - ia->x1) * fx, (a.y2 + 1 - ia->y1) * fy,
				da.x1, da.y1, da.x2 + 1, da.y2 + 1);
	MBX_STAT_INC(images);
	return !blit3d(8, gpu_addr(b->data), gpu_addr(sb->data), w, n);
}

static void mbx_image(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_draw_image_dsc_t *d = t->draw_dsc;
	lv_area_t ca;

	if (image_scaled_ok(t)) {
		if (!gpu_copy_scaled(u, t, (lv_draw_buf_t *)d->src)) {
			if (lv_area_intersect(&ca, &t->area, u->clip_area))
				note_cpu_draw(u->target_layer, &ca);
			lv_draw_sw_image(u, d, &t->area);	/* (1:1, unscaled) */
		}
		return;
	}

	if (u->target_layer->draw_buf->header.cf == LV_COLOR_FORMAT_ARGB8888) {
		if (!mbx_image_premul(u, t, (lv_draw_buf_t *)d->src)) {
			if (lv_area_intersect(&ca, &t->area, u->clip_area))
				note_cpu_draw(u->target_layer, &ca);
			lv_draw_sw_image(u, d, &t->area);
		}
		MBX_STAT_INC(images);
		return;
	}

	/* the CPU does not write snapshots after taking them: no clean */
	if (!gpu_copy(u, t, (lv_draw_buf_t *)d->src, d->image_area.x1, d->image_area.y1,
		      d->opa, false)) {
		if (lv_area_intersect(&ca, &t->area, u->clip_area))
			note_cpu_draw(u->target_layer, &ca);
		lv_draw_sw_image(u, d, &t->area);
	}
	MBX_STAT_INC(images);
}

/* ------------------------------------------- direct TA composite batcher */

#define TA_RING_BUFS	4
#define TA_RING_WORDS	2048
#define TA_HDR_STRIP	0x48000000u
#define TA_HDR_LIST	0x78020000u
#define TA_VGP_ENABLES	0xb00e4000u
#define TA_FMT_XYZC	0x503u
#define TA_FMT_XYZCUV	0x2503u
#define TA_TSP_PLAIN	0xe0000000u
#define TA_ISP_FILL_OPA	0x22200e80u
#define TA_ISP_FILL_BLD	0x96202e80u
#define TA_ISP_TEX_OPA	0x22220e80u
#define TA_ISP_TEX_BLD	0x96222e80u

static struct mbx_buf ta_ring[TA_RING_BUFS];
static uint32_t ta_slot_seq[TA_RING_BUFS];
static unsigned ta_ring_idx;
static int ta_ring_state;	/* 0 = uninit, 1 = ok, -1 = unavailable */
uint32_t ta_active_seq;

void ta_wait_active(void)
{
	if (ta_active_seq && mbx >= 0) {
		struct mbx_wait w = { .wait_seq = ta_active_seq, .timeout_ms = 1000 };

		mbx_wait_jobs(mbx, &w);
	}
	ta_active_seq = 0;
	memset(ta_slot_seq, 0, sizeof(ta_slot_seq));
}

void mbx_gpu_wait_ta(void)
{
	ta_wait_active();
}

void ta_ring_free(void)
{
	int i;

	ta_wait_active();
	for (i = 0; i < TA_RING_BUFS; i++) {
		if (ta_ring[i].cpu) {
			mbx_free(mbx, &ta_ring[i]);
			memset(&ta_ring[i], 0, sizeof(ta_ring[i]));
		}
	}
	ta_ring_idx = 0;
	ta_ring_state = 0;
}

static bool ta_ring_ensure(void)
{
	int i;

	if (ta_ring_state)
		return ta_ring_state > 0;
	for (i = 0; i < TA_RING_BUFS; i++) {
		if (mbx_alloc(mbx, TA_RING_WORDS * 4,
			      MBX_ALLOC_CONTIG | MBX_ALLOC_CACHED | MBX_ALLOC_NO_GPU,
			      &ta_ring[i])) {
			while (--i >= 0)
				mbx_free(mbx, &ta_ring[i]);
			ta_ring_state = -1;
			return false;
		}
	}
	ta_ring_state = 1;
	return true;
}

static inline uint32_t ta_abgr(lv_color_t c, uint8_t a)
{
	return ((uint32_t)a << 24) | ((uint32_t)c.blue << 16) | ((uint32_t)c.green << 8) | c.red;
}

static bool ta_target_ok(const lv_layer_t *layer)
{
	const lv_draw_buf_t *b = layer->draw_buf;

	return !transparent && layer->color_format == LV_COLOR_FORMAT_XRGB8888 &&
	       layer->buf_area.x1 == 0 && layer->buf_area.y1 == 0 &&
	       b && in_pool(b->data) && !(gpu_addr(b->data) & 127) &&
	       !(b->header.stride & 63) && b->header.w > 0 && b->header.h > 0 &&
	       b->header.w <= 1024 && b->header.h <= 1024;
}

static bool ta_src_ok(const lv_draw_buf_t *sb, lv_opa_t opa, bool allow_blend)
{
	uint32_t spx, right;

	if (!sb || !in_pool(sb->data) || (gpu_addr(sb->data) & 127) || (sb->header.stride & 31))
		return false;
	if (!sb->header.w || !sb->header.h || sb->header.w > 1024 || sb->header.h > 1024)
		return false;
	spx = sb->header.stride / 4;
	if (spx > 2040)
		return false;
	right = (spx > 8 && !(spx & (spx - 1))) ? spx : sb->header.w;
	if (right > 1024)
		return false;
	if (sb->header.cf == LV_COLOR_FORMAT_ARGB8888)
		return allow_blend;
	if (sb->header.cf == LV_COLOR_FORMAT_XRGB8888)
		return opa >= LV_OPA_MAX || allow_blend;
	return false;
}

static bool ta_task_ok(const lv_draw_task_t *t, bool allow_blend)
{
	if (t->type == LV_DRAW_TASK_TYPE_FILL) {
		const lv_draw_fill_dsc_t *d = t->draw_dsc;

		return d->grad.dir == LV_GRAD_DIR_NONE && d->radius == 0 &&
		       d->opa > LV_OPA_MIN && (d->opa >= LV_OPA_MAX || allow_blend);
	}
	if (t->type == LV_DRAW_TASK_TYPE_IMAGE) {
		const lv_draw_image_dsc_t *d = t->draw_dsc;

		if (image_scaled_ok(t))
			return ta_src_ok((const lv_draw_buf_t *)d->src, d->opa, allow_blend);
		if (!image_ok(t) || d->opa <= LV_OPA_MIN)
			return false;
		return ta_src_ok((const lv_draw_buf_t *)d->src, d->opa, allow_blend);
	}
	if (t->type == LV_DRAW_TASK_TYPE_LAYER) {
		const lv_draw_image_dsc_t *d = t->draw_dsc;
		const lv_layer_t *src = (const lv_layer_t *)d->src;

		return layer_ok(t, src) && d->opa > LV_OPA_MIN &&
		       ta_src_ok(src->draw_buf, d->opa, allow_blend);
	}
	return false;
}

struct ta_stream {
	uint32_t *w;
	uint32_t n;
	uint32_t last_isp, last_fmt, last_tex[3];
	uint32_t block_tris;
	bool have_state;
};

static void ta_begin(struct ta_stream *s, uint32_t *words)
{
	uint32_t n = 0;

	words[n++] = 0x10000010u; words[n++] = 0;
	words[n++] = 0x10000073u; words[n++] = 0x22200e00u; words[n++] = TA_TSP_PLAIN;
	words[n++] = 0; words[n++] = TA_FMT_XYZC; words[n++] = 0;
	words[n++] = TA_HDR_LIST; words[n++] = TA_VGP_ENABLES;
	words[n++] = 0; words[n++] = mbx_f(-2.f); words[n++] = 0; words[n++] = 0; words[n++] = 0;
	words[n++] = 0; words[n++] = mbx_f(-2.f); words[n++] = mbx_f(1.f); words[n++] = 0; words[n++] = 0;
	words[n++] = 0; words[n++] = mbx_f(-1.f); words[n++] = 0; words[n++] = 0; words[n++] = 0;
	words[n++] = 3;
	s->w = words;
	s->n = n;
	s->block_tris = 0;
	s->have_state = false;
}

static bool ta_emit_quad(struct ta_stream *s, uint32_t isp, const uint32_t *tex,
			 int x0, int y0, int x1, int y1, float u0, float v0,
			 float u1, float v1, uint32_t abgr)
{
	uint32_t fmt = tex ? TA_FMT_XYZCUV : TA_FMT_XYZC;
	bool same = s->have_state && isp == s->last_isp && fmt == s->last_fmt &&
		    (!tex || (tex[0] == s->last_tex[0] && tex[1] == s->last_tex[1] &&
			      tex[2] == s->last_tex[2]));
	uint32_t need = (same ? (s->block_tris ? 1u : 2u) :
			 ((s->block_tris ? 1u : 0u) + (tex ? 8u : 5u) + 2u)) +
			4u * (tex ? 7u : 5u) + 2u;
	uint32_t *p = s->w + s->n;
	uint32_t z = mbx_f(0.5f);

	if (s->n + need > TA_RING_WORDS)
		return false;
	if (!same) {
		if (s->block_tris) {
			*p++ = 3;
			s->block_tris = 0;
		}
		*p++ = 0x10000063u | (tex ? 4u : 0u);
		*p++ = isp;
		*p++ = TA_TSP_PLAIN;
		if (tex) {
			*p++ = tex[0];
			*p++ = tex[1];
			*p++ = tex[2];
			s->last_tex[0] = tex[0];
			s->last_tex[1] = tex[1];
			s->last_tex[2] = tex[2];
		}
		*p++ = fmt;
		*p++ = 0;
		s->last_isp = isp;
		s->last_fmt = fmt;
		s->have_state = true;
	}
	if (s->block_tris >= 32) {
		*p++ = 3;
		s->block_tris = 0;
	}
	if (!s->block_tris) {
		*p++ = TA_HDR_STRIP;
		*p++ = TA_VGP_ENABLES;
	} else {
		*p++ = 2;
	}
	if (tex) {
		uint32_t fx0 = mbx_f((float)x0), fx1 = mbx_f((float)x1);
		uint32_t fy0 = mbx_f((float)y0), fy1 = mbx_f((float)y1);
		uint32_t fu0 = mbx_f(u0), fu1 = mbx_f(u1), fv0 = mbx_f(v0), fv1 = mbx_f(v1);

		p[0] = 0; p[1] = fx0; p[2] = fy0; p[3] = z; p[4] = abgr; p[5] = fu0; p[6] = fv0;
		p[7] = 0; p[8] = fx0; p[9] = fy1; p[10] = z; p[11] = abgr; p[12] = fu0; p[13] = fv1;
		p[14] = 0; p[15] = fx1; p[16] = fy0; p[17] = z; p[18] = abgr; p[19] = fu1; p[20] = fv0;
		p[21] = 0; p[22] = fx1; p[23] = fy1; p[24] = z; p[25] = abgr; p[26] = fu1; p[27] = fv1;
		p += 28;
	} else {
		uint32_t fx0 = mbx_f((float)x0), fx1 = mbx_f((float)x1);
		uint32_t fy0 = mbx_f((float)y0), fy1 = mbx_f((float)y1);

		p[0] = 0; p[1] = fx0; p[2] = fy0; p[3] = z; p[4] = abgr;
		p[5] = 0; p[6] = fx0; p[7] = fy1; p[8] = z; p[9] = abgr;
		p[10] = 0; p[11] = fx1; p[12] = fy0; p[13] = z; p[14] = abgr;
		p[15] = 0; p[16] = fx1; p[17] = fy1; p[18] = z; p[19] = abgr;
		p += 20;
	}
	s->block_tris += 2;
	s->n = (uint32_t)(p - s->w);
	return true;
}

static bool ta_emit_task(struct ta_stream *s, const lv_draw_task_t *t, lv_area_t *dirty)
{
	lv_area_t ca;

	if (t->type == LV_DRAW_TASK_TYPE_FILL) {
		const lv_draw_fill_dsc_t *d = t->draw_dsc;
		uint32_t isp = d->opa >= LV_OPA_MAX ? TA_ISP_FILL_OPA : TA_ISP_FILL_BLD;
		uint32_t abgr = ta_abgr(d->color, d->opa >= LV_OPA_MAX ? 255 : d->opa);

		if (!lv_area_intersect(&ca, &t->area, &t->clip_area))
			return true;
		if (!ta_emit_quad(s, isp, NULL, ca.x1, ca.y1, ca.x2 + 1, ca.y2 + 1,
				  0.f, 0.f, 0.f, 0.f, abgr))
			return false;
	} else {
		const lv_draw_image_dsc_t *d = t->draw_dsc;
		const lv_layer_t *src_l = t->type == LV_DRAW_TASK_TYPE_LAYER ?
					  (const lv_layer_t *)d->src : NULL;
		const lv_draw_buf_t *sb = src_l ? src_l->draw_buf : (const lv_draw_buf_t *)d->src;
		bool scaled = t->type == LV_DRAW_TASK_TYPE_IMAGE && image_scaled_ok(t);
		uint32_t spx = sb->header.stride / 4;
		uint32_t right = (spx > 8 && !(spx & (spx - 1))) ? spx : sb->header.w;
		int lw = mbx_log2_ceil8(right), lh = mbx_log2_ceil8(sb->header.h);
		float tw = (float)(1u << lw), th = (float)(1u << lh);
		float sx0, sy0, sx1, sy1;
		uint32_t tex[3], isp, abgr;
		bool opq_s = sb->header.cf == LV_COLOR_FORMAT_XRGB8888;
		bool opq_d = d->opa >= LV_OPA_MAX;

		if (scaled) {
			const lv_area_t *ia = &d->image_area;
			float fx = (float)sb->header.w / lv_area_get_width(ia);
			float fy = (float)sb->header.h / lv_area_get_height(ia);

			if (!lv_area_intersect(&ca, ia, &t->clip_area) ||
			    !lv_area_intersect(&ca, &ca, &t->area))
				return true;
			sx0 = (ca.x1 - ia->x1) * fx;
			sy0 = (ca.y1 - ia->y1) * fy;
			sx1 = (ca.x2 + 1 - ia->x1) * fx;
			sy1 = (ca.y2 + 1 - ia->y1) * fy;
			if (sb->header.w > 1) {
				float maxx = sb->header.w - 0.5f;

				if (sx0 < 0.5f) sx0 = 0.5f; else if (sx0 > maxx) sx0 = maxx;
				if (sx1 < 0.5f) sx1 = 0.5f; else if (sx1 > maxx) sx1 = maxx;
			}
			if (sb->header.h > 1) {
				float maxy = sb->header.h - 0.5f;

				if (sy0 < 0.5f) sy0 = 0.5f; else if (sy0 > maxy) sy0 = maxy;
				if (sy1 < 0.5f) sy1 = 0.5f; else if (sy1 > maxy) sy1 = maxy;
			}
		} else {
			int32_t sx_off = src_l ? src_l->buf_area.x1 : d->image_area.x1;
			int32_t sy_off = src_l ? src_l->buf_area.y1 : d->image_area.y1;

			if (!lv_area_intersect(&ca, &t->area, &t->clip_area))
				return true;
			sx0 = (float)(ca.x1 - sx_off);
			sy0 = (float)(ca.y1 - sy_off);
			sx1 = (float)(ca.x2 + 1 - sx_off);
			sy1 = (float)(ca.y2 + 1 - sy_off);
		}
		tex[0] = (2u << 30) | (2u << 28) | ((uint32_t)(lw - 3) << 24) |
			 ((uint32_t)(lh - 3) << 20) | 0x18000u | ((spx & 8u) ? 1u : 0u);
		tex[1] = (scaled ? 0x80000000u : 0u) | (7u << 25) |
			 ((gpu_addr(sb->data) >> 7) & 0x3ffffu) | ((spx & 0x7f0u) << 14);
		tex[2] = opq_s ? (opq_d ? 0xd6086610u : 0x86086610u)
			       : (opq_d ? 0xd6087610u : 0x86084610u);
		isp = (opq_s && opq_d) ? TA_ISP_TEX_OPA : TA_ISP_TEX_BLD;
		abgr = opq_d ? 0xffffffffu : (((uint32_t)d->opa << 24) | 0x00ffffffu);
		if (!ta_emit_quad(s, isp, tex, ca.x1, ca.y1, ca.x2 + 1, ca.y2 + 1,
				  sx0 / tw, sy0 / th, sx1 / tw, sy1 / th, abgr))
			return false;
	}
	if (dirty->x1 > dirty->x2) {
		*dirty = ca;
	} else {
		dirty->x1 = LV_MIN(dirty->x1, ca.x1);
		dirty->y1 = LV_MIN(dirty->y1, ca.y1);
		dirty->x2 = LV_MAX(dirty->x2, ca.x2);
		dirty->y2 = LV_MAX(dirty->y2, ca.y2);
	}
	return true;
}

static int ta_batch_run(lv_layer_t *layer, lv_draw_task_t *first)
{
	lv_draw_buf_t *b = layer->draw_buf;
	bool allow_blend = !kms_shell;
	lv_draw_task_t *u, *last = NULL;
	lv_area_t dirty = { 1, 1, 0, 0 }, ca;
	struct ta_stream s;
	struct mbx_ta_frame fr;
	bool use_clear_bg = false, can_async;
	uint32_t clear_argb = 0, idx;
	int count = 0;
	bool worthy = false;

	if (layer->draw_task_head != first || !ta_target_ok(layer) ||
	    !ta_task_ok(first, allow_blend))
		return 0;
	for (u = first; u && u->state == LV_DRAW_TASK_STATE_QUEUED &&
			u->preferred_draw_unit_id == DRAW_UNIT_ID_MBX &&
			ta_task_ok(u, allow_blend); u = u->next) {
		if (u->type == LV_DRAW_TASK_TYPE_IMAGE) {
			const lv_draw_image_dsc_t *d = u->draw_dsc;
			const lv_draw_buf_t *sb = (const lv_draw_buf_t *)d->src;

			if (image_scaled_ok(u) ||
			    (allow_blend && (d->opa < LV_OPA_MAX ||
					     sb->header.cf == LV_COLOR_FORMAT_ARGB8888)))
				worthy = true;
		} else if (u->type == LV_DRAW_TASK_TYPE_LAYER) {
			const lv_draw_image_dsc_t *d = u->draw_dsc;
			const lv_layer_t *sl = (const lv_layer_t *)d->src;

			if (allow_blend && (d->opa < LV_OPA_MAX ||
					    sl->color_format == LV_COLOR_FORMAT_ARGB8888))
				worthy = true;
		}
		count++;
		last = u;
	}
	if (count < 2 && !worthy)
		return 0;
	if (!ta_ring_ensure())
		return 0;

	rect_flush();
	idx = ta_ring_idx;
	if (ta_slot_seq[idx]) {
		struct mbx_wait w = { .wait_seq = ta_slot_seq[idx], .timeout_ms = 1000 };

		mbx_wait_jobs(mbx, &w);
		ta_slot_seq[idx] = 0;
	}

	ta_begin(&s, ta_ring[idx].cpu);
	u = first;
	if (first->type == LV_DRAW_TASK_TYPE_FILL && !frame_drew_any) {
		const lv_draw_fill_dsc_t *d = first->draw_dsc;

		if (d->opa >= LV_OPA_MAX &&
		    lv_area_intersect(&ca, &first->area, &first->clip_area) &&
		    ca.x1 == 0 && ca.y1 == 0 &&
		    ca.x2 == (int32_t)b->header.w - 1 &&
		    ca.y2 == (int32_t)b->header.h - 1) {
			use_clear_bg = true;
			clear_argb = 0xff000000u | lv_color_to_u32(d->color);
			dirty = ca;
			u = first->next;
		}
	}

	count = use_clear_bg ? 1 : 0;
	last = use_clear_bg ? first : NULL;
	for (; u && u->state == LV_DRAW_TASK_STATE_QUEUED &&
		  u->preferred_draw_unit_id == DRAW_UNIT_ID_MBX &&
		  ta_task_ok(u, allow_blend); u = u->next) {
		if (!ta_emit_task(&s, u, &dirty))
			break;
		count++;
		last = u;
	}
	if (!last || (count < 2 && !worthy))
		return 0;
	if (!use_clear_bg && !worthy && count == 2 &&
	    first->type == LV_DRAW_TASK_TYPE_FILL && last->type == LV_DRAW_TASK_TYPE_IMAGE) {
		lv_area_t cov, fca;

		if (task_covers(last, &cov) &&
		    lv_area_intersect(&fca, &first->area, &first->clip_area) &&
		    lv_area_is_in(&fca, &cov, 0))
			return 0;
	}

	if (s.block_tris) {
		s.w[s.n++] = 3;
		s.block_tris = 0;
	}
	s.w[s.n++] = 0xf0000000u;

	for (u = first; ; u = u->next) {
		if (u->type == LV_DRAW_TASK_TYPE_LAYER) {
			const lv_draw_image_dsc_t *d = u->draw_dsc;
			const lv_layer_t *sl = (const lv_layer_t *)d->src;
			lv_area_t sa;

			if (lv_area_intersect(&sa, &u->area, &u->clip_area)) {
				lv_area_move(&sa, -sl->buf_area.x1, -sl->buf_area.y1);
				buf_clean(sl->draw_buf, &sa);
			}
		}
		if (u == last)
			break;
	}

	if (frame_cpu_dirty) {
		lv_area_t inv = {
			0,
			cpu_dirty_y1 <= cpu_dirty_y2 ? cpu_dirty_y1 : 0,
			(int32_t)b->header.w - 1,
			cpu_dirty_y1 <= cpu_dirty_y2 ? cpu_dirty_y2 : (int32_t)b->header.h - 1,
		};

		if (dirty.x1 <= dirty.x2) {
			inv.y1 = LV_MIN(inv.y1, dirty.y1);
			inv.y2 = LV_MAX(inv.y2, dirty.y2);
		}
		buf_invalidate(b, &inv);
		mbx_kms_note_buf_invalidated(b->data, &inv);
	} else if (dirty.x1 <= dirty.x2 && mbx_kms_buf_needs_invalidate(b->data)) {
		buf_invalidate(b, &dirty);
		mbx_kms_note_buf_invalidated(b->data, &dirty);
	}

	can_async = mbx_kms_can_async_ta(layer, last);
	memset(&fr, 0, sizeof(fr));
	fr.words = ta_ring[idx].handle;
	fr.count = s.n;
	fr.dst_gpu = gpu_addr(b->data);
	fr.width = b->header.w;
	fr.height = b->header.h;
	fr.stride = b->header.stride;
	fr.flags = MBX_TA_GEM_WORDS;
	if (use_clear_bg) {
		fr.flags |= MBX_TA_CLEAR_BG | MBX_TA_DISCARD;
		fr.clear_color = clear_argb;
	}
	if (can_async)
		fr.flags |= MBX_TA_ASYNC | MBX_TA_FENCE;

	MBX_STAT_INC(submits);
	if (mbx_ta_submit(mbx, &fr))
		return 0;

	note_gpu_draw(layer);
	if (can_async) {
		ta_slot_seq[idx] = fr.seq;
		mbx_kms_note_async_ta(fr.seq);
		ta_ring_idx = (idx + 1) & (TA_RING_BUFS - 1);
	}

	for (u = first; ; u = u->next) {
		if (u->type == LV_DRAW_TASK_TYPE_FILL) {
			const lv_draw_fill_dsc_t *d = u->draw_dsc;

			if (d->opa >= LV_OPA_MAX)
				MBX_STAT_INC(fills);
			else
				MBX_STAT_INC(blends);
		} else if (u->type == LV_DRAW_TASK_TYPE_LAYER) {
			MBX_STAT_INC(layers);
		} else {
			MBX_STAT_INC(images);
		}
		u->state = LV_DRAW_TASK_STATE_READY;
		if (u == last)
			break;
	}
	return count;
}

#ifdef N72_DEBUG
/* N72_GPU_ADBG: every task the GPU draws, on stderr */
static int async_dbg = -1;
#define ADBG(...) do { if (async_dbg) fprintf(stderr, __VA_ARGS__); } while (0)
static int trim_on = -1, no_ta = -1;
#else
#define ADBG(...) do { } while (0)
#endif

static lv_layer_t *defer_layer;		/* a fill or TA batch is held back in this layer */
static lv_draw_task_t *defer_t, *defer_tail;

int32_t mbx_dispatch(lv_draw_unit_t *u, lv_layer_t *layer)
{
	lv_draw_task_t *t;
	bool want_defer;
	int batch;
#ifdef N72_DEBUG
	if (trim_on < 0)
		trim_on = !getenv("N72_NO_TRIM");
	if (no_ta < 0)
		no_ta = getenv("N72_NO_TA") != NULL;
	if (async_dbg < 0)
		async_dbg = getenv("N72_GPU_ADBG") != NULL;
#else
	const bool trim_on = true, no_ta = false;
#endif
	t = lv_draw_get_next_available_task(layer, NULL, DRAW_UNIT_ID_MBX);
	if (!t || t->preferred_draw_unit_id != DRAW_UNIT_ID_MBX)
		return LV_DRAW_UNIT_IDLE;
	if (!lv_draw_layer_alloc_buf(layer))
		return LV_DRAW_UNIT_IDLE;

	/*
	 * LVGL (without an OS) dispatches after every task it creates, so a
	 * page's background fill or early sprite would run before what follows
	 * exists as tasks. Hold back look-ahead candidates until a dispatch
	 * comes with no new task behind them.
	 */
	want_defer = (trim_on && trim_candidate(t)) ||
		     (!no_ta && layer->draw_task_head == t && ta_target_ok(layer) &&
		      ta_task_ok(t, !kms_shell));
	if (want_defer && (!defer_layer || defer_layer == layer)) {
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

	if (!no_ta && (batch = ta_batch_run(layer, t)) > 0) {
		ADBG("mbx: TA batch %d tasks on layer %p\n", batch, (void *)layer);
		lv_draw_dispatch_request();
		return batch;
	}

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
