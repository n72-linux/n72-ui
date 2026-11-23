// SPDX-License-Identifier: MIT
/* iOS 4.2.1's motion: the curve tables, the path callbacks and the paging spring (ios_motion.h) */
#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_anim_private.h"	/* (ios_motion.h reads act_time) */
#include "ios_motion.h"

const uint32_t ios_curve_tab[IOS_CURVES][65] = {
	/* default (0.25, 0.1, 0.25, 1) */ {
		0, 488, 1142, 1972, 2987, 4194, 5597, 7193, 8971,
		10916, 13005, 15207, 17489, 19817, 22159, 24485, 26772, 29001,
		31159, 33236, 35226, 37126, 38937, 40658, 42292, 43841, 45308,
		46697, 48012, 49254, 50429, 51538, 52586, 53575, 54509, 55388,
		56218, 56998, 57733, 58424, 59072, 59680, 60250, 60783, 61280,
		61744, 62175, 62575, 62945, 63285, 63598, 63884, 64145, 64380,
		64591, 64779, 64944, 65088, 65210, 65312, 65394, 65457, 65501,
		65527, 65536,
	},
	/* ease_in_out (0.42, 0, 0.58, 1) */ {
		0, 30, 123, 278, 497, 782, 1133, 1552, 2039,
		2596, 3222, 3919, 4686, 5525, 6435, 7415, 8465, 9584,
		10770, 12023, 13339, 14717, 16153, 17645, 19187, 20776, 22408,
		24076, 25776, 27501, 29246, 31004, 32768, 34532, 36290, 38035,
		39760, 41460, 43128, 44760, 46349, 47891, 49383, 50819, 52197,
		53513, 54766, 55952, 57071, 58121, 59101, 60011, 60850, 61617,
		62314, 62940, 63497, 63984, 64403, 64754, 65039, 65258, 65413,
		65506, 65536,
	},
	/* ease_in (0.42, 0, 1, 1) */ {
		0, 30, 117, 259, 452, 696, 987, 1323, 1703,
		2125, 2587, 3087, 3625, 4199, 4808, 5450, 6125, 6832,
		7569, 8335, 9130, 9954, 10804, 11681, 12584, 13512, 14465,
		15442, 16442, 17465, 18511, 19578, 20667, 21777, 22909, 24060,
		25231, 26423, 27633, 28863, 30112, 31379, 32664, 33968, 35290,
		36630, 37987, 39362, 40754, 42164, 43591, 45036, 46498, 47977,
		49474, 50989, 52522, 54073, 55643, 57233, 58843, 60474, 62130,
		63813, 65536,
	},
	/* ease_out (0, 0, 0.58, 1) */ {
		0, 1723, 3406, 5062, 6693, 8303, 9893, 11463, 13014,
		14547, 16062, 17559, 19038, 20500, 21945, 23372, 24782, 26174,
		27549, 28906, 30246, 31568, 32872, 34157, 35424, 36673, 37903,
		39113, 40305, 41476, 42627, 43759, 44869, 45958, 47025, 48071,
		49094, 50094, 51071, 52024, 52952, 53855, 54732, 55582, 56406,
		57201, 57967, 58704, 59411, 60086, 60728, 61337, 61911, 62449,
		62949, 63411, 63833, 64213, 64549, 64840, 65084, 65277, 65419,
		65506, 65536,
	},
	/* uikit4 (0.66, 0, 0.33, 1) */ {
		0, 12, 51, 117, 212, 338, 497, 691, 923,
		1195, 1511, 1874, 2288, 2756, 3285, 3879, 4545, 5289,
		6120, 7048, 8082, 9236, 10522, 11957, 13557, 15342, 17329,
		19534, 21964, 24616, 27462, 30450, 33501, 36525, 39436, 42171,
		44695, 46994, 49073, 50944, 52625, 54135, 55490, 56707, 57800,
		58783, 59665, 60457, 61168, 61804, 62373, 62880, 63330, 63728,
		64077, 64381, 64644, 64868, 65055, 65209, 65331, 65423, 65487,
		65524, 65536,
	},
};

/* lv_anim path callbacks */
int32_t ios_path_ease_in_out(const lv_anim_t *a) { return ios_anim_value(a, IOS_EASE_IN_OUT); }
int32_t ios_path_ease_in(const lv_anim_t *a) { return ios_anim_value(a, IOS_EASE_IN); }
int32_t ios_path_ease_out(const lv_anim_t *a) { return ios_anim_value(a, IOS_EASE_OUT); }

/* UIScrollView's pull and friction for a page @w wide (initWithFrame:) */
void ios_pager_init(struct ios_pager *p, float w)
{
	float k = (float)(0.000359090909090909 + w * -1.84659090909091e-07);
	float f = (float)(0.966818181818182 + w * 9.94318181818158e-06);
	struct ios_spring_m m = { 1.0 - (double)f * k, f, -(double)f * k, f };
	int n;

	p->pm[0] = (struct ios_spring_m){ 1, 0, 0, 1 };
	for (n = 1; n <= IOS_PAGE_MAX_STEPS; n++) {
		struct ios_spring_m q = p->pm[n - 1];

		p->pm[n] = (struct ios_spring_m){
			q.a * m.a + q.b * m.c, q.a * m.b + q.b * m.d,
			q.c * m.a + q.d * m.c, q.c * m.b + q.d * m.d,
		};
	}
}

/*
 * Advance @n whole ms (iOS: (int) of the frame's ms, the rest dropped, so the
 * settle runs at ~96 % of real time at 60 Hz). True once settled on target.
 */
bool ios_pager_step(struct ios_pager *p, int n)
{
	const struct ios_spring_m *m, *m1;
	double e = p->x - p->target, e_prev, e_new;
	float nx;

	if (n < 1)
		return false;
	if (n > IOS_PAGE_MAX_STEPS)
		n = IOS_PAGE_MAX_STEPS;
	m = &p->pm[n];
	m1 = &p->pm[n - 1];
	e_prev = m1->a * e + m1->b * p->v;	/* before the last step: its dx */
	e_new = m->a * e + m->b * p->v;
	p->v = m->c * e + m->d * p->v;
	nx = roundf((float)(p->target + e_new));
	if (nx != p->target && nx == p->x && fabs(p->v) < 0.5)
		nx += e_prev > 0 ? -1.f : 1.f;	/* (dx = -e_prev) */
	if (fabs(p->v) < 0.01 && fabs(e_prev) <= 1.0 && fabsf(nx - p->target) < 5.f) {
		p->x = p->target;
		p->v = 0;
		return true;
	}
	p->x = nx;
	return false;
}
