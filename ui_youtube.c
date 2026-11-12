/*
 * ui_youtube.c - n72-ui's YouTube pages, #included from ui_m3.c (after the
 * Files page: it uses m3_page, push, job_run, shq, app_launch_cb...).
 *
 *   YouTube    a search bar, recent searches, videos watched
 *   Search     the field and keyboard over the results; more results load
 *              as the list nears its end
 *   Video      a large thumbnail, details, Play / Listen, quality, the
 *              description
 *
 * The network side is n72-yt (YouTube's InnerTube API over libcurl, run
 * as a background job); playback is n72-player with the separate video
 * and audio streams YouTube serves (--audio), started as an application
 * from the Play button, so the launcher's playback controls come with it.
 * Thumbnails come as raw RGB565 files from n72-yt, drawn as they are.
 *
 * Recent searches, history and the quality setting are kept in
 * YT_DIR on the flash.
 */

#include <ctype.h>

#define YT_DIR		"/root/.config/n72-yt"
#define YT_CACHE	"/tmp/n72-yt"		/* n72-yt's default */
#define YT_TW		112			/* list thumbnails, 16:9 */
#define YT_TH		63
#define YT_RECENT_MAX	8
#define YT_HIST_MAX	30
#define YT_HIST_SHOWN	12
#define YT_ROWS_MAX	200			/* a search stops loading more here */
#define YT_STALE_MS	(30 * 60 * 1000)	/* stream addresses last ~6 h; renew after this */

struct yt_vid {
	char id[16];
	char title[160];
	char chan[96];
	char len[16];		/* "3:33", "LIVE", "SOON" or "" */
	char meta[72];		/* "138M views * 6 years ago" */
};

/* a list row's image: the thumbnail once loaded */
struct yt_img {
	lv_image_dsc_t dsc;
	uint8_t *data;
};

static int yt_quality = 360;	/* 240 or 360 (the decoder's 720x576 limit) */

/* ------------------------------------------------------------ files */

static void yt_mkdir(void)
{
	mkdir("/root/.config", 0755);
	mkdir(YT_DIR, 0755);
}

static void yt_conf_load(void)
{
	FILE *f = fopen(YT_DIR "/conf", "r");
	char line[64];
	int v;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "quality=%d", &v) == 1 && (v == 240 || v == 360))
			yt_quality = v;
	fclose(f);
}

static void yt_conf_save(void)
{
	FILE *f;

	yt_mkdir();
	f = fopen(YT_DIR "/conf", "w");
	if (!f)
		return;
	fprintf(f, "quality=%d\n", yt_quality);
	fclose(f);
}

/*
 * Lines of a small list file: @line goes first, any line equal to it
 * (the same first field: up to a tab) leaves, at most @max are kept.
 */
static void yt_list_add(const char *path, const char *line, int max)
{
	char tmp[PATH_MAX], buf[512];
	size_t key = strcspn(line, "\t");
	FILE *in, *out;
	int n = 1;

	yt_mkdir();
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	out = fopen(tmp, "w");
	if (!out)
		return;
	fprintf(out, "%s\n", line);
	in = fopen(path, "r");
	while (in && n < max && fgets(buf, sizeof(buf), in)) {
		size_t k = strcspn(buf, "\t\n");

		if (k == key && !strncasecmp(buf, line, key))
			continue;
		if (buf[0] == '\n')
			continue;
		fputs(buf, out);
		if (!strchr(buf, '\n'))
			fputc('\n', out);
		n++;
	}
	if (in)
		fclose(in);
	if (fclose(out) == 0)
		rename(tmp, path);
}

/* Split @s at tabs into @f (at most @n fields); returns how many. */
static int yt_split(char *s, char **f, int n)
{
	int i = 0;

	while (i < n) {
		f[i++] = s;
		s = strchr(s, '\t');
		if (!s)
			break;
		*s++ = 0;
	}
	return i;
}

static void yt_vid_from(struct yt_vid *v, char **f, int n)
{
	memset(v, 0, sizeof(*v));
	snprintf(v->id, sizeof(v->id), "%s", n > 0 ? f[0] : "");
	snprintf(v->title, sizeof(v->title), "%s", n > 1 ? f[1] : "");
	snprintf(v->chan, sizeof(v->chan), "%s", n > 2 ? f[2] : "");
	snprintf(v->len, sizeof(v->len), "%s", n > 3 ? f[3] : "");
	if (n > 5 && f[4][0] && f[5][0])
		snprintf(v->meta, sizeof(v->meta), "%s * %s", f[4], f[5]);
	else if (n > 4)
		snprintf(v->meta, sizeof(v->meta), "%s", f[4][0] ? f[4] : n > 5 ? f[5] : "");
}

/* A video id as n72-yt writes it (letters, digits, - and _). */
static bool yt_id_ok(const char *id)
{
	size_t n = strlen(id);

	if (n < 6 || n > 15)
		return false;
	for (; *id; id++)
		if (!isalnum((unsigned char)*id) && *id != '-' && *id != '_')
			return false;
	return true;
}

static void yt_history_add(const struct yt_vid *v)
{
	char line[512];

	snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s", v->id, v->title, v->chan,
		 v->len, v->meta);
	yt_list_add(YT_DIR "/history", line, YT_HIST_MAX);
}

/* ------------------------------------------------------------ formatting */

/* 1836433696 -> "1.8B views" */
static void yt_count(const char *num, char *out, size_t len)
{
	double v = atof(num);

	if (!num[0])
		out[0] = 0;
	else if (v >= 1e9)
		snprintf(out, len, "%.1fB views", v / 1e9);
	else if (v >= 1e6)
		snprintf(out, len, "%.*fM views", v < 1e7 ? 1 : 0, v / 1e6);
	else if (v >= 1e3)
		snprintf(out, len, "%.*fK views", v < 1e4 ? 1 : 0, v / 1e3);
	else
		snprintf(out, len, "%.0f views", v);
}

