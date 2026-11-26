// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): files, YouTube and Games. */
#include "ui_m3_priv.h"

/* -------------------------------------------------------------- files */

/*
 * Files: a page a folder, from FILES_ROOT (Back goes up a level, as any
 * page). Folders first, then files, by name; video and sound open with
 * n72-video, launched from the row as an application from its tile.
 */
struct fent {
	char *name;
	bool dir;
	int count;		/* a folder's items, -1 until a row shows it */
	off_t size;
};

static int fent_cmp(const void *a, const void *b)
{
	const struct fent *x = a, *y = b;

	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

static bool files_is_ipa(const char *name)
{
	const char *e = strrchr(name, '.');

	return e && !strcasecmp(e + 1, "ipa");
}

/* video, sound, picture, iOS app or other, by the name's extension (*kind: its word) */
static const char *files_icon(const char *name, bool *media, const char **kind)
{
	static const char *const video[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi",
		"flv", "3gp", "h264", "264", "m2v", "mpg", "mpeg", "ts" };
	static const char *const audio[] = { "mp3", "m4a", "aac", "ogg", "oga", "opus",
		"wav", "flac" };
	static const char *const image[] = { "png", "jpg", "jpeg", "gif", "bmp", "webp" };
	const char *e = strrchr(name, '.');
	size_t i;

	*media = false;
	*kind = "File";
	if (!e)
		return LV_SYMBOL_FILE;
	e++;
	if (!strcasecmp(e, "ipa"))
		return *kind = "iOS app", LV_SYMBOL_DOWNLOAD;
	for (i = 0; i < sizeof(video) / sizeof(video[0]); i++)
		if (!strcasecmp(e, video[i]))
			return *media = true, *kind = "Video", LV_SYMBOL_VIDEO;
	for (i = 0; i < sizeof(audio) / sizeof(audio[0]); i++)
		if (!strcasecmp(e, audio[i]))
			return *media = true, *kind = "Audio", LV_SYMBOL_AUDIO;
	for (i = 0; i < sizeof(image) / sizeof(image[0]); i++)
		if (!strcasecmp(e, image[i]))
			return *kind = "Picture", LV_SYMBOL_IMAGE;
	return LV_SYMBOL_FILE;
}

static void files_size(off_t n, char *buf, size_t len)
{
	if (n < 1024)
		snprintf(buf, len, "%ld bytes", (long)n);
	else if (n < 1024 * 1024)
		snprintf(buf, len, "%.1f kB", n / 1024.0);
	else if (n < 1024L * 1024 * 1024)
		snprintf(buf, len, "%.1f MB", n / (1024.0 * 1024));
	else
		snprintf(buf, len, "%.1f GB", n / (1024.0 * 1024 * 1024));
}

/* A folder's items (visible ones; counted up to 999) */
static int files_count(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;
	int n = 0;

	if (!d)
		return -2;
	while (n < 999 && (de = readdir(d)))
		if (de->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

/*
 * A folder's page builds rows only for what is on screen: each card is as
 * tall as all its rows, and a few row objects move and take the entries
 * that scroll into view. With a row for every entry (6 objects each), a
 * folder of 300 made every scroll step move ~1800 objects, and dropped
 * glides to 20-40 fps.
 */
#define FROW_H		64	/* a row (m3_row_ex) */
#define FROW_PAD	4	/* the card's padding */
#define FROW_POOL	14	/* rows a card has: a screen, landscape or portrait, and a row more each side */

struct fpage {
	char *path;
	struct fent *v;
	int n, nd;		/* entries, folders (first) of them */
	lv_obj_t *body;
	struct flist {
		lv_obj_t *card;
		int base, n;	/* its entries: v[base], ... v[base + n - 1] */
		lv_obj_t *row[FROW_POOL];
		int at[FROW_POOL];	/* the entry (of the card) a row shows; -1 none */
	} list[2];		/* folders, files */
};

static int floor_div(int a, int b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static void files_bind(struct fpage *fp, struct flist *l, int j, int i)
{
	struct fent *f = &fp->v[l->base + i];
	lv_obj_t *r = l->row[j], *badge = lv_obj_get_child(r, 0), *col = lv_obj_get_child(r, 1);
	lv_obj_t *glyph = lv_obj_get_child(badge, 0);
	const char *icon;
	lv_color_t bg, fg;
	char sub[48];

	if (f->dir) {
		if (f->count == -1) {
			char full[PATH_MAX];

			snprintf(full, sizeof(full), "%s/%s", fp->path, f->name);
			f->count = files_count(full);
		}
		icon = LV_SYMBOL_DIRECTORY;
		bg = M.primary_c;
		fg = M.on_primary_c;
		snprintf(sub, sizeof(sub), f->count < 0 ? "Folder" : f->count == 0 ? "Empty" :
			 f->count == 1 ? "1 item" : f->count == 999 ? "999+ items" : "%d items",
			 f->count);
	} else {
		const char *kind;
		char size[24];
		bool media, action;

		icon = files_icon(f->name, &media, &kind);
		action = media || files_is_ipa(f->name);
		bg = action ? M.tertiary_c : M.secondary_c;
		fg = action ? M.on_tertiary_c : M.on_secondary_c;
		files_size(f->size, size, sizeof(size));
		snprintf(sub, sizeof(sub), "%s, %s", kind, size);	/* (the fonts are ASCII) */
	}
	lv_obj_set_pos(r, 0, i * FROW_H);
	lv_obj_set_user_data(r, (void *)(intptr_t)(l->base + i));
	lv_obj_set_style_bg_color(badge, bg, 0);
	lv_obj_set_style_text_color(glyph, fg, 0);
	lv_label_set_text(glyph, icon);
	lv_label_set_text(lv_obj_get_child(col, 0), f->name);
	lv_label_set_text(lv_obj_get_child(col, 1), sub);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
	l->at[j] = i;
}

/*
 * Rows for the entries on screen, a row beyond each edge, in each of @nl
 * cards (struct flist): a card is as tall as all its entries, and holds
 * only FROW_POOL rows, which @bind(ctx, l, j, i) moves and fills (row j,
 * entry i of the card) as the page scrolls. (Files; the Games page.)
 */
static void rows_sync(lv_obj_t *body, struct flist *ls, int nl,
		      void (*bind)(void *ctx, struct flist *l, int j, int i), void *ctx)
{
	lv_area_t bc, cc;
	int k, i, j;

	lv_obj_get_coords(body, &bc);
	for (k = 0; k < nl; k++) {
		struct flist *l = &ls[k];
		int first, last;

		if (!l->card)
			continue;
		if (l->n <= FROW_POOL) {
			first = 0;
			last = l->n;
		} else {
			lv_obj_get_coords(l->card, &cc);
			first = LV_MAX(floor_div(bc.y1 - cc.y1 - FROW_PAD, FROW_H) - 1, 0);
			last = LV_MIN(floor_div(bc.y2 - cc.y1 - FROW_PAD, FROW_H) + 2, l->n);
		}
		for (j = 0; j < FROW_POOL; j++)
			if (l->row[j] && l->at[j] >= 0 && (l->at[j] < first || l->at[j] >= last)) {
				l->at[j] = -1;
				lv_obj_add_flag(l->row[j], LV_OBJ_FLAG_HIDDEN);
			}
		for (i = first; i < last; i++) {
			int free_j = -1;

			for (j = 0; j < FROW_POOL && l->row[j]; j++) {
				if (l->at[j] == i)
					break;
				if (l->at[j] < 0 && free_j < 0)
					free_j = j;
			}
			if (j < FROW_POOL && l->row[j])
				continue;	/* shown already */
			if (free_j < 0)
				break;		/* (no row left: a taller screen than counted) */
			bind(ctx, l, free_j, i);
		}
	}
}

static void files_bind_cb(void *ctx, struct flist *l, int j, int i)
{
	files_bind(ctx, l, j, i);
}

static void files_sync(struct fpage *fp)
{
	rows_sync(fp->body, fp->list, 2, files_bind_cb, fp);
}

void files_open_dir(const char *path);

static void files_row_cb(lv_event_t *e)
{
	struct fpage *fp = lv_event_get_user_data(e);
	lv_obj_t *r = lv_event_get_current_target(e);
	int i = (int)(intptr_t)lv_obj_get_user_data(r);
	char full[PATH_MAX], q[PATH_MAX * 2 + 16], cmd[PATH_MAX * 2 + 32];
	const char *kind;
	bool media;

	if (i < 0 || i >= fp->n)
		return;
	snprintf(full, sizeof(full), "%s/%s", fp->path, fp->v[i].name);
	if (fp->v[i].dir) {
		files_open_dir(full);
		return;
	}
	if (files_is_ipa(fp->v[i].name)) {
		open_ipa_install(full);
		return;
	}
	files_icon(fp->v[i].name, &media, &kind);
	if (!media)
		return;
	/* opened as an application from its tile */
	shq(full, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "n72-video %s", q);
	app_launch(cmd, r);
}

static void files_page_ev(lv_event_t *e)
{
	struct fpage *fp = lv_event_get_user_data(e);

	if (lv_event_get_code(e) != LV_EVENT_DELETE) {
		files_sync(fp);
		return;
	}
	for (int i = 0; i < fp->n; i++)
		free(fp->v[i].name);
	free(fp->v);
	free(fp->path);
	free(fp);
}

void files_open_dir(const char *path)
{
	nav_mark(NULL, path);
	const char *title = !strcmp(path, FILES_ROOT) ? "Files" : strrchr(path, '/') + 1;
	struct fpage *fp = calloc(1, sizeof(*fp));
	lv_obj_t *scr, *body, *c, *row;
	struct dirent *de;
	int cap = 0, k, j;
	DIR *d;

	if (!fp || !(fp->path = strdup(path))) {
		free(fp);
		return;
	}
	body = m3_page(title, &scr);
	fp->body = body;
	lv_obj_add_event_cb(body, files_page_ev, LV_EVENT_DELETE, fp);
	d = opendir(path);
	while (d && (de = readdir(d))) {
		struct stat st;

		if (de->d_name[0] == '.' || fstatat(dirfd(d), de->d_name, &st, 0))
			continue;
		if (fp->n == cap) {
			struct fent *nv = realloc(fp->v, (cap ? cap * 2 : 64) * sizeof(*nv));

			if (!nv)
				break;
			fp->v = nv;
			cap = cap ? cap * 2 : 64;
		}
		fp->v[fp->n].name = strdup(de->d_name);
		if (!fp->v[fp->n].name)
			break;
		fp->v[fp->n].dir = S_ISDIR(st.st_mode);
		fp->v[fp->n].count = -1;
		fp->v[fp->n].size = st.st_size;
		fp->n++;
	}
	if (fp->n)
		qsort(fp->v, fp->n, sizeof(*fp->v), fent_cmp);
	if (!fp->n) {
		/* empty state: the icon large, a headline, what to do */
		c = m3_card(body, M.surface_c);
		lv_obj_set_height(c, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(c, 24, 0);
		lv_obj_set_style_pad_row(c, 8, 0);
		m3_badge(c, LV_SYMBOL_DIRECTORY, M.surface_c_highest, M.on_surface_v, 56);
		m3_label(c, d ? "Nothing here yet" : "Cannot open this folder",
			 &lv_font_montserrat_16, M.on_surface);
		row = m3_label(c, d ? "Copy files over SSH or USB into this folder." :
			       "You may not have permission to read it.",
			       &lv_font_montserrat_14, M.on_surface_v);
		lv_obj_set_width(row, lv_pct(100));
		lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
	} else {
		while (fp->nd < fp->n && fp->v[fp->nd].dir)
			fp->nd++;
		fp->list[0].n = fp->nd;
		fp->list[1].base = fp->nd;
		fp->list[1].n = fp->n - fp->nd;
		for (k = 0; k < 2; k++) {	/* a section and its card: folders, then files */
			struct flist *l = &fp->list[k];

			if (!l->n)
				continue;
			m3_section(body, k ? "Files" : "Folders");
			l->card = c = m3_card(body, M.surface_c);
			lv_obj_set_style_pad_all(c, FROW_PAD, 0);
			lv_obj_set_height(c, l->n * FROW_H + 2 * FROW_PAD);
			for (j = 0; j < FROW_POOL && j < l->n; j++) {
				l->row[j] = m3_row_ex(c, LV_SYMBOL_FILE, M.secondary_c, M.on_secondary_c,
						      "", "", files_row_cb, fp, NULL, k == 0);
				lv_obj_add_flag(l->row[j], LV_OBJ_FLAG_HIDDEN);
				l->at[j] = -1;
			}
		}
		lv_obj_add_event_cb(body, files_page_ev, LV_EVENT_SCROLL, fp);
		lv_obj_add_event_cb(body, files_page_ev, LV_EVENT_SIZE_CHANGED, fp);
		lv_obj_update_layout(scr);
		files_sync(fp);
	}
	if (d)
		closedir(d);
	push(scr);
}

static void open_files(lv_event_t *e)
{
	LV_UNUSED(e);
	files_open_dir(FILES_ROOT);
}

#include "ui_youtube.c"	/* YouTube: search, a video's page, Play/Listen */
#include "ui_games.c"	/* Games: the library, a game started in RetroArch */

void open_paint(lv_event_t *e);

/* An idle moment: the picture of the shade made ready before the next drag. */
static void shade_pic_warm(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (app_pid > 0 || app_dormant || !shade || screen_off || locked || sd.on || sd.scr ||
	    za.phase || tr.scr || lv_anim_count_running() || (touch_indev &&
	    lv_indev_get_state(touch_indev) == LV_INDEV_STATE_PRESSED))
		return;
	if (!(shade_pic && !shade_pic_dirty && shade_pic_min == bp_minute()))
		shade_pic_get();
	/* up to date: nothing to do before the minute changes (a change of
	 * the shade's contents brings it back, shade_changed) */
	if (shade_pic && !shade_pic_dirty)
		lv_timer_set_period(t, (uint32_t)(60 - time(NULL) % 60) * 1000 + 100);
}

lv_obj_t *home_apps;		/* the tile grid */
bool home_build_quiet;		/* build_home() loads nothing */
lv_timer_t *shade_warm_timer;

void build_home(void)
{
	lv_obj_t *bar, *apps;

	conf_load();
	if (!wp.loaded)	/* (before the theme: its colours may come from the wallpaper) */
		wp_load();
	m3_apply_theme();
	home_scr = lv_obj_create(NULL);
	m3_screen(home_scr);
	home_wp = wp_put(home_scr, WP_HOME);
	lv_obj_add_event_cb(home_scr, home_gesture_cb, LV_EVENT_GESTURE, NULL);
	home_geom();	/* (the dock's side: the rest is the bar's, the pages', the dots') */

	/* status bar: time, Wi-Fi and Bluetooth, battery; drag down opens the shade */
	bar = lv_obj_create(home_scr);
	plain(bar);
	lv_obj_set_size(bar, PW, 40);
	lv_obj_set_pos(bar, PX, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	status_row(bar, &status_time, &status_wifi, &status_bt, &batt_lbl);
	/* a note before Wi-Fi while something plays in the background */
	status_np = m3_label(bar, LV_SYMBOL_AUDIO, &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_move_to_index(status_np, (int32_t)lv_obj_get_index(status_wifi));
	lv_obj_add_flag(status_np, LV_OBJ_FLAG_HIDDEN);
	if (home_wp)
		lv_obj_set_style_text_color(status_np, WP_TEXT, 0);
	np_sync();
	home_bar = bar;
	if (home_wp) {
		lv_obj_set_style_text_color(status_time, WP_TEXT, 0);
		lv_obj_set_style_text_color(status_wifi, WP_TEXT, 0);
		lv_obj_set_style_text_color(status_bt, WP_TEXT, 0);
		lv_obj_set_style_text_color(batt_lbl, WP_TEXT, 0);
	}
	/* the card on page 0 shows the time: the bar there does not (the
	 * label stays, hidden: the bar over applications copies it; on the
	 * other pages it shows, pg_show_page) */
	lv_obj_add_flag(status_time, LV_OBJ_FLAG_HIDDEN);

	/*
	 * The pages (paging.c): one strip under the bar, as wide as all of
	 * them; its first child the card (page 0), then the tiles, placed
	 * by pg_layout(). Dots at the bottom when there is more than one.
	 */
	/* (in a window of their place: turned, the dock is beside it) */
	home_view = lv_obj_create(home_scr);
	lv_obj_remove_style_all(home_view);
	lv_obj_remove_flag(home_view, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(home_view, PX, PG_TOP);
	lv_obj_set_size(home_view, PW, PB - PG_TOP - PG_DOTS_H);
	apps = home_apps = lv_obj_create(home_view);
	plain(apps);
	lv_obj_set_pos(apps, 0, 0);
	lv_obj_set_size(apps, PW, PB - PG_TOP - PG_DOTS_H);
	lv_obj_set_style_bg_opa(apps, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(apps, 0, 0);
	lv_obj_remove_flag(apps, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	/* a widget: the time large, the date, battery and Wi-Fi */
	{
		lv_obj_t *c = home_card = lv_obj_create(apps);

		plain(c);
		lv_obj_set_size(c, PW - 32, PG_CARD_H);
		lv_obj_set_pos(c, 16, 4);
		/* over a wallpaper the card blurs what lies behind it */
		if (!home_wp || !wp_glass_attach(c, PX + 16, PG_TOP + 4, PW - 32, PG_CARD_H, 24)) {
			lv_obj_set_style_radius(c, 24, 0);
			lv_obj_set_style_bg_color(c, M.surface_c_high, 0);
			lv_obj_set_style_bg_opa(c, home_wp ? LV_OPA_80 : LV_OPA_COVER, 0);
		}
		lv_obj_set_style_pad_all(c, 0, 0);
		lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		home_clock = m3_label(c, "", &lv_font_montserrat_48, M.on_surface);
		lv_obj_align(home_clock, LV_ALIGN_TOP_LEFT, 18, 8);
		home_date = m3_label(c, "", &lv_font_montserrat_16, M.primary);
		lv_obj_align(home_date, LV_ALIGN_BOTTOM_LEFT, 20, -12);
		wx_card_build(c);	/* the right side: the weather */
		wx_init();
	}

	app_tile(apps, LV_SYMBOL_SETTINGS, "Settings", M.primary_c, M.on_primary_c, open_settings);
	settings_tile = page_tile_anim(apps, open_settings);
	app_tile(apps, LV_SYMBOL_EDIT, "Paint", M.tertiary_c, M.on_tertiary_c, open_paint);
	page_tile_anim(apps, open_paint);
	app_tile(apps, LV_SYMBOL_DIRECTORY, "Files", M.secondary_c, M.on_secondary_c, open_files);
	page_tile_anim(apps, open_files);
	app_tile(apps, LV_SYMBOL_PLAY, "YouTube", M.tertiary_c, M.on_tertiary_c, open_youtube);
	page_tile_anim(apps, open_youtube);
	app_tile(apps, N72_SYMBOL_GAMEPAD, "Games", M.primary_c, M.on_primary_c, open_games);
	page_tile_anim(apps, open_games);
	app_tiles(apps);

	home_dots = lv_obj_create(home_scr);
	plain(home_dots);
	lv_obj_set_size(home_dots, PW, PG_DOTS_H);
	lv_obj_set_pos(home_dots, PX, PB - PG_DOTS_H);
	lv_obj_set_style_bg_opa(home_dots, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(home_dots, 0, 0);
	lv_obj_set_style_pad_column(home_dots, 9, 0);
	lv_obj_remove_flag(home_dots, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(home_dots, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	dock_build();	/* (and the pages' layout) */

	build_shade();
	app_bar_build();	/* the bar over applications and pages */

	status_refresh(true);
	home_timer = awake_timer(home_update, 2000, NULL);
	shade_warm_timer = awake_timer(shade_pic_warm, 1000, NULL);
	pg_warm_timer = awake_timer(pg_warm, 1000, NULL);
	home_update(NULL);
	if (app_pid <= 0 && !home_build_quiet)	/* (a turn keeps what shows) */
		lv_screen_load(home_scr);
	/* (ui_reopen: main, once the sound card is known) */
}

/* After a theme or accent change: start afresh (state is in CONF_PATH). */
void ui_restart(const char *open_page)
{
	conf_save();
	if (open_page)
		setenv("N72_UI_OPEN", open_page, 1);
	if (tty_fd >= 0)
		close(tty_fd);
	mbx_gpu_handoff();	/* the screen keeps this frame until the new one */
	execv("/proc/self/exe", self_argv);
}
