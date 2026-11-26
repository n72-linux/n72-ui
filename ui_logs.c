// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): logs. */
#include "ui_m3_priv.h"

/* --------------------------------------------------------------- logs */

/*
 * The kernel log on the screen, for when there is no USB or Wi-Fi to read
 * it over: the network addresses, then the tail of the ring buffer,
 * refreshed every 2 s and kept scrolled to the end. Volume keys scroll.
 */
#define LOGS_TAIL	6144	/* bytes of log on the page */

/*
 * The log as labels of LOGS_CHUNK lines each: LVGL lays out and draws a
 * label by walking its text from the start, so one 6 KB label cost ~25 ms
 * a frame while scrolling. Chunks off screen are skipped whole. Lines are
 * only ever added to the last chunk and whole chunks dropped from the top,
 * so a new message measures at most LOGS_CHUNK lines again (cutting the
 * text anew at every update moved every chunk's lines: all of them were
 * measured again).
 *
 * New messages come from /dev/kmsg, which the main loop waits on: nothing
 * runs while the kernel is quiet. (Reading the whole ring buffer every
 * 2 s to look for news, as before, took the kernel 31 ms each time with
 * 125 KB in it: a dropped frame or two every 2 s on this page.) The page
 * starts with the end of the ring, which klogctl() returns for a short
 * buffer.
 */
#define LOGS_CHUNK	8	/* small labels: LVGL walks each one from its start */
#define LOGS_MAXCH	96
#define LOGS_LINE_MAX	400	/* bytes of one line shown */

static lv_obj_t *logs_net, *logs_box;
static lv_timer_t *logs_timer;		/* the addresses (and the log, without /dev/kmsg) */
static lv_timer_t *logs_show_t;		/* new lines onto the page, 10 times a second at most */
static int logs_kmsg = -1;
static struct {
	lv_obj_t *label;
	int bytes;
} logs_chunk[LOGS_MAXCH];
static int logs_nchunks, logs_bytes;
static char *logs_open;			/* text of the last chunk, still filling */
static int logs_open_len, logs_open_cap, logs_open_lines;
static bool logs_open_dirty;
static int32_t logs_gone_h;		/* height dropped from the top since the last logs_show() */
static bool logs_at_end = true;		/* the view is at the end of the log */
static char *logs_seen;			/* (without /dev/kmsg) the ring's end as last read */

static void logs_put_open(void)
{
	if (logs_open_dirty && logs_nchunks)
		lv_label_set_text(logs_chunk[logs_nchunks - 1].label, logs_open);
	logs_open_dirty = false;
}

static void logs_drop_first(void)
{
	logs_gone_h += lv_obj_get_height(logs_chunk[0].label);
	lv_obj_delete(logs_chunk[0].label);
	logs_bytes -= logs_chunk[0].bytes;
	logs_nchunks--;
	memmove(&logs_chunk[0], &logs_chunk[1], logs_nchunks * sizeof(logs_chunk[0]));
}

