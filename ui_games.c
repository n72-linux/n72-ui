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

static void games_scan(const char *dir, int depth, struct game *v, int *n)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d)) && *n < GAMES_MAX) {
		char path[PATH_MAX];
		struct stat st;
		const char *sys;
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

static void games_free_ud(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/* A row: the game starts, in landscape on the side last held */
static void games_row_cb(lv_event_t *e)
{
	const char *path = lv_event_get_user_data(e);
	char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 64];

	if (!path)
		return;
	game_rot = ui_rot ? ui_rot : rot_side;
	shq(path, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), GAMES_LAUNCH " --rot %d %s", game_rot, q);
	app_launch(cmd, lv_event_get_current_target(e));
}

static void open_games(lv_event_t *e)
{
	struct game *v = calloc(GAMES_MAX, sizeof(*v));
	lv_obj_t *scr, *body;
	const char *last = NULL;
	int n = 0, i;
	size_t k;

	LV_UNUSED(e);
	if (!v)
		return;
	nav_mark(open_games, NULL);	/* rebuilt when the screen turns */
	for (k = 0; k < sizeof(games_dirs) / sizeof(games_dirs[0]); k++)
		games_scan(games_dirs[k], 1, v, &n);
	qsort(v, n, sizeof(*v), games_cmp);

	body = m3_page("Games", &scr);
	if (access(GAMES_LAUNCH, X_OK)) {
		m3_note(body, "RetroArch is not installed (" GAMES_LAUNCH " is missing).");
	} else if (!n) {
		m3_note(body, "No games yet. Put them in /root/Games, in folders by "
			      "system or all together: NES, Super Nintendo, Game Boy "
			      "(Color, Advance), Mega Drive, Master System, Game Gear, "
			      "PlayStation (.cue, .chd, .pbp) and arcade (.zip).");
	}
	for (i = 0; i < n; i++) {
		lv_obj_t *row;

		if (!last || strcmp(last, v[i].system))
			m3_section(body, v[i].system);
		last = v[i].system;
		row = m3_row_ex(body, LV_SYMBOL_PLAY, M.tertiary_c, M.on_tertiary_c,
				v[i].name, v[i].system, games_row_cb, v[i].path, NULL, false);
		lv_obj_add_event_cb(row, games_free_ud, LV_EVENT_DELETE, v[i].path);
	}
	if (n == GAMES_MAX)
		m3_note(body, "Only the first 600 games are shown.");
	free(v);
	push(scr);
}