static void yt_duration(int s, char *out, size_t len)
{
	if (s >= 3600)
		snprintf(out, len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, len, "%d:%02d", s / 60, s % 60);
}

/* ------------------------------------------------------------ widgets */

/* A magnifying glass: a ring and its handle (the symbol font has none). */
static lv_obj_t *yt_search_icon(lv_obj_t *parent, lv_color_t c)
{
	static const lv_point_precise_t handle[] = { { 13, 13 }, { 19, 19 } };
	lv_obj_t *box = lv_obj_create(parent), *ring, *ln;

	plain(box);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, 22, 22);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	ring = lv_obj_create(box);
	lv_obj_remove_style_all(ring);
	lv_obj_set_size(ring, 15, 15);
	lv_obj_set_pos(ring, 1, 1);
	lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(ring, 2, 0);
	lv_obj_set_style_border_color(ring, c, 0);
	lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
	lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
	ln = lv_line_create(box);
	lv_line_set_points(ln, handle, 2);
	lv_obj_set_style_line_width(ln, 3, 0);
	lv_obj_set_style_line_color(ln, c, 0);
	lv_obj_set_style_line_rounded(ln, true, 0);
	return box;
}

/* An M3 circular progress indicator, @size across. */
static lv_obj_t *yt_spinner(lv_obj_t *parent, int size)
{
	lv_obj_t *s = lv_spinner_create(parent);

	lv_obj_set_size(s, size, size);
	lv_obj_set_style_arc_width(s, size >= 40 ? 4 : 3, 0);
	lv_obj_set_style_arc_width(s, size >= 40 ? 4 : 3, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(s, M.surface_c_highest, 0);
	lv_obj_set_style_arc_color(s, M.primary, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(s, true, LV_PART_INDICATOR);
	lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
	return s;
}

static void yt_img_free(lv_event_t *e)
{
	struct yt_img *t = lv_event_get_user_data(e);

	if (t) {
		lv_image_cache_drop(&t->dsc);
		free(t->data);
		free(t);
	}
}

/* A thumbnail place: @w x @h, rounded, tinted until the picture comes. */
static lv_obj_t *yt_thumb(lv_obj_t *parent, int w, int h, int radius, const char *len)
{
	lv_obj_t *img = lv_image_create(parent);

	lv_obj_set_size(img, w, h);
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_style_radius(img, radius, 0);	/* (images draw rounded by it) */
	lv_obj_set_style_bg_color(img, M.surface_c_highest, 0);
	lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
	lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	if (len && len[0]) {
		bool live = !strcmp(len, "LIVE") || !strcmp(len, "SOON");
		lv_obj_t *b = m3_label(img, len, &lv_font_montserrat_12,
				       live ? lv_color_white() : lv_color_hex(0xf0f0f0));

		lv_obj_set_style_bg_color(b, live ? lv_color_hex(0xcc0000) : lv_color_black(), 0);
		lv_obj_set_style_bg_opa(b, live ? LV_OPA_COVER : LV_OPA_80, 0);
		lv_obj_set_style_radius(b, 4, 0);
		lv_obj_set_style_pad_hor(b, 4, 0);
		lv_obj_set_style_pad_ver(b, 1, 0);
		lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
	}
	return img;
}

/* The thumbnail of @id at @w x @h from n72-yt's cache onto @img; false if not there. */
static bool yt_thumb_load(lv_obj_t *img, const char *id, int w, int h)
{
	size_t sz = (size_t)w * h * 2;
	struct yt_img *t;
	char path[128];
	FILE *f;

	if (lv_obj_get_user_data(img))
		return true;		/* (already) */
	snprintf(path, sizeof(path), YT_CACHE "/%s-%dx%d.565", id, w, h);
	f = fopen(path, "rb");
	if (!f)
		return false;
	t = calloc(1, sizeof(*t));
	if (t)
		t->data = malloc(sz);
	if (!t || !t->data || fread(t->data, 1, sz, f) != sz) {
		fclose(f);
		if (t)
			free(t->data);
		free(t);
		return false;
	}
	fclose(f);
	t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	t->dsc.header.w = w;
	t->dsc.header.h = h;
	t->dsc.header.stride = w * 2;
	t->dsc.data_size = sz;
	t->dsc.data = t->data;
	lv_image_set_src(img, &t->dsc);
	lv_obj_set_user_data(img, t);
	lv_obj_add_event_cb(img, yt_img_free, LV_EVENT_DELETE, t);
	/* pictures of the page or of the row taken before are stale now */
	pf_label_changed(img);
	bp_changed(lv_obj_get_screen(img));
	return true;
}

/* @row is drawn from a scroll picture now (its live look would not show). */
static bool yt_row_cached(lv_obj_t *row)
{
	for (int i = 0; i < sc.n; i++)
		if (sc.live[i] == row)
			return true;
	return false;
}

/* An empty or error state: a large icon, a headline, a line, a button. */
static lv_obj_t *yt_state(lv_obj_t *parent, const char *icon, const char *head,
			  const char *line, const char *btn, lv_event_cb_t cb)
{
	lv_obj_t *c = m3_card(parent, M.surface_c), *l;

	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_ver(c, 24, 0);
	lv_obj_set_style_pad_row(c, 8, 0);
	m3_badge(c, icon, M.surface_c_highest, M.on_surface_v, 56);
	l = m3_label(c, head, &lv_font_montserrat_16, M.on_surface);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	if (line) {
		l = m3_label(c, line, &lv_font_montserrat_14, M.on_surface_v);
		lv_obj_set_width(l, lv_pct(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	}
	if (btn) {
		lv_obj_t *b = m3_button(c, btn, false, cb, NULL);

		lv_obj_set_style_margin_top(b, 8, 0);
	}
	return c;
}

static void yt_open_video(const struct yt_vid *v);

static void yt_row_cb(lv_event_t *e)
{
	yt_open_video(lv_event_get_user_data(e));
}

static void yt_free_ud(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/*
 * A video in a list (an M3 list item with a large leading thumbnail): the
 * thumbnail with its length, the title on two lines, the channel, views
 * and age. *img_out: the thumbnail, to load later.
 */
static lv_obj_t *yt_row(lv_obj_t *parent, const struct yt_vid *v, lv_obj_t **img_out)
{
	lv_obj_t *r = lv_obj_create(parent), *col, *t, *img;
	struct yt_vid *ud = malloc(sizeof(*ud));
	int lh = lv_font_get_line_height(&lv_font_montserrat_14);

	plain(r);
	lv_obj_set_size(r, lv_pct(100), YT_TH + 12);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(r, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_style_radius(r, 12, 0);
	lv_obj_set_style_pad_all(r, 6, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(r, 10, 0);
	img = yt_thumb(r, YT_TW, YT_TH, 8, v->len);
	col = lv_obj_create(r);
	plain(col);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_height(col, YT_TH);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	t = m3_label(col, v->title[0] ? v->title : "Untitled video", &lv_font_montserrat_14,
		     M.on_surface);
	lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
	lv_obj_set_size(t, lv_pct(100), 2 * lh);
	t = m3_label(col, v->chan, &lv_font_montserrat_12, M.on_surface_v);
	lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
	lv_obj_set_width(t, lv_pct(100));
	if (v->meta[0]) {
		t = m3_label(col, v->meta, &lv_font_montserrat_12, M.on_surface_v);
		lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
		lv_obj_set_width(t, lv_pct(100));
	}
	if (ud) {
		*ud = *v;
		lv_obj_add_event_cb(r, yt_row_cb, LV_EVENT_CLICKED, ud);
		lv_obj_add_event_cb(r, yt_free_ud, LV_EVENT_DELETE, ud);
	}
	if (img_out)
		*img_out = img;
	return r;
}

/*
 * Thumbnails for rows: a list of (row, image, id); one n72-yt job fetches
 * the missing ones, then they go on. A row on a scroll picture at that
 * moment gets its thumbnail once the page is still (yt_thumbs_apply).
 */
#define YT_PEND_MAX	YT_ROWS_MAX

static struct {
	lv_obj_t *row[YT_PEND_MAX], *img[YT_PEND_MAX];
	char id[YT_PEND_MAX][16];
	int n;
	lv_obj_t *owner;	/* the body they are on */
	lv_timer_t *retry;
	bool running;
} yth;

static void yt_thumbs_apply(void);

static void yt_thumbs_retry(lv_timer_t *t)
{
	LV_UNUSED(t);
	yt_thumbs_apply();
}

static void yt_thumbs_apply(void)
{
	int i, k = 0;

	for (i = 0; i < yth.n; i++) {
		if (!lv_obj_is_valid(yth.img[i]))
			continue;
		if (yt_row_cached(yth.row[i]) || !yt_thumb_load(yth.img[i], yth.id[i], YT_TW, YT_TH)) {
			/* later (on a picture now), or not fetched yet */
			yth.row[k] = yth.row[i];
			yth.img[k] = yth.img[i];
			memcpy(yth.id[k], yth.id[i], sizeof(yth.id[k]));
			k++;
		}
	}
	yth.n = k;
	if (yth.retry && (!k || yth.running)) {
		lv_timer_delete(yth.retry);
		yth.retry = NULL;
	}
	/* some waited for a scroll to end: look again soon */
	if (k && !yth.running && !yth.retry) {
		bool waiting = false;

		for (i = 0; i < k; i++)
			waiting |= yt_row_cached(yth.row[i]);
		if (waiting)
			yth.retry = lv_timer_create(yt_thumbs_retry, 300, NULL);
		else
			yth.n = 0;	/* not on YouTube's servers: placeholders stay */
	}
}

static void yt_thumbs_fetch(void);

static void yt_thumbs_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	yth.running = false;
	yt_thumbs_apply();
	yt_thumbs_fetch();	/* rows added meanwhile */
}

/* Fetch thumbnails for the rows still without one (one job at a time). */
static void yt_thumbs_fetch(void)
{
	char cmd[YT_PEND_MAX * 17 + 64];
	size_t o;
	int i, n = 0;

	if (yth.running || !yth.n || !lv_obj_is_valid(yth.owner))
		return;
	yt_thumbs_apply();		/* (cached already: no download) */
	o = snprintf(cmd, sizeof(cmd), "n72-yt thumbs %d %d", YT_TW, YT_TH);
	for (i = 0; i < yth.n && o + 18 < sizeof(cmd); i++)
		if (lv_obj_is_valid(yth.img[i]) && !lv_obj_get_user_data(yth.img[i])) {
			o += snprintf(cmd + o, sizeof(cmd) - o, " %s", yth.id[i]);
			n++;
		}
	if (!n)
		return;
	yth.running = true;
	job_run(cmd, yth.owner, yt_thumbs_done, NULL);
}

static void yt_thumbs_owner_gone(lv_event_t *e)
{
	if (yth.owner == lv_event_get_current_target(e)) {
		yth.owner = NULL;
		yth.n = 0;
		yth.running = false;	/* (its job's result is dropped with the page) */
		if (yth.retry) {
			lv_timer_delete(yth.retry);
			yth.retry = NULL;
		}
	}
}

/* Rows on @body to get thumbnails (another page's pending ones are dropped). */
static void yt_thumbs_want(lv_obj_t *body, lv_obj_t *row, lv_obj_t *img, const char *id)
{
	if (yth.owner != body) {
		yth.n = 0;
		yth.running = false;
		yth.owner = body;
		lv_obj_add_event_cb(body, yt_thumbs_owner_gone, LV_EVENT_DELETE, NULL);
	}
	if (yth.n >= YT_PEND_MAX || !yt_id_ok(id))
		return;
	yth.row[yth.n] = row;
	yth.img[yth.n] = img;
	snprintf(yth.id[yth.n], sizeof(yth.id[0]), "%s", id);
	yth.n++;
}

/* ------------------------------------------------------------ search */

static struct {
	lv_obj_t *scr, *body, *ta, *kb, *recent, *tail;
	char query[128];
	char cont[1024];
	int rows;
	unsigned gen;		/* bumped by each new search: older answers are dropped */
	bool loading;
	lv_timer_t *more_timer;
} ys;

static void yt_search_run(const char *q);

static void yt_kb_show(bool on)
{
	if (!ys.kb)
		return;
	if (on) {
		lv_obj_remove_flag(ys.kb, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_state(ys.ta, LV_STATE_FOCUSED);
		lv_obj_set_height(ys.body, H - 56 - lv_obj_get_height(ys.kb));
	} else {
		lv_obj_add_flag(ys.kb, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_state(ys.ta, LV_STATE_FOCUSED);
		lv_obj_set_height(ys.body, H - 56);
	}
}

static void yt_kb_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_READY) {
		const char *q = lv_textarea_get_text(ys.ta);

		while (*q == ' ')
			q++;
		if (*q)
			yt_search_run(q);
	} else if (code == LV_EVENT_CANCEL) {
		if (ys.rows)
			yt_kb_show(false);
		else
			go_back();
	}
}

static void yt_ta_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	yt_kb_show(true);
}

static void yt_recent_cb(lv_event_t *e)
{
	const char *q = lv_event_get_user_data(e);

	lv_textarea_set_text(ys.ta, q);
	yt_search_run(q);
}

/* Recent searches as rows under the field, before any results. */
static void yt_recent_rows(lv_obj_t *body)
{
	FILE *f = fopen(YT_DIR "/searches", "r");
	char line[160];
	lv_obj_t *c = NULL;

	while (f && fgets(line, sizeof(line), f)) {
		char *ud;
		lv_obj_t *r;

		line[strcspn(line, "\t\n")] = 0;
		if (!line[0] || !(ud = strdup(line)))
			continue;
		if (!c) {
			c = lv_obj_create(body);
			plain(c);
			lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
			lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
			lv_obj_set_style_pad_all(c, 0, 0);
			lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
			lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
			ys.recent = c;
		}
		r = m3_row_ex(c, LV_SYMBOL_LOOP, M.surface_c_high, M.on_surface_v, ud, NULL,
			      yt_recent_cb, ud, NULL, false);
		lv_obj_set_height(r, 48);
		lv_obj_add_event_cb(r, yt_free_ud, LV_EVENT_DELETE, ud);
	}
	if (f)
		fclose(f);
}

static void yt_tail_set(lv_obj_t *o)
{
	if (ys.tail && lv_obj_is_valid(ys.tail))
		lv_obj_delete(ys.tail);
	ys.tail = o;
}

/* A spinner row at the end of the list. */
static void yt_tail_spinner(void)
{
	lv_obj_t *r = lv_obj_create(ys.body);

	plain(r);
	lv_obj_set_size(r, lv_pct(100), 64);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_center(yt_spinner(r, 32));
	yt_tail_set(r);
}

static void yt_retry_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (ys.query[0])
		yt_search_run(ys.query);
}

static void yt_more(void);

static void yt_results(const char *out, void *ud)
{
	unsigned gen = (unsigned)(uintptr_t)ud;
	char *buf, *line, *save = NULL, err[256] = "";
	bool more = !!(ys.rows);
	int added = 0;

	if (gen != ys.gen)
		return;			/* an older search */
	ys.loading = false;
	ys.cont[0] = 0;
	buf = strdup(out);
	if (!buf)
		return;
	yt_tail_set(NULL);
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[7];
		int n = yt_split(line, f, 7);

		if (!strcmp(f[0], "V") && n >= 3 && ys.rows < YT_ROWS_MAX && yt_id_ok(f[1])) {
			struct yt_vid v;
			lv_obj_t *img, *row;

			yt_vid_from(&v, f + 1, n - 1);
			row = yt_row(ys.body, &v, &img);
			yt_thumbs_want(ys.body, row, img, v.id);
			ys.rows++;
			added++;
		} else if (!strcmp(f[0], "C") && n >= 2) {
			snprintf(ys.cont, sizeof(ys.cont), "%s", f[1]);
		} else if (!strcmp(f[0], "E") && n >= 2) {
			snprintf(err, sizeof(err), "%s", f[1]);
		}
	}
	free(buf);
	if (!added && !more) {
		if (!err[0])
			snprintf(err, sizeof(err), "%s", out[0] ? "Unexpected answer" :
				 "The YouTube helper (n72-yt) did not answer");
		yt_tail_set(yt_state(ys.body, strstr(err, "No videos") ? LV_SYMBOL_EYE_CLOSE :
				     LV_SYMBOL_WARNING,
				     strstr(err, "No videos") ? "No videos found" : "Could not search",
				     strstr(err, "No videos") ? "Try other words." : err,
				     strstr(err, "No videos") ? NULL : "Try again", yt_retry_cb));
	} else if (err[0] && more) {
		m3_note(ys.body, "Could not load more results.");
		ys.cont[0] = 0;
	}
	if (ys.rows >= YT_ROWS_MAX)
		ys.cont[0] = 0;
	bp_changed(ys.scr);
	yt_thumbs_fetch();
	/* a short first page that does not fill the screen: the next at once */
	if (ys.cont[0] && lv_obj_get_scroll_bottom(ys.body) < H / 2)
		yt_more();
}

static void yt_more(void)
{
	char cmd[1200], q[1100];

	if (ys.loading || !ys.cont[0])
		return;
	ys.loading = true;
	shq(ys.cont, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "n72-yt more %s", q);
	yt_tail_spinner();
	job_run(cmd, ys.body, yt_results, (void *)(uintptr_t)ys.gen);
}

/* Near the end of the list (once it is still): the next page. */
static void yt_more_check(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (!ys.body || !lv_obj_is_valid(ys.body))
		return;
	if (sc.owner == ys.body || tr.scr)
		return;		/* still moving, or pictures in place: look again */
	lv_timer_delete(ys.more_timer);
	ys.more_timer = NULL;
	if (lv_obj_get_scroll_bottom(ys.body) < H)
		yt_more();
}

static void yt_body_scroll(lv_event_t *e)
{
	LV_UNUSED(e);
	if (lv_obj_has_state(ys.ta, LV_STATE_FOCUSED) && ys.rows)
		yt_kb_show(false);	/* scrolling the results: the keyboard goes */
	if (ys.cont[0] && !ys.loading && !ys.more_timer &&
	    lv_obj_get_scroll_bottom(ys.body) < H)
		ys.more_timer = lv_timer_create(yt_more_check, 120, NULL);
}

static void yt_search_run(const char *qtext)
{
	char cmd[400], q[300];

	snprintf(ys.query, sizeof(ys.query), "%s", qtext);
	yt_list_add(YT_DIR "/searches", ys.query, YT_RECENT_MAX);
	ys.gen++;
	ys.rows = 0;
	ys.cont[0] = 0;
	ys.loading = true;
	yt_kb_show(false);
	/* the old list, recent searches and any message go */
	lv_obj_clean(ys.body);
	ys.tail = NULL;
	ys.recent = NULL;
	yth.n = 0;
	yt_tail_spinner();
	lv_obj_scroll_to_y(ys.body, 0, LV_ANIM_OFF);
	shq(ys.query, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "n72-yt search %s", q);
	job_run(cmd, ys.body, yt_results, (void *)(uintptr_t)ys.gen);
}

static void yt_search_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	if (ys.more_timer)
		lv_timer_delete(ys.more_timer);
	memset(&ys, 0, sizeof(ys));
}

/* The search page: the field in the top bar, the keyboard, then results. */
static void yt_open_search(const char *query)
{
	lv_obj_t *bar;

	if (ys.scr)
		return;			/* (one at a time) */
	ys.body = m3_page("", &ys.scr);
	lv_obj_add_event_cb(ys.scr, yt_search_deleted, LV_EVENT_DELETE, NULL);
	bar = lv_obj_get_child(ys.scr, 0);	/* m3_page's top bar */
	ys.ta = lv_textarea_create(bar);
	lv_textarea_set_one_line(ys.ta, true);
	lv_textarea_set_placeholder_text(ys.ta, "Search YouTube");
	lv_textarea_set_max_length(ys.ta, 100);
	plain(ys.ta);
	lv_obj_set_size(ys.ta, W - 56 - 8, 44);
	lv_obj_align(ys.ta, LV_ALIGN_LEFT_MID, 52, 0);
	lv_obj_set_style_radius(ys.ta, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(ys.ta, M.surface_c_high, 0);
	lv_obj_set_style_bg_opa(ys.ta, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(ys.ta, 18, 0);
	lv_obj_set_style_pad_ver(ys.ta, 11, 0);
	lv_obj_set_style_text_font(ys.ta, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(ys.ta, M.on_surface, 0);
	lv_obj_set_style_text_color(ys.ta, M.on_surface_v, LV_PART_TEXTAREA_PLACEHOLDER);
	lv_obj_set_style_bg_color(ys.ta, M.primary, LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_add_event_cb(ys.ta, yt_ta_cb, LV_EVENT_CLICKED, NULL);

	ys.kb = lv_keyboard_create(ys.scr);
	lv_obj_set_size(ys.kb, W, LV_MIN(200, H * 5 / 12));
	lv_obj_align(ys.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(ys.kb, M.surface_c, 0);
	lv_obj_set_style_radius(ys.kb, 0, 0);
	lv_obj_set_style_border_width(ys.kb, 0, 0);
	lv_obj_set_style_bg_color(ys.kb, M.surface_c_highest, LV_PART_ITEMS);
	lv_obj_set_style_text_color(ys.kb, M.on_surface, LV_PART_ITEMS);
	lv_obj_set_style_radius(ys.kb, 8, LV_PART_ITEMS);
	lv_obj_set_style_border_width(ys.kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(ys.kb, 0, LV_PART_ITEMS);
	lv_keyboard_set_textarea(ys.kb, ys.ta);
	lv_obj_add_event_cb(ys.kb, yt_kb_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ys.kb, yt_kb_cb, LV_EVENT_CANCEL, NULL);
	lv_obj_add_event_cb(ys.body, yt_body_scroll, LV_EVENT_SCROLL, NULL);

	if (query && *query) {
		lv_textarea_set_text(ys.ta, query);
		yt_kb_show(false);
		push(ys.scr);
		yt_search_run(query);
	} else {
		yt_recent_rows(ys.body);
		yt_kb_show(true);
		push(ys.scr);
	}
}

/* ------------------------------------------------------------ video */

#define YT_CMD_MAX	8192

static struct {
	lv_obj_t *scr, *body, *img, *title, *meta, *play, *listen, *status, *desc, *seg[2];
	struct yt_vid v;
	char title_full[200];
	char play_cmd[YT_CMD_MAX], listen_cmd[YT_CMD_MAX];
	unsigned gen;
	uint32_t resolved_at;
	bool ready, desc_more;
	char *desc_text;
} yv;

static void yt_resolve(void);

static void yt_btn_enable(lv_obj_t *b, bool on)
{
	if (on) {
		lv_obj_remove_state(b, LV_STATE_DISABLED);
		lv_obj_set_style_opa(b, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(b, LV_STATE_DISABLED);
		lv_obj_set_style_opa(b, LV_OPA_50, 0);
	}
}

static void yt_status(const char *text, bool error)
{
	if (!yv.status)
		return;
	lv_obj_set_style_text_color(yv.status, error ? M.error : M.on_surface_v, 0);
	label_set_if_changed(yv.status, text);
	if (text[0])
		lv_obj_remove_flag(yv.status, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(yv.status, LV_OBJ_FLAG_HIDDEN);
}

/* The description: "\n" back to line ends, first ~400 characters until "More". */
static void yt_desc_show(void)
{
	char *s;
	size_t n;

	if (!yv.desc || !yv.desc_text)
		return;
	n = strlen(yv.desc_text);
	s = malloc(n + 8);
	if (!s)
		return;
	memcpy(s, yv.desc_text, n + 1);
	if (!yv.desc_more && n > 400) {
		size_t cut = 400;

		while (cut > 300 && s[cut] != ' ' && s[cut] != '\n')
			cut--;
		strcpy(s + cut, "...");
	}
	label_set_if_changed(yv.desc, s);
	free(s);
}

static void yt_desc_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	yv.desc_more = !yv.desc_more;
	yt_desc_show();
}

static void yt_resolved(const char *out, void *ud)
{
	unsigned gen = (unsigned)(uintptr_t)ud;
	char *buf, *line, *save = NULL, err[256] = "", t[300];
	static char a[3072], u[3072];
	char title[200] = "";
	bool have_info = false;

	if (gen != yv.gen)
		return;
	a[0] = u[0] = 0;
	buf = strdup(out);
	if (!buf)
		return;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[6];
		int n = yt_split(line, f, 6);

		if (!strcmp(f[0], "I") && n >= 5) {
			char views[32], len[16], meta[96];

			snprintf(title, sizeof(title), "%s", f[1]);
			yt_count(f[3], views, sizeof(views));
			if (n >= 6 && f[5][0] == '1')
				snprintf(len, sizeof(len), "Live");
			else
				yt_duration(atoi(f[4]), len, sizeof(len));
			if (f[1][0])
				label_set_if_changed(yv.title, f[1]);
			snprintf(meta, sizeof(meta), "%s%s%s * %s", f[2], f[2][0] ? "\n" : "",
				 views[0] ? views : "", len);
			label_set_if_changed(yv.meta, meta);
			have_info = true;
		} else if (!strcmp(f[0], "D") && n >= 2) {
			char *s = f[1], *o = f[1];

			/* "\n" back to a line end, "\\" to a backslash */
			while (*s) {
				if (s[0] == '\\' && s[1] == 'n')
					*o++ = '\n', s += 2;
				else if (s[0] == '\\' && s[1] == '\\')
					*o++ = '\\', s += 2;
				else
					*o++ = *s++;
			}
			*o = 0;
			free(yv.desc_text);
			yv.desc_text = strdup(f[1][0] ? f[1] : "No description.");
			yt_desc_show();
		} else if (!strcmp(f[0], "U") && n >= 2) {
			snprintf(u, sizeof(u), "%s", f[1]);
		} else if (!strcmp(f[0], "A") && n >= 2) {
			snprintf(a, sizeof(a), "%s", f[1]);
		} else if (!strcmp(f[0], "H") && n >= 2) {
			snprintf(err, sizeof(err), "Live streams cannot be played yet.");
		} else if (!strcmp(f[0], "E") && n >= 2) {
			snprintf(err, sizeof(err), "%s", f[1]);
		}
	}
	free(buf);
	LV_UNUSED(have_info);
	if (title[0])
		snprintf(yv.title_full, sizeof(yv.title_full), "%s", title);
	if (!a[0] || err[0]) {
		yt_status(err[0] ? err : out[0] ? "This video cannot be played here." :
			  "The YouTube helper (n72-yt) did not answer.", true);
		yv.ready = false;
		return;
	}
	/*
	 * Commands for app_launch_cb: n72-player with the video stream and its
	 * sound as a second one (Listen: the sound alone).
	 */
	{
		static char qa[3200], qu[3200];

		shq(yv.title_full[0] ? yv.title_full : yv.v.title, t, sizeof(t));
		shq(a, qa, sizeof(qa));
		if (u[0]) {
			shq(u, qu, sizeof(qu));
			snprintf(yv.play_cmd, sizeof(yv.play_cmd), "n72-player --title %s --audio %s %s",
				 t, qa, qu);
		} else {
			yv.play_cmd[0] = 0;
		}
		snprintf(yv.listen_cmd, sizeof(yv.listen_cmd), "n72-player --title %s %s", t, qa);
	}
	yv.ready = true;
	yv.resolved_at = lv_tick_get();
	yt_btn_enable(yv.play, yv.play_cmd[0] != 0);
	yt_btn_enable(yv.listen, true);
	yt_status("", false);
}

static void yt_resolve(void)
{
	char cmd[128];

	yv.gen++;
	yv.ready = false;
	yt_btn_enable(yv.play, false);
	yt_btn_enable(yv.listen, false);
	yt_status("Getting the video...", false);
	snprintf(cmd, sizeof(cmd), "n72-yt video %s -q %d", yv.v.id, yt_quality);
	job_run(cmd, yv.body, yt_resolved, (void *)(uintptr_t)yv.gen);
}

/* Before app_launch_cb (added after this on the same button): history. */
static void yt_play_pre(lv_event_t *e)
{
	struct yt_vid h = yv.v;
	lv_obj_t *b = lv_event_get_current_target(e);

	LV_UNUSED(b);
	if (!yv.ready)
		return;
	if (yv.title_full[0])
		snprintf(h.title, sizeof(h.title), "%s", yv.title_full);
	yt_history_add(&h);
}

static void yt_video_shown(lv_event_t *e)
{
	LV_UNUSED(e);
	/* back on the page after a long while: the addresses may have expired */
	if (yv.ready && lv_tick_elaps(yv.resolved_at) > YT_STALE_MS)
		yt_resolve();
}

static void yt_seg_cb(lv_event_t *e)
{
	int q = (int)(intptr_t)lv_event_get_user_data(e);

	if (q == yt_quality)
		return;
	yt_quality = q;
	yt_conf_save();
	for (int i = 0; i < 2; i++) {
		bool on = (i == 0) == (q == 360);

		lv_obj_set_style_bg_color(yv.seg[i], on ? M.secondary_c : M.surface, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(yv.seg[i], 0),
					    on ? M.on_secondary_c : M.on_surface, 0);
	}
	bp_changed(yv.scr);
	yt_resolve();
}

/* An M3 segmented button: 360p | 240p. */
static lv_obj_t *yt_quality_seg(lv_obj_t *parent)
{
	static const int qs[2] = { 360, 240 };
	lv_obj_t *row = lv_obj_create(parent);

	plain(row);
	lv_obj_set_size(row, lv_pct(100), 44);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_column(row, 0, 0);
	lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_color(row, M.outline, 0);
	lv_obj_set_style_pad_all(row, 3, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	for (int i = 0; i < 2; i++) {
		bool on = qs[i] == yt_quality;
		lv_obj_t *b = lv_obj_create(row), *l;
		char txt[24];

		plain(b);
		lv_obj_set_height(b, lv_pct(100));
		lv_obj_set_flex_grow(b, 1);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, on ? M.secondary_c : M.surface, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
		snprintf(txt, sizeof(txt), "%dp%s", qs[i], i ? "  saves data" : "  sharper");
		l = m3_label(b, txt, &lv_font_montserrat_14, on ? M.on_secondary_c : M.on_surface);
		lv_obj_center(l);
		lv_obj_add_event_cb(b, yt_seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)qs[i]);
		yv.seg[i] = b;
	}
	return row;
}

static void yt_video_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	free(yv.desc_text);
	memset(&yv, 0, sizeof(yv));
}

static void yt_big_thumb_done(const char *out, void *ud)
{
	int bw = W - 2 * STATUS_PAD;

	LV_UNUSED(out);
	LV_UNUSED(ud);
	if (yv.img && lv_obj_is_valid(yv.img))
		yt_thumb_load(yv.img, yv.v.id, bw, bw * 9 / 16);
}

static lv_obj_t *yt_action(lv_obj_t *parent, const char *icon, const char *text,
			   bool filled, char *cmd)
{
	lv_obj_t *b = lv_button_create(parent), *l;
	char txt[48];

	plain(b);
	lv_obj_set_height(b, 48);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(b, filled ? M.primary : M.secondary_c, 0);
	snprintf(txt, sizeof(txt), "%s  %s", icon, text);
	l = m3_label(b, txt, &lv_font_montserrat_16, filled ? M.on_primary : M.on_secondary_c);
	lv_obj_center(l);
	/* history first, then the launch (callbacks run in the order added) */
	lv_obj_add_event_cb(b, yt_play_pre, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(b, app_launch_cb, LV_EVENT_CLICKED, cmd);
	return b;
}

static void yt_open_video(const struct yt_vid *v)
{
	lv_obj_t *c, *row;
	int bw = W - 2 * STATUS_PAD;

	if (yv.scr || !yt_id_ok(v->id))
		return;
	yv.v = *v;
	yv.body = m3_page("", &yv.scr);
	lv_obj_add_event_cb(yv.scr, yt_video_deleted, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(yv.scr, yt_video_shown, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_set_style_pad_row(yv.body, 12, 0);

	yv.img = yt_thumb(yv.body, bw, bw * 9 / 16, 16, v->len);
	if (!yt_thumb_load(yv.img, v->id, bw, bw * 9 / 16)) {
		char cmd[96];

		snprintf(cmd, sizeof(cmd), "n72-yt thumbs %d %d %s", bw, bw * 9 / 16, v->id);
		job_run(cmd, yv.body, yt_big_thumb_done, NULL);
	}

	yv.title = m3_label(yv.body, v->title[0] ? v->title : "Untitled video",
			    &lv_font_montserrat_20, M.on_surface);
	lv_obj_set_width(yv.title, lv_pct(100));
	lv_label_set_long_mode(yv.title, LV_LABEL_LONG_WRAP);
	yv.meta = m3_label(yv.body, v->chan, &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_set_width(yv.meta, lv_pct(100));
	lv_label_set_long_mode(yv.meta, LV_LABEL_LONG_WRAP);

	row = lv_obj_create(yv.body);
	plain(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	yv.play = yt_action(row, LV_SYMBOL_PLAY, "Play", true, yv.play_cmd);
	yv.listen = yt_action(row, LV_SYMBOL_AUDIO, "Listen", false, yv.listen_cmd);

	yv.status = m3_label(yv.body, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_set_width(yv.status, lv_pct(100));
	lv_label_set_long_mode(yv.status, LV_LABEL_LONG_WRAP);

	m3_section(yv.body, "Quality");
	yt_quality_seg(yv.body);

	m3_section(yv.body, "Description");
	c = m3_card(yv.body, M.surface_c);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(c, yt_desc_cb, LV_EVENT_CLICKED, NULL);
	yv.desc = m3_label(c, "", &lv_font_montserrat_14, M.on_surface_v);
	lv_obj_set_width(yv.desc, lv_pct(100));
	lv_label_set_long_mode(yv.desc, LV_LABEL_LONG_WRAP);

	push(yv.scr);
	yt_resolve();
}

/* ------------------------------------------------------------ home */

static struct {
	lv_obj_t *scr, *body, *chips;
	long chips_mtime;
	lv_obj_t *hist[YT_HIST_SHOWN + 1];	/* its section title and rows, on the body */
	int nhist;
	long hist_mtime;
} yh;

static void yt_bar_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	yt_open_search(NULL);
}

static void yt_chip_cb(lv_event_t *e)
{
	yt_open_search(lv_event_get_user_data(e));
}

/* An M3 suggestion chip. */
static lv_obj_t *yt_chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_obj_create(parent), *l;

	plain(b);
	lv_obj_set_size(b, LV_SIZE_CONTENT, 32);
	lv_obj_set_style_radius(b, 8, 0);
	lv_obj_set_style_border_width(b, 1, 0);
	lv_obj_set_style_border_color(b, M.outline_v, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(b, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(b, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_style_pad_hor(b, 12, 0);
	lv_obj_set_style_pad_ver(b, 0, 0);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	l = m3_label(b, text, &lv_font_montserrat_14, M.on_surface_v);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_width(l, W - 2 * STATUS_PAD - 32, 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

static long yt_mtime(const char *path)
{
	struct stat st;

	return stat(path, &st) ? 0 : (long)st.st_mtime;
}

#define yt_hist_mtime()	yt_mtime(YT_DIR "/history")

/* Recent searches as chips, under the search bar (rebuilt when they change). */
static void yt_home_chips(void)
{
	FILE *f;
	char line[160];
	int n = 0;

	if (yh.chips && lv_obj_is_valid(yh.chips))
		lv_obj_delete(yh.chips);
	yh.chips = NULL;
	yh.chips_mtime = yt_mtime(YT_DIR "/searches");
	f = fopen(YT_DIR "/searches", "r");
	while (f && n < 6 && fgets(line, sizeof(line), f)) {
		char *ud;

		line[strcspn(line, "\t\n")] = 0;
		if (!line[0] || !(ud = strdup(line)))
			continue;
		if (!yh.chips) {
			yh.chips = lv_obj_create(yh.body);
			plain(yh.chips);
			lv_obj_remove_style_all(yh.chips);
			lv_obj_set_size(yh.chips, lv_pct(100), LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(yh.chips, LV_FLEX_FLOW_ROW_WRAP);
			lv_obj_set_style_pad_column(yh.chips, 8, 0);
			lv_obj_set_style_pad_row(yh.chips, 8, 0);
			lv_obj_remove_flag(yh.chips, LV_OBJ_FLAG_SCROLLABLE);
			lv_obj_move_to_index(yh.chips, 1);	/* right under the search bar */
		}
		lv_obj_add_event_cb(yt_chip(yh.chips, ud, yt_chip_cb, ud), yt_free_ud,
				    LV_EVENT_DELETE, ud);
		n++;
	}
	if (f)
		fclose(f);
	bp_changed(yh.scr);
}

/*
 * "Watched": the history, newest first. The rows are the body's own
 * children (each scrolls as one picture; a column of them taller than the
 * screen would be drawn live).
 */
static void yt_home_history(void)
{
	FILE *f;
	char line[512];
	int n = 0;

	if (!yh.body || !lv_obj_is_valid(yh.body))
		return;
	for (int i = 0; i < yh.nhist; i++)
		if (lv_obj_is_valid(yh.hist[i]))
			lv_obj_delete(yh.hist[i]);
	yh.nhist = 0;
	yh.hist_mtime = yt_hist_mtime();
	f = fopen(YT_DIR "/history", "r");
	while (f && n < YT_HIST_SHOWN && fgets(line, sizeof(line), f)) {
		char *fl[6];
		struct yt_vid v;
		lv_obj_t *row, *img;
		int k;

		line[strcspn(line, "\n")] = 0;
		k = yt_split(line, fl, 6);
		if (k < 2 || !yt_id_ok(fl[0]))
			continue;
		memset(&v, 0, sizeof(v));
		snprintf(v.id, sizeof(v.id), "%s", fl[0]);
		snprintf(v.title, sizeof(v.title), "%s", fl[1]);
		snprintf(v.chan, sizeof(v.chan), "%s", k > 2 ? fl[2] : "");
		snprintf(v.len, sizeof(v.len), "%s", k > 3 ? fl[3] : "");
		snprintf(v.meta, sizeof(v.meta), "%s", k > 4 ? fl[4] : "");
		if (!yh.nhist) {
			m3_section(yh.body, "Watched");
			yh.hist[yh.nhist++] = lv_obj_get_child(yh.body, -1);
		}
		row = yt_row(yh.body, &v, &img);
		yh.hist[yh.nhist++] = row;
		yt_thumbs_want(yh.body, row, img, v.id);
		n++;
	}
	if (f)
		fclose(f);
	if (!yh.nhist)
		yh.hist[yh.nhist++] = yt_state(yh.body, LV_SYMBOL_VIDEO, "Find something to watch",
					       "Search above. Videos play in 360p with sound on the "
					       "speaker, earphones or Bluetooth.", NULL, NULL);
	bp_changed(yh.scr);
	yt_thumbs_fetch();
}

static void yt_home_shown(lv_event_t *e)
{
	LV_UNUSED(e);
	/* searched or watched while away: those parts again */
	if (yt_mtime(YT_DIR "/searches") != yh.chips_mtime)
		yt_home_chips();
	if (yt_hist_mtime() != yh.hist_mtime)
		yt_home_history();
}

static void yt_home_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&yh, 0, sizeof(yh));
}

static void open_youtube(lv_event_t *e)
{
	lv_obj_t *bar;

	LV_UNUSED(e);
	if (yh.scr)
		return;
	yt_conf_load();
	yh.body = m3_page("YouTube", &yh.scr);
	lv_obj_add_event_cb(yh.scr, yt_home_deleted, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(yh.scr, yt_home_shown, LV_EVENT_SCREEN_LOADED, NULL);

	/* an M3 search bar: a full pill that opens the search page */
	bar = lv_obj_create(yh.body);
	plain(bar);
	lv_obj_set_size(bar, lv_pct(100), 52);
	lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(bar, M.surface_c_high, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, M.surface_c_highest, LV_STATE_PRESSED);
	lv_obj_set_style_pad_hor(bar, 18, 0);
	lv_obj_set_style_pad_column(bar, 14, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(bar, yt_bar_cb, LV_EVENT_CLICKED, NULL);
	yt_search_icon(bar, M.on_surface);
	m3_label(bar, "Search YouTube", &lv_font_montserrat_16, M.on_surface_v);

	yt_home_chips();
	yt_home_history();
	push(yh.scr);
}
