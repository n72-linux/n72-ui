/*
 * ui_games.c - n72-ui's Games page, #included from ui_m3.c (after the
 * Files and YouTube pages: it uses m3_page, push, m3_row_ex, shq,
 * app_launch...).
 *
 *   Games    every game in GAMES_DIRS (one folder level deep), grouped by
 *            system; a row starts it in RetroArch (n72-retro's
 *            n72-retro-launch: the core from the file type, the picture
 *            turned by the display, the gamepad overlay in this theme)
 *
 * A game is an application like any other (the card grows from its row;
 * Home ends it, and RetroArch saves on the way out), but it plays in
 * landscape: on the side the iPod was last held sideways (game_rot), which
 * the launcher's layer follows while it plays (rot_wanted).
 */

#include <strings.h>
#include "lvgl/src/libs/lodepng/lodepng.h"

#define GAMES_LAUNCH	"/root/retro/n72-retro-launch"
#define GAMES_MAX	600

static const char *const games_dirs[] = { "/root/Games", "/root/retro/games" };

/* file types and their systems (as n72-retro-launch picks the cores) */
static const struct { const char *ext, *system; } games_types[] = {
	{ "nes", "NES" }, { "fds", "NES" }, { "unf", "NES" }, { "unif", "NES" },
	{ "sfc", "Super Nintendo" }, { "smc", "Super Nintendo" },
	{ "fig", "Super Nintendo" }, { "swc", "Super Nintendo" },
	{ "gb", "Game Boy" }, { "dmg", "Game Boy" }, { "gbc", "Game Boy Color" },
	{ "gba", "Game Boy Advance" },
	{ "md", "Mega Drive" }, { "gen", "Mega Drive" }, { "smd", "Mega Drive" },
	{ "bin", "Mega Drive" }, { "32x", "32X" }, { "sms", "Master System" },
	{ "gg", "Game Gear" }, { "sg", "SG-1000" },
	{ "cue", "PlayStation" }, { "chd", "PlayStation" }, { "pbp", "PlayStation" },
	{ "m3u", "PlayStation" }, { "ccd", "PlayStation" }, { "iso", "PlayStation" },
	{ "zip", "Arcade" },
};

struct game {
	char *path;
	char name[96];
	const char *system;
	int order;		/* the system's place in games_types */
	lv_draw_buf_t *art;	/* its box art, small (games_art_next) */
	int8_t art_state;	/* 0 not looked for yet, 1 none, 2 art, 3 being made */
};

#define GAMES_ART_CACHE	"/var/cache/n72-ui/art"	/* box art made small, by --thumb */
static bool games_art_busy;		/* a --thumb running */

#define GAMES_STATES	"/root/retro/states"	/* (defaults.cfg's savestate_directory) */
#define GAMES_THUMBS	"/root/retro/thumbnails"	/* RetroArch's thumbnail layout */
#define GAMES_ART_W	44	/* box art in a row: at most this */
#define GAMES_ART_H	56

/* libretro's database names: RetroArch's thumbnail folders */
static const struct { const char *system, *db; } games_dbs[] = {
	{ "NES", "Nintendo - Nintendo Entertainment System" },
	{ "Super Nintendo", "Nintendo - Super Nintendo Entertainment System" },
	{ "Game Boy", "Nintendo - Game Boy" },
	{ "Game Boy Color", "Nintendo - Game Boy Color" },
	{ "Game Boy Advance", "Nintendo - Game Boy Advance" },
	{ "Mega Drive", "Sega - Mega Drive - Genesis" },
	{ "32X", "Sega - 32X" },
	{ "Master System", "Sega - Master System - Mark III" },
	{ "Game Gear", "Sega - Game Gear" },
	{ "SG-1000", "Sega - SG-1000" },
	{ "PlayStation", "Sony - PlayStation" },
	{ "Arcade", "MAME" },
};

/* "/x/Super_Game (USA).sfc" -> "Super_Game (USA)": the file's own name, as
 * RetroArch names its states and thumbnails */
static void games_base(const char *path, char *out, size_t len)
{
	const char *b = strrchr(path, '/'), *dot;
	size_t n;

	b = b ? b + 1 : path;
	dot = strrchr(b, '.');
	n = dot ? (size_t)(dot - b) : strlen(b);
	if (n >= len)
		n = len - 1;
	memcpy(out, b, n);
	out[n] = 0;
}

/*
 * A PNG, smaller: fitted into max_w x max_h (never larger than it is),
 * each pixel the average of the ones it covers (box art scaled down
 * without nearest-neighbour shimmer), alpha kept (ARGB8888, straight).
 * NULL if it cannot be read or is over 1.5 megapixels (decoding one
 * takes w x h x 4 bytes twice). (Box art in the rows, the save pictures.)
 */
