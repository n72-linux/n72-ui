/*
 * Wallpapers for the home and lock screens (included by ui_m3.c).
 *
 * A picture is prepared once, when it is chosen: `n72-ui --wallpaper SRC
 * home|lock|both` runs as a background command (its own process, so the
 * UI never waits on a decoder). It decodes the JPEG (LVGL's tjpgd, scaled
 * by 1/2..1/8 while decoding) or PNG (lodepng), crops it to fill the
 * screen upright and turned, resizes with an area average (bilinear when
 * the picture is smaller), darkens it a little where text sits on it
 * (status bar, the lock clock, the app grid), and writes raw XRGB8888
 * frames to CONF_DIR: no decoding at start, ~1 ms to read one in.
 * The display dithers in hardware (CLCD 0x300 = 0x80000001), so no
 * dithering here.
 *
 * At run time a frame is one opaque image under the screen's objects: the
 * GPU copies the redrawn areas of it (image_ok: XRGB8888, 32-byte stride),
 * and LVGL skips the screen's own background under it (it covers).
 */

#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "lvgl/src/libs/lodepng/lodepng.h"

#define WP_DIR		CONF_DIR
#define WP_META		WP_DIR "/wallpaper.meta"
#define WP_PW		320	/* upright */
#define WP_PH		480

static const char *const wp_kind_name[WP_KINDS] = { "home", "lock" };

static void wp_path(char *buf, size_t len, int kind, int turned)
{
	snprintf(buf, len, WP_DIR "/wp-%s-%c.raw", wp_kind_name[kind], turned ? 'l' : 'p');
}

/* ------------------------------------------------------ the conversion */

struct wp_img {
	uint8_t *rgb;		/* w x h, R G B */
	int w, h;
	lv_draw_buf_t *db;	/* (a PNG: rgb is inside its decoder's buffer) */
};

static void wp_img_free(struct wp_img *im)
{
	if (im->db)
		lv_draw_buf_destroy(im->db);
	else
		free(im->rgb);
	im->rgb = NULL;
	im->db = NULL;
}

struct wp_jpg {
	FILE *f;
	struct wp_img *img;
};

static size_t wp_jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
	struct wp_jpg *j = jd->device;

	if (!buf)
		return fseek(j->f, (long)n, SEEK_CUR) ? 0 : n;
	return fread(buf, 1, n, j->f);
}

static int wp_jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
	struct wp_jpg *j = jd->device;
	struct wp_img *im = j->img;
	const uint8_t *s = bitmap;
	int w = r->right - r->left + 1, y;

	/* (LVGL's tjpgd gives B G R) */
	for (y = r->top; y <= r->bottom; y++, s += (size_t)w * 3) {
		uint8_t *o;
		int x, n;

		if (y >= im->h || r->left >= im->w)
			continue;
		o = im->rgb + ((size_t)y * im->w + r->left) * 3;
		n = LV_MIN(w, im->w - r->left);
		for (x = 0; x < n; x++, o += 3) {
			o[0] = s[x * 3 + 2];
			o[1] = s[x * 3 + 1];
			o[2] = s[x * 3];
		}
	}
	return 1;
}

/* Decoded at a scale that leaves the short side >= 640 px where it can. */
static int wp_load_jpg(const char *path, struct wp_img *im)
{
	static uint8_t pool[16384];
	struct wp_jpg j = { fopen(path, "rb"), im };
	JDEC jd;
	JRESULT r;
	int s;

	if (!j.f)
		return -1;
	r = jd_prepare(&jd, wp_jpg_in, pool, sizeof(pool), &j);
	if (r != JDR_OK) {
		fclose(j.f);
		fprintf(stderr, "wallpaper: %s: not a baseline JPEG (%d)\n", path, r);
		return -2;
	}
	for (s = 3; s > 0 && (LV_MIN(jd.width, jd.height) >> s) < 640; s--)
		;
	im->w = jd.width >> s;
	im->h = jd.height >> s;
	im->rgb = calloc((size_t)im->w * im->h, 3);
	if (!im->rgb) {
		fclose(j.f);
		fprintf(stderr, "wallpaper: %s: no memory for %d x %d\n", path, im->w, im->h);
		return -3;
	}
	r = jd_decomp(&jd, wp_jpg_out, (uint8_t)s);
	fclose(j.f);
	if (r != JDR_OK) {
		fprintf(stderr, "wallpaper: %s: decoding failed (%d)\n", path, r);
		return -2;
	}
	return 0;
}

