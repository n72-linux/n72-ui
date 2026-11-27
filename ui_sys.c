// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): background commands, and the system state. */
#include "ui_m3_priv.h"

/* ------------------------------------------------- background commands */

/*
 * Run a shell command in the background; when it ends, done() gets its
 * output (NUL-terminated, truncated at 8 KiB) unless the object @owner
 * was deleted meanwhile (the page was closed).
 */
struct job {
	pid_t pid;
	int fd;
	int pidfd;		/* -1: watched by the 150 ms timer instead */
	char buf[8192];
	size_t len;
	void (*done)(const char *out, void *ud);
	void *ud;
	lv_obj_t *owner;
};

/*
 * A job ends when its shell does (a command it started in the background
 * may keep the output pipe open): the shell's pidfd turns readable then.
 * The pipe is watched too, read as output comes (a job writing more than
 * the pipe holds would otherwise wait for us). Without a pidfd, or with
 * the main loop's watch table full, a 150 ms timer looks instead.
 */
#define JOB_WATCHED 8
static struct job *job_w[JOB_WATCHED];

/* Read what the job wrote; true at the end of its output (EOF). */
static bool job_drain(struct job *j)
{
	ssize_t n = -1;

	while (j->len < sizeof(j->buf) - 1 &&
	       (n = read(j->fd, j->buf + j->len, sizeof(j->buf) - 1 - j->len)) > 0)
		j->len += n;
	if (j->len >= sizeof(j->buf) - 1) {
		char sink[512];	/* (beyond 8 KiB: dropped, so the job is not held up) */

		while ((n = read(j->fd, sink, sizeof(sink))) > 0)
			;
	}
	return n == 0;
}

static void job_end(struct job *j)
{
	job_drain(j);
	j->buf[j->len] = 0;
	close(j->fd);
	if (j->done && (!j->owner || lv_obj_is_valid(j->owner)))
		j->done(j->buf, j->ud);
	free(j);
}

static struct job *job_by_fd(int fd, int *slot)
{
	for (int i = 0; i < JOB_WATCHED; i++)
		if (job_w[i] && (job_w[i]->fd == fd || job_w[i]->pidfd == fd)) {
			*slot = i;
			return job_w[i];
		}
	return NULL;
}

static void job_ev(int fd)
{
	int slot, st;
	struct job *j = job_by_fd(fd, &slot);

	if (!j) {
		ui_unwatch(fd);
		return;
	}
	if (fd == j->fd) {
		/* at EOF the pipe stays readable: watch only the shell from here */
		if (job_drain(j))
			ui_unwatch(j->fd);
		return;
	}
	/* the shell ended */
	if (waitpid(j->pid, &st, WNOHANG) == 0)
		return;		/* (not yet reaped: the next readiness) */
	ui_unwatch(j->fd);
	ui_unwatch(j->pidfd);
	close(j->pidfd);
	job_w[slot] = NULL;
	job_end(j);
}

static bool job_watch(struct job *j)
{
	int slot = -1;

	for (int i = 0; i < JOB_WATCHED; i++)
		if (!job_w[i]) {
			slot = i;
			break;
		}
	if (slot < 0 || nwatch > NWATCH - 2)
		return false;
	j->pidfd = syscall(SYS_pidfd_open, j->pid, 0);
	if (j->pidfd < 0)
		return false;
	fcntl(j->pidfd, F_SETFD, FD_CLOEXEC);
	job_w[slot] = j;
	ui_watch(j->fd, job_ev);
	ui_watch(j->pidfd, job_ev);
	return true;
}

static void job_poll(lv_timer_t *t)
{
	struct job *j = lv_timer_get_user_data(t);
	int st;

	job_drain(j);
	if (waitpid(j->pid, &st, WNOHANG) == 0)
		return;
	lv_timer_delete(t);
	job_end(j);
}

int job_run(const char *cmd, lv_obj_t *owner,
		   void (*done)(const char *out, void *ud), void *ud);

/* A job asked for during a page slide, started once the slide is over. */
struct job_later {
	char *cmd;
	lv_obj_t *owner;
	void (*done)(const char *out, void *ud);
	void *ud;
};

static void job_later_cb(lv_timer_t *t)
{
	struct job_later *l = lv_timer_get_user_data(t);

	if (tr.scr)
		return;			/* still sliding (a second one): look again */
	lv_timer_delete(t);
	if (!l->owner || lv_obj_is_valid(l->owner))
		job_run(l->cmd, l->owner, l->done, l->ud);
	free(l->cmd);
	free(l);
}