static lv_draw_buf_t *games_png_thumb(const char *path, int max_w, int max_h)
{
	unsigned char *data = NULL, *out = NULL;
	unsigned w, h, err, tw, th, x, y;
	lv_draw_buf_t *b = NULL;
	LodePNGState st;
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || n > (8 << 20) ||
	    fseek(f, 0, SEEK_SET) || !(data = malloc((size_t)n)) ||
	    fread(data, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(data);
		return NULL;
	}
	fclose(f);
	lodepng_state_init(&st);
	st.info_raw.colortype = LCT_RGBA;
	st.info_raw.bitdepth = 8;
	err = lodepng_inspect(&w, &h, &st, data, (size_t)n);
	if (!err && w && h && (uint64_t)w * h <= 1536 * 1024)
		err = lodepng_decode(&out, &w, &h, &st, data, (size_t)n);
	else
		err = err ? err : 1;
	lodepng_state_cleanup(&st);
	free(data);
	if (err || !out) {
		if (out)
			lv_draw_buf_destroy((lv_draw_buf_t *)out);
		return NULL;
	}
	/* (LVGL's lodepng gives an LVGL draw buffer, R G B A packed) */
	if ((uint64_t)w * max_h > (uint64_t)h * max_w) {	/* wide: by width */
		tw = LV_MIN((unsigned)max_w, w);
		th = LV_MAX(1u, (unsigned)((uint64_t)h * tw / w));
	} else {
		th = LV_MIN((unsigned)max_h, h);
		tw = LV_MAX(1u, (unsigned)((uint64_t)w * th / h));
	}
	b = lv_draw_buf_create(tw, th, LV_COLOR_FORMAT_ARGB8888, 0);
	if (b) {
		const uint8_t *rgba = ((lv_draw_buf_t *)out)->data;

		for (y = 0; y < th; y++) {
			unsigned y0 = (unsigned)((uint64_t)y * h / th), y1 = (unsigned)((uint64_t)(y + 1) * h / th);
			uint8_t *o = b->data + y * b->header.stride;

			if (y1 <= y0)
				y1 = y0 + 1;
			for (x = 0; x < tw; x++, o += 4) {
				unsigned x0 = (unsigned)((uint64_t)x * w / tw), x1 = (unsigned)((uint64_t)(x + 1) * w / tw);
				uint32_t r = 0, g = 0, bl = 0, a = 0, cnt = 0, xx, yy;

				if (x1 <= x0)
					x1 = x0 + 1;
				for (yy = y0; yy < y1; yy++)
					for (xx = x0; xx < x1; xx++) {
						const uint8_t *q = rgba + ((size_t)yy * w + xx) * 4;

						r += q[0] * q[3];	/* (by alpha: no dark fringes) */
						g += q[1] * q[3];
						bl += q[2] * q[3];
						a += q[3];
						cnt++;
					}
				o[0] = a ? (uint8_t)(bl / a) : 0;
				o[1] = a ? (uint8_t)(g / a) : 0;
				o[2] = a ? (uint8_t)(r / a) : 0;
				o[3] = (uint8_t)(a / cnt);
			}
		}
	}
	lv_draw_buf_destroy((lv_draw_buf_t *)out);
	return b;
}

/*
 * A game's box art, if the user has put one: a PNG beside the game with its
 * name ("Super Mario World (USA).png"), or RetroArch's own thumbnail layout,
 * GAMES_THUMBS/<database>/Named_Boxarts/<name>.png (its characters
 * &*\\/:`<>?| as _). None is downloaded. Its path into @out; false if
 * there is none.
 */
static bool games_art_src(const struct game *g, char *out, size_t len, struct stat *st)
{
	char base[128], *c;
	const char *dot = strrchr(g->path, '.');
	size_t i;

	if (dot && (size_t)(dot - g->path) + 5 < len) {
		memcpy(out, g->path, (size_t)(dot - g->path));
		strcpy(out + (dot - g->path), ".png");
		if (!stat(out, st) && S_ISREG(st->st_mode))
			return true;
	}
	games_base(g->path, base, sizeof(base));
	for (c = base; *c; c++)
		if (strchr("&*/:`<>?\\|", *c))
			*c = '_';
	for (i = 0; i < sizeof(games_dbs) / sizeof(games_dbs[0]); i++)
		if (!strcmp(g->system, games_dbs[i].system)) {
			snprintf(out, len, GAMES_THUMBS "/%s/Named_Boxarts/%s.png",
				 games_dbs[i].db, base);
			return !stat(out, st) && S_ISREG(st->st_mode);
		}
	return false;
}

/*
 * The cache: a picture made small once (n72-ui --thumb, in a process of its
 * own: a large PNG takes a second or more to decode here, which on the UI
 * thread stalled the page), kept as GAMES_ART_CACHE/<key>.argb, the key
 * from the picture's path, size, time and the size asked for (a changed
 * picture is made again). "N72A", width and height (16 bits each, little
 * endian), then the pixels, ARGB8888 (straight alpha), row after row.
 */
static void games_art_cache_path(const char *src, const struct stat *st, int w, int h,
				 char *out, size_t len)
{
	uint64_t k = 1469598103934665603ULL;	/* FNV-1a */
	char key[PATH_MAX + 64];
	const unsigned char *c;

	snprintf(key, sizeof(key), "%s|%lld|%lld|%dx%d", src, (long long)st->st_size,
		 (long long)st->st_mtime, w, h);
	for (c = (const unsigned char *)key; *c; c++)
		k = (k ^ *c) * 1099511628211ULL;
	snprintf(out, len, GAMES_ART_CACHE "/%016llx.argb", (unsigned long long)k);
}

static lv_draw_buf_t *games_art_cache_load(const char *path)
{
	unsigned char hd[8];
	lv_draw_buf_t *b = NULL;
	FILE *f = fopen(path, "rb");
	unsigned w, h, y;

	struct stat st;

	if (!f)
		return NULL;
	if (fread(hd, 1, 8, f) == 8 && !memcmp(hd, "N72A", 4) && !fstat(fileno(f), &st)) {
		w = hd[4] | hd[5] << 8;
		h = hd[6] | hd[7] << 8;
		/* (the whole picture or nothing: its size is the header's) */
		if (w && h && w <= 512 && h <= 512 && st.st_size == 8 + (off_t)w * h * 4 &&
		    (b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0))) {
			for (y = 0; y < h; y++)
				if (fread(b->data + y * b->header.stride, 4, w, f) != w) {
					lv_draw_buf_destroy(b);
					b = NULL;
					break;
				}
		}
	}
	fclose(f);
	if (b)
		utimensat(AT_FDCWD, path, NULL, 0);	/* (used now: see games_art_sweep) */
	return b;
}

/*
 * Cache files not used for 30 days (their games gone, or their pictures
 * replaced: a new key), and any half-written ones (.tmp: a helper killed
 * halfway), are removed when the Games page opens.
 */
static void games_art_sweep(void)
{
	DIR *d = opendir(GAMES_ART_CACHE);
	struct dirent *de;
	time_t old = time(NULL) - 30 * 24 * 3600;

	if (!d)
		return;
	while ((de = readdir(d))) {
		char path[PATH_MAX];
		struct stat st;
		size_t n = strlen(de->d_name);

		if (de->d_name[0] == '.' ||
		    (size_t)snprintf(path, sizeof(path), GAMES_ART_CACHE "/%s", de->d_name) >= sizeof(path) ||
		    stat(path, &st))
			continue;
		if ((n > 4 && !strcmp(de->d_name + n - 4, ".tmp") && !games_art_busy) ||
		    st.st_mtime < old)
			unlink(path);
	}
	closedir(d);
}

