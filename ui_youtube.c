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
#define YT_OFFLINE_DIR	YT_DIR "/offline"
#define YT_OFFLINE_IDX	YT_OFFLINE_DIR "/index"
#define YT_SEP		" \xE2\x80\xA2 "		/* " • " */
#define YT_CACHE	"/tmp/n72-yt"		/* n72-yt's default */
#define YT_TW		112			/* list thumbnails, 16:9 */
#define YT_TH		63
#define YT_RECENT_MAX	8
#define YT_HIST_MAX	30
#define YT_HIST_SHOWN	12
#define YT_OFFLINE_MAX	200
#define YT_OFF_SHOWN	64
#define YT_ROWS_MAX	200			/* a search stops loading more here */
#define YT_STALE_MS	(30 * 60 * 1000)	/* stream addresses last ~6 h; renew after this */

struct yt_vid {
	char id[16];
	char title[160];
	char chan[96];
	char len[16];		/* "3:33", "LIVE", "SOON" or "" */
	char meta[72];		/* "138M views * 6 years ago" */
};

struct yt_off {
	struct yt_vid v;
	char mode[16];		/* "audio", "240p", "360p" */
	long long bytes;
	bool has_video;
	bool has_audio;
};

/* a list row's image: the thumbnail once loaded */
struct yt_img {
	lv_image_dsc_t dsc;
	uint8_t *data;
};

static int yt_quality = 360;
/*
 * Numbers for background requests, never reused: a page rebuilt (closed and
 * opened again, or a turn) must not take an answer meant for the old one.
 */
static unsigned yt_gen;	/* 240 or 360 (the decoder's 720x576 limit) */

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
		snprintf(v->meta, sizeof(v->meta), "%s" YT_SEP "%s", f[4], f[5]);
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

static void yt_size_fmt(long long b, char *out, size_t len)
{
	if (b < 1024)
		snprintf(out, len, "%lld B", b);
	else if (b < 1024LL * 1024)
		snprintf(out, len, "%.0f kB", b / 1024.0);
	else if (b < 1024LL * 1024 * 1024)
		snprintf(out, len, "%.1f MB", b / (1024.0 * 1024));
	else
		snprintf(out, len, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

static void yt_offline_mkdir(const char *id)
{
	char dir[256];

	yt_mkdir();
	mkdir(YT_OFFLINE_DIR, 0755);
	if (id && yt_id_ok(id)) {
		snprintf(dir, sizeof(dir), YT_OFFLINE_DIR "/%s", id);
		mkdir(dir, 0755);
	}
}

static void yt_offline_paths(const char *id, char *audio, size_t alen, char *video, size_t vlen)
{
	if (audio)
		snprintf(audio, alen, YT_OFFLINE_DIR "/%s/audio.m4a", id);
	if (video)
		snprintf(video, vlen, YT_OFFLINE_DIR "/%s/video.mp4", id);
}

static bool yt_offline_parse(char *line, struct yt_off *out)
{
	char *f[7];
	char ap[256], vp[256];
	int k;

	line[strcspn(line, "\r\n")] = 0;
	k = yt_split(line, f, 7);
	if (k < 2 || !yt_id_ok(f[0]))
		return false;
	memset(out, 0, sizeof(*out));
	snprintf(out->v.id, sizeof(out->v.id), "%s", f[0]);
	snprintf(out->v.title, sizeof(out->v.title), "%s", f[1]);
	snprintf(out->v.chan, sizeof(out->v.chan), "%s", k > 2 ? f[2] : "");
	snprintf(out->v.len, sizeof(out->v.len), "%s", k > 3 ? f[3] : "");
	snprintf(out->v.meta, sizeof(out->v.meta), "%s", k > 4 ? f[4] : "");
	snprintf(out->mode, sizeof(out->mode), "%s", k > 5 && f[5][0] ? f[5] : "audio");
	out->bytes = k > 6 ? atoll(f[6]) : 0;
	yt_offline_paths(out->v.id, ap, sizeof(ap), vp, sizeof(vp));
	out->has_audio = access(ap, R_OK) == 0;
	out->has_video = access(vp, R_OK) == 0;
	return out->has_audio;
}

static bool yt_offline_find(const char *id, struct yt_off *out)
{
	FILE *f;
	char line[512];

	if (!yt_id_ok(id))
		return false;
	f = fopen(YT_OFFLINE_IDX, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		struct yt_off tmp;

		if (yt_offline_parse(line, &tmp) && !strcmp(tmp.v.id, id)) {
			if (out)
				*out = tmp;
			fclose(f);
			return true;
		}
	}
	fclose(f);
	return false;
}

static void yt_offline_add(const struct yt_vid *v, const char *mode, long long bytes)
{
	char line[512];

	yt_offline_mkdir(NULL);
	snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\t%s\t%lld",
		 v->id, v->title, v->chan, v->len, v->meta, mode, bytes);
	yt_list_add(YT_OFFLINE_IDX, line, YT_OFFLINE_MAX);
}

static void yt_offline_unlink_parts(const char *id)
{
	static const char *const parts[] = { "audio.m4a.part", "video.mp4.part", "desc.txt.part" };
	char dir[256], p[300];

	if (!yt_id_ok(id))
		return;
	snprintf(dir, sizeof(dir), YT_OFFLINE_DIR "/%s", id);
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		snprintf(p, sizeof(p), "%s/%s", dir, parts[i]);
		unlink(p);
	}
	rmdir(dir);	/* succeeds only if nothing saved remains */
}

static void yt_dl_cancel(void);
static pid_t yt_dl_active_pid(const char *id);

static void yt_offline_remove(const char *id)
{
	static const char *const files[] = {
		"audio.m4a", "audio.m4a.part", "video.mp4", "video.mp4.part",
		"desc.txt", "desc.txt.part", "112x63.565", "288x162.565", "217x122.565"
	};
	char dir[256], p[300], tmp[PATH_MAX], buf[512];
	FILE *in, *out;
	size_t idlen;
	int kept = 0;

	if (!yt_id_ok(id))
		return;
	if (yt_dl_active_pid(id) > 0)
		yt_dl_cancel();
	snprintf(dir, sizeof(dir), YT_OFFLINE_DIR "/%s", id);
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
		unlink(p);
	}
	rmdir(dir);

	snprintf(tmp, sizeof(tmp), "%s.new", YT_OFFLINE_IDX);
	in = fopen(YT_OFFLINE_IDX, "r");
	if (!in)
		return;
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return;
	}
	idlen = strlen(id);
	while (fgets(buf, sizeof(buf), in)) {
		size_t k = strcspn(buf, "\t\r\n");

		if (k == idlen && !strncmp(buf, id, idlen))
			continue;
		if (buf[0] == '\n')
			continue;
		fputs(buf, out);
		if (!strchr(buf, '\n'))
			fputc('\n', out);
		kept++;
	}
	fclose(in);
	if (fclose(out) == 0) {
		if (kept)
			rename(tmp, YT_OFFLINE_IDX);
		else {
			unlink(tmp);
			unlink(YT_OFFLINE_IDX);
		}
	} else {
		unlink(tmp);
	}
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
	snprintf(path, sizeof(path), YT_OFFLINE_DIR "/%s/%dx%d.565", id, w, h);
	f = fopen(path, "rb");
	if (!f) {
		snprintf(path, sizeof(path), YT_CACHE "/%s-%dx%d.565", id, w, h);
		f = fopen(path, "rb");
	}
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