/*
 * PNG: whole, up to 4 megapixels (lodepng decodes all of it; a phone's
 * screenshot is 3-3.6); alpha over black. 8-bit RGB and RGBA are decoded
 * as they are, other kinds converted to RGBA, and the pixels are packed to
 * R G B inside the decoder's own buffer: at 4 megapixels the peak is
 * the file, 16 MB of buffer and lodepng's 12 MB of scanlines while it
 * decodes (an 8-megapixel limit, a second buffer for the conversion and a
 * copy for R G B came to 60-90 MB, next to n72-ui and an application).
 */
#define WP_PNG_MAX_PX	4200000

static int wp_load_png(const char *path, struct wp_img *im)
{
	unsigned char *out = NULL, *data = NULL;
	unsigned w, h, err;
	FILE *f = fopen(path, "rb");
	LodePNGState st;
	long n;
	int bpp;

	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) ||
	    !(data = malloc((size_t)n)) || fread(data, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(data);
		fprintf(stderr, "wallpaper: %s: cannot read it\n", path);
		return -1;
	}
	fclose(f);
	lodepng_state_init(&st);
	err = lodepng_inspect(&w, &h, &st, data, (size_t)n);
	if (err || (uint64_t)w * h > WP_PNG_MAX_PX) {
		lodepng_state_cleanup(&st);
		free(data);
		fprintf(stderr, "wallpaper: %s: %s\n", path,
			err ? "not a PNG" : "over 4 megapixels (a JPEG of it works)");
		return -2;
	}
	if (st.info_png.color.bitdepth == 8 &&
	    (st.info_png.color.colortype == LCT_RGB || st.info_png.color.colortype == LCT_RGBA))
		st.info_raw.colortype = st.info_png.color.colortype;	/* as it is */
	else
		st.info_raw.colortype = LCT_RGBA;
	st.info_raw.bitdepth = 8;
	bpp = st.info_raw.colortype == LCT_RGB ? 3 : 4;
	err = lodepng_decode(&out, &w, &h, &st, data, (size_t)n);
	lodepng_state_cleanup(&st);
	free(data);
	if (err || !out) {
		if (out)
			lv_draw_buf_destroy((lv_draw_buf_t *)out);
		fprintf(stderr, "wallpaper: %s: %s\n", path, err ? lodepng_error_text(err) : "no memory");
		return -2;
	}
	/*
	 * (LVGL's lodepng gives an LVGL draw buffer, its pixels packed in the
	 * PNG's own order: R G B or R G B A, straight alpha)
	 */
	im->db = (lv_draw_buf_t *)out;
	im->rgb = im->db->data;
	im->w = (int)w;
	im->h = (int)h;
	{
		const uint8_t *q = im->rgb;
		uint8_t *o = im->rgb;
		size_t i, np = (size_t)w * h;

		for (i = 0; i < np; i++, q += bpp, o += 3) {	/* (o never passes q) */
			unsigned a = bpp == 4 ? q[3] : 255, r = q[0], g = q[1], b = q[2];

			o[0] = (uint8_t)(r * a / 255);
			o[1] = (uint8_t)(g * a / 255);
			o[2] = (uint8_t)(b * a / 255);
		}
	}
	return 0;
}

/*
 * @im cropped (centred) to fill @dw x @dh, resized into @out (XRGB8888):
 * an area average when shrinking (each output pixel the mean of the source
 * pixels it covers), bilinear when growing.
 */