/* 0: started (or to start after the slide); -1: it could not be (@done is not called) */
int job_run(const char *cmd, lv_obj_t *owner,
		   void (*done)(const char *out, void *ud), void *ud)
{
	struct job *j;
	posix_spawn_file_actions_t fa;
	char *argv[] = { "sh", "-c", (char *)cmd, NULL };
	int p[2];

	/*
	 * Not during a slide: on one core the command (bluetoothctl alone is
	 * ~160 ms of CPU) takes the slide's frames. Its result lands a moment
	 * later instead.
	 */
	if (tr.scr) {
		struct job_later *l = calloc(1, sizeof(*l));

		if (l && (l->cmd = strdup(cmd))) {
			l->owner = owner;
			l->done = done;
			l->ud = ud;
			lv_timer_create(job_later_cb, 50, l);
			return 0;
		}
		free(l);
	}
	j = calloc(1, sizeof(*j));
	if (!j || pipe(p)) {
		free(j);
		return -1;
	}
	/*
	 * posix_spawn, not fork: glibc's is a vfork-style clone, so the page
	 * tables of this process (GPU pool, LVGL) are not copied for a
	 * command that only execs a shell.
	 */
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_adddup2(&fa, p[1], 2);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	if (posix_spawn(&j->pid, "/bin/sh", &fa, NULL, argv, environ))
		j->pid = -1;
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (j->pid < 0) {
		close(p[0]);
		free(j);
		return -1;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	j->fd = p[0];
	j->done = done;
	j->ud = ud;
	j->owner = owner;
	j->pidfd = -1;
	if (!job_watch(j))
		lv_timer_create(job_poll, 150, j);
	return 0;
}

/* Shell-quote @s into @out ('...' with ' escaped). */
void shq(const char *s, char *out, size_t len)
{
	size_t o = 0;

	if (len < 3)
		return;
	out[o++] = '\'';
	for (; *s && o + 5 < len; s++) {
		if (*s == '\'') {
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			out[o++] = *s;
		}
	}
	out[o++] = '\'';
	out[o] = 0;
}

/* A program is installed: a path as given, a bare name looked up on PATH
 * (the distribution decides between /sbin, /usr/sbin, /usr/bin, ...). */
bool have_tool(const char *name)
{
	const char *path, *p;
	char buf[256];

	if (strchr(name, '/'))
		return access(name, X_OK) == 0;
	path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	for (p = path; *p; ) {
		size_t n = strcspn(p, ":");
		if (n && n + strlen(name) + 2 <= sizeof(buf)) {
			memcpy(buf, p, n);
			buf[n] = '/';
			strcpy(buf + n + 1, name);
			if (access(buf, X_OK) == 0)
				return true;
		}
		p += n;
		if (*p == ':')
			p++;
	}
	return false;
}

/* -------------------------------------------------------- system state */

int read_sysfs_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int v = def;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

bool usb_powered(void)
{
	return read_sysfs_int("/sys/class/power_supply/usb/online", 0) == 1;
}

bool usb_sdp(void)
{
	char buf[64] = "";
	FILE *f = fopen("/sys/class/power_supply/usb/usb_type", "r");

	if (!f)
		return usb_powered();
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return strstr(buf, "[SDP]") != NULL;
}

/*
 * Set a label's text only when it changes: lv_label_set_text() always
 * invalidates the label, and a 1 s timer rewriting unchanged text was a
 * redraw and a GPU flush every second.
 */
void shade_changed(lv_obj_t *o);

void label_set_if_changed(lv_obj_t *lbl, const char *text)
{
	const char *cur;

	if (!lbl)
		return;
	cur = lv_label_get_text(lbl);

	if (!cur || strcmp(cur, text)) {
		lv_label_set_text(lbl, text);
		shade_changed(lbl);
		bp_changed(lv_obj_get_screen(lbl));
		pf_label_changed(lbl);
	}
}

void ip_of(const char *ifname, char *out, size_t len)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	snprintf(out, len, "-");
	if (fd < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (!ioctl(fd, SIOCGIFADDR, &ifr))
		snprintf(out, len, "%s",
			 inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	close(fd);
}

/* wpa_cli status: connected SSID (or "") from a "key=value" listing */
void kv_get(const char *text, const char *key, char *out, size_t len)
{
	size_t kl = strlen(key);
	const char *p = text;

	out[0] = 0;
	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t n = strcspn(p + kl + 1, "\n");

			if (n >= len)
				n = len - 1;
			memcpy(out, p + kl + 1, n);
			out[n] = 0;
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

char wifi_ssid[64];	/* connected network, "" if none */

/* The radio is on: the card is there and not switched off (n72-wifi). */
bool wifi_powered(void)
{
	return access("/sys/class/net/wlan0", F_OK) == 0 &&
	       access("/var/run/wifi.off", F_OK) != 0;
}
bool wifi_up;
bool bt_powered;
void bt_auto_kick(void);

void net_ui_sync(void);

void wifi_status_done(const char *out, void *ud)
{
	char st[32] = "";

	LV_UNUSED(ud);
	kv_get(out, "wpa_state", st, sizeof(st));
	wifi_up = !strcmp(st, "COMPLETED");
	kv_get(out, "ssid", wifi_ssid, sizeof(wifi_ssid));
	if (!wifi_up)
		wifi_ssid[0] = 0;
	net_ui_sync();
}

void bt_show_done(const char *out, void *ud)
{
	static bool was;

	LV_UNUSED(ud);
	bt_powered = strstr(out, "Powered: yes") != NULL;
	if (bt_powered && !was)
		bt_auto_kick();		/* (switched on: look for the devices now) */
	was = bt_powered;
	net_ui_sync();
}

/*
 * wpa_cli is a small static binary (~10 ms); bluetoothctl starts glib and
 * D-Bus (~160 ms), so Bluetooth is only asked at start-up: after that its
 * state changes only through the Bluetooth page, which reads it back.
 */
void status_refresh(bool with_bt)
{
	if (have_tool("wpa_cli"))
		job_run("wpa_cli -i wlan0 status", NULL, wifi_status_done, NULL);
	if (with_bt && have_tool("bluetoothctl"))
		job_run("bluetoothctl show", NULL, bt_show_done, NULL);
}