/* `n72-ui --thumb SRC OUT W H`: SRC fitted into W x H, written to OUT (the
 * cache's format); 0 when done. Run by the Games page (games_art_next). */
int games_thumb_main(int argc, char **argv)
{
	char tmp[PATH_MAX + 8];
	lv_draw_buf_t *b;
	FILE *f;
	unsigned y, w, h;
	unsigned char hd[8] = { 'N', '7', '2', 'A' };
	int ok = 1;

	struct sched_param sp = { 0 };

	if (argc < 6)
		return 2;
	/* only when the UI has nothing to do: one core, and nice alone still
	 * took frames from a scroll (11 of 117 over 20 ms, one 47 ms) */
	sched_setscheduler(0, SCHED_IDLE, &sp);
	lv_init();	/* (lodepng gives its picture as an LVGL draw buffer) */
	b = games_png_thumb(argv[2], atoi(argv[4]), atoi(argv[5]));
	if (!b)
		return 1;
	w = b->header.w;
	h = b->header.h;
	hd[4] = w & 0xff; hd[5] = w >> 8; hd[6] = h & 0xff; hd[7] = h >> 8;
	snprintf(tmp, sizeof(tmp), "%s.tmp", argv[3]);
	if (!(f = fopen(tmp, "wb")))
		return 1;
	if (fwrite(hd, 1, 8, f) != 8)
		ok = 0;
	for (y = 0; ok && y < h; y++)
		if (fwrite(b->data + y * b->header.stride, 4, w, f) != w)
			ok = 0;
	if (fclose(f) || !ok || rename(tmp, argv[3])) {
		unlink(tmp);
		return 1;
	}
	return 0;
}

static const char *games_system_of(const char *path, int *order)
{
	const char *dot = strrchr(path, '.');
	char ext[8];
	size_t i;

	if (!dot || strlen(dot + 1) >= sizeof(ext))
		return NULL;
	for (i = 0; dot[1 + i]; i++)
		ext[i] = (char)tolower((unsigned char)dot[1 + i]);
	ext[i] = 0;
	for (i = 0; i < sizeof(games_types) / sizeof(games_types[0]); i++)
		if (!strcmp(ext, games_types[i].ext)) {
			*order = (int)i;
			return games_types[i].system;
		}
	return NULL;
}

/* "Super_Game (USA).sfc" -> "Super Game (USA)" */
static void games_name(const char *path, char *out, size_t len)
{
	const char *base = strrchr(path, '/');
	const char *dot;
	size_t n, i;

	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	n = dot ? (size_t)(dot - base) : strlen(base);
	if (n >= len)
		n = len - 1;
	for (i = 0; i < n; i++)
		out[i] = base[i] == '_' ? ' ' : base[i];
	out[n] = 0;
}

/* ".md" is also Markdown: a text file there is not a Mega Drive game */
static bool games_is_text(const char *path)
{
	unsigned char b[256];
	FILE *f = fopen(path, "rb");
	size_t n, i;

	if (!f)
		return true;
	n = fread(b, 1, sizeof(b), f);
	fclose(f);
	for (i = 0; i < n; i++)
		if (b[i] < 0x09 || (b[i] > 0x0d && b[i] < 0x20))
			return false;	/* a control byte: binary */
	return true;
}

/* a .bin beside a .cue of the same name is that disc's track, not a game */
static bool games_is_cue_track(const char *path)
{
	char cue[PATH_MAX];
	const char *dot = strrchr(path, '.');
	size_t n;

	if (!dot || strcasecmp(dot, ".bin"))
		return false;
	n = (size_t)(dot - path);
	if (n + 5 > sizeof(cue))
		return false;
	memcpy(cue, path, n);
	strcpy(cue + n, ".cue");
	return access(cue, R_OK) == 0;
}

static void gp_named(const char *path, const char *system);

/*
 * A .zip is an arcade set (MAME's: many files, no ROM extensions) or one
 * game zipped (a .sfc in it, ...): the system of the first file in it whose
 * type is a console's. From the zip's central directory (its end only is
 * read). .bin is not counted (arcade sets have them). n72-retro-launch
 * does the same with unzip -l.
 */
static const char *games_zip_system(const char *path, int *order)
{
	unsigned char tail[1024], *cd = NULL;
	const char *sys = NULL;
	FILE *f = fopen(path, "rb");
	long size, at;
	size_t n, i;
	uint32_t cd_size, cd_off;
	unsigned cnt, k;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22)
		goto out;
	at = size > (long)sizeof(tail) ? size - (long)sizeof(tail) : 0;
	if (fseek(f, at, SEEK_SET) || (n = fread(tail, 1, sizeof(tail), f)) < 22)
		goto out;
	for (i = n - 22; ; i--) {	/* the end record (after a comment, if any) */
		if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6)
			break;
		if (!i)
			goto out;
	}
	cnt = tail[i + 10] | tail[i + 11] << 8;
	cd_size = tail[i + 12] | tail[i + 13] << 8 | tail[i + 14] << 16 | (uint32_t)tail[i + 15] << 24;
	cd_off = tail[i + 16] | tail[i + 17] << 8 | tail[i + 18] << 16 | (uint32_t)tail[i + 19] << 24;
	if (!cd_size || cd_size > 256 * 1024 || (long)cd_off + (long)cd_size > size ||
	    !(cd = malloc(cd_size)) || fseek(f, cd_off, SEEK_SET) ||
	    fread(cd, 1, cd_size, f) != cd_size)
		goto out;
	for (i = 0, k = 0; k < cnt && i + 46 <= cd_size; k++) {
		unsigned nl = cd[i + 28] | cd[i + 29] << 8, xl = cd[i + 30] | cd[i + 31] << 8,
			 cl = cd[i + 32] | cd[i + 33] << 8;
		char name[256];
		const char *s;
		int o;

		if (memcmp(cd + i, "PK\1\2", 4) || i + 46 + nl > cd_size)
			break;
		snprintf(name, sizeof(name), "%.*s", (int)nl, (const char *)cd + i + 46);
		i += 46 + nl + xl + cl;
		if (strrchr(name, '.') && strcasecmp(strrchr(name, '.'), ".bin") &&
		    (s = games_system_of(name, &o)) && strcmp(s, "Arcade")) {
			sys = s;
			*order = o;
			break;
		}
	}
