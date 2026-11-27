// SPDX-License-Identifier: MIT
/* The launcher's system sleep: s2idle, deep sleep, the idle timer, wake-up bookkeeping. */
#include "ui_m3_priv.h"

/* ---------------------------------------------------------------- sleep */

uint64_t woke_us;	/* when the last system sleep ended */

void drain(int fd)
{
	struct input_event ev[16];

	if (fd >= 0)
		while (read(fd, ev, sizeof(ev)) > 0)
			;
}

/*
 * Suspend to RAM ("deep" in /sys/power/mem_sleep). Which state "mem" means
 * is chosen there: n72-base leaves it at s2idle, and n72-ui picks deep only
 * for its own deep sleeps (deep_sleep).
 */
#define MEM_SLEEP	"/sys/power/mem_sleep"

static bool deep_offered(void)
{
	char buf[64] = "";
	FILE *f = fopen(MEM_SLEEP, "r");

	if (!f)
		return false;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return strstr(buf, "deep") != NULL;
}

static bool mem_sleep_set(const char *mode)
{
	int fd = open(MEM_SLEEP, O_WRONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = write(fd, mode, strlen(mode)) == (ssize_t)strlen(mode);
	if (!ok)
		fprintf(stderr, "n72-ui: mem_sleep %s: %s\n", mode, strerror(errno));
	close(fd);
	return ok;
}

static int sleep_errno;		/* why the last write_sleep_state failed */

static bool write_sleep_state(const char *state)
{
	int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
	ssize_t r;

	sleep_errno = 0;
	if (fd < 0) {
		sleep_errno = errno;
		return false;
	}
	r = write(fd, state, strlen(state));
	if (r != (ssize_t)strlen(state))
		sleep_errno = errno;
	close(fd);
	if (r != (ssize_t)strlen(state))
		fprintf(stderr, "n72-ui: %s failed: %s\n", state, strerror(sleep_errno));
	return r == (ssize_t)strlen(state);
}

/*
 * What tells a wake that wants the screen from any other: how often the
 * two buttons have interrupted, the cable, and the alarm that was set.
 */
struct wake_marks {
	long wlan, home, power;
	int wlan_irq, home_irq, power_irq;
	bool usb;		/* the cable is in */
	long alarm;		/* the RTC's wake alarm, seconds since 1970, or 0 */
	bool jack;		/* earphones in the jack */
	unsigned pmu_int[3];	/* the PMU's last interrupt (INT1..INT3) ... */
	unsigned long pmu_n;	/* ... and how many it has had (apple-n72-pmu last_irq) */
};

/*
 * (the PMU's last interrupt, logged at each deep wake: N72_WAKE_LOG=1)
 */
#define PMU_LAST_IRQ	"/sys/bus/i2c/devices/0-0073/last_irq"

static void wake_marks(struct wake_marks *m)
{
	char line[256];
	FILE *f;

	memset(m, 0, sizeof(*m));
	m->wlan_irq = m->home_irq = m->power_irq = -1;
	m->usb = usb_powered();
	{	/* (read without changing jack_in: jack_poll notices the change itself) */
		unsigned long sw[1] = { 0 };

		m->jack = jack_fd >= 0 && ioctl(jack_fd, EVIOCGSW(sizeof(sw)), sw) >= 0 &&
			  (sw[0] & (1UL << SW_HEADPHONE_INSERT));
	}
	f = fopen(PMU_LAST_IRQ, "r");
	if (f) {
		if (fscanf(f, "%x %x %x %lu", &m->pmu_int[0], &m->pmu_int[1], &m->pmu_int[2],
			   &m->pmu_n) != 4)
			m->pmu_n = 0;
		fclose(f);
	}
	f = fopen("/sys/class/rtc/rtc0/wakealarm", "r");
	if (f) {
		if (fscanf(f, "%ld", &m->alarm) != 1)
			m->alarm = 0;
		fclose(f);
	}
	/*
	 * An alarm whose time has passed still reads as set. Taken for
	 * somebody else's, a stale test alarm would keep the system out of
	 * deep sleep for good. A past alarm will not ring again: clear it.
	 */
	if (m->alarm && m->alarm < time(NULL))
		m->alarm = 0;
	f = fopen("/proc/interrupts", "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *colon = strchr(line, ':'), *end;
		long n;
		int irq;

		if (!colon)
			continue;
		line[strcspn(line, "\n")] = 0;
		irq = atoi(line);
		n = strtol(colon + 1, &end, 10);
		if (end == colon + 1)
			continue;
		end = line + strlen(line);
		if (end - line > 15 && !strcmp(end - 15, "brcmf_host_wake")) {
			m->wlan = n;
			m->wlan_irq = irq;
		} else if (end - line > 5 && !strcmp(end - 5, " Home")) {
			m->home = n;
			m->home_irq = irq;
		} else if (end - line > 6 && !strcmp(end - 6, " Power")) {
			m->power = n;
			m->power_irq = irq;
		}
	}
	fclose(f);
}

/* The interrupt that ended the last suspend to idle, or -1. */
static int wake_irq(void)
{
	char buf[32] = "";
	FILE *f = fopen("/sys/power/pm_wakeup_irq", "r");

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	return buf[0] ? atoi(buf) : -1;
}

/*
 * Is the screen wanted after this wake? For Power and Home, for the cable
 * going in or out, and for an alarm somebody set (the RTC's wakealarm).
 * Anything else that wakes the system (a frame for this station over
 * Wi-Fi, which stays associated in suspend to idle so that an SSH login
 * gets in; a firmware event) is served in the dark, and the system sleeps
 * on. The signs of a button: the kernel names its interrupt as the one
 * that woke it (suspend to idle), or its interrupt has counted up. A press
 * seen a moment later lights the screen as well (dark_linger): after the
 * deep sleep, where the wake comes through the boot loader, that is the
 * only sign of a button there is.
 */
static bool woke_for_screen(const struct wake_marks *before, bool slept)
{
	struct wake_marks now;
	int irq = slept ? wake_irq() : -1;

	wake_marks(&now);
	if (now.home != before->home || now.power != before->power)
		return true;
	if (irq >= 0 && (irq == now.home_irq || irq == now.power_irq))
		return true;
	if (now.usb != before->usb && conf.wake_plug)	/* Settings > Display */
		return true;
	return before->alarm && time(NULL) >= before->alarm;
}

/*
 * After a suspend to RAM the SoC was off: Power or Home wakes the PMU in
 * hardware, the boot loader brings Linux back, and the SoC's own button
 * interrupts never see that press. The PMU leaves no event for it
 * either. What else can end a deep sleep is known: the PMU's unmasked
 * events (the cable in or out, the headphone jack; live masks f7 bf bb),
 * and an alarm (ours is cleared before a deep sleep). So a deep wake with
 * the cable and the jack as they were and no alarm due was a button.
 * (Wi-Fi is off in deep sleep; its host wake is masked.)
 */
static bool deep_woke_by_button(const struct wake_marks *before)
{
	struct wake_marks now;

	wake_marks(&now);
	if (now.usb != before->usb)
		return false;		/* (woke_for_screen has decided that one) */
	if (before->alarm && time(NULL) >= before->alarm)
		return false;
	if (now.jack != before->jack)
		return false;		/* the headphone jack: the screen stays dark */
	return true;
}

/*
 * Is somebody logged in over SSH, running something? dropbear forks one
 * process per connection; a connection counts while that process has a
 * child (the shell or the command). One left behind with nothing under it
 * does not keep the system awake.
 */
static bool ssh_sessions(void)
{
	struct { int pid, ppid; bool dropbear; } pr[256];
	int n = 0;
	glob_t g;
	bool found = false;

	if (glob("/proc/[0-9]*/stat", 0, NULL, &g))
		return false;
	for (size_t i = 0; i < g.gl_pathc && n < 256; i++) {
		char buf[256] = "", *comm, *end;
		FILE *f = fopen(g.gl_pathv[i], "r");

		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && (comm = strchr(buf, '(')) &&
		    (end = strrchr(buf, ')'))) {
			pr[n].pid = atoi(buf);
			pr[n].dropbear = end - comm == 9 && !strncmp(comm + 1, "dropbear", 8);
			pr[n].ppid = 0;
			sscanf(end + 1, " %*c %d", &pr[n].ppid);
			n++;
		}
		fclose(f);
	}
	globfree(&g);
	for (int i = 0; i < n && !found; i++) {
		bool session = false;

		if (!pr[i].dropbear)
			continue;
		for (int k = 0; k < n; k++)	/* its parent is the listener */
			if (pr[k].pid == pr[i].ppid && pr[k].dropbear)
				session = true;
		for (int k = 0; k < n && session && !found; k++)
			found = pr[k].ppid == pr[i].pid;
	}
	return found;
}