static int wp_fit(const struct wp_img *im, uint32_t *out, int dw, int dh)
{
	int cx, cy, cw, ch, x, y;

	if ((int64_t)im->w * dh > (int64_t)im->h * dw) {
		ch = im->h;
		cw = (int)((int64_t)im->h * dw / dh);
	} else {
		cw = im->w;
		ch = (int)((int64_t)im->w * dh / dw);
	}
	cx = (im->w - cw) / 2;
	cy = (im->h - ch) / 2;
	if (cw >= dw && ch >= dh) {
		/* separable box: rows first (dw sums per source row), then columns */
		uint32_t *rows = malloc((size_t)dw * ch * 3 * sizeof(uint32_t));
		int *xs = malloc((dw + 1) * sizeof(int));

		if (!rows || !xs) {
			free(rows);
			free(xs);
			return -1;
		}
		for (x = 0; x <= dw; x++)
			xs[x] = cx + (int)((int64_t)x * cw / dw);
		for (y = 0; y < ch; y++) {
			const uint8_t *s = im->rgb + ((size_t)(cy + y) * im->w) * 3;
			uint32_t *o = rows + (size_t)y * dw * 3;

			for (x = 0; x < dw; x++) {
				uint32_t r = 0, g = 0, b = 0;
				int k, n = xs[x + 1] - xs[x];

				for (k = xs[x]; k < xs[x + 1]; k++) {
					r += s[k * 3];
					g += s[k * 3 + 1];
					b += s[k * 3 + 2];
				}
				o[x * 3] = r * 256 / n;	/* (x256: precision for the second pass) */
				o[x * 3 + 1] = g * 256 / n;
				o[x * 3 + 2] = b * 256 / n;
			}
		}
		for (y = 0; y < dh; y++) {
			int y0 = (int)((int64_t)y * ch / dh), y1 = (int)((int64_t)(y + 1) * ch / dh), k;

			for (x = 0; x < dw; x++) {
				uint64_t r = 0, g = 0, b = 0;

				for (k = y0; k < y1; k++) {
					const uint32_t *o = rows + ((size_t)k * dw + x) * 3;

					r += o[0];
					g += o[1];
					b += o[2];
				}
				r = (r / (y1 - y0) + 128) >> 8;
				g = (g / (y1 - y0) + 128) >> 8;
				b = (b / (y1 - y0) + 128) >> 8;
				out[(size_t)y * dw + x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
			}
		}
		free(rows);
		free(xs);
		return 0;
	}
	for (y = 0; y < dh; y++) {
		float fy = cy + (y + 0.5f) * ch / dh - 0.5f;
		int y0 = LV_CLAMP(0, (int)floorf(fy), im->h - 1), y1 = LV_MIN(y0 + 1, im->h - 1);
		float ty = LV_CLAMP(0.f, fy - y0, 1.f);

		for (x = 0; x < dw; x++) {
			float fx = cx + (x + 0.5f) * cw / dw - 0.5f;
			int x0 = LV_CLAMP(0, (int)floorf(fx), im->w - 1), x1 = LV_MIN(x0 + 1, im->w - 1);
			float tx = LV_CLAMP(0.f, fx - x0, 1.f);
			uint32_t px = 0xff000000u;
			int c;

			for (c = 0; c < 3; c++) {
				float a = im->rgb[((size_t)y0 * im->w + x0) * 3 + c] * (1 - tx) +
					  im->rgb[((size_t)y0 * im->w + x1) * 3 + c] * tx;
				float b = im->rgb[((size_t)y1 * im->w + x0) * 3 + c] * (1 - tx) +
					  im->rgb[((size_t)y1 * im->w + x1) * 3 + c] * tx;

				px |= (uint32_t)lroundf(a * (1 - ty) + b * ty) << (16 - 8 * c);
			}
			out[(size_t)y * dw + x] = px;
		}
	}
	return 0;
}

/*
 * Darker where text sits (out of 256 taken away, by height): the status bar
 * (25 % at the top edge, gone by 64 px), and for home the app grid (up to
 * 16 % at the bottom), for the lock screen the clock (30 % at the top, gone
 * by 60 % of the height) and the line at the bottom (20 %).
 */
static int wp_shade(int kind, int y, int h)
{
	int k = 0, f = y * 1000 / h;	/* (0..999 of the height) */

	if (y < 64)
		k = 64 * (64 - y) / 64;
	if (kind == WP_HOME) {
		if (f > 330)
			k = LV_MAX(k, 41 * (f - 330) / 670);
	} else {
		if (f < 600)
			k = LV_MAX(k, 77 * (600 - f) / 600);
		if (f > 780)
			k = LV_MAX(k, 51 * (f - 780) / 220);
	}
	return k;
}

static int wp_finish(int kind, uint32_t *px, int w, int h, int *luma)
{
	uint64_t ly = 0, n = 0;
	int x, y, y0 = kind == WP_HOME ? h * 36 / 100 : h / 8, y1 = kind == WP_HOME ? h : h * 45 / 100;

	for (y = 0; y < h; y++) {
		int k = 256 - wp_shade(kind, y, h);

		for (x = 0; x < w; x++) {
			uint32_t p = px[(size_t)y * w + x];
			uint32_t r = (p >> 16 & 255) * k >> 8, g = (p >> 8 & 255) * k >> 8, b = (p & 255) * k >> 8;

			px[(size_t)y * w + x] = 0xff000000u | r << 16 | g << 8 | b;
			if (luma && y >= y0 && y < y1) {
				ly += (r * 77 + g * 150 + b * 29) >> 8;
				n++;
			}
		}
	}
	if (luma)
		*luma = n ? (int)(ly / n) : 128;
	return 0;
}

/* The picture's accent: mean hue of its coloured pixels, weighted by chroma. */
static void wp_accent(const uint32_t *px, int n, int *hue, int *sat)
{
	double sx = 0, sy = 0, wsum = 0, csum = 0;
	int i;

	for (i = 0; i < n; i += 7) {
		int r = px[i] >> 16 & 255, g = px[i] >> 8 & 255, b = px[i] & 255;
		int mx = LV_MAX(r, LV_MAX(g, b)), mn = LV_MIN(r, LV_MIN(g, b)), c = mx - mn;
		double h;

		if (c < 24)
			continue;
		if (mx == r)
			h = fmod((double)(g - b) / c, 6.0);
		else if (mx == g)
			h = (double)(b - r) / c + 2;
		else
			h = (double)(r - g) / c + 4;
		h *= M_PI / 3;
		sx += c * cos(h);
		sy += c * sin(h);
		wsum += c;
		csum++;
	}
	if (wsum <= 0) {
		*hue = -1;	/* no colour to speak of */
		return;
	}
	*hue = ((int)lround(atan2(sy, sx) * 180 / M_PI) + 360) % 360;
	/* a picture of strong colours gives a stronger accent, within M3's range */
	*sat = LV_CLAMP(35, (int)(wsum / csum * 100 / 255), 65);
}

static int wp_write(int kind, int turned, const uint32_t *px, int w, int h)
{
	char path[96], tmp[100];
	FILE *f;
	size_t n = (size_t)w * h;

	wp_path(path, sizeof(path), kind, turned);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "wb");
	if (!f)
		return -1;
	if (fwrite(px, 4, n, f) != n || fflush(f) || fsync(fileno(f))) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	fclose(f);
	return rename(tmp, path);
}