out:
	free(cd);
	fclose(f);
	return sys;
}

static void games_scan(const char *dir, int depth, struct game *v, int *n)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d)) && *n < GAMES_MAX) {
		char path[PATH_MAX];
		struct stat st;
		const char *sys, *z;
		int order = 0;

		if (de->d_name[0] == '.')
			continue;
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= sizeof(path))
			continue;
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth > 0)
				games_scan(path, depth - 1, v, n);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !(sys = games_system_of(path, &order)) ||
		    games_is_cue_track(path))
			continue;
		if (!strcasecmp(strrchr(path, '.'), ".md") && games_is_text(path))
			continue;
		if (!strcasecmp(strrchr(path, '.'), ".zip") && (z = games_zip_system(path, &order)))
			sys = z;	/* one game zipped */
		v[*n].path = strdup(path);
		if (!v[*n].path)
			continue;
		games_name(path, v[*n].name, sizeof(v[*n].name));
		v[*n].system = sys;
		v[*n].order = order;
		(*n)++;
	}
	closedir(d);
}

static int games_cmp(const void *a, const void *b)
{
	const struct game *x = a, *y = b;
	int c = strcmp(x->system, y->system);

	return c ? c : strcasecmp(x->name, y->name);
}

/*
 * The page: a section and a card for each system, the rows reused as the
 * page scrolls (rows_sync, as Files does): a library of hundreds of games
 * makes a few dozen rows.
 */
struct gpage {
	struct game *v;
	int n, nl;
	lv_obj_t *body;
	struct flist *list;	/* a card a system: v[base] ... v[base + n - 1] */
	lv_timer_t *art_timer;	/* box art looked for, a game a tick */
	lv_draw_buf_t *cont_pic;	/* the Continue card's picture */
	int cont;		/* the game to continue, or -1 */
};

static void games_art_next(lv_timer_t *t);

static void games_row_cb(lv_event_t *e);

/*
 * A row's picture: the box art if the game has one (the badge gives way to
 * it), else the gamepad badge. Rows are built with an image in the badge
 * (games_row_new).
 */
static void games_row_art(lv_obj_t *r, const struct game *g)
{
	lv_obj_t *badge = lv_obj_get_child(r, 0), *glyph = lv_obj_get_child(badge, 0),
		 *img = lv_obj_get_child(badge, 1);

	if (g->art_state == 2 && g->art) {
		lv_obj_set_size(badge, g->art->header.w, g->art->header.h);
		lv_obj_set_style_radius(badge, 6, 0);
		lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
		lv_obj_add_flag(glyph, LV_OBJ_FLAG_HIDDEN);
		lv_image_set_src(img, g->art);
		lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_set_size(badge, 40, 40);
		lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
		lv_obj_remove_flag(glyph, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
	}
}

static void games_bind(void *ctx, struct flist *l, int j, int i)
{
	struct gpage *gp_ = ctx;
	struct game *g = &gp_->v[l->base + i];
	lv_obj_t *r = l->row[j], *col = lv_obj_get_child(r, 1);

	lv_obj_set_pos(r, 0, i * FROW_H);
	lv_obj_set_user_data(r, (void *)(intptr_t)(l->base + i));
	lv_label_set_text(lv_obj_get_child(col, 0), g->name);
	lv_label_set_text(lv_obj_get_child(col, 1), g->system);
	games_row_art(r, g);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
	l->at[j] = i;
	if (!g->art_state && gp_->art_timer)
		lv_timer_resume(gp_->art_timer);	/* (its art, a tick later) */
}

/*
 * Box art for the games on screen, a game a tick: from the cache when it
 * has been made (a small file: quick), else made by n72-ui --thumb (one
 * at a time, at low priority), the row keeping its badge until it lands
 * (games_art_made). The timer sleeps once those games have been looked at;
 * a scroll or a picture made wakes it.
 */
static struct gpage *games_cur;		/* the Games page up, if any */

static void games_art_made(const char *out, void *ud);

/* The row showing game @i of page @g, if one does */
static lv_obj_t *games_row_of(struct gpage *g, int i)
{
	int k, j;

	for (k = 0; k < g->nl; k++) {
		struct flist *l = &g->list[k];

		if (i < l->base || i >= l->base + l->n)
			continue;
		for (j = 0; j < FROW_POOL && l->row[j]; j++)
			if (l->at[j] == i - l->base)
				return l->row[j];
	}
	return NULL;
}

static void games_art_next(lv_timer_t *t)
{
	struct gpage *g = lv_timer_get_user_data(t);
	int k, j;

	if (app_pid != 0 || lv_obj_get_screen(g->body) != lv_screen_active()) {
		lv_timer_pause(t);
		return;
	}
	for (k = 0; k < g->nl; k++) {
		struct flist *l = &g->list[k];

		for (j = 0; j < FROW_POOL && l->row[j]; j++) {
			char src[PATH_MAX], cache[PATH_MAX], q1[PATH_MAX * 2], q2[PATH_MAX * 2],
			     cmd[PATH_MAX * 4 + 96];
			struct game *gm;
			struct stat st;

			if (l->at[j] < 0)
				continue;
			gm = &g->v[l->base + l->at[j]];
			if (gm->art_state)
				continue;
			if (!games_art_src(gm, src, sizeof(src), &st)) {
				gm->art_state = 1;
				continue;	/* (a stat or two: the next at once) */
			}
			games_art_cache_path(src, &st, GAMES_ART_W, GAMES_ART_H, cache, sizeof(cache));
			if ((gm->art = games_art_cache_load(cache))) {
				gm->art_state = 2;
				games_row_art(l->row[j], gm);
				return;		/* one a tick */
			}
			if (games_art_busy)
				continue;	/* (its turn once the one running is made) */
			mkdir("/var/cache/n72-ui", 0755);
			mkdir(GAMES_ART_CACHE, 0755);
			shq(src, q1, sizeof(q1));
			shq(cache, q2, sizeof(q2));
			snprintf(cmd, sizeof(cmd), "nice -n 10 /proc/%d/exe --thumb %s %s %d %d",
				 (int)getpid(), q1, q2, GAMES_ART_W, GAMES_ART_H);
			if (job_run(cmd, NULL, games_art_made, strdup(gm->path))) {
				gm->art_state = 1;	/* (no memory to start it) */
				continue;
			}
			games_art_busy = true;
			gm->art_state = 3;
			return;
		}
	}
	lv_timer_pause(t);
}

/* A --thumb has ended: its game's art from the cache, then the next */
static void games_art_made(const char *out, void *ud)
{
	char *path = ud;
	struct gpage *g = games_cur;
	int i;

	LV_UNUSED(out);
	games_art_busy = false;
	/* (the page it was made for may have gone, or been rebuilt by a turn:
	 * the game is found by its path in the page up now) */
	for (i = 0; g && path && i < g->n; i++) {
		struct game *gm = &g->v[i];
		char src[PATH_MAX], cache[PATH_MAX];
		struct stat st;
		lv_obj_t *r;

		if (strcmp(gm->path, path))
			continue;
		if (gm->art_state == 2)
			break;
		gm->art_state = 1;
		if (games_art_src(gm, src, sizeof(src), &st)) {
			games_art_cache_path(src, &st, GAMES_ART_W, GAMES_ART_H, cache, sizeof(cache));
			if ((gm->art = games_art_cache_load(cache)))
				gm->art_state = 2;
		}
		if (gm->art && (r = games_row_of(g, i)))
			games_row_art(r, gm);
		break;
	}
	free(path);
	if (g && g->art_timer)
		lv_timer_resume(g->art_timer);	/* (the next one) */
}

static void games_page_ev(lv_event_t *e)
{
	struct gpage *g = lv_event_get_user_data(e);
	int i;

	if (lv_event_get_code(e) != LV_EVENT_DELETE) {
		rows_sync(g->body, g->list, g->nl, games_bind, g);
		return;
	}
	if (g->art_timer)
		lv_timer_delete(g->art_timer);
	if (games_cur == g)
		games_cur = NULL;
	for (i = 0; i < g->n; i++) {
		free(g->v[i].path);
		if (g->v[i].art)
			lv_draw_buf_destroy(g->v[i].art);
	}
	if (g->cont_pic)
		lv_draw_buf_destroy(g->cont_pic);
	free(g->v);
	free(g->list);
	free(g);
}

/* Game i starts, in landscape on the side last held, growing from @from */
static void games_start(struct gpage *g, int i, lv_obj_t *from)
{
	char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 64];

	if (i < 0 || i >= g->n)
		return;
	game_rot = ui_rot ? ui_rot : rot_side;
	gp_named(g->v[i].path, g->v[i].system);
	shq(g->v[i].path, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), GAMES_LAUNCH " --rot %d %s", game_rot, q);
	app_launch(cmd, from);
}