/* A section title with a compact "Clear" text button on the right. */
static lv_obj_t *yt_section_clear(lv_obj_t *parent, const char *title, lv_event_cb_t cb,
				  lv_obj_t **lbl_out)
{
	lv_obj_t *row = lv_obj_create(parent), *btn, *lbl;

	plain(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_left(row, 8, 0);
	lv_obj_set_style_pad_right(row, 0, 0);
	lv_obj_set_style_pad_top(row, 8, 0);
	lv_obj_set_style_pad_bottom(row, 0, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	m3_label(row, title, &lv_font_montserrat_14, M.primary);
	btn = lv_button_create(row);
	plain(btn);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 28);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(btn, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_style_pad_hor(btn, 10, 0);
	lv_obj_set_style_pad_ver(btn, 0, 0);
	lv_obj_set_ext_click_area(btn, 8);
	lbl = m3_label(btn, "Clear", &lv_font_montserrat_14, M.primary);
	lv_obj_center(lbl);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	if (lbl_out)
		*lbl_out = lbl;
	return row;
}

static void yt_open_video(const struct yt_vid *v);

/*
 * What the search and video pages showed, kept past the pages themselves:
 * a turn of the screen deletes every page and opens them again (ui_turn),
 * through openers that take no arguments.
 */
static char yt_re_query[128];
static struct yt_vid yt_re_vid;

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
 * thumbnail with its length, the title on two lines, the channel, and a
 * supporting line. *img_out: the thumbnail, to load later.
 */
static lv_obj_t *yt_row_meta(lv_obj_t *parent, const struct yt_vid *v, const char *sub2,
			     lv_obj_t **img_out)
{
	lv_obj_t *r = lv_obj_create(parent), *col, *t, *img;
	struct yt_vid *ud = malloc(sizeof(*ud));
	int lh = lv_font_get_line_height(&lv_font_montserrat_14);

	plain(r);
	lv_obj_set_size(r, lv_pct(100), YT_TH + 14);
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
	lv_obj_set_style_pad_row(col, 0, 0);
	lv_obj_set_height(col, YT_TH + 4);
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
	if (sub2 && sub2[0]) {
		t = m3_label(col, sub2, &lv_font_montserrat_12, M.on_surface_v);
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

static lv_obj_t *yt_row(lv_obj_t *parent, const struct yt_vid *v, lv_obj_t **img_out)
{
	return yt_row_meta(parent, v, v->meta, img_out);
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
	bool asked[YT_PEND_MAX];	/* in a fetch that has ended */
	bool in_job[YT_PEND_MAX];	/* in the fetch running now */
	int n;
	lv_obj_t *owner;	/* the body they are on */
	lv_timer_t *retry;
	bool running;
} yth;

static void yt_thumbs_apply(void);

/* A queued thumbnail's image was deleted (its row or page went): wherever
 * its entry sits now, it is blanked (entries move as the queue compacts,
 * so obj_track() on the slots would not do). */
static void yt_pend_gone(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_target(e);

	for (int i = 0; i < yth.n; i++)
		if (yth.img[i] == t)
			yth.img[i] = yth.row[i] = NULL;
}

static void yt_thumbs_retry(lv_timer_t *t)
{
	LV_UNUSED(t);
	yt_thumbs_apply();
}

/*
 * Thumbnails that are in the cache go on their rows. A row drawn from a
 * scroll picture just now waits (a timer looks again); one that a finished
 * fetch did not bring (no thumbnail on YouTube's side) keeps its tint.
 */
static void yt_thumbs_apply(void)
{
	bool waiting = false;
	int i, k = 0;

	for (i = 0; i < yth.n; i++) {
		bool cached;

		if (!yth.img[i] || lv_obj_get_user_data(yth.img[i]))
			continue;
		cached = yt_row_cached(yth.row[i]);
		if (!cached && yt_thumb_load(yth.img[i], yth.id[i], YT_TW, YT_TH))
			continue;
		if (!cached && yth.asked[i])
			continue;	/* fetched and not there: give up on it */
		waiting |= cached;
		if (k != i) {
			yth.row[k] = yth.row[i];
			yth.img[k] = yth.img[i];
			yth.asked[k] = yth.asked[i];
			yth.in_job[k] = yth.in_job[i];
			memcpy(yth.id[k], yth.id[i], sizeof(yth.id[k]));
		}
		k++;
	}
	yth.n = k;
	if (waiting && !yth.retry) {
		yth.retry = lv_timer_create(yt_thumbs_retry, 300, NULL);
	} else if (!waiting && yth.retry) {
		lv_timer_delete(yth.retry);
		yth.retry = NULL;
	}
}

static void yt_thumbs_fetch(void);

static void yt_thumbs_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	yth.running = false;
	for (int i = 0; i < yth.n; i++)
		if (yth.in_job[i]) {
			yth.in_job[i] = false;
			yth.asked[i] = true;
		}
	yt_thumbs_apply();
	yt_thumbs_fetch();	/* rows added meanwhile */
}

/* Fetch thumbnails for the rows still without one (one job at a time). */
static void yt_thumbs_fetch(void)
{
	char cmd[YT_PEND_MAX * 17 + 64];
	size_t o;
	int i, n = 0;

	if (yth.running || !yth.n || !yth.owner)
		return;
	yt_thumbs_apply();		/* (in the cache already: no download) */
	o = snprintf(cmd, sizeof(cmd), "n72-yt thumbs %d %d", YT_TW, YT_TH);
	for (i = 0; i < yth.n && o + 18 < sizeof(cmd); i++)
		if (!yth.asked[i] && yth.img[i] &&
		    !lv_obj_get_user_data(yth.img[i])) {
			o += snprintf(cmd + o, sizeof(cmd) - o, " %s", yth.id[i]);
			yth.in_job[i] = true;
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
	lv_obj_add_event_cb(img, yt_pend_gone, LV_EVENT_DELETE, NULL);
	yth.asked[yth.n] = false;
	yth.in_job[yth.n] = false;
	snprintf(yth.id[yth.n], sizeof(yth.id[0]), "%s", id);
	yth.n++;
}

/* ------------------------------------------------------------ search */

static struct {
	lv_obj_t *scr, *body, *ta, *kb, *recent, *tail, *clear_lbl;
	char query[128];
	char cont[1024];
	int rows;
	unsigned gen;		/* bumped by each new search: older answers are dropped */
	bool loading, clear_confirm;
	lv_timer_t *more_timer, *clear_timer;
} ys;

static void yt_search_run(const char *q);

/* The system keyboard came or went: the list ends above it. */
static void yt_kb_vis(bool shown, int32_t h)
{
	if (ys.body)
		lv_obj_set_height(ys.body, H - 56 - (shown ? h : 0));
}

static void yt_kb_show(bool on)
{
	if (!ys.ta)
		return;
	if (on)
		kbd_show_for(ys.ta, yt_kb_vis);
	else if (kbd.ta == ys.ta)
		kbd_hide();
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

static void yt_recent_cb(lv_event_t *e)
{
	const char *q = lv_event_get_user_data(e);

	lv_textarea_set_text(ys.ta, q);
	yt_search_run(q);
}

static void yt_search_clear_reset(lv_timer_t *t)
{
	LV_UNUSED(t);
	ys.clear_timer = NULL;
	ys.clear_confirm = false;
	if (ys.clear_lbl) {
		lv_obj_set_style_text_color(ys.clear_lbl, M.primary, 0);
		label_set_if_changed(ys.clear_lbl, "Clear");
	}
	bp_changed(ys.scr);
}

static void yt_search_clear_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!ys.clear_confirm) {
		ys.clear_confirm = true;
		if (ys.clear_lbl) {
			lv_obj_set_style_text_color(ys.clear_lbl, M.error, 0);
			label_set_if_changed(ys.clear_lbl, "Clear all?");
		}
		if (ys.clear_timer) {
			lv_timer_reset(ys.clear_timer);
		} else {
			ys.clear_timer = lv_timer_create(yt_search_clear_reset, 4000, NULL);
			lv_timer_set_repeat_count(ys.clear_timer, 1);
		}
		bp_changed(ys.scr);
		return;
	}
	if (ys.clear_timer) {
		lv_timer_delete(ys.clear_timer);
		ys.clear_timer = NULL;
	}
	ys.clear_confirm = false;
	unlink(YT_DIR "/searches");
	if (ys.recent) {
		lv_obj_delete(ys.recent);
		ys.recent = NULL;
	}
	bp_changed(ys.scr);
}

/* Recent searches as rows under the field, before any results. */
static void yt_recent_rows(lv_obj_t *body)
{
	FILE *f = fopen(YT_DIR "/searches", "r");
	char line[160];
	lv_obj_t *c = NULL;

	ys.clear_confirm = false;
	ys.clear_lbl = NULL;
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
			obj_track(&ys.recent);
			yt_section_clear(c, "Recent searches", yt_search_clear_cb, &ys.clear_lbl);
			obj_track(&ys.clear_lbl);
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
	if (ys.tail)
		lv_obj_delete(ys.tail);
	ys.tail = o;
	obj_track(&ys.tail);
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
	if (!ys.body)
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
	if (qtext != yt_re_query)	/* (a reopen passes yt_re_query itself) */
		snprintf(yt_re_query, sizeof(yt_re_query), "%s", qtext);
	yt_list_add(YT_DIR "/searches", ys.query, YT_RECENT_MAX);
	ys.gen = ++yt_gen;
	ys.rows = 0;
	ys.cont[0] = 0;
	ys.loading = true;
	yt_kb_show(false);
	if (ys.clear_timer) {
		lv_timer_delete(ys.clear_timer);
		ys.clear_timer = NULL;
	}
	ys.clear_confirm = false;
	ys.clear_lbl = NULL;
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
	if (ys.clear_timer)
		lv_timer_delete(ys.clear_timer);
	memset(&ys, 0, sizeof(ys));
}

/* The search page: the field in the top bar, the keyboard, then results. */
static void yt_reopen_search(lv_event_t *e);

static void yt_open_search(const char *query)
{
	lv_obj_t *bar;

	if (ys.scr)
		return;			/* (one at a time) */
	nav_mark(yt_reopen_search, NULL);	/* opened again when the screen turns */
	if (query != yt_re_query)
		snprintf(yt_re_query, sizeof(yt_re_query), "%s", query ? query : "");
	ys.body = m3_page("", &ys.scr);
	obj_track(&ys.scr);
	obj_track(&ys.body);
	lv_obj_add_event_cb(ys.scr, yt_search_deleted, LV_EVENT_DELETE, NULL);
	bar = lv_obj_get_child(ys.scr, 0);	/* m3_page's top bar */
	ys.ta = lv_textarea_create(bar);
	obj_track(&ys.ta);
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
	/*
	 * A steady cursor: LVGL runs the blink as an endless animation for
	 * every text area, focused or not, and ui_turn waits for no animation
	 * to be running (rot_can_turn), so a blinking field anywhere on the
	 * page stack kept the screen from ever turning. (Fewer redraws, too.)
	 * Set before any text: each set_text restarts the blink by this value.
	 */
	lv_obj_set_style_anim_duration(ys.ta, 0, LV_PART_CURSOR);
	lv_textarea_set_text(ys.ta, "");	/* (the one started at creation goes) */
	/* the system keyboard (kbd.c): Enter searches, its Hide is Cancel */
	kbd_attach(ys.ta, yt_kb_vis, false);
	lv_obj_add_event_cb(ys.ta, yt_kb_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ys.ta, yt_kb_cb, LV_EVENT_CANCEL, NULL);
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

static void yt_reopen_search(lv_event_t *e)
{
	LV_UNUSED(e);
	yt_open_search(yt_re_query[0] ? yt_re_query : NULL);
}

/* ------------------------------------------------------------ download */

static struct {
	pid_t pid;
	int fd, pidfd;
	lv_timer_t *poll;
	char line[1024];
	size_t llen;
	struct yt_vid v;
	bool audio_only;
	int quality, pct;
	long long done_b, total_b;
	char err[256], err_id[16];
	bool ok, canceled;
	char out_mode[16];
	long long out_bytes;
} ydl = { .fd = -1, .pidfd = -1 };

static void yt_dl_ui_sync(void);
static void yt_video_after_dl(const char *id);
static void yt_offline_refresh(void);
static void yt_home_offline(void);
static void yt_home_history(void);

bool yt_dl_busy(void)
{
	return ydl.pid > 0;
}

static pid_t yt_dl_active_pid(const char *id)
{
	return (ydl.pid > 0 && id && !strcmp(ydl.v.id, id)) ? ydl.pid : 0;
}

static void yt_dl_cancel(void)
{
	if (ydl.pid <= 0)
		return;
	ydl.canceled = true;
	kill(-ydl.pid, SIGTERM);
}

static void yt_dl_parse_line(char *ln)
{
	char *f[6];
	int n = yt_split(ln, f, 6);

	if (!strcmp(f[0], "I") && n >= 5) {
		if (f[1][0])
			snprintf(ydl.v.title, sizeof(ydl.v.title), "%s", f[1]);
		if (f[2][0])
			snprintf(ydl.v.chan, sizeof(ydl.v.chan), "%s", f[2]);
		if (atoi(f[4]) > 0)
			yt_duration(atoi(f[4]), ydl.v.len, sizeof(ydl.v.len));
	} else if (!strcmp(f[0], "P") && n >= 4) {
		ydl.pct = LV_CLAMP(0, atoi(f[1]), 100);
		ydl.done_b = atoll(f[2]);
		ydl.total_b = atoll(f[3]);
		yt_dl_ui_sync();
	} else if (!strcmp(f[0], "O") && n >= 4) {
		ydl.ok = true;
		snprintf(ydl.out_mode, sizeof(ydl.out_mode), "%s", f[2]);
		ydl.out_bytes = atoll(f[3]);
	} else if (!strcmp(f[0], "E") && n >= 2) {
		snprintf(ydl.err, sizeof(ydl.err), "%s", f[1]);
		snprintf(ydl.err_id, sizeof(ydl.err_id), "%s", ydl.v.id);
	}
}

static void yt_dl_drain(bool *eof)
{
	char buf[512];
	ssize_t n;

	if (eof)
		*eof = false;
	if (ydl.fd < 0)
		return;
	while ((n = read(ydl.fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\r')
				continue;
			if (c == '\n') {
				ydl.line[ydl.llen] = 0;
				if (ydl.llen)
					yt_dl_parse_line(ydl.line);
				ydl.llen = 0;
			} else if (ydl.llen + 1 < sizeof(ydl.line)) {
				ydl.line[ydl.llen++] = c;
			}
		}
	}
	if (n == 0 && eof)
		*eof = true;
}

static void yt_dl_finish(void)
{
	char done_id[16];
	bool ok = ydl.ok && !ydl.canceled;

	yt_dl_drain(NULL);
	if (ydl.llen) {
		ydl.line[ydl.llen] = 0;
		yt_dl_parse_line(ydl.line);
		ydl.llen = 0;
	}
	if (ydl.fd >= 0) {
		ui_unwatch(ydl.fd);
		close(ydl.fd);
		ydl.fd = -1;
	}
	if (ydl.pidfd >= 0) {
		ui_unwatch(ydl.pidfd);
		close(ydl.pidfd);
		ydl.pidfd = -1;
	}
	if (ydl.poll) {
		lv_timer_delete(ydl.poll);
		ydl.poll = NULL;
	}
	ydl.pid = 0;
	snprintf(done_id, sizeof(done_id), "%s", ydl.v.id);
	if (ok) {
		yt_offline_add(&ydl.v, ydl.out_mode, ydl.out_bytes);
		ydl.err[0] = 0;
		ydl.err_id[0] = 0;
	} else {
		yt_offline_unlink_parts(done_id);
		if (!ydl.canceled && !ydl.err[0]) {
			snprintf(ydl.err, sizeof(ydl.err), "Download failed");
			snprintf(ydl.err_id, sizeof(ydl.err_id), "%s", done_id);
		}
	}
	yt_video_after_dl(done_id);
	yt_offline_refresh();
	yt_home_offline();
	yt_home_history();
}

static void ydl_ev(int fd)
{
	int st;

	if (fd == ydl.fd) {
		bool eof = false;

		yt_dl_drain(&eof);
		if (eof)
			ui_unwatch(ydl.fd);
		return;
	}
	if (fd == ydl.pidfd && ydl.pid > 0 && waitpid(ydl.pid, &st, WNOHANG) == ydl.pid)
		yt_dl_finish();
}

static void ydl_poll_cb(lv_timer_t *t)
{
	int st;

	LV_UNUSED(t);
	yt_dl_drain(NULL);
	if (ydl.pid > 0 && waitpid(ydl.pid, &st, WNOHANG) == ydl.pid)
		yt_dl_finish();
}

static void yt_dl_start(const struct yt_vid *v, bool audio_only, int quality)
{
	char dir[256], qdir[300], cmd[512];
	char *argv[] = { "sh", "-c", cmd, NULL };
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	int p[2];
	pid_t pid;

	if (ydl.pid > 0 || !yt_id_ok(v->id))
		return;
	yt_offline_mkdir(v->id);
	snprintf(dir, sizeof(dir), YT_OFFLINE_DIR "/%s", v->id);
	shq(dir, qdir, sizeof(qdir));
	snprintf(cmd, sizeof(cmd), "exec n72-yt download %s -o %s -q %d%s",
		 v->id, qdir, quality, audio_only ? " -a" : "");
	if (pipe(p))
		return;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_adddup2(&fa, p[1], 2);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&at, 0);
	if (posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ))
		pid = -1;
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (pid <= 0) {
		close(p[0]);
		return;
	}
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	memset(&ydl, 0, sizeof(ydl));
	ydl.pid = pid;
	ydl.fd = p[0];
	ydl.pidfd = -1;
	ydl.v = *v;
	ydl.audio_only = audio_only;
	ydl.quality = quality;
	if (nwatch <= NWATCH - 2) {
		ydl.pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
		if (ydl.pidfd >= 0) {
			fcntl(ydl.pidfd, F_SETFD, FD_CLOEXEC);
			ui_watch(ydl.fd, ydl_ev);
			ui_watch(ydl.pidfd, ydl_ev);
		}
	}
	if (ydl.pidfd < 0)
		ydl.poll = lv_timer_create(ydl_poll_cb, 200, NULL);
	yt_video_after_dl(v->id);
	yt_offline_refresh();
	yt_home_offline();
	yt_dl_ui_sync();
}

/* ------------------------------------------------------------ video */

#define YT_CMD_MAX	8192

static struct {
	lv_obj_t *scr, *body, *img, *title, *meta, *play, *listen, *status, *desc, *seg[2];
	lv_obj_t *off_card, *dl_lbl, *dl_bar, *rm_lbl;
	struct yt_vid v;
	char title_full[200];
	char play_cmd[YT_CMD_MAX], listen_cmd[YT_CMD_MAX];
	unsigned gen;
	uint32_t resolved_at;
	bool ready, desc_more, offline, offline_has_video, rm_confirm;
	char *desc_text;
	int tw, th;		/* the thumbnail's size on this page */
	lv_timer_t *rm_timer;
} yv;

static void yt_resolve(void);
static void yt_video_off_sync(void);

static void yt_btn_enable(lv_obj_t *b, bool on)
{
	if (!b)
		return;
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

static void yt_video_load_desc(void)
{
	char path[256];
	FILE *f;
	long sz;
	char *buf;

	if (yv.desc_text || !yv.desc)
		return;
	snprintf(path, sizeof(path), YT_OFFLINE_DIR "/%s/desc.txt", yv.v.id);
	f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	if (sz < 0)
		sz = 0;
	if (sz > 4096)
		sz = 4096;
	rewind(f);
	buf = malloc((size_t)sz + 1);
	if (buf) {
		size_t got = fread(buf, 1, (size_t)sz, f);

		buf[got] = 0;
		yv.desc_text = buf[0] ? buf : strdup("No description.");
		if (yv.desc_text != buf)
			free(buf);
		yt_desc_show();
	}
	fclose(f);
}

static bool yt_video_apply_offline(void)
{
	struct yt_off off;
	char ap[256], vp[256], qa[320], qv[320], t[300], sz[32], msg[96];

	if (!yt_offline_find(yv.v.id, &off)) {
		yv.offline = false;
		yv.offline_has_video = false;
		return false;
	}
	yv.offline = true;
	yv.offline_has_video = off.has_video;
	if (!yv.title_full[0] && off.v.title[0])
		snprintf(yv.title_full, sizeof(yv.title_full), "%s", off.v.title);
	if (off.v.title[0] && yv.title)
		label_set_if_changed(yv.title, off.v.title);
	if (!yv.v.chan[0] && off.v.chan[0])
		snprintf(yv.v.chan, sizeof(yv.v.chan), "%s", off.v.chan);
	if (yv.meta) {
		char m[128];

		snprintf(m, sizeof(m), "%s%s%s", yv.v.chan,
			 yv.v.chan[0] && yv.v.len[0] ? "\n" : "", yv.v.len);
		if (m[0])
			label_set_if_changed(yv.meta, m);
	}
	yt_offline_paths(yv.v.id, ap, sizeof(ap), vp, sizeof(vp));
	shq(yv.title_full[0] ? yv.title_full : yv.v.title, t, sizeof(t));
	shq(ap, qa, sizeof(qa));
	snprintf(yv.listen_cmd, sizeof(yv.listen_cmd), "n72-player --title %s %s", t, qa);
	if (off.has_video) {
		shq(vp, qv, sizeof(qv));
		snprintf(yv.play_cmd, sizeof(yv.play_cmd), "n72-player --title %s --audio %s %s",
			 t, qa, qv);
	} else {
		yv.play_cmd[0] = 0;
	}
	yv.ready = true;
	yv.resolved_at = lv_tick_get();
	yt_btn_enable(yv.play, off.has_video);
	yt_btn_enable(yv.listen, true);
	yt_video_load_desc();
	yt_size_fmt(off.bytes, sz, sizeof(sz));
	snprintf(msg, sizeof(msg), "Saved offline (%s" YT_SEP "%s)",
		 !strcmp(off.mode, "audio") ? "Audio" : off.mode, sz);
	yt_status(msg, false);
	return true;
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
			if (f[1][0] && yv.title)
				label_set_if_changed(yv.title, f[1]);
			if (f[2][0])
				snprintf(yv.v.chan, sizeof(yv.v.chan), "%s", f[2]);
			snprintf(meta, sizeof(meta), "%s%s%s%s%s", f[2], f[2][0] ? "\n" : "",
				 views, views[0] ? YT_SEP : "", len);
			if (yv.meta)
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
		if (yv.offline) {
			yv.ready = true;
			yt_btn_enable(yv.listen, true);
			yt_btn_enable(yv.play, yv.offline_has_video);
			return;
		}
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
		if (!yv.offline)
			snprintf(yv.listen_cmd, sizeof(yv.listen_cmd), "n72-player --title %s %s", t, qa);
	}
	yv.ready = true;
	yv.resolved_at = lv_tick_get();
	yt_btn_enable(yv.play, yv.play_cmd[0] != 0);
	yt_btn_enable(yv.listen, true);
	if (!yv.offline)
		yt_status("", false);
}

static void yt_resolve(void)
{
	char cmd[128];

	yv.gen = ++yt_gen;
	if (!yv.offline) {
		yv.ready = false;
		yt_btn_enable(yv.play, false);
		yt_btn_enable(yv.listen, false);
		yt_status("Getting the video...", false);
	} else {
		yt_btn_enable(yv.play, yv.offline_has_video);
	}
	snprintf(cmd, sizeof(cmd), "n72-yt video %s -q %d", yv.v.id, yt_quality);
	job_run(cmd, yv.body, yt_resolved, (void *)(uintptr_t)yv.gen);
}

/* Before app_launch_cb (added after this on the same button): history. */
static void yt_play_pre(lv_event_t *e)
{
	struct yt_vid h = yv.v;
	lv_obj_t *b = lv_event_get_current_target(e);

	if (!yv.ready)
		return;
	if (yv.title_full[0])
		snprintf(h.title, sizeof(h.title), "%s", yv.title_full);
	yt_history_add(&h);
	if (b == yv.listen)
		vc_set_audio_meta(yv.v.id, yv.v.chan, h.title);
	else
		vc_set_audio_meta(NULL, NULL, NULL);
}

static void yt_video_shown(lv_event_t *e)
{
	LV_UNUSED(e);
	/* back on the page after a long while: the addresses may have expired */
	if (!yv.offline_has_video && yv.ready && lv_tick_elaps(yv.resolved_at) > YT_STALE_MS)
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
	yt_video_off_sync();
	bp_changed(yv.scr);
	if (!yv.offline_has_video)
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
		obj_track(&yv.seg[i]);
	}
	return row;
}

static void yt_video_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	if (yv.rm_timer)
		lv_timer_delete(yv.rm_timer);
	free(yv.desc_text);
	memset(&yv, 0, sizeof(yv));
}

static void yt_big_thumb_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	if (yv.img)
		yt_thumb_load(yv.img, yv.v.id, yv.tw, yv.th);
}