/* the meta file: what is set, mean lumas, the accent */
static void wp_meta_read(void)
{
	FILE *f = fopen(WP_META, "r");
	char line[64];
	int v;

	wp.luma[WP_HOME] = wp.luma[WP_LOCK] = 128;
	wp.hue = -1;
	wp.sat = 48;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		/* (out of range: the defaults) */
		if (sscanf(line, "home_luma=%d", &v) == 1 && v >= 0 && v <= 255)
			wp.luma[WP_HOME] = v;
		else if (sscanf(line, "lock_luma=%d", &v) == 1 && v >= 0 && v <= 255)
			wp.luma[WP_LOCK] = v;
		else if (sscanf(line, "hue=%d", &v) == 1 && v >= -1 && v < 360)
			wp.hue = v;
		else if (sscanf(line, "sat=%d", &v) == 1 && v >= 0 && v <= 100)
			wp.sat = v;
	}
	fclose(f);
}

static void wp_meta_write(void)
{
	FILE *f = fopen(WP_META ".new", "w");

	if (!f)
		return;
	fprintf(f, "home_luma=%d\nlock_luma=%d\nhue=%d\nsat=%d\n", wp.luma[WP_HOME],
		wp.luma[WP_LOCK], wp.hue, wp.sat);
	if (fflush(f) == 0)
		fsync(fileno(f));
	fclose(f);
	rename(WP_META ".new", WP_META);
}

