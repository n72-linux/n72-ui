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

/* main.c and audio.c */
extern int keys_fd;
uint64_t mono_us(void);
extern bool screen_off;
void screen_set(bool on);
extern int jack_fd;
void ui_watch(int fd, void (*cb)(int fd));
extern lv_timer_t *idle_timer;

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
void idle_timer_sync(void);
extern bool app_dormant;

#ifdef N72_DEBUG
/* bench.c */
int bench_text(int n, bool label_only);
int bench(void);
void n72_trace(const char *what, long v);
#else
#define n72_trace(what, v) do { } while (0)
#endif

#define W ui_w

#define H ui_h

#define NWATCH 32	/* (background commands take two each: ui_m3.c job_watch) */

/* main.c and audio.c, used by the user interface */
extern int32_t ui_w;
extern int32_t ui_h;
extern char *self_argv[4];
extern lv_obj_t *home_scr;
extern lv_indev_t *touch_indev;
extern int tty_fd;
int backlight_get(void);
void backlight_set(int pct);
extern char rt_node[64];
lv_timer_t *awake_timer(lv_timer_cb_t cb, uint32_t ms, void *ud);
void awake_timer_delete(lv_timer_t *t);
extern bool jack_in;
extern bool snd_present;
void snd_apply_volume(int pct);
void snd_apply_output(bool speaker_when_unplugged);
bool jack_poll(void);
void snd_init(int vol_pct, bool speaker_when_unplugged);
extern int nwatch;
void ui_watch_ev(int fd, void (*cb)(int fd), short events);
void ui_unwatch(int fd);
lv_point_t rt_logical(lv_point_t p);

#endif