/*
 * Awake but not for a person, screen dark: stay up for a few seconds, and
 * for as long as an SSH session lasts, then sleep on. True: Power or Home
 * was pressed (or is reported now, as the press that woke us): the screen
 * is wanted.
 */
static bool dark_linger(void)
{
	uint64_t until = mono_us() + 4000000, checked = 0;

	for (;;) {
		struct pollfd p = { .fd = keys_fd, .events = POLLIN };
		struct input_event ev;

		poll(&p, 1, 250);
		while (keys_fd >= 0 && read(keys_fd, &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && ev.value == 1 &&
			    (ev.code == KEY_POWER || ev.code == KEY_HOMEPAGE))
				return true;
		if (mono_us() - checked > 1000000) {
			checked = mono_us();
			if (ssh_sessions())
				until = checked + 4000000;
		}
		if (mono_us() > until)
			return false;
	}
}

/*
 * Before a sleep: tell the kernel the wakeup events seen so far
 * (/sys/power/wakeup_count). False when one came in meanwhile (an alarm
 * that rang, a key): the sleep would miss it, so do not sleep now. Without
 * the file, sleep anyway.
 */
static bool wakeup_count_arm(void)
{
	char buf[32];
	ssize_t n;
	int fd = open("/sys/power/wakeup_count", O_RDWR | O_CLOEXEC);
	bool ok = true;

	if (fd < 0)
		return true;
	n = read(fd, buf, sizeof(buf) - 1);	/* (waits while an event is being handled) */
	if (n > 0) {
		buf[n] = 0;
		ok = lseek(fd, 0, SEEK_SET) == 0 && write(fd, buf, strlen(buf)) > 0;
	}
	close(fd);
	return ok;
}

/* the RTC's wake alarm: "+N" seconds from now, or 0 to clear it */
static void wake_alarm(const char *v)
{
	int fd = open("/sys/class/rtc/rtc0/wakealarm", O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, "0", 1) < 0 || (strcmp(v, "0") && write(fd, v, strlen(v)) < 0))
		fprintf(stderr, "n72-ui: wakealarm %s: %s\n", v, strerror(errno));
	close(fd);
}

