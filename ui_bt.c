// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): Bluetooth, and a Bluetooth device. */
#include "ui_m3_priv.h"

/* ---------------------------------------------------------- Bluetooth */

lv_obj_t *bt_page, *bt_list, *bt_switch, *bt_scan_lbl, *bt_state_lbl;

void bt_page_refresh(void);

static void bt_cmd_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bt_page_refresh();
}

struct bt_dev { char mac[18]; char name[64]; bool paired, connected; };
static struct bt_dev bdevs[16];
static int n_bdevs;
static char bt_list_sig[16 * 24];	/* what the list shows: rebuilt only on a change */

/*
 * Connect automatically (a device's page): the paired devices the iPod
 * reaches for itself (bt_auto_tick), on unless turned off. Kept on the
 * flash: /var/lib/n72/bt-auto, a line "MAC off" for each one turned off.
 */
#define BT_AUTO_PATH "/var/lib/n72/bt-auto"

/*
 * Automatic and manual connects take turns: two pages at once make the
 * controller refuse the second ("br-connection-busy", then BlueZ's
 * "br-connection-unknown"). The automatic run (bt_auto_tick) holds
 * BT_AUTO_RUN while it pages and gives up between devices as soon as
 * BT_MANUAL_RUN appears; a manual connect sets BT_MANUAL_RUN and waits for
 * the device being paged to finish (8 s at most) before it starts.
 */
#define BT_MANUAL(cmd) "touch " BT_MANUAL_RUN "; i=0; while [ -e " BT_AUTO_RUN " ] && " \
	"[ $i -lt 40 ]; do sleep 0.25; i=$((i+1)); done; " cmd "; r=$?; rm -f " BT_MANUAL_RUN "; exit $r"

/*
 * Devices the user disconnected: not reconnected automatically until they
 * connect again (by a tap, or by themselves) or Bluetooth is switched off.
 */
#define BT_USER_OFF_MAX 8
static char bt_user_off[BT_USER_OFF_MAX][18];

static bool bt_user_off_has(const char *mac)
{
	for (int i = 0; i < BT_USER_OFF_MAX; i++)
		if (bt_user_off[i][0] && !strcasecmp(bt_user_off[i], mac))
			return true;
	return false;
}

static void bt_user_off_set(const char *mac, bool off)
{
	int free_i = -1;

	for (int i = 0; i < BT_USER_OFF_MAX; i++) {
		if (bt_user_off[i][0] && !strcasecmp(bt_user_off[i], mac)) {
			if (!off)
				bt_user_off[i][0] = 0;
			return;
		}
		if (!bt_user_off[i][0] && free_i < 0)
			free_i = i;
	}
	if (off)
		snprintf(bt_user_off[free_i >= 0 ? free_i : 0], 18, "%s", mac);
}

static bool bt_auto_get(const char *mac)
{
	char line[64];
	bool on = true;
	FILE *f = fopen(BT_AUTO_PATH, "r");

	while (f && fgets(line, sizeof(line), f))
		if (!strncasecmp(line, mac, 17) && strstr(line, " off"))
			on = false;
	if (f)
		fclose(f);
	return on;
}

static void bt_auto_set(const char *mac, bool on)
{
	char line[64], buf[2048] = "";
	size_t o = 0;
	FILE *f = fopen(BT_AUTO_PATH, "r");

	while (f && fgets(line, sizeof(line), f))
		if (strncasecmp(line, mac, 17) && o + strlen(line) < sizeof(buf)) {
			memcpy(buf + o, line, strlen(line));
			o += strlen(line);
		}
	if (f)
		fclose(f);
	buf[o] = 0;
	mkdir("/var/lib/n72", 0755);
	f = fopen(BT_AUTO_PATH ".new", "w");
	if (!f)
		return;
	fputs(buf, f);
	if (!on)
		fprintf(f, "%s off\n", mac);
	fclose(f);
	rename(BT_AUTO_PATH ".new", BT_AUTO_PATH);
}

static void open_bt_dev_cb(lv_event_t *e);

