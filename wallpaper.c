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
int wp_convert_main(int argc, char **argv)
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
		for (t = 0; t < 1; t++) {	/* (upright only: turned, the screen turns it) */
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

static struct {
	lv_obj_t *img;
	int kind;
} wp_slots[WP_HOLDS];
static bool wp_suspended;

static void wp_slot_gone(lv_event_t *e)
{
	lv_obj_t *img = lv_event_get_current_target(e);

	for (int i = 0; i < WP_HOLDS; i++)
		if (wp_slots[i].img == img) {
			wp_slots[i].img = NULL;
			break;
		}
}

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
bool wp_hold(lv_obj_t *img, lv_draw_buf_t *b)
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

static void wp_blur_clear(void);

/* (Re)read what is set: at the start, and after a conversion. Frames on demand. */
void wp_load(void)
{
	int k, t;

	wp_blur_clear();
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
lv_draw_buf_t *wp_upright(int kind)
{
	if (wp_suspended || !wp.set[kind])
		return NULL;
	if (!wp.pic[kind][0])
		wp.pic[kind][0] = wp_read(kind, 0);
	return wp.pic[kind][0];
}

/*
 * Turned, the picture does not turn with the screen: it stays as it is in
 * the hand (the user's choice), so the screen shows the upright frame
 * turned back a quarter (one GPU copy, ~600 KB, kept while turned). Layer
 * 0 then holds the upright frame as it is under a turned home too, and a
 * conversion makes no turned frame.
 */
lv_draw_buf_t *wp_pic(int kind)
{
	lv_draw_buf_t *u = wp_upright(kind);

	if (!u || !ui_rot) {
		wp_drop(kind, 1);
		return u;
	}
	if (wp.pic[kind][1] && wp.turn[kind] != ui_rot)
		wp_drop(kind, 1);
	if (!wp.pic[kind][1]) {
		wp.pic[kind][1] = mbx_gpu_turn_buf(u, 360 - ui_rot);	/* (the screen's turn, undone) */
		wp.turn[kind] = ui_rot;
	}
	return wp.pic[kind][1];
}

/* The wallpaper under everything on @scr (its first child); false: none set. */
bool wp_put(lv_obj_t *scr, int kind)
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
	for (int k = 0; k < WP_HOLDS; k++) {
		if (!wp_slots[k].img) {
			wp_slots[k].img = i;
			wp_slots[k].kind = kind;
			lv_obj_add_event_cb(i, wp_slot_gone, LV_EVENT_DELETE, NULL);
			break;
		}
	}
	return true;
}

/* ----------------------------------------------------- frosted glass */

#define WP_GLASS_R	24
#define WP_GLASS_MAX	8

static bool glass_lut_ok;
static uint8_t glass_corner_a[WP_GLASS_R][WP_GLASS_R];
static uint8_t glass_corner_rim[WP_GLASS_R][WP_GLASS_R];

static struct {
	uint8_t *rgb;		/* bw * bh * 3, untinted blurred wallpaper */
	int bw, bh;
	int w, h, rot;
} wp_blur[WP_KINDS];

static struct {
	uint32_t *px;		/* PW * PG_CARD_H, XRGB8888 (0xffRRGGBB) */
	int px0, pw;
	uint32_t tint;
	bool dark;
} wp_card_strip;

static struct {
	lv_obj_t *obj;
	lv_draw_buf_t *b;
	int kind;
	int32_t sx, sy, w, h, r;
} wp_glass_slots[WP_GLASS_MAX];

static void wp_glass_lut_init(void)
{
	int dy, dx;

	if (glass_lut_ok)
		return;
	for (dy = 0; dy < WP_GLASS_R; dy++) {
		for (dx = 0; dx < WP_GLASS_R; dx++) {
			float dist = hypotf((float)(WP_GLASS_R - 0.5f) - (float)dx,
					    (float)(WP_GLASS_R - 0.5f) - (float)dy);
			float sdf = (float)WP_GLASS_R - dist;
			float a = LV_CLAMP(0.0f, sdf + 0.5f, 1.0f);
			float rw = LV_CLAMP(0.0f, 1.0f - fabsf(sdf - 0.7f) / 0.85f, 1.0f) * a;

			glass_corner_a[dy][dx] = (uint8_t)lroundf(a * 255.0f);
			glass_corner_rim[dy][dx] = (uint8_t)lroundf(rw * 255.0f);
		}
	}
	glass_lut_ok = true;
}

static inline uint32_t glass_apply_rim(uint32_t rgb, uint8_t rim_w, bool top_half, bool dark)
{
	uint32_t base = dark ? (top_half ? 46u : 26u) : (top_half ? 96u : 56u);
	uint32_t k = (base * rim_w + 128u) >> 8;
	uint32_t r = (rgb >> 16) & 255u, g = (rgb >> 8) & 255u, b = rgb & 255u;

	if (!k)
		return rgb & 0x00ffffffu;
	r += ((255u - r) * k + 128u) >> 8;
	g += ((255u - g) * k + 128u) >> 8;
	b += ((255u - b) * k + 128u) >> 8;
	return (r << 16) | (g << 8) | b;
}

static bool wp_kind_has_wp(int kind)
{
	return kind == WP_HOME ? home_wp : kind == WP_LOCK ? lock_wp : false;
}

static uint32_t wp_glass_tint(int kind, bool *dark_out)
{
	bool dark = kind == WP_LOCK || conf.dark;

	if (dark_out)
		*dark_out = dark;
	/* The lock screen always uses light text/icons, so keep its glass dark in light mode too. */
	if (kind == WP_LOCK && !conf.dark)
		return lv_color_to_u32(lv_color_mix(M.surface_c_high, lv_color_black(), 44)) & 0xffffffu;
	return lv_color_to_u32(M.surface_c_high) & 0xffffffu;
}

static void wp_glass_fallback(lv_obj_t *obj, int kind, int32_t radius)
{
	lv_obj_set_style_bg_image_src(obj, NULL, 0);
	lv_obj_set_style_radius(obj, radius, 0);
	if (kind == WP_LOCK) {
		lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(obj, LV_OPA_50, 0);
	} else {
		lv_obj_set_style_bg_color(obj, M.surface_c_high, 0);
		lv_obj_set_style_bg_opa(obj, home_wp ? LV_OPA_80 : LV_OPA_COVER, 0);
	}
}

static void wp_blur_clear_kind(int kind)
{
	if (kind < 0 || kind >= WP_KINDS)
		return;
	free(wp_blur[kind].rgb);
	memset(&wp_blur[kind], 0, sizeof(wp_blur[kind]));
	if (kind == WP_HOME) {
		free(wp_card_strip.px);
		memset(&wp_card_strip, 0, sizeof(wp_card_strip));
	}
}

static void wp_blur_clear(void)
{
	for (int k = 0; k < WP_KINDS; k++)
		wp_blur_clear_kind(k);
}

/* 3-pass separable 9-tap binomial Gaussian [1, 8, 28, 56, 70, 56, 28, 8, 1] / 256. */
static bool wp_blur_ensure(int kind)
{
	lv_draw_buf_t *src;
	int bw, bh, x, y, pass;
	uint8_t *rgb, *tmp;

	if (kind < 0 || kind >= WP_KINDS || !wp_kind_has_wp(kind) || wp_suspended)
		return false;
	if (wp_blur[kind].rgb && wp_blur[kind].w == W && wp_blur[kind].h == H && wp_blur[kind].rot == ui_rot)
		return true;
	src = wp_pic(kind);
	if (!src || (int)src->header.w != W || (int)src->header.h != H)
		return false;
	wp_blur_clear_kind(kind);
	bw = W / 4;
	bh = H / 4;
	if (bw < 2 || bh < 2)
		return false;
	rgb = malloc((size_t)bw * bh * 3);
	tmp = malloc((size_t)bw * bh * 3);
	if (!rgb || !tmp) {
		free(rgb);
		free(tmp);
		return false;
	}
	/* 4x4 box downsample from XRGB8888 */
	for (y = 0; y < bh; y++) {
		const uint32_t *r0 = (const uint32_t *)(src->data + (size_t)(y * 4 + 0) * src->header.stride);
		const uint32_t *r1 = (const uint32_t *)(src->data + (size_t)(y * 4 + 1) * src->header.stride);
		const uint32_t *r2 = (const uint32_t *)(src->data + (size_t)(y * 4 + 2) * src->header.stride);
		const uint32_t *r3 = (const uint32_t *)(src->data + (size_t)(y * 4 + 3) * src->header.stride);
		uint8_t *o = rgb + (size_t)y * bw * 3;

		for (x = 0; x < bw; x++) {
			int sx = x * 4, k;
			uint32_t sr = 0, sg = 0, sb = 0;

			for (k = 0; k < 4; k++) {
				uint32_t p0 = r0[sx + k], p1 = r1[sx + k];
				uint32_t p2 = r2[sx + k], p3 = r3[sx + k];

				sr += ((p0 >> 16) & 255u) + ((p1 >> 16) & 255u) +
				      ((p2 >> 16) & 255u) + ((p3 >> 16) & 255u);
				sg += ((p0 >> 8) & 255u) + ((p1 >> 8) & 255u) +
				      ((p2 >> 8) & 255u) + ((p3 >> 8) & 255u);
				sb += (p0 & 255u) + (p1 & 255u) + (p2 & 255u) + (p3 & 255u);
			}
			o[x * 3 + 0] = (uint8_t)((sr + 8u) >> 4);
			o[x * 3 + 1] = (uint8_t)((sg + 8u) >> 4);
			o[x * 3 + 2] = (uint8_t)((sb + 8u) >> 4);
		}
	}
	for (pass = 0; pass < 3; pass++) {
		for (y = 0; y < bh; y++) {
			const uint8_t *in = rgb + (size_t)y * bw * 3;
			uint8_t *out = tmp + (size_t)y * bw * 3;

			for (x = 0; x < bw; x++) {
				int xm4 = LV_MAX(x - 4, 0), xm3 = LV_MAX(x - 3, 0);
				int xm2 = LV_MAX(x - 2, 0), xm1 = LV_MAX(x - 1, 0);
				int xp1 = LV_MIN(x + 1, bw - 1), xp2 = LV_MIN(x + 2, bw - 1);
				int xp3 = LV_MIN(x + 3, bw - 1), xp4 = LV_MIN(x + 4, bw - 1);
				int c;

				for (c = 0; c < 3; c++) {
					uint32_t s = in[xm4 * 3 + c] + in[xp4 * 3 + c]
						   + 8u * (in[xm3 * 3 + c] + in[xp3 * 3 + c])
						   + 28u * (in[xm2 * 3 + c] + in[xp2 * 3 + c])
						   + 56u * (in[xm1 * 3 + c] + in[xp1 * 3 + c])
						   + 70u * in[x * 3 + c];

					out[x * 3 + c] = (uint8_t)((s + 128u) >> 8);
				}
			}
		}
		for (y = 0; y < bh; y++) {
			const uint8_t *ym4 = tmp + (size_t)LV_MAX(y - 4, 0) * bw * 3;
			const uint8_t *ym3 = tmp + (size_t)LV_MAX(y - 3, 0) * bw * 3;
			const uint8_t *ym2 = tmp + (size_t)LV_MAX(y - 2, 0) * bw * 3;
			const uint8_t *ym1 = tmp + (size_t)LV_MAX(y - 1, 0) * bw * 3;
			const uint8_t *y0p = tmp + (size_t)y * bw * 3;
			const uint8_t *yp1 = tmp + (size_t)LV_MIN(y + 1, bh - 1) * bw * 3;
			const uint8_t *yp2 = tmp + (size_t)LV_MIN(y + 2, bh - 1) * bw * 3;
			const uint8_t *yp3 = tmp + (size_t)LV_MIN(y + 3, bh - 1) * bw * 3;
			const uint8_t *yp4 = tmp + (size_t)LV_MIN(y + 4, bh - 1) * bw * 3;
			uint8_t *out = rgb + (size_t)y * bw * 3;

			for (x = 0; x < bw * 3; x++) {
				uint32_t s = ym4[x] + yp4[x]
					   + 8u * (ym3[x] + yp3[x])
					   + 28u * (ym2[x] + yp2[x])
					   + 56u * (ym1[x] + yp1[x])
					   + 70u * y0p[x];

				out[x] = (uint8_t)((s + 128u) >> 8);
			}
		}
	}
	free(tmp);
	wp_blur[kind].rgb = rgb;
	wp_blur[kind].bw = bw;
	wp_blur[kind].bh = bh;
	wp_blur[kind].w = W;
	wp_blur[kind].h = H;
	wp_blur[kind].rot = ui_rot;
	return true;
}

static void wp_blur_upsample_rect(int kind, uint32_t *out, uint32_t out_stride_px,
				  int32_t sx, int32_t sy, int32_t rw, int32_t rh)
{
	int bw = wp_blur[kind].bw, bh = wp_blur[kind].bh;
	int x0[512], x1[512], tx[512];
	bool dark;
	uint32_t tint = wp_glass_tint(kind, &dark);
	uint32_t tint_a = dark ? 148u : 158u, wp_a = 256u - tint_a;
	uint32_t tr = ((tint >> 16) & 255u) * tint_a;
	uint32_t tg = ((tint >> 8) & 255u) * tint_a;
	uint32_t tb = (tint & 255u) * tint_a;
	int32_t x, y;

	if (!wp_blur[kind].rgb || rw <= 0 || rh <= 0 || rw > 512)
		return;
	for (x = 0; x < rw; x++) {
		int fx = LV_CLAMP(0, (int)(sx + x) * 64 - 96, (bw - 1) * 256);

		x0[x] = fx >> 8;
		x1[x] = LV_MIN(x0[x] + 1, bw - 1);
		tx[x] = fx & 255;
	}
	for (y = 0; y < rh; y++) {
		int fy = LV_CLAMP(0, (int)(sy + y) * 64 - 96, (bh - 1) * 256);
		int y0 = fy >> 8, y1 = LV_MIN(y0 + 1, bh - 1);
		int ty = fy & 255, ity = 256 - ty;
		const uint8_t *row0 = wp_blur[kind].rgb + (size_t)y0 * bw * 3;
		const uint8_t *row1 = wp_blur[kind].rgb + (size_t)y1 * bw * 3;
		uint32_t *dst = out + (size_t)y * out_stride_px;

		for (x = 0; x < rw; x++) {
			int ix0 = x0[x] * 3, ix1 = x1[x] * 3;
			int txv = tx[x], itx = 256 - txv;
			int w00 = itx * ity, w10 = txv * ity, w01 = itx * ty, w11 = txv * ty;
			int r = (row0[ix0 + 0] * w00 + row0[ix1 + 0] * w10 +
				 row1[ix0 + 0] * w01 + row1[ix1 + 0] * w11 + 32768) >> 16;
			int g = (row0[ix0 + 1] * w00 + row0[ix1 + 1] * w10 +
				 row1[ix0 + 1] * w01 + row1[ix1 + 1] * w11 + 32768) >> 16;
			int b = (row0[ix0 + 2] * w00 + row0[ix1 + 2] * w10 +
				 row1[ix0 + 2] * w01 + row1[ix1 + 2] * w11 + 32768) >> 16;
			int l = (r * 77 + g * 150 + b * 29) >> 8;

			r = LV_CLAMP(0, l + (((r - l) * 320) >> 8), 255);
			g = LV_CLAMP(0, l + (((g - l) * 320) >> 8), 255);
			b = LV_CLAMP(0, l + (((b - l) * 320) >> 8), 255);

			r = (int)((tr + (uint32_t)r * wp_a + 128u) >> 8);
			g = (int)((tg + (uint32_t)g * wp_a + 128u) >> 8);
			b = (int)((tb + (uint32_t)b * wp_a + 128u) >> 8);
			dst[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
		}
	}
}

static lv_draw_buf_t *wp_glass_build(int kind, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t radius)
{
	lv_draw_buf_t *b;
	int32_t r = radius == WP_GLASS_R && w >= 2 * WP_GLASS_R && h >= 2 * WP_GLASS_R ? WP_GLASS_R : 0;
	bool dark = kind == WP_LOCK || conf.dark;
	int32_t x, y;

	if (!wp_blur_ensure(kind) || w <= 0 || h <= 0 || w > 512)
		return NULL;
	wp_glass_lut_init();
	b = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_ARGB8888, 0);
	if (!b)
		return NULL;
	wp_blur_upsample_rect(kind, (uint32_t *)b->data, b->header.stride / 4, sx, sy, w, h);
	/* Only home reuses its blur across multiple widgets and sliding strips. */
	if (kind == WP_LOCK)
		wp_blur_clear_kind(WP_LOCK);
	for (y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(b->data + (size_t)y * b->header.stride);
		bool top_half = y < h / 2;

		if (r && (y < r || y >= h - r)) {
			int32_t cy = y < r ? y : h - 1 - y;

			if (y == 0 || y == h - 1) {
				for (x = r; x < w - r; x++)
					row[x] = 0xff000000u | glass_apply_rim(row[x], 255u, top_half, dark);
			}
			for (x = 0; x < r; x++) {
				uint8_t a = glass_corner_a[cy][x];

				if (!a) {
					row[x] = 0;
				} else {
					uint8_t rim = glass_corner_rim[cy][x];
					uint32_t rgb = rim ? glass_apply_rim(row[x], rim, top_half, dark)
							   : (row[x] & 0x00ffffffu);

					row[x] = ((uint32_t)a << 24) | rgb;
				}
			}
			for (x = w - r; x < w; x++) {
				uint8_t a = glass_corner_a[cy][w - 1 - x];

				if (!a) {
					row[x] = 0;
				} else {
					uint8_t rim = glass_corner_rim[cy][w - 1 - x];
					uint32_t rgb = rim ? glass_apply_rim(row[x], rim, top_half, dark)
							   : (row[x] & 0x00ffffffu);

					row[x] = ((uint32_t)a << 24) | rgb;
				}
			}
		} else {
			if (y == 0 || y == h - 1) {
				for (x = 0; x < w; x++)
					row[x] = 0xff000000u | glass_apply_rim(row[x], 255u, top_half, dark);
			} else {
				row[0] = 0xff000000u | glass_apply_rim(row[0], 255u, top_half, dark);
				row[w - 1] = 0xff000000u | glass_apply_rim(row[w - 1], 255u, top_half, dark);
			}
		}
	}
	lv_draw_buf_flush_cache(b, NULL);
	return b;
}

static void wp_glass_obj_gone(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target(e);

	for (int i = 0; i < WP_GLASS_MAX; i++) {
		if (wp_glass_slots[i].obj == obj) {
			if (wp_glass_slots[i].b)
				wp_free(wp_glass_slots[i].b);
			memset(&wp_glass_slots[i], 0, sizeof(wp_glass_slots[i]));
			break;
		}
	}
}

bool wp_glass_attach(lv_obj_t *obj, int kind, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t radius)
{
	int slot = -1;
	lv_draw_buf_t *b;

	if (!obj || kind < 0 || kind >= WP_KINDS || !wp_kind_has_wp(kind) || wp_suspended)
		return false;
	for (int i = 0; i < WP_GLASS_MAX; i++) {
		if (wp_glass_slots[i].obj == obj) {
			slot = i;
			break;
		}
		if (slot < 0 && !wp_glass_slots[i].obj)
			slot = i;
	}
	if (slot < 0)
		return false;
	if (wp_glass_slots[slot].obj == obj && wp_glass_slots[slot].b &&
	    wp_glass_slots[slot].kind == kind && wp_glass_slots[slot].sx == sx &&
	    wp_glass_slots[slot].sy == sy && wp_glass_slots[slot].w == w &&
	    wp_glass_slots[slot].h == h && wp_glass_slots[slot].r == radius)
		return true;
	b = wp_glass_build(kind, sx, sy, w, h, radius);
	if (!b)
		return false;
	if (wp_glass_slots[slot].obj == obj) {
		lv_obj_set_style_bg_image_src(obj, NULL, 0);
		if (wp_glass_slots[slot].b)
			wp_free(wp_glass_slots[slot].b);
	} else {
		lv_obj_add_event_cb(obj, wp_glass_obj_gone, LV_EVENT_DELETE, NULL);
	}
	wp_glass_slots[slot].obj = obj;
	wp_glass_slots[slot].b = b;
	wp_glass_slots[slot].kind = kind;
	wp_glass_slots[slot].sx = sx;
	wp_glass_slots[slot].sy = sy;
	wp_glass_slots[slot].w = w;
	wp_glass_slots[slot].h = h;
	wp_glass_slots[slot].r = radius;
	lv_obj_set_style_radius(obj, 0, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_image_src(obj, b, 0);
	lv_obj_set_style_bg_image_opa(obj, LV_OPA_COVER, 0);
	return true;
}

void wp_glass_detach(lv_obj_t *obj)
{
	if (!obj)
		return;
	for (int i = 0; i < WP_GLASS_MAX; i++) {
		if (wp_glass_slots[i].obj == obj) {
			int kind = wp_glass_slots[i].kind;
			int32_t r = wp_glass_slots[i].r;

			lv_obj_remove_event_cb(obj, wp_glass_obj_gone);
			wp_glass_fallback(obj, kind, r);
			if (wp_glass_slots[i].b)
				wp_free(wp_glass_slots[i].b);
			memset(&wp_glass_slots[i], 0, sizeof(wp_glass_slots[i]));
			break;
		}
	}
}

const uint32_t *wp_glass_card_strip(int32_t *out_w, int32_t *out_h)
{
	uint32_t tint = lv_color_to_u32(M.surface_c_high) & 0xffffffu;

	if (!wp_blur_ensure(WP_HOME) || PW <= 0 || PW > 512)
		return NULL;
	if (!wp_card_strip.px || wp_card_strip.px0 != PX || wp_card_strip.pw != PW ||
	    wp_card_strip.tint != tint || wp_card_strip.dark != conf.dark) {
		free(wp_card_strip.px);
		wp_card_strip.px = malloc((size_t)PW * PG_CARD_H * sizeof(uint32_t));
		if (!wp_card_strip.px)
			return NULL;
		wp_blur_upsample_rect(WP_HOME, wp_card_strip.px, (uint32_t)PW, PX, PG_TOP + 4, PW, PG_CARD_H);
		wp_card_strip.px0 = PX;
		wp_card_strip.pw = PW;
		wp_card_strip.tint = tint;
		wp_card_strip.dark = conf.dark;
	}
	if (out_w)
		*out_w = PW;
	if (out_h)
		*out_h = PG_CARD_H;
	return wp_card_strip.px;
}

static inline uint32_t glass_premul_px(uint32_t rgb, uint32_t al)
{
	uint32_t rb = (rgb & 0x00ff00ffu) * al + 0x00800080u;
	uint32_t g = ((rgb & 0x0000ff00u) >> 8) * al + 128u;

	rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	g = (g + (g >> 8)) & 0x0000ff00u;
	return (al << 24) | rb | g;
}

void wp_glass_compose_card(lv_draw_buf_t *dst, const uint32_t *strip, int32_t strip_w,
			   const lv_draw_buf_t *fg, const uint16_t *fg_x0, const uint16_t *fg_x1,
			   int32_t vx, int32_t radius, bool premul)
{
	int32_t cw = (int32_t)dst->header.w, ch = (int32_t)dst->header.h;
	int32_t r = radius == WP_GLASS_R && cw >= 2 * WP_GLASS_R && ch >= 2 * WP_GLASS_R ? WP_GLASS_R : 0;
	int32_t cx0 = LV_MAX(0, -vx), cx1 = LV_MIN(cw, strip_w - vx);
	int32_t y;

	if (cx0 >= cx1)
		return;
	wp_glass_lut_init();
	for (y = 0; y < ch; y++) {
		uint32_t *drow = (uint32_t *)(dst->data + (size_t)y * dst->header.stride);
		const uint32_t *srow = strip + (size_t)y * strip_w + vx;
		const uint32_t *frow = (const uint32_t *)(fg->data + (size_t)y * fg->header.stride);
		bool top_half = y < ch / 2;
		int32_t fx0 = LV_MAX(cx0, (int32_t)fg_x0[y]);
		int32_t fx1 = LV_MIN(cx1, (int32_t)fg_x1[y] + 1);
		int32_t x;

		memcpy(drow + cx0, srow + cx0, (size_t)(cx1 - cx0) * 4);
		if (y == 0 || y == ch - 1) {
			int32_t rx0 = LV_MAX(cx0, r), rx1 = LV_MIN(cx1, cw - r);

			for (x = rx0; x < rx1; x++)
				drow[x] = 0xff000000u | glass_apply_rim(drow[x], 255u, top_half, conf.dark);
		} else if (y >= r && y < ch - r) {
			if (cx0 == 0)
				drow[0] = 0xff000000u | glass_apply_rim(drow[0], 255u, top_half, conf.dark);
			if (cx1 == cw)
				drow[cw - 1] = 0xff000000u | glass_apply_rim(drow[cw - 1], 255u, top_half, conf.dark);
		}
		for (x = fx0; x < fx1; x++) {
			uint32_t s = frow[x], a = s >> 24;

			if (!a)
				continue;
			if (a >= 253u) {
				drow[x] = 0xff000000u | (s & 0x00ffffffu);
			} else {
				uint32_t d = drow[x], inv = 255u - a;
				uint32_t rb = (((s & 0x00ff00ffu) * a + (d & 0x00ff00ffu) * inv) >> 8) & 0x00ff00ffu;
				uint32_t g = ((((s & 0x0000ff00u) >> 8) * a + ((d & 0x0000ff00u) >> 8) * inv)) & 0x0000ff00u;

				drow[x] = 0xff000000u | rb | g;
			}
		}
		if (r && (y < r || y >= ch - r)) {
			int32_t cy = y < r ? y : ch - 1 - y;
			int32_t lx1 = LV_MIN(cx1, r), rx0 = LV_MAX(cx0, cw - r);

			for (x = cx0; x < lx1; x++) {
				uint8_t a = glass_corner_a[cy][x];

				if (!a) {
					drow[x] = 0;
				} else {
					uint8_t rim = glass_corner_rim[cy][x];
					uint32_t rgb = rim ? glass_apply_rim(drow[x], rim, top_half, conf.dark)
							   : (drow[x] & 0x00ffffffu);

					drow[x] = premul && a < 255u ? glass_premul_px(rgb, a)
								     : (((uint32_t)a << 24) | rgb);
				}
			}
			for (x = rx0; x < cx1; x++) {
				uint8_t a = glass_corner_a[cy][cw - 1 - x];

				if (!a) {
					drow[x] = 0;
				} else {
					uint8_t rim = glass_corner_rim[cy][cw - 1 - x];
					uint32_t rgb = rim ? glass_apply_rim(drow[x], rim, top_half, conf.dark)
							   : (drow[x] & 0x00ffffffu);

					drow[x] = premul && a < 255u ? glass_premul_px(rgb, a)
								     : (((uint32_t)a << 24) | rgb);
				}
			}
		}
	}
}

void wp_suspend(void)
{
	int i, k, t;

	if (wp_suspended)
		return;
	for (i = 0; i < WP_GLASS_MAX; i++) {
		lv_obj_t *obj = wp_glass_slots[i].obj;

		if (obj && lv_obj_is_valid(obj))
			lv_obj_set_style_bg_image_src(obj, NULL, 0);
		if (wp_glass_slots[i].b) {
			wp_free(wp_glass_slots[i].b);
			wp_glass_slots[i].b = NULL;
		}
	}
	wp_blur_clear();
	for (i = 0; i < WP_HOLDS; i++) {
		lv_obj_t *img = wp_slots[i].img;

		if (!img || !lv_obj_is_valid(img))
			continue;
		lv_image_set_src(img, NULL);
		lv_obj_remove_event_cb(img, wp_image_gone);
	}
	for (i = 0; i < WP_HOLDS; i++) {
		wp_holds[i].b = NULL;
		wp_holds[i].refs = 0;
		wp_holds[i].gone = false;
	}
	for (k = 0; k < WP_KINDS; k++) {
		for (t = 0; t < 2; t++) {
			if (wp.pic[k][t]) {
				wp_free(wp.pic[k][t]);
				wp.pic[k][t] = NULL;
			}
		}
	}
	wp_suspended = true;
}

void wp_resume(void)
{
	int i;

	if (!wp_suspended)
		return;
	wp_suspended = false;
	for (i = 0; i < WP_HOLDS; i++) {
		lv_obj_t *img = wp_slots[i].img;
		lv_draw_buf_t *b;

		if (!img || !lv_obj_is_valid(img))
			continue;
		b = wp_pic(wp_slots[i].kind);
		if (b && wp_hold(img, b)) {
			lv_image_set_src(img, b);
			lv_obj_invalidate(img);
		}
	}
	for (i = 0; i < WP_GLASS_MAX; i++) {
		lv_obj_t *obj = wp_glass_slots[i].obj;
		lv_draw_buf_t *b;

		if (!obj || !lv_obj_is_valid(obj) || wp_glass_slots[i].b)
			continue;
		b = wp_glass_build(wp_glass_slots[i].kind, wp_glass_slots[i].sx, wp_glass_slots[i].sy,
				   wp_glass_slots[i].w, wp_glass_slots[i].h, wp_glass_slots[i].r);
		if (b) {
			wp_glass_slots[i].b = b;
			lv_obj_set_style_radius(obj, 0, 0);
			lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
			lv_obj_set_style_bg_image_src(obj, b, 0);
			lv_obj_set_style_bg_image_opa(obj, LV_OPA_COVER, 0);
		} else {
			wp_glass_fallback(obj, wp_glass_slots[i].kind, wp_glass_slots[i].r);
		}
		lv_obj_invalidate(obj);
	}
}

/* ------------------------------------------------------- Settings page */

#define WP_PICTURES	"/root/Pictures"

static lv_obj_t *wp_status_lbl;
static char *wp_choice;		/* the picture picked, waiting for where */

void open_wallpaper(lv_event_t *e);

/*
 * A conversion is over: the screens are built again with the new frames,
 * as after a turn (ui_rebuild), when nothing is in the way: not while dark,
 * locked (the lock screen goes when unlocked), a slide or a home swipe.
 * With the home picture's colours in use the palette changes too, which
 * only a restart makes: that waits for no application as well (a restart
 * would lose it, and the lock).
 */
static lv_timer_t *wp_apply_t;	/* (a try, once, just after an event) */
static bool wp_apply_due;		/* new frames wait to be shown */
static bool wp_apply_restart;

static bool wp_apply_try(void)
{
	/* (pg.scr: also the frame between a swipe's arrival and home taking over) */
	if (app_dormant || mbx_gpu_is_suspended() ||
	    screen_off || locked || tr.scr || sc.owner || pg.on || pg.settling || pg.scr)
		return false;
	if (wp_apply_restart) {
		if (app_pid > 0)
			return false;
		ui_restart("wallpaper");
	}
	wp_load();
	if (app_pid <= 0) {
		/*
		 * The rebuild shows an empty screen while home and the pages
		 * are made again: a picture of the screen as it was lies over
		 * it on the top layer until the new ones are there.
		 */
		lv_draw_buf_t *pic;
		lv_obj_t *cover = NULL;

		lv_refr_now(NULL);
		pic = mbx_gpu_grab_screen();
		if (pic) {
			cover = lv_image_create(lv_layer_top());
			lv_image_set_src(cover, pic);
			lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_pos(cover, 0, 0);
		}
		ui_rebuild(true);
		if (cover) {
			lv_obj_delete(cover);
			lv_image_cache_drop(pic);
			lv_draw_buf_destroy(pic);
		}
		lv_obj_invalidate(lv_screen_active());
	} else {
		ui_rebuild(false);
	}
	return true;
}

static void wp_apply_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	wp_apply_t = NULL;	/* (one run: LVGL deletes it) */
	if (!wp_apply_due)
		return;
	wp_apply_due = false;	/* (the rebuild passes the kicks: no second try) */
	if (!wp_apply_try())
		wp_apply_due = true;	/* (the next event tries again) */
}

/*
 * Something that kept the new frames waiting may have ended (the screen
 * lit, an unlock, a slide's or swipe's end, a scroll let go, an
 * application closed): one try, a moment later, outside the caller.
 * Nothing runs in between: no timer looks while dark or locked.
 */
void wp_apply_kick(void)
{
	if (!wp_apply_due || wp_apply_t)
		return;
	wp_apply_t = lv_timer_create(wp_apply_cb, 30, NULL);
	lv_timer_set_repeat_count(wp_apply_t, 1);
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
	wp_apply_due = true;
	wp_apply_kick();
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

void open_wallpaper(lv_event_t *e)
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
	lock_col_section(body);
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