/* A row */
static void games_row_cb(lv_event_t *e)
{
	lv_obj_t *r = lv_event_get_current_target(e);

	games_start(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(r), r);
}

/* The Continue card (its button, or the card) */
static void games_cont_cb(lv_event_t *e)
{
	struct gpage *g = lv_event_get_user_data(e);
	lv_obj_t *card = lv_obj_get_user_data(lv_event_get_current_target(e));

	games_start(g, g->cont, card ? card : lv_event_get_current_target(e));
}

/*
 * The game last played: the newest of RetroArch's auto saves (written when
 * a game ends; savestate_auto_load brings it back at the next start), in
 * GAMES_STATES/<core>/<name>.state.auto, matched to a game by its file
 * name. -1 if none. @when gets the save's time, @pic its picture's path.
 */
static int games_last_played(struct gpage *g, time_t *when, char *pic, size_t pic_len)
{
	DIR *d = opendir(GAMES_STATES), *cd;
	struct dirent *de, *se;
	char best[PATH_MAX] = "", bname[128] = "";
	time_t best_t = 0;
	int i;

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		char dir[PATH_MAX];

		if (de->d_name[0] == '.' ||
		    (size_t)snprintf(dir, sizeof(dir), GAMES_STATES "/%s", de->d_name) >= sizeof(dir) ||
		    !(cd = opendir(dir)))
			continue;
		while ((se = readdir(cd))) {
			size_t n = strlen(se->d_name);
			char path[PATH_MAX];
			struct stat st;

			if (n <= 11 || strcmp(se->d_name + n - 11, ".state.auto") ||
			    (size_t)snprintf(path, sizeof(path), "%s/%s", dir, se->d_name) >= sizeof(path) ||
			    stat(path, &st) || st.st_mtime <= best_t)
				continue;
			best_t = st.st_mtime;
			snprintf(best, sizeof(best), "%s", path);
			snprintf(bname, sizeof(bname), "%.*s", (int)(n - 11), se->d_name);
		}
		closedir(cd);
	}
	closedir(d);
	if (!best_t)
		return -1;
	for (i = 0; i < g->n; i++) {
		char base[128];

		games_base(g->v[i].path, base, sizeof(base));
		if (!strcmp(base, bname)) {
			*when = best_t;
			snprintf(pic, pic_len, "%s.png", best);
			return i;
		}
	}
	return -1;		/* (a game no longer in the library) */
}