/* `n72-ui --wallpaper SRC home|lock|both|none-home|none-lock`; 0 when done */
static int wp_convert_main(int argc, char **argv)
{
	struct wp_img im = { 0 };
	const char *src = argc > 2 ? argv[2] : "", *what = argc > 3 ? argv[3] : "both";
	bool kinds[WP_KINDS] = { strcmp(what, "lock") != 0, strcmp(what, "home") != 0 };
	uint32_t *px;
	int k, t, r;
	size_t n = strlen(src);

	lv_init();	/* (lodepng gives its picture as an LVGL draw buffer) */
	mkdir(WP_DIR, 0755);
	wp_meta_read();
	if (!strncmp(what, "none-", 5)) {	/* (removal) */
		k = !strcmp(what + 5, "lock") ? WP_LOCK : WP_HOME;
		for (t = 0; t < 2; t++) {
			char path[96];

			wp_path(path, sizeof(path), k, t);
			unlink(path);
		}
		if (k == WP_HOME) {	/* (its accent goes with it) */
			wp.hue = -1;
			wp_meta_write();
		}
		return 0;
	}
	if (n > 4 && !strcasecmp(src + n - 4, ".png"))
		r = wp_load_png(src, &im);
	else
		r = wp_load_jpg(src, &im);
	if (r) {
		wp_img_free(&im);
		return 2;
	}
	px = malloc((size_t)WP_PW * WP_PH * 4);
	if (!px) {
		wp_img_free(&im);
		fprintf(stderr, "wallpaper: no memory\n");
		return 3;
	}
	for (k = 0; k < WP_KINDS; k++) {
		if (!kinds[k])
			continue;
		for (t = 0; t < 2; t++) {
			int w = t ? WP_PH : WP_PW, h = t ? WP_PW : WP_PH;

			if (wp_fit(&im, px, w, h)) {
				free(px);
				wp_img_free(&im);
				fprintf(stderr, "wallpaper: no memory to resize it\n");
				return 3;
			}
			if (!t && k == WP_HOME)	/* (the accent is the home picture's) */
				wp_accent(px, w * h, &wp.hue, &wp.sat);	/* (before the shading) */
			wp_finish(k, px, w, h, t ? NULL : &wp.luma[k]);
			if (wp_write(k, t, px, w, h)) {
				free(px);
				wp_img_free(&im);
				fprintf(stderr, "wallpaper: cannot write it: %s\n", strerror(errno));
				return 4;
			}
		}
	}
	wp_meta_write();
	free(px);
	wp_img_free(&im);
	return 0;
}

/* --------------------------------------------------------- at run time */

static lv_draw_buf_t *wp_read(int kind, int turned)
{
	int w = turned ? WP_PH : WP_PW, h = turned ? WP_PW : WP_PH, y;
	lv_draw_buf_t *b;
	char path[96];
	FILE *f;

	wp_path(path, sizeof(path), kind, turned);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_XRGB8888, 0);
	if (!b) {
		fclose(f);
		return NULL;
	}
	for (y = 0; y < h; y++)
		if (fread(b->data + (size_t)y * b->header.stride, 4, w, f) != (size_t)w) {
			lv_draw_buf_destroy(b);
			fclose(f);
			return NULL;
		}
	fclose(f);
	lv_draw_buf_flush_cache(b, NULL);	/* (the GPU reads it) */
	return b;
}