/* A transparent flex container without padding (layout only). */
static lv_obj_t *yt_box(lv_obj_t *parent, lv_flex_flow_t flow, int gap)
{
	lv_obj_t *o = lv_obj_create(parent);

	plain(o);
	lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_style_pad_gap(o, gap, 0);
	lv_obj_set_flex_flow(o, flow);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	return o;
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

static void yt_reopen_video(lv_event_t *e)
{
	LV_UNUSED(e);
	yt_open_video(&yt_re_vid);
}

static void yt_dl_video_cb(lv_event_t *e)
{
	struct yt_vid v = yv.v;

	LV_UNUSED(e);
	if (yv.title_full[0])
		snprintf(v.title, sizeof(v.title), "%s", yv.title_full);
	yt_dl_start(&v, false, yt_quality);
}

static void yt_dl_audio_cb(lv_event_t *e)
{
	struct yt_vid v = yv.v;

	LV_UNUSED(e);
	if (yv.title_full[0])
		snprintf(v.title, sizeof(v.title), "%s", yv.title_full);
	yt_dl_start(&v, true, yt_quality);
}

static void yt_dl_cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	yt_dl_cancel();
}

static void yt_video_rm_reset(lv_timer_t *t)
{
	LV_UNUSED(t);
	yv.rm_timer = NULL;
	yv.rm_confirm = false;
	if (yv.rm_lbl) {
		lv_obj_set_style_text_color(yv.rm_lbl, M.on_secondary_c, 0);
		label_set_if_changed(yv.rm_lbl, LV_SYMBOL_TRASH "  Remove");
	}
	bp_changed(yv.scr);
}