/* seconds since boot, counting the time asleep */
static time_t boot_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec;
}

static bool audio_playing(void);

/*
 * Suspend to RAM (the SoC powers off) once the system has slept this long
 * in suspend to idle, which wakes quickly and keeps Wi-Fi associated.
 */
#define DEEP_AFTER_S	180
/* (deep_sleep: a refused suspend to RAM backs off) */
#define DEEP_RETRY_MIN_S	180
#define DEEP_RETRY_MAX_S	1800
static time_t deep_retry_at, deep_retry_gap = DEEP_RETRY_MIN_S;

/*
 * May the sleep go deep now? Not on USB power, where the charger's events
 * wake the PMU straight out of hibernate; not while sound plays (that keeps
 * the system up anyway) or an application is open; not with N72_SLEEP=freeze;
 * and only where the kernel offers it.
 */
static bool deep_allowed(void)
{
	const char *e = getenv("N72_SLEEP");

	if ((e && !strcmp(e, "freeze")) || usb_powered() || audio_playing() || app_pid > 0)
		return false;
	if (deep_retry_at && boot_s() < deep_retry_at)
		return false;		/* refused lately: deep_sleep */
	return deep_offered();
}

/*
 * A suspend to RAM the kernel refused (a driver's suspend failed: the
 * attempt costs seconds of the CPU and the radio each time) is not tried
 * again at once: the next one waits 3 min, then twice as long after each
 * further refusal, up to 30 min, meanwhile suspend to idle. A deep sleep
 * that works starts over (avoiding repeated ~9 s wake loops when a driver
 * like hci resume times out).
 */

