// SPDX-License-Identifier: MIT
/* n72-ui --bench: full-screen redraw timings and the text benchmark. */
#include "main_priv.h"

/* ---------------------------------------------------------------- bench */

static double redraw_ms(int n)
{
	uint64_t t0;
	int i;

	lv_refr_now(NULL);
	t0 = mono_us();
	for (i = 0; i < n; i++) {
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
	}
	return (double)(mono_us() - t0) / 1000.0 / n;
}

/*
 * Text: one screen of log-like lines in a single label (Logs), then many
 * short labels (Settings-like rows). "--bench text [n]" prints both; the
 * text is fixed, so runs compare.
 */
static uint32_t lv_color_32_checksum(const lv_draw_buf_t *db)
{
	const uint32_t *p = (const uint32_t *)db->data;
	uint32_t h = 2166136261u, i, nw = db->header.stride / 4 * db->header.h;

	for (i = 0; i < nw; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* FNV-1a over the frame just shown (the active scan-out buffer's pixels) */
static uint32_t screen_checksum(void)
{
	lv_display_t *disp = lv_display_get_default();
	lv_draw_buf_t *db = lv_display_get_buf_active(disp);
	uint32_t h = 2166136261u, x, y;

	if (!db)
		return 0;
	for (y = 0; y < db->header.h; y++) {
		const uint32_t *p = (const uint32_t *)(db->data + y * db->header.stride);

		for (x = 0; x < db->header.w; x++)
			h = (h ^ (p[x] & 0xffffff)) * 16777619u;
	}
	return h;
}

int bench_text(int n, bool label_only)
{
	uint32_t scr_sum[2] = { 0, 0 };
	static char buf[8192];
	static const char *const words[] = {
		"wlan0:", "brcmfmac", "mmc0:", "new", "SDIO", "card", "at", "address", "0001",
		"apple-mbx", "3b000000.gpu:", "VGP", "program", "255", "words", "PM:", "suspend",
		"entry", "(s2idle)", "exit", "dwc2", "38400000.usb:", "resuming", "gadget", "g_ether",
	};
	lv_obj_t *scr, *l;
	double big, rows;
	size_t o = 0;
	unsigned seed = 1;
	int i, line = 0;

	while (o < sizeof(buf) - 64 && line < 60) {
		int w, nw = 4 + line % 5;

		o += snprintf(buf + o, sizeof(buf) - o, "[%5d.%06d] ", 3 + line * 7, (line * 7919) % 1000000);
		for (w = 0; w < nw; w++) {
			seed = seed * 1103515245u + 12345u;
			o += snprintf(buf + o, sizeof(buf) - o, "%s ", words[(seed >> 16) % (sizeof(words) / sizeof(words[0]))]);
		}
		buf[o++] = '\n';
		line++;
	}
	buf[o] = 0;

	if (label_only)
		goto canvas;
	scr = lv_obj_create(NULL);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	l = lv_label_create(scr);
	lv_obj_set_width(l, 312);
	lv_obj_set_pos(l, 4, 4);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
	lv_label_set_text_static(l, buf);
	lv_screen_load(scr);
	big = redraw_ms(n);
	scr_sum[0] = screen_checksum();

	scr = lv_obj_create(NULL);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	for (i = 0; i < 16; i++) {
		lv_obj_t *t = lv_label_create(scr), *v = lv_label_create(scr);

		lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
		lv_label_set_text_fmt(t, "Setting row %d", i);
		lv_obj_set_pos(t, 12, 8 + i * 29);
		lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
		lv_label_set_text(v, i & 1 ? "On" : "Automatic");
		lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -12, 10 + i * 29);
	}
	lv_screen_load(scr);
	rows = redraw_ms(n);
	scr_sum[1] = screen_checksum();
	printf("n72-ui text bench: log label (%zu chars, 60 lines) %.2f ms, 32 short labels %.2f ms; screen checksums %08x %08x\n",
	       o, big, rows, (unsigned)scr_sum[0], (unsigned)scr_sum[1]);

canvas:
	/* the label alone, drawn by the CPU into an off-screen XRGB8888 canvas (the display format): no display pacing */
	{
		lv_draw_buf_t *db = lv_draw_buf_create(320, 480, LV_COLOR_FORMAT_XRGB8888, 0);
		lv_obj_t *cv = lv_canvas_create(lv_screen_active());
		lv_draw_label_dsc_t d;
		lv_area_t a = { 4, 4, 315, 479 };
		lv_layer_t layer;
		uint64_t t0, t;
		double best = 1e9, sum = 0;

		lv_canvas_set_draw_buf(cv, db);
		lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
		lv_draw_label_dsc_init(&d);
		d.text = buf;
		d.font = &lv_font_montserrat_12;
		d.color = lv_color_white();
		for (i = 0; i < n; i++) {
			lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_COVER);
			t0 = mono_us();
			lv_canvas_init_layer(cv, &layer);
			lv_draw_label(&layer, &d, &a);
			lv_canvas_finish_layer(cv, &layer);
			t = mono_us() - t0;
			sum += t;
			if (t < best)
				best = t;
		}
		printf("  log label into a canvas: avg %.2f ms, best %.2f ms; checksum %08x\n",
		       sum / n / 1000.0, best / 1000.0, (unsigned)lv_color_32_checksum(db));
	}
	return 0;
}

/* Redraw the home screen, then a fill-heavy screen of plain rectangles. */
int bench(void)
{
	lv_obj_t *scr;
	double home, rects, overlay;
	int i;

	home = redraw_ms(20);
	fprintf(stderr, "bench: home %.1f ms\n", home);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x203040), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	for (i = 0; i < 24; i++) {
		lv_obj_t *r = lv_obj_create(scr);

		lv_obj_set_size(r, 96, 72);
		lv_obj_set_pos(r, 8 + (i % 3) * 104, 8 + (i / 3) * 58);
		lv_obj_set_style_radius(r, 0, 0);
		lv_obj_set_style_border_width(r, 0, 0);
		lv_obj_set_style_bg_color(r, lv_color_hsv_to_rgb(i * 15, 70, 90), 0);
	}
	lv_screen_load(scr);
	rects = redraw_ms(20);
	fprintf(stderr, "bench: rectangles %.1f ms\n", rects);

	/* Translucent plain panels and an opacity layer over the rectangles. */
	for (i = 0; i < 3; i++) {
		lv_obj_t *p = lv_obj_create(scr);

		lv_obj_set_size(p, 280, 110);
		lv_obj_set_pos(p, 20, 20 + i * 150);
		lv_obj_set_style_radius(p, 0, 0);
		lv_obj_set_style_border_width(p, 0, 0);
		lv_obj_set_style_bg_color(p, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(p, LV_OPA_30, 0);
	}
	{
		lv_obj_t *grp = lv_obj_create(scr), *l;

		lv_obj_set_size(grp, 240, 300);
		lv_obj_center(grp);
		lv_obj_set_style_opa_layered(grp, LV_OPA_70, 0);	/* drawn via a layer */
		lv_obj_set_style_radius(grp, 0, 0);
		lv_obj_set_style_bg_color(grp, lv_color_hex(0x2a9d8f), 0);
		l = lv_label_create(grp);
		lv_label_set_text(l, "Layer with 70% opacity");
		lv_obj_center(l);
	}
	overlay = redraw_ms(20);

	printf("n72-ui bench (%s): home %.1f ms, 24 rectangles %.1f ms, + translucent panels and layer %.1f ms\n",
	       mbx_gpu_stats.submits ? "GPU" : "CPU", home, rects, overlay);
	printf("  GPU submits %lu: fills %lu, blended fills %lu, layers %lu, flushes %lu; pool misses %lu\n",
	       mbx_gpu_stats.submits, mbx_gpu_stats.fills, mbx_gpu_stats.blends,
	       mbx_gpu_stats.layers, mbx_gpu_stats.flushes, mbx_gpu_stats.pool_misses);
	printf("  time in GPU ioctls: sync %.1f ms (%llu KiB), 2D submit %.1f ms, 3D blit %.1f ms; %lu tasks async\n",
	       mbx_gpu_stats.sync_us / 1000, mbx_gpu_stats.sync_bytes / 1024,
	       mbx_gpu_stats.submit_us / 1000, mbx_gpu_stats.blit3d_us / 1000,
	       mbx_gpu_stats.async_done);
	printf("  fills left to the CPU: radius %lu, gradient %lu, small %lu\n",
	       mbx_gpu_stats.fill_radius, mbx_gpu_stats.fill_grad, mbx_gpu_stats.fill_small);
	printf("  tasks seen by type:");
	for (i = 0; i < 16; i++)
		if (mbx_gpu_stats.seen[i])
			printf(" %d:%lu", i, mbx_gpu_stats.seen[i]);
	printf("\n");
	if (getenv("N72_BENCH_SHOT"))
		sleep(2);	/* leave the last frame up for a screenshot */
	return 0;
}