static void yt_video_rm_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!yv.rm_confirm) {
		yv.rm_confirm = true;
		if (yv.rm_lbl) {
			lv_obj_set_style_text_color(yv.rm_lbl, M.error, 0);
			label_set_if_changed(yv.rm_lbl, "Remove?");
		}
		if (yv.rm_timer) {
			lv_timer_reset(yv.rm_timer);
		} else {
			yv.rm_timer = lv_timer_create(yt_video_rm_reset, 4000, NULL);
			lv_timer_set_repeat_count(yv.rm_timer, 1);
		}
		bp_changed(yv.scr);
		return;
	}
	if (yv.rm_timer) {
		lv_timer_delete(yv.rm_timer);
		yv.rm_timer = NULL;
	}
	yv.rm_confirm = false;
	yt_offline_remove(yv.v.id);
	yv.offline = false;
	yv.offline_has_video = false;
	yv.play_cmd[0] = 0;
	yv.listen_cmd[0] = 0;
	yt_video_off_sync();
	yt_resolve();
}

static void yt_dl_fmt_prog(char *out, size_t len)
{
	char d[24], t[24];
	char mode[16];

	if (ydl.audio_only)
		snprintf(mode, sizeof(mode), "audio");
	else
		snprintf(mode, sizeof(mode), "%dp", ydl.quality);
	if (ydl.total_b > 0) {
		yt_size_fmt(ydl.done_b, d, sizeof(d));
		yt_size_fmt(ydl.total_b, t, sizeof(t));
		snprintf(out, len, "Downloading %s... %d%% (%s / %s)", mode, ydl.pct, d, t);
	} else {
		snprintf(out, len, "Starting %s download...", mode);
	}
}