static void bt_dev_cb(lv_event_t *e)
{
	struct bt_dev *d = lv_event_get_user_data(e);
	char cmd[400];

	/*
	 * Pairing needs an agent: bluetoothctl's own, without input or output
	 * (headphones and speakers pair "just works"; one asking for a PIN
	 * fails here). Audio devices connect only when bluez-alsa (bluealsa)
	 * runs: it is what offers BlueZ the A2DP profile.
	 */
	bt_user_off_set(d->mac, d->connected);
	if (d->connected)
		snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s", d->mac);
	else if (d->paired)
		snprintf(cmd, sizeof(cmd), BT_MANUAL("bluetoothctl --timeout 20 connect %s"), d->mac);
	else
		snprintf(cmd, sizeof(cmd), BT_MANUAL("bluetoothctl --agent NoInputNoOutput --timeout 25 "
			 "pair %s && bluetoothctl trust %s && bluetoothctl --timeout 20 connect %s"),
			 d->mac, d->mac, d->mac);
	{
		/* the row says what is going on (its second line) */
		lv_obj_t *col = lv_obj_get_child(lv_event_get_current_target(e), 1);
		lv_obj_t *sub = col ? lv_obj_get_child(col, 1) : NULL;

		if (sub) {
			lv_label_set_text(sub, d->connected ? "Disconnecting..." : "Connecting...");
			lv_obj_remove_flag(sub, LV_OBJ_FLAG_HIDDEN);
		}
	}
	job_run(cmd, bt_page, bt_cmd_done, NULL);
}

static void bt_forget_cb(lv_event_t *e)
{
	struct bt_dev *d = lv_event_get_user_data(e);
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "bluetoothctl remove %s", d->mac);
	job_run(cmd, bt_page, bt_cmd_done, NULL);
}