/* One suspend to RAM: "mem" as deep for this sleep, s2idle again after. */
static bool deep_sleep(void)
{
	bool ok;

	if (!mem_sleep_set("deep"))
		return false;
	ok = write_sleep_state("mem");
	mem_sleep_set("s2idle");
	if (ok) {
		deep_retry_gap = DEEP_RETRY_MIN_S;
	} else if (sleep_errno != EBUSY) {
		deep_retry_at = boot_s() + deep_retry_gap;
		fprintf(stderr, "n72-ui: suspend to RAM refused: suspend to idle, again in %ld s\n",
			(long)deep_retry_gap);
		deep_retry_gap = deep_retry_gap * 2 > DEEP_RETRY_MAX_S ? DEEP_RETRY_MAX_S :
					 deep_retry_gap * 2;
	}
	return ok;
}

/*
 * Sleep until Power or Home is pressed, the cable goes in or out, or a set
 * alarm rings: suspend to idle first, and after DEEP_AFTER_S suspend to RAM
 * when deep_allowed (an RTC alarm of our own wakes the system for the
 * change, and again every DEEP_AFTER_S while deep sleep is not allowed;
 * it is left alone when somebody else has set one). Whatever else wakes
 * the system is served with the screen dark, and the sleep goes on
 * (woke_for_screen, dark_linger). N72_SLEEP=0 turns sleep off,
 * N72_SLEEP=freeze always uses suspend to idle. Returns false when sleep
 * is unavailable or refused, so the caller can fall back to blanking the
 * screen.
 */
static int last_plug = -1;

bool system_sleep(void)
{
	const char *e = getenv("N72_SLEEP");
	time_t start = boot_s();
	bool ok, own = false, keep_dark = false;	/* the alarm set is ours */
	int busy_tries = 0;

	if (e && !strcmp(e, "0"))
		return false;
	for (;;) {
		struct wake_marks before;
		bool deep = boot_s() - start >= DEEP_AFTER_S && deep_allowed();
		bool slept = false, slept_deep = false;

		if (deep && own) {	/* nothing to wake for in deep sleep */
			wake_alarm("0");
			own = false;
		}
		wake_marks(&before);
		if (!deep && !before.alarm) {
			char at[16];
			time_t in = DEEP_AFTER_S - (boot_s() - start);

			if (in <= 0 && deep_retry_at > boot_s())
				in = deep_retry_at - boot_s();	/* (deep held back: wake for the retry) */
			snprintf(at, sizeof(at), "+%ld", (long)(in > 0 ? in : DEEP_AFTER_S));
			wake_alarm(at);
			own = true;
		}
		if (own)
			before.alarm = 0;	/* ours: not a wake for the screen */
		/* (the caller has switched the panel off: screen_set(false)) */
		if (!wakeup_count_arm()) {
			ok = true;	/* an event came first: look at it, as after a wake */
		} else {
			slept_deep = deep && deep_sleep();
			ok = slept_deep;
			if (!ok && !(deep && sleep_errno == EBUSY))
				/* not deep, or the PMU refused: suspend to idle */
				ok = write_sleep_state("freeze");
			/*
			 * Given up at once because a wakeup event came while the
			 * tasks were being frozen (EBUSY: "Freezing user space
			 * processes aborted"): not a refusal, the sleep is wanted
			 * still. Service the wake in the dark like any other wake,
			 * then try again; a run of EBUSY results ends it.
			 */
			if (!ok && sleep_errno == EBUSY && ++busy_tries <= 10) {
				woke_us = mono_us();
				if (woke_for_screen(&before, false)) {
					ok = true;	/* (a wake like any other: the screen goes on) */
					break;
				}
				if (usb_sdp()) {
					ok = true;
					keep_dark = true;
					break;
				}
				if (dark_linger()) {
					ok = true;
					break;
				}
				continue;
			}
			slept = ok;
		}
		woke_us = mono_us();
		if (slept_deep && getenv("N72_WAKE_LOG")) {
			struct wake_marks w;

			wake_marks(&w);
			fprintf(stderr, "n72-ui: deep wake: PMU %02x %02x %02x (%lu, was %lu), Home %ld->%ld, Power %ld->%ld, cable %d->%d\n",
				w.pmu_int[0], w.pmu_int[1], w.pmu_int[2], w.pmu_n, before.pmu_n,
				before.home, w.home, before.power, w.power, before.usb, w.usb);
		}
		if (!ok || woke_for_screen(&before, slept) || (slept_deep && deep_woke_by_button(&before)))
			break;
		if (usb_sdp()) {
			keep_dark = true;
			break;
		}
		if (own) {
			struct wake_marks now;

			wake_marks(&now);
			if (!now.alarm) {	/* ours rang (the RTC disarms it): sleep on */
				own = false;
				continue;
			}
		}
		if (dark_linger())
			break;
	}
	if (own)
		wake_alarm("0");

	/* the wake press and anything touched meanwhile are not input */
	drain(keys_fd);
	if (keep_dark) {
		last_plug = usb_powered();
	} else {
		screen_set(true);
		/* the idle and lock timeouts count from the wake, not from the press
		 * that put it to sleep (which a long sleep has long outlasted) */
		lv_display_trigger_activity(NULL);
	}
	return ok;
}