static lv_obj_t *yt_off_btn(lv_obj_t *parent, const char *txt, bool filled, lv_event_cb_t cb)
{
	lv_obj_t *b = m3_button(parent, txt, filled, cb, NULL);

	lv_obj_set_height(b, 42);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_pad_hor(b, 8, 0);
	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
	return b;
}

static lv_obj_t *yt_dl_bar_make(lv_obj_t *parent, int pct)
{
	lv_obj_t *bar = lv_bar_create(parent);

	lv_obj_set_size(bar, lv_pct(100), 8);
	lv_bar_set_range(bar, 0, 100);
	lv_bar_set_value(bar, pct, LV_ANIM_OFF);
	lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, M.surface_c_highest, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(bar, M.primary, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
	return bar;
}

static void yt_video_off_sync(void)
{
	struct yt_off off;
	lv_obj_t *row, *b;
	char buf[96], sz[24];

	if (!yv.off_card)
		return;
	if (yv.rm_timer) {
		lv_timer_delete(yv.rm_timer);
		yv.rm_timer = NULL;
	}
	yv.rm_confirm = false;
	yv.dl_lbl = NULL;
	yv.dl_bar = NULL;
	yv.rm_lbl = NULL;
	lv_obj_clean(yv.off_card);
	lv_obj_set_flex_flow(yv.off_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(yv.off_card, 10, 0);

	if (ydl.pid > 0 && !strcmp(ydl.v.id, yv.v.id)) {
		yt_dl_fmt_prog(buf, sizeof(buf));
		yv.dl_lbl = m3_label(yv.off_card, buf, &lv_font_montserrat_14, M.on_surface);
		obj_track(&yv.dl_lbl);
		lv_obj_set_width(yv.dl_lbl, lv_pct(100));
		yv.dl_bar = yt_dl_bar_make(yv.off_card, ydl.pct);
		obj_track(&yv.dl_bar);
		b = m3_button(yv.off_card, "Cancel", false, yt_dl_cancel_cb, NULL);
		lv_obj_set_height(b, 40);
	} else if (ydl.pid > 0) {
		snprintf(buf, sizeof(buf), "Another download is running (%d%%)...", ydl.pct);
		yv.dl_lbl = m3_label(yv.off_card, buf, &lv_font_montserrat_14, M.on_surface_v);
		obj_track(&yv.dl_lbl);
		lv_obj_set_width(yv.dl_lbl, lv_pct(100));
	} else if (yt_offline_find(yv.v.id, &off)) {
		yt_size_fmt(off.bytes, sz, sizeof(sz));
		snprintf(buf, sizeof(buf), "Saved offline" YT_SEP "%s" YT_SEP "%s",
			 !strcmp(off.mode, "audio") ? "Audio" : off.mode, sz);
		m3_label(yv.off_card, buf, &lv_font_montserrat_14, M.on_surface);
		row = yt_box(yv.off_card, LV_FLEX_FLOW_ROW, 8);
		lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
		if (!off.has_video) {
			snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD "  Save %dp", yt_quality);
			yt_off_btn(row, buf, false, yt_dl_video_cb);
		}
		b = yt_off_btn(row, LV_SYMBOL_TRASH "  Remove", false, yt_video_rm_cb);
		yv.rm_lbl = lv_obj_get_child(b, 0);
		obj_track(&yv.rm_lbl);
	} else {
		row = yt_box(yv.off_card, LV_FLEX_FLOW_ROW, 8);
		lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
		snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD "  Save %dp", yt_quality);
		yt_off_btn(row, buf, false, yt_dl_video_cb);
		yt_off_btn(row, LV_SYMBOL_AUDIO "  Save audio", false, yt_dl_audio_cb);
		if (ydl.err[0] && !strcmp(ydl.err_id, yv.v.id)) {
			lv_obj_t *el = m3_label(yv.off_card, ydl.err, &lv_font_montserrat_12, M.error);

			lv_obj_set_width(el, lv_pct(100));
			lv_label_set_long_mode(el, LV_LABEL_LONG_WRAP);
		}
	}
	bp_changed(yv.scr);
}

static void yt_video_after_dl(const char *id)
{
	if (!yv.scr || !id || strcmp(yv.v.id, id))
		return;
	yt_video_apply_offline();
	yt_video_off_sync();
}