/*
 * A frame is freed when the table lets go of it (a new picture, the other
 * shape after a turn) and no image shows it any more: the lock screen is
 * kept while locked through a rebuild of home, and a page swipe's frames
 * and the old home last into the rebuild. (Freeing them under their
 * images was a use-after-free; keeping every frame until the next rebuild
 * held up to 4 x 600 KB of the GPU pool.)
 */
#define WP_HOLDS	8
static struct {
	lv_draw_buf_t *b;
	int refs;
	bool gone;		/* out of the table: freed with its last image */
} wp_holds[WP_HOLDS];

static void wp_free(lv_draw_buf_t *b)
{
	lv_image_cache_drop(b);
	lv_draw_buf_destroy(b);
}

static int wp_hold_find(const lv_draw_buf_t *b)
{
	for (int i = 0; i < WP_HOLDS; i++)
		if (wp_holds[i].b == b)
			return i;
	return -1;
}

static void wp_image_gone(lv_event_t *e)
{
	lv_draw_buf_t *b = lv_event_get_user_data(e);
	int i = wp_hold_find(b);

	if (i < 0 || --wp_holds[i].refs > 0)
		return;
	if (wp_holds[i].gone)
		wp_free(b);
	wp_holds[i].b = NULL;
	wp_holds[i].gone = false;
}

/* @img shows @b: @b stays until @img is deleted. false: not kept (show nothing). */
static bool wp_hold(lv_obj_t *img, lv_draw_buf_t *b)
{
	int i = wp_hold_find(b);

	if (i < 0) {
		if ((i = wp_hold_find(NULL)) < 0) {
			fprintf(stderr, "n72-ui: more than %d wallpaper images\n", WP_HOLDS);
			return false;
		}
		wp_holds[i].b = b;
		wp_holds[i].refs = 0;
		wp_holds[i].gone = false;
	}
	wp_holds[i].refs++;
	lv_obj_add_event_cb(img, wp_image_gone, LV_EVENT_DELETE, b);
	return true;
}

static void wp_drop(int kind, int turned)
{
	lv_draw_buf_t *b = wp.pic[kind][turned];
	int i;

	if (!b)
		return;
	wp.pic[kind][turned] = NULL;
	if ((i = wp_hold_find(b)) >= 0)
		wp_holds[i].gone = true;
	else
		wp_free(b);
}

/* (Re)read what is set: at the start, and after a conversion. Frames on demand. */
static void wp_load(void)
{
	int k, t;

	for (k = 0; k < WP_KINDS; k++)
		for (t = 0; t < 2; t++)
			wp_drop(k, t);
	wp_meta_read();
	if (icon_shadow) {	/* (its strength follows the picture; home is built again) */
		lv_image_cache_drop(icon_shadow);
		lv_draw_buf_destroy(icon_shadow);
		icon_shadow = NULL;
	}
	for (k = 0; k < WP_KINDS; k++) {
		char path[96];

		wp_path(path, sizeof(path), k, 0);
		wp.set[k] = !access(path, R_OK);
	}
	wp.loaded = true;
}

/*
 * The frame for @kind at the screen's current shape, or NULL (none set).
 * The other shape's goes (freed once nothing shows it): a turn back reads
 * it again, ~600 KB.
 */
static lv_draw_buf_t *wp_pic(int kind)
{
	int t = W > H;

	if (!wp.set[kind])
		return NULL;
	wp_drop(kind, !t);
	if (!wp.pic[kind][t])
		wp.pic[kind][t] = wp_read(kind, t);
	return wp.pic[kind][t];
}

