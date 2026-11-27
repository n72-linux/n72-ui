# n72-ui

A touch launcher for Linux on the iPod touch 2G (N72AP), built on
[LVGL](https://lvgl.io) 9.2.2 with its Linux framebuffer and evdev drivers.

- Home screen with a clock and weather card, pages of tiles with a dock,
  quick settings shade, lock screen and on-screen keyboard. Built in:
  Settings (Wi-Fi, Bluetooth, USB, Display, Wallpaper, Sound, Power, Date & Time,
  Weather, Storage, About, Logs), Paint (finger drawing), Files, YouTube and
  Games (RetroArch). Other programs add a tile with a manifest
  (`/usr/share/n72-ui/apps/NAME.app`).
![Home screen](screenshot-home.png)

- Buttons & sleep: Home returns to the launcher (in
  a game it opens the pause panel), Power turns the screen off or puts the
  system to sleep and wakes it, the volume keys change the sound volume.
  While connected to a USB host (`SDP` in `/sys/class/power_supply/usb/usb_type`),
  Power and the idle timeout only turn off and lock the screen without suspending,
  keeping USB SSH and MTP live (unplugging while dark puts the iPod to sleep);
  on battery or a wall charger (`DCP`) it enters `s2idle`/deep sleep as usual.

## Gestures and animations (frame rates measured on the device)

| What | How it is drawn | fps |
| --- | --- | --- |
| Quick settings follow the finger on home (open / close) | pictures of home and the shade moved by the GPU; home and scrim drawn only below the shade | 53-61 |
| ... over an application | the shade's picture moved by the CPU (alpha); the dim is added once it settles | 61-69 |
| App opens: a card in the tile's colour grows from the icon, then clears to show the application | home picture + rounded GPU fill; then one translucent GPU fill | 57-58 |
| App closes (Home or its own exit): the card comes up, goes into the icon | one translucent GPU fill; then home picture + rounded GPU fill | 58-61 |
| A page from its tile (Settings, Paint): the same card, over pictures of the page and of home | as above; the page is built once the card covers the screen (one frame of ~65 ms) | 56 open, 59 close |
| Lock screen swipe up (home or over an application) | the lock screen's picture follows the finger, a scrim fades | ~63 |

- The card moves like a spring, not along a timed curve (`za_path_spring`,
  `za_path_glide` in `ui_home.c`). Opening: critically damped, 380 ms, from
  rest, fastest after ~55 ms, 90 % of the way by ~210 ms, then a soft
  landing; the symbol dissolves on the way, the corners (20 px at the icon,
  26 px half way) square off towards the screen's, and the card clears in
  180 ms as soon as it has landed (no hold; the application has as long to
  start as before). Closing is shorter: the card comes up in 100 ms and
  goes in 280 ms, starting gently (no speed in the first frame, fastest
  after ~60 ms), and melts into home's icon at the end. A frame costs what
  it did: the same picture and fill, smaller corners, the symbol for less
  of the way.
- These rates need the display's vblank: when a burst in the `N72_FPS` log
  shows `vblank wait` near 48 ms a frame, the kernel's frame interrupt is
  not coming and every flip waits for its 20 Hz fallback beat (~22 fps,
  whatever is drawn).
- The touch router (`main.c`) offers each launcher touch to `ui_drag()`; a
  vertical drag past 12 px where the shade (or the lock screen) can move is
  taken from LVGL, which loses the press without a click. The release
  settles by position and speed.
- The lock screen goes up whenever the screen goes off; its clock is
  Montserrat SemiBold at 88 px through LVGL's tiny_ttf (`clock_font.h`, SIL
  Open Font License 1.1, from LVGL's demos).
- Manifests (`name=`, `exec=`) may set `icon=` (a symbol name), `color=#rrggbb`
  and `image=` (a .n72i picture) for the tile.
- `N72_FPS=1` logs every burst of frames and the animation phases.
- `N72_TRACE=1` (with `N72_FPS=1`) logs, for every frame that came more than
  20 ms after the one before, what the main loop did in between (timers,
  input, layout, drawing, flush) and the GPU's share.

## GPU

When the MBX render node exists (`/dev/dri/renderD*`, the PowerVR MBX Lite driver
`apple-mbx` in the n72 kernel), n72-ui
draws through the GPU's 2D engine (`mbx_gpu.c` the pool and the GPU, `mbx_draw.c` the
draw unit, `mbx_kms.c` the display and KMS):

- every LVGL draw buffer comes from one GPU-mapped, CPU-cached pool;
- large plain fills (no radius, no gradient) run on the GPU: opaque ones on
  the 2D engine, translucent ones on the 3D core;
- opacity layers are composited by the 3D core;
- the display renders into a full-screen pool buffer and the GPU copies the
  refreshed areas to the framebuffer.

`N72_DEBUG=1` builds include `--bench` and GPU/frame timing stats (`N72_GPU=0` forces the CPU path under debug builds).

Translucent plain fills are blended by the GPU's 3D core, and opacity layers
(`opa_layered`) are composited by it as textured copies (exact two-pass
blending below full opacity). GPU and CPU frames agree to within 2 per colour
channel.