/*
 * Is the Bluetooth output playing? bluealsa has no kernel sound card, so
 * the cards' status says nothing about it: while n72-audio-route names a
 * Bluetooth output (/run/n72-audio: pcm=), bluealsa is asked whether that
 * PCM is running (bluealsa-cli info: "Running: true"). Without this, a
 * screen turned off during music into earbuds on battery puts the system
 * to sleep and Bluetooth suspend drops the link. Only queried while such
 * a route exists: a few times at a screen-off and every 2 s while the
 * screen is off for sound (playback_check).
 */
static bool bt_audio_playing(void)
{
	char line[192], pcm[160] = "", cmd[224];
	bool on = false;
	FILE *f = fopen("/run/n72-audio", "r");

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "pcm=", 4))
			snprintf(pcm, sizeof(pcm), "%s", line + 4);
	fclose(f);
	pcm[strcspn(pcm, "\n")] = 0;
	if (!pcm[0] || strchr(pcm, '\'') || !have_tool("bluealsa-cli"))
		return false;
	snprintf(cmd, sizeof(cmd), "bluealsa-cli info '%s' 2>/dev/null", pcm);
	f = popen(cmd, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "Running: true", 13))
			on = true;
	pclose(f);
	return on;
}

/* Is sound playing? (any ALSA playback stream running, or Bluetooth's) */
static bool audio_playing(void)
{
	glob_t g;
	bool on = false;

	if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g))
		return bt_audio_playing();
	for (size_t i = 0; i < g.gl_pathc && !on; i++) {
		char buf[64] = "";
		FILE *f = fopen(g.gl_pathv[i], "r");

		if (!f)
			continue;
		on = fgets(buf, sizeof(buf), f) && strstr(buf, "RUNNING");
		fclose(f);
	}
	globfree(&g);
	return on || bt_audio_playing();
}

/*
 * Power, the idle timeout and Settings > Power > Sleep: sleep, or with
 * sound playing only turn the screen off, and sleep once it stops
 * (playback_check). Screen off with the cable out lets the kernel gate
 * the display and USB and idle at the system clock's slowest divider.
 */
bool screen_off_for_sound;
lv_timer_t *playback_timer;	/* runs only while screen_off_for_sound */

void screen_off_or_sleep(void)
{
	/*
	 * The panel off first, then the lock screen drawn into it, then the
	 * system sleep: drawing it lit showed it for the second the sleep
	 * takes, and the kernel's own panel bring-up on wake showed whatever
	 * was in the controller before the layers were back.
	 */
	screen_set(false);
	if (usb_sdp())
		return;
	if (audio_playing() || yt_dl_busy()) {
		screen_off_for_sound = true;
		screen_set(false);
		if (playback_timer)
			lv_timer_resume(playback_timer);
		return;
	}
	if (!system_sleep())
		screen_set(false);
}