/* The Continue card: the last game's picture where it was left, a button */
static void games_cont_card(struct gpage *g, lv_obj_t *body)
{
	char pic[PATH_MAX + 8], when[64];
	struct tm tm, now_tm;
	time_t t = 0, now = time(NULL);
	lv_obj_t *card, *frame, *col, *b, *l;

	g->cont = games_last_played(g, &t, pic, sizeof(pic));
	if (g->cont < 0)
		return;
	m3_section(body, "Continue");
	card = m3_card(body, M.surface_c);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(card, 12, 0);
	lv_obj_set_style_pad_all(card, 12, 0);

	frame = lv_obj_create(card);
	plain(frame);
	lv_obj_set_size(frame, 112, 84);
	lv_obj_set_style_pad_all(frame, 0, 0);
	lv_obj_set_style_radius(frame, 12, 0);
	lv_obj_set_style_clip_corner(frame, true, 0);
	lv_obj_set_style_bg_color(frame, M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
	lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	if ((g->cont_pic = games_png_thumb(pic, 112, 84))) {
		lv_obj_t *im = lv_image_create(frame);

		lv_image_set_src(im, g->cont_pic);
		lv_obj_center(im);
	} else {
		lv_obj_center(m3_label(frame, N72_SYMBOL_GAMEPAD, &n72_icon_24, M.on_surface_v));
	}

	col = lv_obj_create(card);
	lv_obj_remove_style_all(col);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	l = m3_label(col, g->v[g->cont].name, &lv_font_montserrat_16, M.on_surface);
	lv_obj_set_size(l, lv_pct(100), 20);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	localtime_r(&t, &tm);
	localtime_r(&now, &now_tm);
	if (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday)
		strftime(when, sizeof(when), "Played %H:%M", &tm);
	else
		strftime(when, sizeof(when), "Played %e %b", &tm);
	l = m3_label(col, g->v[g->cont].system, &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_set_size(l, lv_pct(100), 18);	/* (one line, "..." when long) */
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	l = m3_label(col, when, &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_set_style_pad_bottom(l, 8, 0);
	b = m3_button(col, LV_SYMBOL_PLAY "  Continue", true, games_cont_cb, g);
	lv_obj_set_width(b, lv_pct(100));	/* (the column's width: it fits) */
	lv_obj_set_style_pad_hor(b, 8, 0);
	lv_obj_set_user_data(b, card);	/* (the game grows out of the card) */
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_user_data(card, card);
	lv_obj_add_event_cb(card, games_cont_cb, LV_EVENT_CLICKED, g);
}

/* A pooled row: m3_row_ex's, with an image in its badge for box art */
static lv_obj_t *games_row_new(lv_obj_t *card, struct gpage *g)
{
	lv_obj_t *r = m3_row_ex(card, N72_SYMBOL_GAMEPAD, M.tertiary_c, M.on_tertiary_c,
				"", "", games_row_cb, g, NULL, false);
	lv_obj_t *img = lv_image_create(lv_obj_get_child(r, 0));

	lv_obj_center(img);
	lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
	return r;
}

void open_games(lv_event_t *e)
{
	struct gpage *g = calloc(1, sizeof(*g));
	lv_obj_t *scr, *body;
	int i, j;
	size_t k;

	LV_UNUSED(e);
	if (!g || !(g->v = calloc(GAMES_MAX, sizeof(*g->v)))) {
		free(g);
		return;
	}
	nav_mark(open_games, NULL);	/* rebuilt when the screen turns */
	for (k = 0; k < sizeof(games_dirs) / sizeof(games_dirs[0]); k++)
		games_scan(games_dirs[k], 1, g->v, &g->n);
	qsort(g->v, g->n, sizeof(*g->v), games_cmp);
	for (i = 0; i < g->n; i++)	/* (the systems: one card each) */
		if (!i || strcmp(g->v[i - 1].system, g->v[i].system))
			g->nl++;
	g->list = calloc(g->nl ? g->nl : 1, sizeof(*g->list));
	g->cont = -1;

	body = m3_page("Games", &scr);
	g->body = body;
	lv_obj_add_event_cb(body, games_page_ev, LV_EVENT_DELETE, g);
	if (g->list && g->n)
		games_cont_card(g, body);
	if (access(GAMES_LAUNCH, X_OK)) {
		m3_note(body, "RetroArch is not installed (" GAMES_LAUNCH " is missing).");
	} else if (!g->n || !g->list) {
		m3_note(body, "No games yet. Put them in /root/Games, in folders by "
			      "system or all together: NES, Super Nintendo, Game Boy "
			      "(Color, Advance), Mega Drive, Master System, Game Gear, "
			      "PlayStation (.cue, .chd, .pbp) and arcade (.zip). Box "
			      "art: a PNG beside a game with its name.");
	}
	for (i = 0, k = 0; g->list && i < g->n; k++) {
		struct flist *l = &g->list[k];
		lv_obj_t *c;

		l->base = i;
		while (i < g->n && !strcmp(g->v[i].system, g->v[l->base].system))
			i++;
		l->n = i - l->base;
		m3_section(body, g->v[l->base].system);
		l->card = c = m3_card(body, M.surface_c);
		lv_obj_set_style_pad_all(c, FROW_PAD, 0);
		lv_obj_set_height(c, l->n * FROW_H + 2 * FROW_PAD);
		for (j = 0; j < FROW_POOL && j < l->n; j++) {
			l->row[j] = games_row_new(c, g);
			lv_obj_add_flag(l->row[j], LV_OBJ_FLAG_HIDDEN);
			l->at[j] = -1;
		}
	}
	if (g->n == GAMES_MAX)
		m3_note(body, "Only the first 600 games are shown.");
	lv_obj_add_event_cb(body, games_page_ev, LV_EVENT_SCROLL, g);
	lv_obj_add_event_cb(body, games_page_ev, LV_EVENT_SIZE_CHANGED, g);
	g->art_timer = lv_timer_create(games_art_next, 15, g);
	games_cur = g;
	games_art_sweep();
	lv_obj_update_layout(scr);
	rows_sync(body, g->list, g->nl, games_bind, g);
	push(scr);
}

/*
 * The pause panel: Home over a game pauses it and shows this card over the
 * picture (RetroArch keeps its layer; the card is on the launcher's, so
 * every touch is the launcher's while it is up). Resume, save or load the
 * game's state (slot 0, RetroArch's own .state files; the card shows the
 * picture of the last save), RetroArch's own menu (its settings), Quit.
 * A tap beside the card resumes; Home again ends the game, as Home did
 * before (RetroArch saves on the way out).
 *
 * RetroArch is driven through its UDP command port (network_cmd_enable,
 * n72-retro-launch): PAUSE_TOGGLE only toggles, so the panel asks
 * GET_STATUS first and toggles until the answer is what it wants.
 */
#define GP_PORT		55355
#define GP_STATES	GAMES_STATES
#define GP_THUMB_H	84

static struct {
	lv_obj_t *scrim, *thumb, *saved, *load_btn;
	lv_draw_buf_t *pic;
	int sock;
	int want;		/* -1 nothing, 0 playing, 1 paused */
	int tries;
	const char *then;	/* a command once the wish is met */
	lv_timer_t *timer;
	char core[64], base[128];	/* from GET_STATUS: the states' folder and name */
	time_t saved_at;	/* the state file's time before a save */
	int save_looks;
	char name[96];
	const char *system;
} gp = { .sock = -1, .want = -1 };

static void gp_build(void);
static void gp_close(void);
static void gp_reply(int fd);

bool gp_up(void)
{
	return gp.scrim != NULL;
}

static void gp_send(const char *m)
{
	if (gp.sock < 0) {
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(GP_PORT),
					 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		gp.sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (gp.sock < 0)
			return;
		if (connect(gp.sock, (struct sockaddr *)&a, sizeof(a))) {
			close(gp.sock);
			gp.sock = -1;
			return;
		}
		ui_watch(gp.sock, gp_reply);
	}
	if (send(gp.sock, m, strlen(m), 0) < 0)
		;	/* (RetroArch not listening yet: the next look sends again) */
}

static void gp_timer_cb(lv_timer_t *t);

static void gp_later(uint32_t ms)
{
	if (gp.timer)
		lv_timer_delete(gp.timer);
	gp.timer = lv_timer_create(gp_timer_cb, ms, NULL);
	lv_timer_set_repeat_count(gp.timer, 1);
}

/* Ask how RetroArch is (the answer comes to gp_reply) */
static void gp_sync(void)
{
	gp_send("GET_STATUS");
	gp_later(400);		/* no answer: ask again (gp_timer_cb) */
}

/* @paused: be paused, or playing; then send @then (or NULL) */
static void gp_want(int paused, const char *then)
{
	gp.want = paused;
	gp.then = then;
	gp.tries = 0;
	gp_sync();
}

static void gp_status(bool paused)
{
	if (gp.want < 0)
		return;
	if (paused == (gp.want == 1)) {
		gp.want = -1;
		if (gp.then)
			gp_send(gp.then);
		gp.then = NULL;
		if (gp.timer) {
			lv_timer_delete(gp.timer);
			gp.timer = NULL;
		}
		return;
	}
	if (gp.tries++ >= 3) {	/* (it does not take: leave it) */
		fprintf(stderr, "n72-ui: RetroArch did not %s\n", gp.want ? "pause" : "resume");
		gp.want = -1;
		gp.then = NULL;
		return;
	}
	gp_send("PAUSE_TOGGLE");
	gp_later(120);		/* (taken at its next frame) then asked again */
}

static bool gp_thumb_load(void);

/* "GET_STATUS PLAYING Snes9x 2005,gothicvania,crc32=88d63376" */
static void gp_reply(int fd)
{
	char b[512], *p, *c;
	ssize_t n;

	while ((n = recv(fd, b, sizeof(b) - 1, 0)) >= 0 || errno == ECONNREFUSED) {
		bool paused;

		if (n < 0)
			continue;	/* (a refusal of an earlier send: nobody listened) */
		b[n] = 0;
		if (strncmp(b, "GET_STATUS ", 11))
			continue;
		p = b + 11;
		paused = !strncmp(p, "PAUSED", 6);
		if (strncmp(p, "PLAYING ", 8) && strncmp(p, "PAUSED ", 7))
			continue;	/* (CONTENTLESS: not a game) */
		p = strchr(p, ' ') + 1;
		if ((c = strchr(p, ','))) {
			char core[64], base[128];
			char *e;

			snprintf(core, sizeof(core), "%.*s", (int)(c - p), p);
			e = strchr(c + 1, ',');
			snprintf(base, sizeof(base), "%.*s", e ? (int)(e - c - 1) : (int)strlen(c + 1), c + 1);
			if (strcmp(core, gp.core) || strcmp(base, gp.base)) {
				snprintf(gp.core, sizeof(gp.core), "%s", core);
				snprintf(gp.base, sizeof(gp.base), "%s", base);
				gp_thumb_load();
			}
		}
		gp_status(paused);
	}
}

static void gp_state_path(char *out, size_t len, const char *ext)
{
	snprintf(out, len, GP_STATES "/%s/%s.state%s", gp.core, gp.base, ext);
}

static time_t gp_state_time_of(const char *ext)
{
	char path[PATH_MAX];
	struct stat st;

	if (!gp.core[0])
		return 0;
	gp_state_path(path, sizeof(path), ext);
	return stat(path, &st) ? 0 : st.st_mtime;
}

static time_t gp_state_time(void)
{
	return gp_state_time_of("");
}

/* The save's picture (RetroArch's .state.png), smaller, and its time: true
 * once the picture is up */
static bool gp_thumb_load(void)
{
	char path[PATH_MAX], when[64];
	time_t t = gp_state_time();

	if (!gp_up())
		return false;
	if (gp.load_btn) {
		if (t)
			lv_obj_remove_state(gp.load_btn, LV_STATE_DISABLED);
		else
			lv_obj_add_state(gp.load_btn, LV_STATE_DISABLED);
	}
	if (!t) {
		lv_label_set_text(gp.saved, "No save yet");
		return false;
	}
	{
		struct tm tm, now_tm;
		time_t now = time(NULL);

		localtime_r(&t, &tm);
		localtime_r(&now, &now_tm);
		if (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday)
			strftime(when, sizeof(when), "Saved %H:%M", &tm);
		else
			strftime(when, sizeof(when), "Saved %e %b, %H:%M", &tm);
		lv_label_set_text(gp.saved, when);
	}
	gp_state_path(path, sizeof(path), ".png");
	{
		lv_draw_buf_t *b = games_png_thumb(path, 120, GP_THUMB_H);

		if (!b)
			return false;
		lv_image_set_src(gp.thumb, b);
		lv_obj_set_size(gp.thumb, b->header.w, b->header.h);
		if (gp.pic)
			lv_draw_buf_destroy(gp.pic);
		gp.pic = b;
		return true;
	}
}

/* After Save: the file changes when RetroArch has written it */
static void gp_timer_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	gp.timer = NULL;
	if (gp.save_looks > 0) {
		time_t now = gp_state_time();

		/* RetroArch writes the state, then its picture: both new */
		if (now && now != gp.saved_at && gp_state_time_of(".png") >= now &&
		    gp_thumb_load()) {
			gp.save_looks = 0;
		} else if (--gp.save_looks == 0) {
			if (gp_up() && (!now || now == gp.saved_at))
				lv_label_set_text(gp.saved, "Not saved");
			else
				gp_thumb_load();	/* (saved; the picture as it is) */
		} else {
			gp_later(250);
		}
		return;
	}
	if (gp.want >= 0)
		gp_sync();
}

static void gp_resume_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	gp_close();
	gp_want(0, NULL);
}

static void gp_save_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	gp.saved_at = gp_state_time();
	gp_send("SAVE_STATE");
	lv_label_set_text(gp.saved, "Saving...");
	gp.save_looks = 12;	/* (3 s) */
	gp_later(250);
}