| Scene (full redraw) | CPU | GPU |
| --- | --- | --- |
| Home screen | 35.0 ms | 24.1 ms |
| 24 plain rectangles | 12.8 ms | 9.9 ms |
| + translucent panels and a 70% opacity layer | 34.5 ms | 28.6 ms |

## Responsiveness

The main loop sleeps in `poll()` on the touch screen and buttons, hands touch
input to LVGL as soon as it arrives and renders the result straight away;
LVGL's own timers run at 16 ms. With the kernel's fast touch reads (zephyr2
at 4.5 MHz, ~0.2 ms per frame) touch reports arrive at ~60 Hz instead of ~42.

## Build

LVGL goes in `../lvgl-src/lvgl`: v9.2.2 with our patches on top (the working
tree's is v9.2.2 plus eight commits marked `(n72)`). The GPU part links
`../n72-gles` (`build/libn72mbxmem.a`; `build-clang/` for clang). From the workspace root, on the Mac (the cross gcc of
`n72-alpine/toolchain`, GCC 16.2, see its README, into `build/n72-ui/`;
`N72_CC=clang` for clang, the build `pgo/n72ui.profdata` applies to, into
`build/n72-ui-clang/`):

    sh n72-ui/build.sh
    N72_DEBUG=1 sh n72-ui/build.sh   # with --bench and timing diagnostics (into build/n72-ui-debug/)

The static binary is `n72-ui` in the corresponding build folder (`n72-ui.debug` beside it is the
unstripped copy). `n72-alpine/build.sh` packs it as the `n72-ui` package
(`/usr/bin/n72-ui`) for the Alpine root; the OpenRC service `n72-ui`
(`n72-alpine/pkg/n72-ui/files/etc/init.d/n72-ui`) starts it at boot, restarts
it if it dies, and logs to `/tmp/n72-ui.log`. Add `noui` to the kernel command
line to skip it.

## Environment

| Variable | Meaning |
| --- | --- |
| `N72_FB` | Framebuffer (default `/dev/fb0`) |
| `N72_TOUCH` / `N72_KEYS` | Event devices (default: found by name) |
| `N72_TOUCH_CAL` | `min_x min_y max_x max_y` raw touch range; swap a pair to flip an axis |
| `N72_TOUCH_SWAP` | `1` swaps the touch axes |
| `N72_GPU` | With `N72_DEBUG=1`: `0` draws with the CPU only (default: GPU when MBX render node exists) |
| `N72_LATENCY` | With `N72_DEBUG=1`: `1` logs touch-to-screen latency every 2 s (stderr) |
| `N72_FPS` | With `N72_DEBUG=1`: logs frame bursts and animation phases |
| `N72_TRACE` | With `N72_DEBUG=1` + `N72_FPS=1`: logs main-loop marks before late frames |
| `N72_FADE` | `0`: no backlight fades |
| `N72_IDLE_SLEEP` | Seconds without input before the screen goes off or the system sleeps (`0`: never); overrides the setting |
| `N72_APPS_DIR` | Folder of tile manifests (default `/usr/share/n72-ui/apps`; test hook) |
| `N72_UI_OPEN` | Internal: the page to reopen after a restart from a settings change |
| `N72_STATUS_LOG` | Logs status events (battery, Wi-Fi, clock set) and Bluetooth auto-connect tries |
| `N72_NO_SCROLL_CACHE` / `N72_NO_PREFETCH` | Set: no scroll pictures / prefetch (for measurements) |
| `N72_SC_KEEP` | Set: scroll pictures are kept after the scroll (test) |
| `N72_SLEEP` | `freeze`: never deep sleep; `0`: no system sleep (the screen only blanks) |
| `N72_TOUCH_RAW` | Touch frames as the panel gives them (no holding of a lifting finger) (debug) |
| `N72_ROT` | `90` or `270`: start turned (test hook) |
| `N72_DRAGS` | With `N72_DEBUG=1`: `wait:x0,y0,x1,y1,ms ...` replays drags through a virtual pointer |
| `N72_BENCH_SHOT` | With `N72_DEBUG=1` + `--bench`: leaves the last frame up 2 s for a screenshot |
| `N72_FRAMES` | With `N72_DEBUG=1`: logs every frame (time, interval, render time) |
| `N72_WAKE_LOG` | Logs the wake sources after each deep sleep (debug) |
| `N72_NOFLIP` | With `N72_DEBUG=1`: single draw buffer copied to the framebuffer |
| `N72_NO_TRIM` | With `N72_DEBUG=1`: disables look-ahead background fill trimming |
| `N72_GPU_ADBG` | With `N72_DEBUG=1`: logs every task the GPU draws |
| `N72_ALPHACHECK` | With `N72_DEBUG=1`: reports non-opaque pixels in opaque frames |
| `N72_KMS_HANDOFF` | Internal: set by n72-ui for the n72-ui it execs (theme change) to take over the display |
| `TZ` | Clock time zone (the OpenRC service sets it from `/etc/TZ`) |

## Kernel

Runs on the n72 branch of [n72-linux/linux](https://github.com/n72-linux/linux/tree/n72)
(n72-clcd display (KMS; `/dev/fb0` for the console layer), Zephyr2 touchscreen,
gpio-keys).

## License

MIT (see `LICENSE`). LVGL is MIT-licensed as well.
