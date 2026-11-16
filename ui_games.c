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
};

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
};

static void games_row_cb(lv_event_t *e);

static void games_bind(void *ctx, struct flist *l, int j, int i)
{
	struct gpage *gp_ = ctx;
	struct game *g = &gp_->v[l->base + i];
	lv_obj_t *r = l->row[j], *col = lv_obj_get_child(r, 1);

	lv_obj_set_pos(r, 0, i * FROW_H);
	lv_obj_set_user_data(r, (void *)(intptr_t)(l->base + i));
	lv_label_set_text(lv_obj_get_child(col, 0), g->name);
	lv_label_set_text(lv_obj_get_child(col, 1), g->system);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN);
	l->at[j] = i;
}

static void games_page_ev(lv_event_t *e)
{
	struct gpage *g = lv_event_get_user_data(e);
	int i;

	if (lv_event_get_code(e) != LV_EVENT_DELETE) {
		rows_sync(g->body, g->list, g->nl, games_bind, g);
		return;
	}
	for (i = 0; i < g->n; i++)
		free(g->v[i].path);
	free(g->v);
	free(g->list);
	free(g);
}

/* A row: the game starts, in landscape on the side last held */
static void games_row_cb(lv_event_t *e)
{
	struct gpage *g = lv_event_get_user_data(e);
	lv_obj_t *r = lv_event_get_current_target(e);
	int i = (int)(intptr_t)lv_obj_get_user_data(r);
	char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 64];

	if (i < 0 || i >= g->n)
		return;
	game_rot = ui_rot ? ui_rot : rot_side;
	gp_named(g->v[i].path, g->v[i].system);
	shq(g->v[i].path, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), GAMES_LAUNCH " --rot %d %s", game_rot, q);
	app_launch(cmd, r);
}

static void open_games(lv_event_t *e)
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

	body = m3_page("Games", &scr);
	g->body = body;
	lv_obj_add_event_cb(body, games_page_ev, LV_EVENT_DELETE, g);
	if (access(GAMES_LAUNCH, X_OK)) {
		m3_note(body, "RetroArch is not installed (" GAMES_LAUNCH " is missing).");
	} else if (!g->n || !g->list) {
		m3_note(body, "No games yet. Put them in /root/Games, in folders by "
			      "system or all together: NES, Super Nintendo, Game Boy "
			      "(Color, Advance), Mega Drive, Master System, Game Gear, "
			      "PlayStation (.cue, .chd, .pbp) and arcade (.zip).");
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
			l->row[j] = m3_row_ex(c, N72_SYMBOL_GAMEPAD, M.tertiary_c, M.on_tertiary_c,
					      "", "", games_row_cb, g, NULL, false);
			lv_obj_add_flag(l->row[j], LV_OBJ_FLAG_HIDDEN);
			l->at[j] = -1;
		}
	}
	if (g->n == GAMES_MAX)
		m3_note(body, "Only the first 600 games are shown.");
	lv_obj_add_event_cb(body, games_page_ev, LV_EVENT_SCROLL, g);
	lv_obj_add_event_cb(body, games_page_ev, LV_EVENT_SIZE_CHANGED, g);
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
#define GP_STATES	"/root/retro/states"	/* (defaults.cfg's savestate_directory) */
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

static bool gp_up(void)
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
	unsigned char *data = NULL, *out = NULL;
	unsigned w, h, err;
	LodePNGState st;
	time_t t = gp_state_time();
	FILE *f;
	long n;

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
	if (!(f = fopen(path, "rb")))
		return false;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || n > (1 << 20) ||
	    fseek(f, 0, SEEK_SET) || !(data = malloc((size_t)n)) ||
	    fread(data, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(data);
		return false;
	}
	fclose(f);
	lodepng_state_init(&st);
	st.info_raw.colortype = LCT_RGB;
	st.info_raw.bitdepth = 8;
	err = lodepng_inspect(&w, &h, &st, data, (size_t)n);
	if (!err && (uint64_t)w * h <= 1024 * 1024)
		err = lodepng_decode(&out, &w, &h, &st, data, (size_t)n);
	else
		err = err ? err : 1;
	lodepng_state_cleanup(&st);
	free(data);
	if (err || !out) {
		if (out)
			lv_draw_buf_destroy((lv_draw_buf_t *)out);
		return false;
	}
	{
		/* (LVGL's lodepng gives an LVGL draw buffer, R G B packed) */
		const uint8_t *rgb = ((lv_draw_buf_t *)out)->data;
		unsigned step = (h + GP_THUMB_H - 1) / GP_THUMB_H, tw, th, x, y;
		lv_draw_buf_t *b;

		if (!step)
			step = 1;
		tw = w / step;
		th = h / step;
		b = tw && th ? lv_draw_buf_create(tw, th, LV_COLOR_FORMAT_XRGB8888, 0) : NULL;
		if (b) {
			for (y = 0; y < th; y++) {
				uint8_t *o = b->data + y * b->header.stride;

				for (x = 0; x < tw; x++, o += 4) {
					const uint8_t *q = rgb + ((size_t)y * step * w + x * step) * 3;

					o[0] = q[2];
					o[1] = q[1];
					o[2] = q[0];
					o[3] = 0xff;
				}
			}
			lv_image_set_src(gp.thumb, b);
			lv_obj_set_size(gp.thumb, tw, th);
			if (gp.pic)
				lv_draw_buf_destroy(gp.pic);
			gp.pic = b;
		}
		lv_draw_buf_destroy((lv_draw_buf_t *)out);
		return b != NULL;
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
}

/*
 * The Home key (main.c). A game in front: the panel opens and the game
 * pauses; with the panel up, Home ends the game. True if Home was used.
 */
static bool gp_home(void)
{
	if (!app_is_game() || lv_screen_active() != app_scr || locked)
		return false;
	if (gp_up()) {
		gp_close();
		return false;	/* (go_home ends it) */
	}
	app_bar_set(false);
	shade_set(false, false);
	gp_build();
	gp_want(1, NULL);
	return true;
}

/* The game has ended (app_closed) */
static void gp_gone(void)
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
}

/* The game about to start: its name and system for the panel */
static void gp_named(const char *path, const char *system)
{
	games_name(path, gp.name, sizeof(gp.name));
	gp.system = system;	/* (a games_types string: static) */
}
