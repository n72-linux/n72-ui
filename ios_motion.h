/*
 * Smooth scrolling and animation motion curves, computed cheaply.
 *
 * Curves: Core Animation's cubic beziers as 65-entry Q16 tables, read with
 * linear interpolation (within 0.44/1024 of the exact curve; iOS's own
 * solver strays 3-15/1024, LVGL's ease paths 3-8/1024 with 64-bit divides).
 *
 * Paging spring: UIScrollView steps v += (target - x) * pull; v *= friction;
 * x += v once per ms. One step is linear in (x - target, v), so n steps are
 * the n-th power of a 2x2 matrix: kept for n = 1..64, a frame is 6 multiplies
 * whatever its length (iOS loops n times). iOS's per-frame rounding, 1 px
 * nudge and 5 px snap are kept, so the positions are iOS's exactly.
 */
#ifndef N72_IOS_MOTION_H
#define N72_IOS_MOTION_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum { IOS_DEFAULT, IOS_EASE_IN_OUT, IOS_EASE_IN, IOS_EASE_OUT, IOS_UIKIT4, IOS_CURVES };

extern const uint32_t ios_curve_tab[IOS_CURVES][65];

/* @p 0..1024 of the time -> 0..65536 of the way */
static inline uint32_t ios_curve(int c, int32_t p)
{
	const uint32_t *t = ios_curve_tab[c];
	uint32_t i, f;

	if (p <= 0)
		return 0;
	if (p >= 1024)
		return 65536;
	i = (uint32_t)p >> 4;		/* 1024 / 64 = 16 progress units an entry */
	f = (uint32_t)p & 15;
	return t[i] + (((t[i + 1] - t[i]) * f + 8) >> 4);
}

static inline int32_t ios_anim_value(const lv_anim_t *a, int c)
{
	int32_t p = a->duration ? (int32_t)((int64_t)a->act_time * 1024 / a->duration) : 1024;
	int64_t d = (int64_t)(a->end_value - a->start_value) * ios_curve(c, p);

	return a->start_value + (int32_t)((d + (d >= 0 ? 32768 : -32768)) / 65536);
}

/* lv_anim path callbacks */
int32_t ios_path_ease_in_out(const lv_anim_t *a);
int32_t ios_path_ease_in(const lv_anim_t *a);
int32_t ios_path_ease_out(const lv_anim_t *a);

/* ------------------------------------------------------- paging spring */

#define IOS_PAGE_MAX_STEPS	64

struct ios_spring_m { double a, b, c, d; };

struct ios_pager {
	float x;		/* displayed offset, whole px */
	double v;		/* px/ms, + towards larger offsets */
	float target;
	struct ios_spring_m pm[IOS_PAGE_MAX_STEPS + 1];	/* pm[n] = M^n */
};

/* UIScrollView's pull and friction for a page @w wide (initWithFrame:) */
void ios_pager_init(struct ios_pager *p, float w);
/* Advance @n whole ms; true once settled on target (ios_motion.c) */
bool ios_pager_step(struct ios_pager *p, int n);

#endif