/* The wallpaper under everything on @scr (its first child); false: none set. */
static bool wp_put(lv_obj_t *scr, int kind)
{
	lv_draw_buf_t *b = wp_pic(kind);
	lv_obj_t *i;

	if (!b)
		return false;
	i = lv_image_create(scr);
	if (!wp_hold(i, b)) {
		lv_obj_delete(i);
		return false;
	}
	lv_image_set_src(i, b);
	lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(i, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_pos(i, 0, 0);
	lv_obj_move_to_index(i, 0);
	return true;
}

/* ------------------------------------------------------- Settings page */

#define WP_PICTURES	"/root/Pictures"

static lv_obj_t *wp_status_lbl;
static char *wp_choice;		/* the picture picked, waiting for where */

static void open_wallpaper(lv_event_t *e);

/*
 * A conversion is over: the screens are built again with the new frames,
 * as after a turn (ui_rebuild), when nothing is in the way: not while dark,
 * locked (the lock screen goes when unlocked), a slide or a home swipe.
 * With the home picture's colours in use the palette changes too, which
 * only a restart makes: that waits for no application as well (a restart
 * would lose it, and the lock).
 */
static lv_timer_t *wp_apply_t;
static bool wp_apply_restart;

static bool wp_apply_try(void)
{
	if (screen_off || locked || tr.scr || sc.owner || pg.on || pg.settling)
		return false;
	if (wp_apply_restart) {
		if (app_pid > 0)
			return false;
		ui_restart("wallpaper");
	}
	wp_load();
	ui_rebuild(app_pid <= 0);
	return true;
}

static void wp_apply_cb(lv_timer_t *t)
{
	if (wp_apply_try()) {
		lv_timer_delete(t);
		wp_apply_t = NULL;
	}
}

static void wp_status(const char *text)
{
	if (wp_status_lbl)
		lv_label_set_text(wp_status_lbl, text);
}

static void wp_done(const char *out, void *ud)
{
	const char *st = strstr(out, "wp-status ");
	bool home = (intptr_t)ud;

	wp.busy = false;
	if (!st || atoi(st + 10) != 0) {
		/* the converter's first line says why */
		char why[160];
		size_t n = strcspn(out, "\n");

		if (!strncmp(out, "wallpaper: ", 11))
			out += 11, n -= 11;
		snprintf(why, sizeof(why), "The picture could not be used: %.*s", (int)LV_MIN(n, 120), out);
		fprintf(stderr, "n72-ui: %s\n", why);
		wp_status(why);
		return;
	}
	wp_status("");
	wp_apply_restart |= home && conf.wp_accent;
	if (!wp_apply_t && !wp_apply_try())
		wp_apply_t = lv_timer_create(wp_apply_cb, 500, NULL);
}

static void wp_run(const char *src, const char *what)
{
	char q[600], cmd[700];
	bool home = strcmp(what, "lock") && strcmp(what, "none-lock");

	if (wp.busy)
		return;
	shq(src, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "nice -n 10 /proc/%d/exe --wallpaper %s %s 2>&1; echo \"wp-status $?\"",
		 (int)getpid(), q, what);
	if (job_run(cmd, NULL, wp_done, (void *)(intptr_t)home)) {
		wp_status("The picture could not be prepared (no memory to start it)");
		return;
	}
	wp.busy = true;
	wp_status("Preparing the picture...");
}

static void wp_where_cb(lv_event_t *e)
{
	const char *what = lv_event_get_user_data(e);

	if (wp_choice)
		wp_run(wp_choice, what);
}

static void wp_none_cb(lv_event_t *e)
{
	wp_run("-", lv_event_get_user_data(e));
}

static int wp_cmp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static bool wp_is_picture(const char *n)
{
	size_t l = strlen(n);

	return n[0] != '.' && l > 4 && (!strcasecmp(n + l - 4, ".jpg") || !strcasecmp(n + l - 5, ".jpeg") ||
					!strcasecmp(n + l - 4, ".png"));
}

static void wp_pick_open(lv_event_t *e)
{
	const char *name;
	lv_obj_t *scr, *body, *c;

	LV_UNUSED(e);
	if (!wp_choice)
		return;
	nav_mark(wp_pick_open, NULL);	/* (built again after a turn or a new picture) */
	name = strrchr(wp_choice, '/') ? strrchr(wp_choice, '/') + 1 : wp_choice;
	body = m3_page(name, &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_HOME, "Home screen", "Behind the apps", wp_where_cb, "home", NULL);
	m3_row(c, LV_SYMBOL_EYE_CLOSE, "Lock screen", "Behind the clock", wp_where_cb, "lock", NULL);
	m3_row(c, LV_SYMBOL_IMAGE, "Both", "Home and lock screens", wp_where_cb, "both", NULL);
	wp_status_lbl = m3_label(body, wp.busy ? "Preparing the picture..." : "", &lv_font_montserrat_14,
				 M.on_surface_v);
	lv_obj_set_width(wp_status_lbl, lv_pct(100));
	lv_label_set_long_mode(wp_status_lbl, LV_LABEL_LONG_WRAP);	/* (an error's reason) */
	obj_track(&wp_status_lbl);
	push(scr);
}

static void wp_pick_cb(lv_event_t *e)
{
	char path[512];

	snprintf(path, sizeof(path), WP_PICTURES "/%s", (const char *)lv_event_get_user_data(e));
	free(wp_choice);
	wp_choice = strdup(path);
	wp_pick_open(NULL);
}

static void wp_names_free(lv_event_t *e)
{
	char **v = lv_event_get_user_data(e);

	for (int i = 0; v && v[i]; i++)
		free(v[i]);
	free(v);
}

static void wp_accent_cb(lv_event_t *e)
{
	conf.wp_accent = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	ui_restart("wallpaper");	/* (the palette is made at start) */
}

static void open_wallpaper(lv_event_t *e)
{
	nav_mark(open_wallpaper, NULL);
	lv_obj_t *scr, *body, *c;
	DIR *d = opendir(WP_PICTURES);
	struct dirent *de;
	char **names = NULL;
	int n = 0, cap = 0, i;

	LV_UNUSED(e);
	body = m3_page("Wallpaper", &scr);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	m3_row(c, LV_SYMBOL_HOME, "Home screen", wp.set[WP_HOME] ? "Picture" : "None", NULL, NULL, NULL);
	if (wp.set[WP_HOME])
		m3_row(c, LV_SYMBOL_CLOSE, "Remove from home", "Back to the plain colour",
		       wp_none_cb, "none-home", NULL);
	m3_row(c, LV_SYMBOL_EYE_CLOSE, "Lock screen", wp.set[WP_LOCK] ? "Picture" : "None", NULL, NULL, NULL);
	if (wp.set[WP_LOCK])
		m3_row(c, LV_SYMBOL_CLOSE, "Remove from lock", "Back to the plain colour",
		       wp_none_cb, "none-lock", NULL);
	if (wp.set[WP_HOME] && wp.hue >= 0) {
		lv_obj_t *r, *sw;

		r = m3_row(c, LV_SYMBOL_TINT, "Colours from the wallpaper", "The accent of the home picture",
			   NULL, NULL, NULL);
		sw = ios_switch(lv_switch_create(r));
		if (conf.wp_accent)
			lv_obj_add_state(sw, LV_STATE_CHECKED);
		lv_obj_add_event_cb(sw, wp_accent_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}
	while (d && (de = readdir(d))) {
		if (!wp_is_picture(de->d_name))
			continue;
		if (n + 2 > cap) {
			char **nv = realloc(names, (cap = cap ? cap * 2 : 16) * sizeof(*nv));

			if (!nv)
				break;
			names = nv;
		}
		if (!(names[n] = strdup(de->d_name)))
			break;
		names[++n] = NULL;
	}
	if (d)
		closedir(d);
	c = m3_card(body, M.surface_c);
	lv_obj_set_style_pad_all(c, 4, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	if (!n) {
		m3_row(c, LV_SYMBOL_DIRECTORY, "No pictures", "JPEG or PNG files in Files > Pictures",
		       NULL, NULL, NULL);
	} else {
		qsort(names, n, sizeof(*names), wp_cmp);
		for (i = 0; i < n; i++)
			m3_row(c, LV_SYMBOL_IMAGE, names[i], "Choose for home or lock", wp_pick_cb, names[i], NULL);
		lv_obj_add_event_cb(scr, wp_names_free, LV_EVENT_DELETE, names);
	}
	push(scr);
}
