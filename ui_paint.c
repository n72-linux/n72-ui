// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): paint. */
#include "ui_m3_priv.h"

/* -------------------------------------------------------------- paint */

/* A finger-painting toy: each stroke gets the next colour round the wheel. */
static lv_obj_t *paint_canvas;
static void *paint_buf;
static lv_point_t paint_last;
static bool paint_down;
static uint32_t paint_hue;

#define PAINT_W (W - 2 * STATUS_PAD)
#define PAINT_H (H - 56 - 16)

static void paint_clear(void)
{
	lv_canvas_fill_bg(paint_canvas, M.surface_c, LV_OPA_COVER);
}

static void paint_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_draw_line_dsc_t d;
	lv_layer_t layer;
	lv_area_t a;
	lv_point_t p;

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		paint_down = false;
		return;
	}
	if (!indev || (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING))
		return;
	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(paint_canvas, &a);
	p.x -= a.x1;
	p.y -= a.y1;
	if (code == LV_EVENT_PRESSED || !paint_down) {
		paint_last = p;
		paint_down = true;
		paint_hue = (paint_hue + 47) % 360;
	}
	lv_canvas_init_layer(paint_canvas, &layer);
	lv_draw_line_dsc_init(&d);
	d.color = tone(paint_hue, 75, conf.dark ? 70 : 50);
	d.width = 10;
	d.round_start = 1;
	d.round_end = 1;
	d.p1.x = paint_last.x;
	d.p1.y = paint_last.y;
	d.p2.x = p.x;
	d.p2.y = p.y;
	lv_draw_line(&layer, &d);
	lv_canvas_finish_layer(paint_canvas, &layer);
	paint_last = p;
}

static void paint_clear_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (paint_buf)
		paint_clear();
}

static void paint_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(paint_buf);
	paint_buf = NULL;
}

void open_paint(lv_event_t *e)
{
	nav_mark(open_paint, NULL);
	lv_obj_t *scr, *body, *btn;

	LV_UNUSED(e);
	body = m3_page("Paint", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	paint_buf = malloc(LV_CANVAS_BUF_SIZE(PAINT_W, PAINT_H, 32, LV_DRAW_BUF_STRIDE_ALIGN));
	if (!paint_buf) {
		lv_obj_delete(scr);
		return;
	}
	paint_canvas = lv_canvas_create(body);
	obj_track(&paint_canvas);
	lv_canvas_set_buffer(paint_canvas, paint_buf, PAINT_W, PAINT_H, LV_COLOR_FORMAT_XRGB8888);
	lv_obj_add_flag(paint_canvas, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(paint_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(paint_canvas, paint_event, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(scr, paint_delete_cb, LV_EVENT_DELETE, NULL);
	paint_clear();

	/* clear: a tonal icon button at the right of the top bar */
	btn = lv_button_create(lv_obj_get_child(scr, 0));
	plain(btn);
	lv_obj_set_size(btn, 48, 40);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(btn, M.secondary_c, 0);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -8, 0);
	lv_obj_add_event_cb(btn, paint_clear_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(m3_label(btn, LV_SYMBOL_TRASH, &lv_font_montserrat_16, M.on_secondary_c));
	push(scr);
}