/* "Device AA:BB:CC:DD:EE:FF Name"; the paired list follows @@PAIRED@@ */
static void bt_devs_done(const char *out, void *ud)
{
	const char *p, *paired = strstr(out, "@@PAIRED@@"), *conn = strstr(out, "@@CONN@@");
	int i;

	LV_UNUSED(ud);
	n_bdevs = 0;
	for (p = out; p && *p && n_bdevs < (int)(sizeof(bdevs) / sizeof(bdevs[0])); ) {
		char mac[18], name[64];

		if (sscanf(p, "Device %17s %63[^\n]", mac, name) == 2) {
			bool is_conn = conn && p > conn;
			bool is_paired = paired && p > paired && !is_conn;
			bool dup = false;

			for (i = 0; i < n_bdevs; i++)
				if (!strcmp(bdevs[i].mac, mac)) {
					bdevs[i].paired |= is_paired;
					bdevs[i].connected |= is_conn;
					dup = true;
				}
			if (!dup && !is_conn) {
				snprintf(bdevs[n_bdevs].mac, 18, "%s", mac);
				snprintf(bdevs[n_bdevs].name, 64, "%s", name);
				bdevs[n_bdevs].paired = is_paired;
				bdevs[n_bdevs].connected = false;
				n_bdevs++;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!bt_list || !lv_obj_is_valid(bt_list))
		return;
	/* the connected device first */
	for (i = 1; i < n_bdevs; i++)
		if (bdevs[i].connected) {
			struct bt_dev t = bdevs[i];

			memmove(&bdevs[1], &bdevs[0], i * sizeof(bdevs[0]));
			bdevs[0] = t;
			break;
		}
	{
		/* (refreshed every few seconds: rebuilt only when it changed) */
		char sig[sizeof(bt_list_sig)];
		size_t o = 0;

		sig[0] = 0;
		for (i = 0; i < n_bdevs && o < sizeof(sig) - 24; i++)
			o += snprintf(sig + o, sizeof(sig) - o, "%.17s%d%d%d;", bdevs[i].mac,
				      bdevs[i].paired, bdevs[i].connected, (int)strlen(bdevs[i].name));
		if (o < sizeof(sig) - 2)
			snprintf(sig + o, sizeof(sig) - o, "%d", bt_powered);
		if (!strcmp(sig, bt_list_sig) && lv_obj_get_child_count(bt_list)) {
			if (bt_scan_lbl && lv_obj_is_valid(bt_scan_lbl))
				lv_label_set_text(bt_scan_lbl, "Scan");
			return;
		}
		memcpy(bt_list_sig, sig, sizeof(sig));
	}
	lv_obj_clean(bt_list);
	if (!n_bdevs)
		m3_label(bt_list, bt_powered ? "No devices yet: tap Scan" : "Bluetooth is off",
			 &lv_font_montserrat_14, M.on_surface_v);
	for (i = 0; i < n_bdevs; i++) {
		lv_obj_t *r = m3_row_ex(bt_list, LV_SYMBOL_BLUETOOTH, M.secondary_c, M.on_secondary_c,
					bdevs[i].name, bdevs[i].connected ? "Connected" :
					bdevs[i].paired ? "Paired" : "Tap to pair",
					bt_dev_cb, &bdevs[i], NULL, false);

		if (bdevs[i].paired || bdevs[i].connected) {
			/* the device's own page: a gear at the end of the row */
			lv_obj_t *g = lv_button_create(r);

			plain(g);
			lv_obj_set_size(g, 40, 40);
			lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
			lv_obj_set_style_bg_color(g, M.on_surface, LV_STATE_PRESSED);
			lv_obj_set_style_bg_opa(g, LV_OPA_10, LV_STATE_PRESSED);
			lv_obj_set_style_shadow_width(g, 0, 0);
			lv_obj_center(m3_label(g, LV_SYMBOL_SETTINGS, &lv_font_montserrat_16,
					       M.on_surface_v));
			lv_obj_add_event_cb(g, open_bt_dev_cb, LV_EVENT_CLICKED, bdevs[i].mac);
		}
		if (bdevs[i].connected)
			m3_row_current(r);
		if (bdevs[i].paired || bdevs[i].connected)
			lv_obj_add_event_cb(r, bt_forget_cb, LV_EVENT_LONG_PRESSED, &bdevs[i]);
	}
	if (bt_scan_lbl && lv_obj_is_valid(bt_scan_lbl))
		lv_label_set_text(bt_scan_lbl, "Scan");
}

static void bt_show_page_done(const char *out, void *ud)
{
	bt_show_done(out, ud);
	net_ui_sync();
}

void bt_page_refresh(void)
{
	if (!bt_page || !lv_obj_is_valid(bt_page))
		return;
	job_run("bluetoothctl show", bt_page, bt_show_page_done, NULL);
	job_run("bluetoothctl devices; echo @@PAIRED@@; bluetoothctl devices Paired; "
		"echo @@CONN@@; bluetoothctl devices Connected", bt_page, bt_devs_done, NULL);
}

/*
 * Off unbinds the controller's serial device as well: hci_bcm then holds
 * the BCM4325's Bluetooth side in shutdown, and the UART's clock gates
 * close (a powered-off but bound controller keeps the UART clocked). On
 * binds it again (firmware setup, ~3 s) and powers it once BlueZ has it.
 */
#define BT_ON_CMD	"[ -e " BT_SERDEV "/serial0-0 ] || " \
			"echo -n serial0-0 > " BT_SERDEV "/bind; n=0; " \
			"until bluetoothctl show 2>/dev/null | grep -q Powered || [ $n -ge 20 ]; " \
			"do sleep 0.5; n=$((n+1)); done; bluetoothctl power on"

/* the switch is done when BlueZ says so: its answer ends the switch */
static void bt_set_shown(const char *out, void *ud)
{
	bt_show_done(out, ud);
	bt_want = -1;
	net_ui_sync();
	bt_page_refresh();
}

void bt_set_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	job_run("bluetoothctl show", NULL, bt_set_shown, NULL);
}

/* Bluetooth on or off from any switch; a second one waits for the first. */
void bt_set(bool on)
{
	if (bt_want >= 0) {
		net_ui_sync();
		return;
	}
	if (conf.bt != on) {
		conf.bt = on;
		conf_save();
	}
	if (on == bt_powered)
		return;
	bt_want = on;
	net_ui_sync();
	job_run(on ? BT_ON_CMD : BT_OFF_CMD, NULL, bt_set_done, NULL);
}

static void bt_switch_cb(lv_event_t *e)
{
	bt_set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void bt_scan_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (bt_scan_lbl)
		lv_label_set_text(bt_scan_lbl, "Scanning...");
	job_run("bluetoothctl --timeout 8 scan on", bt_page, bt_cmd_done, NULL);
}

void open_bt(lv_event_t *e)
{
	nav_mark(open_bt, NULL);
	lv_obj_t *body, *c, *b;

	LV_UNUSED(e);
	bz_ensure();
	body = m3_page("Bluetooth", &bt_page);
	obj_track(&bt_page);
	if (!have_tool("bluetoothctl")) {
		c = m3_card(body, M.surface_c);
		m3_note(c, "BlueZ (bluetoothctl) is not installed.");
		push(bt_page);
		return;
	}
	/* the switch alone (as Wi-Fi's); the device in use is in the list */
	c = m3_card(body, M.primary_c);
	lv_obj_set_height(c, 64);
	lv_obj_align(m3_label(c, "Use Bluetooth", &lv_font_montserrat_16, M.on_primary_c),
		     LV_ALIGN_LEFT_MID, 0, 0);
	bt_switch = ios_switch(lv_switch_create(c));
	obj_track(&bt_switch);
	lv_obj_align(bt_switch, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(bt_switch, bt_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
	net_ui_sync();

	m3_section(body, "Devices");
	bt_list = lv_obj_create(body);
	obj_track(&bt_list);
	plain(bt_list);
	lv_obj_set_width(bt_list, lv_pct(100));
	lv_obj_set_height(bt_list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(bt_list, M.surface_c, 0);
	lv_obj_set_style_radius(bt_list, 20, 0);
	lv_obj_set_style_pad_all(bt_list, 6, 0);
	lv_obj_set_flex_flow(bt_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(bt_list, LV_OBJ_FLAG_SCROLLABLE);
	b = m3_button(body, "Scan", false, bt_scan_cb, NULL);
	bt_scan_lbl = lv_obj_get_child(b, 0);
	obj_track(&bt_scan_lbl);
	m3_note(body, "Tap a device to connect; its gear for details.");
	bt_list_sig[0] = 0;
	push(bt_page);
	bt_page_refresh();
	/* (live: BlueZ's signals read it again, bz_ev) */
}

/* --------------------------------------------------- a Bluetooth device */

/*
 * A device's page: name, status, address, the sound codec while it plays
 * to it; Connect or Disconnect, Connect automatically, Forget. Live while
 * shown (bluetoothctl info every 2 s).
 */
static char btd_mac[18];
lv_obj_t *btd_page, *btd_card, *btd_status, *btd_codec_row, *btd_codec,
		*btd_conn_lbl, *btd_auto_sw;
static bool btd_connected;

static void btd_show(bool connected, const char *codec)
{
	btd_connected = connected;
	if (btd_status)
		label_set_if_changed(btd_status, connected ? "Connected" : "Not connected");
	if (btd_conn_lbl)
		label_set_if_changed(btd_conn_lbl, connected ? "Disconnect" : "Connect");
	if (btd_card)
		lv_obj_set_style_bg_color(btd_card, connected ? M.primary_c : M.surface_c, 0);
	if (btd_codec_row) {
		if (connected && codec && *codec) {
			label_set_if_changed(btd_codec, codec);
			lv_obj_remove_flag(btd_codec_row, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(btd_codec_row, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

static void btd_info_done(const char *out, void *ud)
{
	const char *c = strstr(out, "Selected codec:");
	char codec[32] = "";

	LV_UNUSED(ud);
	if (c && sscanf(c, "Selected codec: %31[^\n]", codec) != 1)
		codec[0] = 0;
	btd_show(strstr(out, "Connected: yes") != NULL, codec);
}

void btd_refresh(void)
{
	char cmd[300], und[18];
	int i;

	if (!btd_page || !lv_obj_is_valid(btd_page))
		return;
	for (i = 0; i < 17; i++)
		und[i] = btd_mac[i] == ':' ? '_' : btd_mac[i];
	und[17] = 0;
	snprintf(cmd, sizeof(cmd), "bluetoothctl info %s; p=$(bluealsa-cli list-pcms | "
		 "grep 'dev_%s/a2dpsrc/sink$' | head -1); [ -n \"$p\" ] && bluealsa-cli info \"$p\"",
		 btd_mac, und);
	job_run(cmd, btd_page, btd_info_done, NULL);
}

static void btd_cmd_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	btd_refresh();
}

static void btd_conn_cb(lv_event_t *e)
{
	char cmd[256];

	LV_UNUSED(e);
	bt_user_off_set(btd_mac, btd_connected);
	snprintf(cmd, sizeof(cmd), btd_connected ? "bluetoothctl disconnect %s" :
		 BT_MANUAL("bluetoothctl --timeout 20 connect %s"), btd_mac);
	if (btd_status)
		lv_label_set_text(btd_status, btd_connected ? "Disconnecting..." : "Connecting...");
	job_run(cmd, btd_page, btd_cmd_done, NULL);
}

static void btd_auto_cb(lv_event_t *e)
{
	bt_auto_set(btd_mac, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void btd_forget_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bt_auto_set(btd_mac, true);	/* (no note of a device that is gone) */
	if (btd_page && lv_obj_is_valid(btd_page) && lv_screen_active() == btd_page)
		go_back();
}

static void btd_forget_cb(lv_event_t *e)
{
	char cmd[64];

	LV_UNUSED(e);
	snprintf(cmd, sizeof(cmd), "bluetoothctl remove %s", btd_mac);
	job_run(cmd, NULL, btd_forget_done, NULL);
}

static void open_bt_dev(lv_event_t *e)
{
	nav_mark(open_bt_dev, NULL);	/* (reopened on a turn: btd_mac stays) */
	lv_obj_t *body, *c, *col, *row, *b;
	const char *name = btd_mac;
	bool connected = false;
	int i;

	LV_UNUSED(e);
	for (i = 0; i < n_bdevs; i++)
		if (!strcmp(bdevs[i].mac, btd_mac)) {
			name = bdevs[i].name;
			connected = bdevs[i].connected;
		}
	body = m3_page("Device", &btd_page);	/* (the card has the name) */
	obj_track(&btd_page);

	/* the device: badge, name, status, address */
	c = btd_card = m3_card(body, connected ? M.primary_c : M.surface_c);
	obj_track(&btd_card);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 16, 0);
	m3_badge(c, LV_SYMBOL_BLUETOOTH, M.primary, M.on_primary, 56);
	col = lv_obj_create(c);
	plain(col);
	lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(col, 0, 0);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	b = m3_label(col, name, &lv_font_montserrat_20, M.on_surface);
	lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
	lv_obj_set_width(b, lv_pct(100));
	btd_status = m3_label(col, connected ? "Connected" : "Not connected",
			      &lv_font_montserrat_14, M.on_surface_v);
	obj_track(&btd_status);
	m3_label(col, btd_mac, &lv_font_montserrat_12, M.on_surface_v);

	/* settings */
	c = m3_card(body, M.surface_c);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(c, 4, 0);
	row = m3_row_ex(c, LV_SYMBOL_REFRESH, M.secondary_c, M.on_secondary_c,
			"Auto-connect", "When it is in reach", NULL, NULL, NULL, false);
	btd_auto_sw = ios_switch(lv_switch_create(row));
	obj_track(&btd_auto_sw);
	if (bt_auto_get(btd_mac))
		lv_obj_add_state(btd_auto_sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(btd_auto_sw, btd_auto_cb, LV_EVENT_VALUE_CHANGED, NULL);
	btd_codec_row = m3_row_ex(c, LV_SYMBOL_AUDIO, M.tertiary_c, M.on_tertiary_c,
				  "Sound", "", NULL, NULL, &btd_codec, false);
	obj_track(&btd_codec_row);
	obj_track(&btd_codec);
	lv_obj_add_flag(btd_codec_row, LV_OBJ_FLAG_HIDDEN);

	/* actions */
	row = lv_obj_create(body);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 12, 0);
	lv_obj_set_style_pad_top(row, 4, 0);
	b = m3_button(row, connected ? "Disconnect" : "Connect", true, btd_conn_cb, NULL);
	btd_conn_lbl = lv_obj_get_child(b, 0);
	obj_track(&btd_conn_lbl);
	m3_button(row, "Forget", false, btd_forget_cb, NULL);

	btd_connected = connected;
	push(btd_page);
	btd_refresh();		/* (then on BlueZ's signals, bz_ev) */
}

static void open_bt_dev_cb(lv_event_t *e)
{
	snprintf(btd_mac, sizeof(btd_mac), "%s", (const char *)lv_event_get_user_data(e));
	open_bt_dev(NULL);
}

/*
 * Connect automatically: while the screen is on, Bluetooth is on and no
 * sound device is connected, the paired devices left on are tried in turn
 * every 30 s (a speaker switched on is found within that; one that is off
 * costs a page of the radio, ~5 s). n72-audio-route has the sound follow.
 */
static bool bt_auto_busy;
/*
 * Not a fixed 30 s: after each try that finds nothing the next waits twice
 * as long (30 s to 5 min: a page of a device that is off costs the radio
 * ~5 s); the screen waking, Bluetooth switched on and a sound device going
 * start again from 2 s and 30 s (bt_auto_kick).
 */
#define BT_AUTO_MAX_MS	300000
lv_timer_t *bt_auto_timer;
static uint32_t bt_auto_gap = BT_AUTO_MIN_MS;

void bt_auto_kick(void)
{
	bt_auto_gap = BT_AUTO_MIN_MS;
	if (bt_auto_timer) {
		lv_timer_set_period(bt_auto_timer, 2000);
		lv_timer_reset(bt_auto_timer);
	}
}

static void bt_auto_try_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	bt_auto_busy = false;
	if (bt_page && lv_obj_is_valid(bt_page))
		bt_page_refresh();
}

static void bt_auto_paired_done(const char *out, void *ud)
{
	char cmd[1024], mac[18];
	const char *p;
	size_t o;
	bool connected = false;
	int n = 0;

	LV_UNUSED(ud);
	o = (size_t)snprintf(cmd, sizeof(cmd), "touch " BT_AUTO_RUN "; ");
	for (p = out; p && *p; ) {
		if (sscanf(p, "C Device %17s", mac) == 1) {
			/* connected, by a tap or by itself: automatic again later */
			bt_user_off_set(mac, false);
			connected = true;
		} else if (sscanf(p, "Device %17s", mac) == 1 && bt_auto_get(mac) &&
			   !bt_user_off_has(mac) && o < sizeof(cmd) - 160) {
			/* (each device: not if a manual connect is waiting) */
			o += snprintf(cmd + o, sizeof(cmd) - o, "%s{ [ -e " BT_MANUAL_RUN " ] || "
				      "bluetoothctl --timeout 8 connect %s | grep -q 'Connection successful'; }",
				      n++ ? " || " : "", mac);
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (connected)
		bt_auto_gap = BT_AUTO_MIN_MS;
	if (connected || !n) {
		bt_auto_busy = false;
		return;
	}
	snprintf(cmd + o, sizeof(cmd) - o, "; rm -f " BT_AUTO_RUN);
	if (job_run(cmd, NULL, bt_auto_try_done, NULL))
		bt_auto_busy = false;
}

void bt_auto_tick(lv_timer_t *t)
{
	if (t->period != bt_auto_gap)
		lv_timer_set_period(t, bt_auto_gap);
	if (!bt_powered)
		memset(bt_user_off, 0, sizeof(bt_user_off));	/* (off and on: a fresh start) */
	if (app_pid > 0 || app_dormant || bt_auto_busy || screen_off || locked || !bt_powered ||
	    bt_want >= 0 || !access("/run/n72-audio", F_OK) || !access(BT_MANUAL_RUN, F_OK) ||
	    !have_tool("bluetoothctl"))
		return;
	bt_auto_busy = true;
	if (getenv("N72_STATUS_LOG"))
		fprintf(stderr, "n72-ui: connect automatically: a try (next in %u s)\n",
			(unsigned)bt_auto_gap / 1000);
	bt_auto_gap = LV_MIN(bt_auto_gap * 2, BT_AUTO_MAX_MS);	/* (a find sets it back) */
	/* paired, and not connected to anything else of ours ("C " lines: connected) */
	if (job_run("c=$(bluetoothctl devices Connected | grep Device); "
		    "if [ -n \"$c\" ]; then echo \"$c\" | sed 's/^/C /'; else bluetoothctl devices Paired; fi",
		    NULL, bt_auto_paired_done, NULL))
		bt_auto_busy = false;
}