static void yt_open_video(const struct yt_vid *v)
{
	lv_obj_t *c, *row, *side;
	bool wide = W > H;	/* landscape: thumbnail left, details and buttons right */

	if (yv.scr || !yt_id_ok(v->id))
		return;
	nav_mark(yt_reopen_video, NULL);	/* opened again when the screen turns */
	if (v != &yt_re_vid)
		yt_re_vid = *v;
	yv.v = *v;
	snprintf(yv.title_full, sizeof(yv.title_full), "%s", v->title);
	yv.body = m3_page("", &yv.scr);
	obj_track(&yv.scr);
	obj_track(&yv.body);
	lv_obj_add_event_cb(yv.scr, yt_video_deleted, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(yv.scr, yt_video_shown, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_set_style_pad_row(yv.body, 12, 0);

	/*
	 * Portrait: one column. Landscape (a turn): the 16:9 thumbnail across
	 * the whole width would fill the screen and push Play out of sight, so
	 * the top is two panes, the thumbnail half the width.
	 */
	yv.tw = W - 2 * STATUS_PAD;
	side = yv.body;
	row = yv.body;
	if (wide) {
		row = yt_box(yv.body, LV_FLEX_FLOW_ROW, 14);
		lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
		yv.tw = (yv.tw - 14) / 2;
	}
	yv.th = yv.tw * 9 / 16;
	yv.img = yt_thumb(row, yv.tw, yv.th, 16, v->len);
	obj_track(&yv.img);
	if (!yt_thumb_load(yv.img, v->id, yv.tw, yv.th)) {
		char cmd[96];

		snprintf(cmd, sizeof(cmd), "n72-yt thumbs %d %d %s", yv.tw, yv.th, v->id);
		job_run(cmd, yv.body, yt_big_thumb_done, NULL);
	}
	if (wide) {
		side = yt_box(row, LV_FLEX_FLOW_COLUMN, 6);
		lv_obj_set_height(side, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(side, 1);
	}

	yv.title = m3_label(side, v->title[0] ? v->title : "Untitled video",
			    wide ? &lv_font_montserrat_16 : &lv_font_montserrat_20, M.on_surface);
	lv_obj_set_width(yv.title, lv_pct(100));
	if (wide) {
		/* three lines at most beside the thumbnail */
		lv_label_set_long_mode(yv.title, LV_LABEL_LONG_DOT);
		lv_obj_set_height(yv.title, 3 * lv_font_get_line_height(&lv_font_montserrat_16));
	} else {
		lv_label_set_long_mode(yv.title, LV_LABEL_LONG_WRAP);
	}
	obj_track(&yv.title);
	yv.meta = m3_label(side, v->chan, wide ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
			   M.on_surface_v);
	obj_track(&yv.meta);
	lv_obj_set_width(yv.meta, lv_pct(100));
	lv_label_set_long_mode(yv.meta, LV_LABEL_LONG_WRAP);

	row = yt_box(side, LV_FLEX_FLOW_ROW, 8);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	yv.play = yt_action(row, LV_SYMBOL_PLAY, "Play", true, yv.play_cmd);
	yv.listen = yt_action(row, LV_SYMBOL_AUDIO, "Listen", false, yv.listen_cmd);
	obj_track(&yv.play);
	obj_track(&yv.listen);
	if (wide) {
		lv_obj_set_height(yv.play, 44);
		lv_obj_set_height(yv.listen, 44);
	}

	yv.status = m3_label(yv.body, "", &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&yv.status);
	lv_obj_set_width(yv.status, lv_pct(100));
	lv_label_set_long_mode(yv.status, LV_LABEL_LONG_WRAP);

	m3_section(yv.body, "Quality");
	yt_quality_seg(yv.body);

	m3_section(yv.body, "Offline");
	yv.off_card = m3_card(yv.body, M.surface_c);
	obj_track(&yv.off_card);
	yt_video_off_sync();

	m3_section(yv.body, "Description");
	c = m3_card(yv.body, M.surface_c);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(c, yt_desc_cb, LV_EVENT_CLICKED, NULL);
	yv.desc = m3_label(c, "", &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&yv.desc);
	lv_obj_set_width(yv.desc, lv_pct(100));
	lv_label_set_long_mode(yv.desc, LV_LABEL_LONG_WRAP);

	push(yv.scr);
	if (yt_video_apply_offline()) {
		if (!yv.offline_has_video)
			yt_resolve();
	} else {
		yt_resolve();
	}
}

/* ------------------------------------------------------------ offline */

static int yt_off_filter;	/* 0 All, 1 Video, 2 Audio (kept across turns) */

static struct {
	lv_obj_t *scr, *body, *summary, *seg[3];
	lv_obj_t *dl_card, *dl_lbl, *dl_bar;
	lv_obj_t *rows[YT_OFF_SHOWN + 1];
	int nrows;
	long mtime;
	lv_obj_t *del_btn, *del_lbl;
	char del_id[16];
	lv_timer_t *del_timer;
} yo;

static void yt_off_del_reset(void)
{
	if (yo.del_btn)
		lv_obj_set_style_bg_opa(yo.del_btn, LV_OPA_TRANSP, 0);
	if (yo.del_lbl)
		lv_obj_set_style_text_color(yo.del_lbl, M.on_surface_v, 0);
	yo.del_btn = NULL;
	yo.del_lbl = NULL;
	yo.del_id[0] = 0;
}

static void yt_off_del_timer_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	yo.del_timer = NULL;
	yt_off_del_reset();
	bp_changed(yo.scr);
}

static void yt_off_del_cb(lv_event_t *e)
{
	const char *id = lv_event_get_user_data(e);
	lv_obj_t *btn = lv_event_get_current_target(e);
	lv_obj_t *lbl = lv_obj_get_child(btn, 0);

	if (!id || !yt_id_ok(id))
		return;
	if (strcmp(yo.del_id, id)) {
		yt_off_del_reset();
		snprintf(yo.del_id, sizeof(yo.del_id), "%s", id);
		yo.del_btn = btn;
		yo.del_lbl = lbl;
		obj_track(&yo.del_btn);
		obj_track(&yo.del_lbl);
		lv_obj_set_style_bg_color(btn, M.error, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
		if (lbl)
			lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
		if (yo.del_timer) {
			lv_timer_reset(yo.del_timer);
		} else {
			yo.del_timer = lv_timer_create(yt_off_del_timer_cb, 4000, NULL);
			lv_timer_set_repeat_count(yo.del_timer, 1);
		}
		bp_changed(yo.scr);
		return;
	}
	if (yo.del_timer) {
		lv_timer_delete(yo.del_timer);
		yo.del_timer = NULL;
	}
	yt_off_del_reset();
	yt_offline_remove(id);
	yt_offline_refresh();
	yt_home_offline();
	yt_home_history();
}

static lv_obj_t *yt_off_row(lv_obj_t *parent, const struct yt_off *o, lv_obj_t **img_out)
{
	char sub2[64], sz[24];
	lv_obj_t *r, *del, *lbl;
	char *did;

	yt_size_fmt(o->bytes, sz, sizeof(sz));
	snprintf(sub2, sizeof(sub2), "%s" YT_SEP "%s",
		 !strcmp(o->mode, "audio") ? "Audio" : o->mode, sz);
	r = yt_row_meta(parent, &o->v, sub2, img_out);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	del = lv_button_create(r);
	plain(del);
	lv_obj_set_size(del, 36, 36);
	lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(del, M.error, 0);
	lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(del, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(del, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_ext_click_area(del, 6);
	lbl = m3_label(del, LV_SYMBOL_TRASH, &lv_font_montserrat_16, M.on_surface_v);
	lv_obj_center(lbl);
	did = strdup(o->v.id);
	if (did) {
		lv_obj_add_event_cb(del, yt_off_del_cb, LV_EVENT_CLICKED, did);
		lv_obj_add_event_cb(del, yt_free_ud, LV_EVENT_DELETE, did);
	}
	return r;
}

static lv_obj_t *yt_dl_card_make(lv_obj_t *parent, lv_obj_t **lbl_out, lv_obj_t **bar_out)
{
	lv_obj_t *c = m3_card(parent, M.surface_c), *top, *btn, *lbl, *bar;
	char ptxt[96];

	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 8, 0);
	top = yt_box(c, LV_FLEX_FLOW_ROW, 8);
	lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	yt_dl_fmt_prog(ptxt, sizeof(ptxt));
	lbl = m3_label(top, ptxt, &lv_font_montserrat_14, M.on_surface);
	lv_obj_set_flex_grow(lbl, 1);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	btn = lv_button_create(top);
	plain(btn);
	lv_obj_set_size(btn, 32, 32);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(btn, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_center(m3_label(btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, M.on_surface_v));
	lv_obj_add_event_cb(btn, yt_dl_cancel_cb, LV_EVENT_CLICKED, NULL);
	bar = yt_dl_bar_make(c, ydl.pct);
	if (lbl_out)
		*lbl_out = lbl;
	if (bar_out)
		*bar_out = bar;
	return c;
}

static void yt_off_seg_cb(lv_event_t *e)
{
	int f = (int)(intptr_t)lv_event_get_user_data(e);

	if (f == yt_off_filter)
		return;
	yt_off_filter = f;
	for (int i = 0; i < 3; i++) {
		bool on = i == f;

		lv_obj_set_style_bg_color(yo.seg[i], on ? M.secondary_c : M.surface, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(yo.seg[i], 0),
					    on ? M.on_secondary_c : M.on_surface, 0);
	}
	yt_offline_refresh();
}

static lv_obj_t *yt_off_seg(lv_obj_t *parent)
{
	static const char *const names[3] = { "All", "Video", "Audio" };
	lv_obj_t *row = lv_obj_create(parent);

	plain(row);
	lv_obj_set_size(row, lv_pct(100), 40);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_column(row, 0, 0);
	lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_color(row, M.outline, 0);
	lv_obj_set_style_pad_all(row, 3, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	for (int i = 0; i < 3; i++) {
		bool on = i == yt_off_filter;
		lv_obj_t *b = lv_obj_create(row), *l;

		plain(b);
		lv_obj_set_height(b, lv_pct(100));
		lv_obj_set_flex_grow(b, 1);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(b, on ? M.secondary_c : M.surface, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
		l = m3_label(b, names[i], &lv_font_montserrat_14,
			     on ? M.on_secondary_c : M.on_surface);
		lv_obj_center(l);
		lv_obj_add_event_cb(b, yt_off_seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		yo.seg[i] = b;
		obj_track(&yo.seg[i]);
	}
	return row;
}

static long yt_mtime(const char *path);

static void yt_offline_refresh(void)
{
	FILE *f;
	char line[512], sum[96], sz[24], freesz[24] = "";
	struct yt_off items[YT_OFF_SHOWN];
	struct statvfs sv;
	long long total_b = 0;
	int n = 0, shown = 0;

	if (!yo.body)
		return;
	if (yo.del_timer) {
		lv_timer_delete(yo.del_timer);
		yo.del_timer = NULL;
	}
	yt_off_del_reset();
	if (yo.dl_card)
		lv_obj_delete(yo.dl_card);
	yo.dl_card = NULL;
	yo.dl_lbl = NULL;
	yo.dl_bar = NULL;
	for (int i = 0; i < yo.nrows; i++)
		if (yo.rows[i])
			lv_obj_delete(yo.rows[i]);
	yo.nrows = 0;
	yo.mtime = yt_mtime(YT_OFFLINE_IDX);

	f = fopen(YT_OFFLINE_IDX, "r");
	while (f && n < YT_OFF_SHOWN && fgets(line, sizeof(line), f)) {
		if (yt_offline_parse(line, &items[n])) {
			total_b += items[n].bytes;
			n++;
		}
	}
	if (f)
		fclose(f);

	if (!statvfs(YT_OFFLINE_DIR, &sv) || !statvfs("/root", &sv)) {
		yt_size_fmt((long long)sv.f_bavail * sv.f_frsize, freesz, sizeof(freesz));
	}
	if (n > 0) {
		yt_size_fmt(total_b, sz, sizeof(sz));
		if (freesz[0])
			snprintf(sum, sizeof(sum), "%d saved" YT_SEP "%s" YT_SEP "%s free",
				 n, sz, freesz);
		else
			snprintf(sum, sizeof(sum), "%d saved" YT_SEP "%s", n, sz);
	} else if (freesz[0]) {
		snprintf(sum, sizeof(sum), "No saved items" YT_SEP "%s free", freesz);
	} else {
		snprintf(sum, sizeof(sum), "No saved items");
	}
	if (yo.summary)
		label_set_if_changed(yo.summary, sum);

	if (ydl.pid > 0) {
		yo.dl_card = yt_dl_card_make(yo.body, &yo.dl_lbl, &yo.dl_bar);
		obj_track(&yo.dl_card);
		obj_track(&yo.dl_lbl);
		obj_track(&yo.dl_bar);
	}

	for (int i = 0; i < n; i++) {
		lv_obj_t *row, *img;

		if (yt_off_filter == 1 && !items[i].has_video)
			continue;
		if (yt_off_filter == 2 && items[i].has_video)
			continue;
		row = yt_off_row(yo.body, &items[i], &img);
		yo.rows[yo.nrows] = row;
		obj_track(&yo.rows[yo.nrows++]);
		yt_thumbs_want(yo.body, row, img, items[i].v.id);
		shown++;
	}

	if (!shown && ydl.pid <= 0) {
		const char *icon = n == 0 ? LV_SYMBOL_DOWNLOAD :
				   yt_off_filter == 1 ? LV_SYMBOL_VIDEO : LV_SYMBOL_AUDIO;
		const char *head = n == 0 ? "Nothing saved offline" :
				   yt_off_filter == 1 ? "No saved videos" :
				   "No audio-only downloads";
		const char *msg = n == 0 ? "Open any video and tap Save to watch or listen without Wi-Fi." :
				  yt_off_filter == 1 ? "Download a video in 240p or 360p to watch offline." :
				  "Tap Save audio on a video to listen offline.";

		yo.rows[0] = yt_state(yo.body, icon, head, msg, NULL, NULL);
		obj_track(&yo.rows[yo.nrows++]);
	}
	bp_changed(yo.scr);
	yt_thumbs_fetch();
}

static void yt_offline_shown(lv_event_t *e)
{
	LV_UNUSED(e);
	if (yt_mtime(YT_OFFLINE_IDX) != yo.mtime || (ydl.pid > 0) != (yo.dl_bar != NULL))
		yt_offline_refresh();
}

static void yt_offline_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	if (yo.del_timer)
		lv_timer_delete(yo.del_timer);
	memset(&yo, 0, sizeof(yo));
}

static void yt_open_offline_cb(lv_event_t *e);

static void yt_open_offline(void)
{
	if (yo.scr)
		return;
	nav_mark(yt_open_offline_cb, NULL);
	yo.body = m3_page("Offline", &yo.scr);
	obj_track(&yo.scr);
	obj_track(&yo.body);
	lv_obj_add_event_cb(yo.scr, yt_offline_deleted, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(yo.scr, yt_offline_shown, LV_EVENT_SCREEN_LOADED, NULL);

	yo.summary = m3_label(yo.body, "", &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&yo.summary);
	lv_obj_set_width(yo.summary, lv_pct(100));
	lv_obj_set_style_pad_left(yo.summary, 8, 0);

	yt_off_seg(yo.body);
	yt_offline_refresh();
	push(yo.scr);
}

static void yt_open_offline_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	yt_open_offline();
}

/* ------------------------------------------------------------ home */

static struct {
	lv_obj_t *scr, *body, *chips, *hist_clear_lbl, *chips_clear_lbl;
	lv_obj_t *off_card, *off_sub;
	long chips_mtime;
	long off_mtime;
	bool off_dl_shown;
	lv_obj_t *hist[YT_HIST_SHOWN + 1];	/* its section title and rows, on the body */
	int nhist;
	long hist_mtime;
	bool hist_confirm, chips_confirm;
	lv_timer_t *confirm_timer;
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
	{
		char t[32];

		snprintf(t, sizeof(t), "%s", text);
		if (strlen(text) >= sizeof(t))
			strcpy(t + sizeof(t) - 4, "...");
		l = m3_label(b, t, &lv_font_montserrat_14, M.on_surface_v);
	}
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

static void yt_home_chips(void);
static void yt_home_history(void);

static void yt_home_confirm_reset(lv_timer_t *t)
{
	LV_UNUSED(t);
	yh.confirm_timer = NULL;
	yh.hist_confirm = false;
	yh.chips_confirm = false;
	if (yh.hist_clear_lbl) {
		lv_obj_set_style_text_color(yh.hist_clear_lbl, M.primary, 0);
		label_set_if_changed(yh.hist_clear_lbl, "Clear");
	}
	if (yh.chips_clear_lbl) {
		lv_obj_set_style_text_color(yh.chips_clear_lbl, M.primary, 0);
		label_set_if_changed(yh.chips_clear_lbl, "Clear");
	}
	bp_changed(yh.scr);
}

static void yt_home_confirm_arm(void)
{
	if (yh.confirm_timer) {
		lv_timer_reset(yh.confirm_timer);
	} else {
		yh.confirm_timer = lv_timer_create(yt_home_confirm_reset, 4000, NULL);
		lv_timer_set_repeat_count(yh.confirm_timer, 1);
	}
}

static void yt_clear_searches_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!yh.chips_confirm) {
		yh.chips_confirm = true;
		if (yh.chips_clear_lbl) {
			lv_obj_set_style_text_color(yh.chips_clear_lbl, M.error, 0);
			label_set_if_changed(yh.chips_clear_lbl, "Clear all?");
		}
		yt_home_confirm_arm();
		bp_changed(yh.scr);
		return;
	}
	yh.chips_confirm = false;
	unlink(YT_DIR "/searches");
	yt_home_chips();
}

static void yt_clear_hist_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!yh.hist_confirm) {
		yh.hist_confirm = true;
		if (yh.hist_clear_lbl) {
			lv_obj_set_style_text_color(yh.hist_clear_lbl, M.error, 0);
			label_set_if_changed(yh.hist_clear_lbl, "Clear all?");
		}
		yt_home_confirm_arm();
		bp_changed(yh.scr);
		return;
	}
	yh.hist_confirm = false;
	unlink(YT_DIR "/history");
	yt_home_history();
}

/* Recent searches as chips, under the search bar (rebuilt when they change). */
static void yt_home_chips(void)
{
	FILE *f;
	char line[160];
	lv_obj_t *wrap = NULL;
	int n = 0;

	if (yh.chips)
		lv_obj_delete(yh.chips);
	yh.chips = NULL;
	yh.chips_confirm = false;
	yh.chips_clear_lbl = NULL;
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
			lv_obj_set_flex_flow(yh.chips, LV_FLEX_FLOW_COLUMN);
			lv_obj_set_style_pad_row(yh.chips, 6, 0);
			lv_obj_remove_flag(yh.chips, LV_OBJ_FLAG_SCROLLABLE);
			lv_obj_move_to_index(yh.chips, yh.off_card ? 2 : 1);
			obj_track(&yh.chips);
			yt_section_clear(yh.chips, "Recent searches", yt_clear_searches_cb,
					 &yh.chips_clear_lbl);
			obj_track(&yh.chips_clear_lbl);
			wrap = lv_obj_create(yh.chips);
			plain(wrap);
			lv_obj_remove_style_all(wrap);
			lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW_WRAP);
			lv_obj_set_style_pad_column(wrap, 8, 0);
			lv_obj_set_style_pad_row(wrap, 8, 0);
			lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
		}
		lv_obj_add_event_cb(yt_chip(wrap, ud, yt_chip_cb, ud), yt_free_ud,
				    LV_EVENT_DELETE, ud);
		n++;
	}
	if (f)
		fclose(f);
	bp_changed(yh.scr);
}

static void yt_dl_ui_sync(void)
{
	char buf[96];

	if (ydl.pid <= 0)
		return;
	yt_dl_fmt_prog(buf, sizeof(buf));
	if (yv.scr && yv.dl_lbl) {
		if (!strcmp(yv.v.id, ydl.v.id)) {
			label_set_if_changed(yv.dl_lbl, buf);
			if (yv.dl_bar)
				lv_bar_set_value(yv.dl_bar, ydl.pct, LV_ANIM_OFF);
		} else {
			char obuf[64];

			snprintf(obuf, sizeof(obuf), "Another download is running (%d%%)...",
				 ydl.pct);
			label_set_if_changed(yv.dl_lbl, obuf);
		}
		bp_changed(yv.scr);
	}
	if (yo.scr && yo.dl_lbl) {
		label_set_if_changed(yo.dl_lbl, buf);
		if (yo.dl_bar)
			lv_bar_set_value(yo.dl_bar, ydl.pct, LV_ANIM_OFF);
		bp_changed(yo.scr);
	}
	if (yh.scr && yh.off_sub) {
		label_set_if_changed(yh.off_sub, buf);
		bp_changed(yh.scr);
	}
}

static void yt_home_offline(void)
{
	FILE *f;
	char line[512], sub[96], cnt[48], sz[24];
	struct yt_off tmp;
	long long total_b = 0;
	int n_vid = 0, n_aud = 0;

	if (!yh.body)
		return;
	if (yh.off_card)
		lv_obj_delete(yh.off_card);
	yh.off_card = NULL;
	yh.off_sub = NULL;
	yh.off_mtime = yt_mtime(YT_OFFLINE_IDX);
	yh.off_dl_shown = ydl.pid > 0;

	f = fopen(YT_OFFLINE_IDX, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (!yt_offline_parse(line, &tmp))
			continue;
		if (tmp.has_video)
			n_vid++;
		else
			n_aud++;
		total_b += tmp.bytes;
	}
	if (f)
		fclose(f);

	if (n_vid + n_aud == 0 && ydl.pid <= 0 && wifi_up) {
		bp_changed(yh.scr);
		return;
	}

	if (ydl.pid > 0) {
		yt_dl_fmt_prog(sub, sizeof(sub));
	} else if (n_vid + n_aud > 0) {
		yt_size_fmt(total_b, sz, sizeof(sz));
		if (n_vid && n_aud)
			snprintf(cnt, sizeof(cnt), "%d video%s, %d audio",
				 n_vid, n_vid == 1 ? "" : "s", n_aud);
		else if (n_vid)
			snprintf(cnt, sizeof(cnt), "%d video%s", n_vid, n_vid == 1 ? "" : "s");
		else
			snprintf(cnt, sizeof(cnt), "%d audio", n_aud);
		snprintf(sub, sizeof(sub), "%s%s" YT_SEP "%s",
			 !wifi_up ? "No Wi-Fi" YT_SEP : "", cnt, sz);
	} else {
		snprintf(sub, sizeof(sub), "No Wi-Fi" YT_SEP "Nothing saved yet");
	}

	yh.off_card = m3_card(yh.body, M.surface_c);
	lv_obj_set_style_pad_all(yh.off_card, 4, 0);
	lv_obj_move_to_index(yh.off_card, 1);
	obj_track(&yh.off_card);
	m3_row_ex(yh.off_card, LV_SYMBOL_DOWNLOAD, M.primary_c, M.on_primary_c,
		  "Offline", sub, yt_open_offline_cb, NULL, &yh.off_sub, true);
	obj_track(&yh.off_sub);
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

	if (!yh.body)
		return;
	for (int i = 0; i < yh.nhist; i++)
		if (yh.hist[i])
			lv_obj_delete(yh.hist[i]);	/* (its tracker clears the slot) */
	yh.nhist = 0;
	yh.hist_confirm = false;
	yh.hist_clear_lbl = NULL;
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
			yh.hist[yh.nhist] = yt_section_clear(yh.body, "Watched",
							     yt_clear_hist_cb, &yh.hist_clear_lbl);
			obj_track(&yh.hist_clear_lbl);
			obj_track(&yh.hist[yh.nhist++]);
		}
		row = yt_row(yh.body, &v, &img);
		yh.hist[yh.nhist] = row;
		obj_track(&yh.hist[yh.nhist++]);
		yt_thumbs_want(yh.body, row, img, v.id);
		n++;
	}
	if (f)
		fclose(f);
	if (!yh.nhist && !yh.off_card) {
		yh.hist[0] = yt_state(yh.body, LV_SYMBOL_VIDEO, "Find something to watch",
				      "Search above. Videos play in 360p with sound on the "
				      "speaker, earphones or Bluetooth.", NULL, NULL);
		obj_track(&yh.hist[yh.nhist++]);
	}
	bp_changed(yh.scr);
	yt_thumbs_fetch();
}