void playback_check(lv_timer_t *t)
{
	static int quiet;

	if (!screen_off || !screen_off_for_sound) {
		quiet = 0;
		lv_timer_pause(t);
		return;
	}
	if (audio_playing() || yt_dl_busy()) {
		quiet = 0;
		return;
	}
	/* a gap between tracks is not the end: two checks in a row */
	if (++quiet < 2)
		return;
	quiet = 0;
	screen_off_for_sound = false;
	if (usb_sdp())
		return;
	if (!system_sleep())
		screen_set(false);
}

/* Sleep after a while without input, unless connected to a USB host (SDP). */
/*
 * The screen goes dark (locked): on a USB host only the screen, on battery
 * or a wall charger the system sleeps as well.
 */
static void screen_dark(void)
{
	if (usb_sdp())
		screen_set(false);
	else
		screen_off_or_sleep();
	lv_display_trigger_activity(NULL);
}

#define LOCK_IDLE_MS	8000	/* the lock screen, not unlocked: dark again */

/*
 * USB power in or out. The PMU interrupts on both and the kernel sends a
 * power_supply uevent ("usb"), read here from a netlink socket in the main
 * loop's poll(); only without one does idle_check look every second.
 */
static int uevent_fd = -1;

static void plug_changed(void)
{
	int plug = usb_powered();

	if (last_plug < 0 || plug == last_plug) {
		last_plug = plug;
		return;
	}
	last_plug = plug;
	battery_crit_check();
	if (screen_off && conf.wake_plug) {	/* Settings > Display */
		screen_off_for_sound = false;
		screen_set(true);
		lv_display_trigger_activity(NULL);
		return;
	}
	if (screen_off && !plug && !screen_off_for_sound) {
		screen_off_or_sleep();	/* dark on USB, now on battery: sleep */
		return;
	}
	lv_display_trigger_activity(NULL);
}

static void uevent_ev(int fd)
{
	char buf[2048];
	bool usb = false, psy = false;
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (!strcmp(p, "POWER_SUPPLY_NAME=usb"))
				usb = true;
			if (!strcmp(p, "SUBSYSTEM=power_supply"))
				psy = true;
		}
	}
	if (usb)
		plug_changed();
	if (psy) {
		status_kick(true, false);	/* the battery text: bolt, level */
		battery_crit_check();
	}
}

void uevent_init(void)
{
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			NETLINK_KOBJECT_UEVENT);

	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		fd = -1;
	}
	uevent_fd = fd;
	if (fd >= 0)
		ui_watch(fd, uevent_ev);
	plug_changed();		/* (the state now) */
}

/*
 * The idle timeout: the timer runs when the next deadline is due (the
 * lock screen's or Settings > Display's), not every second; input in the
 * meantime only moves the deadline later, which the run then finds.
 * Paused while the screen is dark (an awake timer).
 */
void idle_check(lv_timer_t *t)
{
	long idle_ms = conf.idle_s * 1000L;	/* Settings > Display */
	uint32_t inactive = lv_display_get_inactive_time(NULL), next = 60000;

	if (uevent_fd < 0) {
		plug_changed();
		next = 1000;
	}
	if (screen_off)
		return;
	if (video_playing() || game_playing()) {	/* a video watched, a game played */
		lv_display_trigger_activity(NULL);
		if (app_dormant) {
			lv_timer_pause(t);
			return;
		}
		lv_timer_set_period(t, LV_MIN(next, 5000));
		return;
	}
	if (locked) {
		if (inactive >= LOCK_IDLE_MS) {
			screen_dark();
			return;
		}
		next = LV_MIN(next, LOCK_IDLE_MS - inactive);
	}
	if (idle_ms) {
		if (inactive >= (uint32_t)idle_ms) {
			screen_dark();
			return;
		}
		next = LV_MIN(next, (uint32_t)idle_ms - inactive);
	}
	lv_timer_set_period(t, LV_MAX(next + 20, 250));
}

/* Settings changed the timeout, or the lock screen went up: look now. */
void idle_kick(void)
{
	if (idle_timer && !screen_off)
		lv_timer_ready(idle_timer);
}

void idle_timer_sync(void)
{
	if (!idle_timer || screen_off)
		return;
	if (app_dormant && (video_playing() || game_playing())) {
		lv_display_trigger_activity(NULL);
		lv_timer_pause(idle_timer);
	} else {
		lv_timer_resume(idle_timer);
		lv_timer_ready(idle_timer);
	}
}
