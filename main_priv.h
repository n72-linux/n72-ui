// SPDX-License-Identifier: MIT
/* What main.c, sleep.c and bench.c share. */
#ifndef N72_MAIN_PRIV_H
#define N72_MAIN_PRIV_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#ifndef SCHED_IDLE
#define SCHED_IDLE 5	/* (Linux; glibc shows it only with _GNU_SOURCE) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/indev/lv_indev_private.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/misc/lv_timer_private.h"
#include "mbx_gpu.h"

struct n72_conf {
	bool dark;
	int accent;		/* index into accents[] */
	int idle_s;		/* screen idle timeout, 0 = never */
	int vol;		/* sound volume, 0..100 % (0 = mute) */
	bool spk;		/* speaker when no earphones are plugged */
	bool wake_plug;		/* the screen comes on when the cable goes in or out */
	int autorot;		/* Auto-rotate (quick settings): ROT_AUTO or ROT_LOCKED */
	int rotlock;		/* with it off: the turn it was left in, 0, 90 or 270 */
	int bright;		/* brightness %, 0 = as it is */
	int wifi, bt;		/* left on (1) or off (0); -1 not set */
	bool wp_accent;		/* the colours from the home wallpaper (wallpaper.c) */
	char dock[200];		/* the dock's icons, names joined by '|' (dock.c) */
	bool dock_set;		/* (no "dock=" line: the default) */
	char order[1200];	/* the grid's order, names joined by '|' (dock.c) */
	bool order_set;
	char tz[48];		/* the time zone's name (datetime.c; its rule is /etc/TZ) */
	bool tz_set;
	char wx_name[96];	/* the weather's place (weather.c) */
	double wx_lat, wx_lon;
	bool wx_set, wx_f;	/* a place set; Fahrenheit */
};

/* main.c, with what it includes (audio.c, ui_m3.c ...) */
extern int keys_fd;
uint64_t mono_us(void);
extern bool screen_off;
void screen_set(bool on);
extern int jack_fd;
void ui_watch(int fd, void (*cb)(int fd));
extern lv_timer_t *idle_timer;
extern struct n72_conf conf;
extern pid_t app_pid;
extern bool locked;
bool have_tool(const char *name);
bool usb_powered(void);
void status_kick(bool batt, bool net);
bool game_playing(void);
bool video_playing(void);

/* sleep.c */
extern uint64_t woke_us;
void drain(int fd);
bool system_sleep(void);
extern bool screen_off_for_sound;
extern lv_timer_t *playback_timer;
void screen_off_or_sleep(void);
void playback_check(lv_timer_t *t);
void uevent_init(void);
void idle_check(lv_timer_t *t);
void idle_kick(void);

/* bench.c */
int bench_text(int n, bool label_only);
int bench(void);

#endif