static void yt_home_shown(lv_event_t *e)
{
	LV_UNUSED(e);
	/* searched, downloaded, or watched while away: those parts again */
	if (yt_mtime(YT_DIR "/searches") != yh.chips_mtime)
		yt_home_chips();
	if (yt_mtime(YT_OFFLINE_IDX) != yh.off_mtime || (ydl.pid > 0) != yh.off_dl_shown)
		yt_home_offline();
	if (yt_hist_mtime() != yh.hist_mtime)
		yt_home_history();
}

static void yt_home_deleted(lv_event_t *e)
{
	LV_UNUSED(e);
	if (yh.confirm_timer)
		lv_timer_delete(yh.confirm_timer);
	memset(&yh, 0, sizeof(yh));
}

static void open_youtube(lv_event_t *e)
{
	lv_obj_t *bar, *top, *btn;

	LV_UNUSED(e);
	if (yh.scr)
		return;
	nav_mark(open_youtube, NULL);	/* reopened when the screen turns */
	yt_conf_load();
	yh.body = m3_page("YouTube", &yh.scr);
	obj_track(&yh.scr);
	obj_track(&yh.body);
	lv_obj_add_event_cb(yh.scr, yt_home_deleted, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(yh.scr, yt_home_shown, LV_EVENT_SCREEN_LOADED, NULL);

	top = lv_obj_get_child(yh.scr, 0);
	btn = lv_button_create(top);
	plain(btn);
	lv_obj_set_size(btn, 48, 48);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(btn, M.on_surface, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(btn, yt_open_offline_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(m3_label(btn, LV_SYMBOL_DOWNLOAD, &lv_font_montserrat_20, M.on_surface));

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

	yt_home_offline();
	yt_home_chips();
	yt_home_history();
	push(yh.scr);
}