static void gp_load_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	gp_send("LOAD_STATE");
	gp_close();
	gp_want(0, NULL);
}

static void gp_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	gp_close();
	gp_want(0, "MENU_TOGGLE");	/* (RetroArch's menu pauses the game itself) */
}

static void gp_quit_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	gp_close();
	go_home();		/* the game ends as by Home (it saves on the way out) */
}

static void gp_scrim_cb(lv_event_t *e)
{
	if (lv_event_get_target(e) == gp.scrim)
		gp_resume_cb(e);	/* beside the card */
}

static lv_obj_t *gp_tonal(lv_obj_t *parent, const char *icon, const char *text, lv_event_cb_t cb)
{
	char t[64];
	lv_obj_t *b;

	snprintf(t, sizeof(t), "%s  %s", icon, text);
	b = m3_button(parent, t, false, cb, NULL);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_pad_hor(b, 12, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	return b;
}

static void gp_build(void)
{
	lv_obj_t *card, *head, *body, *left, *right, *row, *b, *frame;

	gp.scrim = lv_obj_create(app_scr);
	plain(gp.scrim);
	lv_obj_set_size(gp.scrim, W, H);
	lv_obj_set_pos(gp.scrim, 0, 0);
	lv_obj_set_style_radius(gp.scrim, 0, 0);
	lv_obj_set_style_bg_color(gp.scrim, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(gp.scrim, LV_OPA_50, 0);
	lv_obj_remove_flag(gp.scrim, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(gp.scrim, gp_scrim_cb, LV_EVENT_CLICKED, NULL);

	card = m3_card(gp.scrim, M.surface_c_high);
	lv_obj_set_width(card, LV_MIN(W - 32, 448));
	lv_obj_set_style_radius(card, 28, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, 14, 0);
	lv_obj_center(card);

	head = lv_obj_create(card);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(head, 2, 0);
	b = m3_label(head, gp.name[0] ? gp.name : "Game", &lv_font_montserrat_20, M.on_surface);
	lv_obj_set_width(b, lv_pct(100));
	lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
	m3_label(head, gp.system ? gp.system : "Paused", &lv_font_montserrat_14, M.on_surface_v);

	body = lv_obj_create(card);
	lv_obj_remove_style_all(body);
	lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(body, 16, 0);

	/* the last save: its picture and time */
	left = lv_obj_create(body);
	lv_obj_remove_style_all(left);
	lv_obj_set_size(left, 120, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(left, 6, 0);
	frame = lv_obj_create(left);
	plain(frame);
	lv_obj_set_size(frame, 120, GP_THUMB_H + 6);
	lv_obj_set_style_pad_all(frame, 0, 0);
	lv_obj_set_style_radius(frame, 12, 0);
	lv_obj_set_style_clip_corner(frame, true, 0);
	lv_obj_set_style_bg_color(frame, M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
	lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	gp.thumb = lv_image_create(frame);
	lv_obj_center(gp.thumb);
	gp.saved = m3_label(left, "", &lv_font_montserrat_12, M.on_surface_v);

	right = lv_obj_create(body);
	lv_obj_remove_style_all(right);
	lv_obj_set_height(right, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(right, 1);
	lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(right, 8, 0);
	b = m3_button(right, LV_SYMBOL_PLAY "  Resume", true, gp_resume_cb, NULL);
	lv_obj_set_width(b, lv_pct(100));
	row = lv_obj_create(right);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 8, 0);
	gp_tonal(row, LV_SYMBOL_SAVE, "Save", gp_save_cb);
	gp.load_btn = gp_tonal(row, LV_SYMBOL_UPLOAD, "Load", gp_load_cb);
	row = lv_obj_create(right);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 8, 0);
	gp_tonal(row, LV_SYMBOL_SETTINGS, "Settings", gp_menu_cb);
	gp_tonal(row, LV_SYMBOL_CLOSE, "Quit", gp_quit_cb);

	gp_thumb_load();	/* (again once GET_STATUS names the files) */
}

static void gp_close(void)
{
	if (gp.scrim)
		lv_obj_delete(gp.scrim);
	gp.scrim = gp.thumb = gp.saved = gp.load_btn = NULL;
	if (gp.pic) {
		lv_draw_buf_destroy(gp.pic);
		gp.pic = NULL;
	}
	app_dormant_sync();
}

/*
 * The Home key (main.c). A game in front: the panel opens and the game
 * pauses; with the panel up, Home ends the game. True if Home was used.
 */
bool gp_home(void)
{
	if (!app_is_game() || lv_screen_active() != app_scr || locked)
		return false;
	if (gp_up()) {
		gp_close();
		return false;	/* (go_home ends it) */
	}
	if (app_dormant)
		app_dormant_wake(false);
	app_bar_set(false);
	shade_set(false, false);
	gp_build();
	gp_want(1, NULL);
	return true;
}

void games_art_resume(void)
{
	if (games_cur && games_cur->art_timer && app_pid == 0 &&
	    lv_obj_get_screen(games_cur->body) == lv_screen_active())
		lv_timer_resume(games_cur->art_timer);
}

/* The game has ended (app_closed) */
void gp_gone(void)
{
	gp_close();
	if (gp.timer) {
		lv_timer_delete(gp.timer);
		gp.timer = NULL;
	}
	if (gp.sock >= 0) {
		ui_unwatch(gp.sock);
		close(gp.sock);
		gp.sock = -1;
	}
	gp.want = -1;
	gp.then = NULL;
	gp.save_looks = 0;
	gp.core[0] = gp.base[0] = 0;
	app_dormant_sync();
}

/* The game about to start: its name and system for the panel */
static void gp_named(const char *path, const char *system)
{
	games_name(path, gp.name, sizeof(gp.name));
	gp.system = system;	/* (a games_types string: static) */
}