/* One more line at the end (not on the screen until logs_show()). */
static void logs_add_line(const char *p, int len)
{
	if (!logs_box)
		return;
	len = LV_MIN(len, LOGS_LINE_MAX);
	if (!logs_nchunks || logs_open_lines >= LOGS_CHUNK) {
		lv_obj_t *l;

		logs_put_open();
		/* (a flood: no need to build what logs_show() would drop) */
		while (logs_nchunks == LOGS_MAXCH || (logs_bytes > 2 * LOGS_TAIL && logs_nchunks > 1))
			logs_drop_first();
		l = m3_label(logs_box, "", &lv_font_montserrat_12, M.on_surface);
		lv_obj_set_width(l, lv_pct(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		logs_chunk[logs_nchunks].label = l;
		logs_chunk[logs_nchunks].bytes = 0;
		logs_nchunks++;
		logs_open_len = logs_open_lines = 0;
	}
	if (logs_open_len + len + 2 > logs_open_cap) {
		int cap = (logs_open_len + len + 2) * 2;
		char *n = realloc(logs_open, cap);

		if (!n)
			return;
		logs_open = n;
		logs_open_cap = cap;
	}
	if (logs_open_lines)
		logs_open[logs_open_len++] = '\n';
	memcpy(logs_open + logs_open_len, p, len);
	logs_open_len += len;
	logs_open[logs_open_len] = 0;
	logs_open_lines++;
	logs_chunk[logs_nchunks - 1].bytes += len + 1;
	logs_bytes += len + 1;
	logs_open_dirty = true;
}

/*
 * The lines added since the last time onto the page. At the end of the log
 * the view stays at the end; scrolled back, it stays on the lines it shows
 * (also when old chunks leave at the top).
 */
static void logs_show(void)
{
	bool at_end = logs_at_end;	/* (the scroll below asks again, half way) */

	if (!logs_box)
		return;
	logs_put_open();
	while (logs_bytes > LOGS_TAIL && logs_nchunks > 1)
		logs_drop_first();
	lv_obj_update_layout(logs_box);
	if (at_end)
		lv_obj_scroll_to_y(logs_box, LV_COORD_MAX, LV_ANIM_OFF);
	else if (logs_gone_h)
		lv_obj_scroll_to_y(logs_box, LV_MAX(lv_obj_get_scroll_y(logs_box) - logs_gone_h, 0),
				   LV_ANIM_OFF);
	logs_gone_h = 0;
	logs_at_end = at_end;
}

/* Where the reader left the page: at the end of the log (new lines keep it there) or further back. */
static void logs_scrolled_cb(lv_event_t *e)
{
	logs_at_end = lv_obj_get_scroll_bottom(lv_event_get_target(e)) <= 2;
}

static void logs_show_cb(lv_timer_t *t)
{
	logs_show();
	lv_timer_pause(t);	/* until there are new lines (logs_kmsg_ev), or the screen lights */
}

static void logs_clear(void)
{
	while (logs_nchunks)
		logs_drop_first();
	logs_bytes = 0;
	logs_open_len = logs_open_lines = 0;
	logs_open_dirty = false;
	logs_gone_h = 0;
}

/*
 * The end of the kernel's ring buffer, as lines: SYSLOG_ACTION_READ_ALL
 * with a buffer shorter than the log gives the newest whole records that
 * fit. Returns false if it is what @seen holds already (then nothing is
 * added); @seen is brought up to date.
 */
static bool logs_fill(char *seen)
{
	char *raw = malloc(LOGS_TAIL + 1);
	int n, i, j;

	if (!raw)
		return false;
	n72_trace("klog>", 0);
	n = klogctl(3, raw, LOGS_TAIL);		/* SYSLOG_ACTION_READ_ALL */
	n72_trace("klog<", n);
	if (n < 0)
		n = 0;
	raw[n] = 0;
	if (seen) {
		if (!strcmp(seen, raw)) {
			free(raw);
			return false;
		}
		memcpy(seen, raw, n + 1);
		logs_clear();
	}
	for (i = 0; i < n; i = j + 1) {
		for (j = i; j < n && raw[j] != '\n'; j++)
			;
		if (raw[i] == '<') {		/* "<6>": the level */
			int k = i + 1;

			while (k < j && raw[k] >= '0' && raw[k] <= '9')
				k++;
			if (k < j && raw[k] == '>')
				i = k + 1;
		}
		logs_add_line(raw + i, j - i);
	}
	free(raw);
	return true;
}

/*
 * New records on /dev/kmsg, one a read: "level,sequence,microseconds,flags;
 * text\n" and then lines of " KEY=value"; a line break inside the text
 * comes as "\x0a". Shown as dmesg and the lines above show them.
 */
static void logs_kmsg_ev(int fd)
{
	static char rec[8192];		/* (the kernel's largest record) */
	char line[LOGS_LINE_MAX + 32];
	bool any = false;
	int budget;

	for (budget = 256; budget; budget--) {	/* (a flood: back to the main loop in between) */
		ssize_t r = read(fd, rec, sizeof(rec) - 1);
		unsigned long long us = 0;
		char *text, *end, *nl;
		int pre;

		if (r < 0 && (errno == EPIPE || errno == EINVAL))
			continue;	/* records overwritten before we read them, or one too long */
		if (r <= 0)
			break;		/* EAGAIN: that was all */
		rec[r] = 0;
		text = strchr(rec, ';');
		if (!text)
			continue;
		sscanf(rec, "%*u,%*u,%llu", &us);
		text++;
		end = strchr(text, '\n');
		if (end)
			*end = 0;
		pre = snprintf(line, sizeof(line), "[%5llu.%06llu] ", us / 1000000, us % 1000000);
		do {
			int len;

			nl = strstr(text, "\\x0a");
			len = nl ? (int)(nl - text) : (int)strlen(text);
			len = LV_MIN(len, LOGS_LINE_MAX);
			memcpy(line + pre, text, len);
			logs_add_line(line, pre + len);
			text = nl ? nl + 4 : text;
		} while (nl);
		any = true;
	}
	/* dark: the lines wait in the last chunk's text for the screen (logs_show_t is an awake timer) */
	if (any && logs_show_t && !screen_off)
		lv_timer_resume(logs_show_t);
}

static void logs_update(lv_timer_t *t)
{
	char usb[32], wlan[32], buf[96];

	LV_UNUSED(t);
	ip_of("usb0", usb, sizeof(usb));
	ip_of("wlan0", wlan, sizeof(wlan));
	snprintf(buf, sizeof(buf), "usb0 %s    wlan0 %s", usb, wlan);
	label_set_if_changed(logs_net, buf);
	/* no /dev/kmsg: look at the end of the ring (cheap: the kernel formats only that) */
	if (logs_kmsg < 0 && logs_seen && logs_fill(logs_seen))
		logs_show();
}

static void logs_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (logs_timer)
		lv_timer_delete(logs_timer);
	logs_timer = NULL;
	if (logs_show_t)
		lv_timer_pause(logs_show_t);
	if (logs_kmsg >= 0) {
		ui_unwatch(logs_kmsg);
		close(logs_kmsg);
		logs_kmsg = -1;
	}
	free(logs_seen);
	logs_seen = NULL;
	free(logs_open);
	logs_open = NULL;
	logs_open_cap = logs_open_len = logs_open_lines = 0;
	logs_open_dirty = false;
	logs_box = NULL;	/* (the chunk labels go with the page) */
	logs_nchunks = logs_bytes = 0;
	logs_gone_h = 0;
}

void open_logs(lv_event_t *e)
{
	nav_mark(open_logs, NULL);
	lv_obj_t *scr, *body;

	LV_UNUSED(e);
	if (logs_box)		/* already open */
		return;
	body = m3_page("Logs", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	logs_net = m3_label(body, "", &lv_font_montserrat_12, M.primary);
	obj_track(&logs_net);
	lv_obj_set_width(logs_net, lv_pct(100));
	logs_box = lv_obj_create(body);
	obj_track(&logs_box);
	plain(logs_box);
	lv_obj_set_width(logs_box, lv_pct(100));
	lv_obj_set_flex_grow(logs_box, 1);
	lv_obj_set_style_pad_all(logs_box, 10, 0);
	lv_obj_set_style_radius(logs_box, 16, 0);
	lv_obj_set_style_bg_color(logs_box, M.surface_c, 0);
	lv_obj_set_scroll_dir(logs_box, LV_DIR_VER);
	fling_accel(logs_box);
	lv_obj_set_flex_flow(logs_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(logs_box, 0, 0);
	logs_nchunks = logs_bytes = 0;
	/*
	 * No scroll cache here: pictures of text chunks cost as much to take
	 * as they save (measured: 16.6 vs 15.8 ms a frame, same hitches), while
	 * the card pages draw ~30% faster with it.
	 */
	/* from now on (opened before the ring's end is read: a message in between shows once) */
	logs_at_end = true;
	lv_obj_add_event_cb(logs_box, logs_scrolled_cb, LV_EVENT_SCROLL, NULL);
	logs_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (logs_kmsg >= 0)
		lseek(logs_kmsg, 0, SEEK_END);
	else if ((logs_seen = malloc(LOGS_TAIL + 1)))
		logs_seen[0] = 0;
	logs_fill(logs_seen);
	logs_show();
	if (logs_kmsg >= 0)
		ui_watch(logs_kmsg, logs_kmsg_ev);
	if (!logs_show_t) {
		logs_show_t = awake_timer(logs_show_cb, 100, NULL);
		if (logs_show_t)
			lv_timer_pause(logs_show_t);
	}
	logs_update(NULL);
	logs_timer = lv_timer_create(logs_update, 2000, NULL);
	lv_obj_add_event_cb(scr, logs_delete_cb, LV_EVENT_DELETE, NULL);
	push(scr);
}

/* After a restart from a settings change: reopen that page. */
void ui_reopen(void)
{
	const char *p = getenv("N72_UI_OPEN");

	if (!p)
		return;
	/* "display" after a theme change; the others for screenshots */
	if (!strcmp(p, "shade"))
		shade_set(true, false);
	else if (!strcmp(p, "paint"))
		open_paint(NULL);
	else if (!strncmp(p, "files:", 6))	/* (tests: a folder's page) */
		files_open_dir(p + 6);
	else if (!strcmp(p, "games"))		/* (tests: the Games page) */
		open_games(NULL);
	else if (strcmp(p, "logs") && strcmp(p, "about") && strcmp(p, "volume") &&
		 strcmp(p, "keyboard"))
		open_settings(NULL);
	if (!strcmp(p, "display"))
		open_display(NULL);
	else if (!strcmp(p, "wifi"))
		open_wifi(NULL);
	else if (!strcmp(p, "bt"))
		open_bt(NULL);
	else if (!strcmp(p, "sound"))
		open_sound(NULL);
	else if (!strcmp(p, "power"))
		open_power(NULL);
	else if (!strcmp(p, "wallpaper"))
		open_wallpaper(NULL);
	else if (!strcmp(p, "apps"))
		open_apps(NULL);
	else if (!strcmp(p, "about"))
		open_about(NULL);
	else if (!strcmp(p, "logs"))
		open_logs(NULL);
	else if (!strcmp(p, "volume"))	/* (screenshots of the volume pop-up) */
		vol_hud_show();
	else if (!strcmp(p, "keyboard"))	/* (screenshots: the keyboard as an application gets it) */
		kbd_show_for(NULL, NULL);
	unsetenv("N72_UI_OPEN");
}
