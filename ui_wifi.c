// SPDX-License-Identifier: MIT
/* n72-ui's user interface (see ui_m3.c): Wi-Fi. */
#include "ui_m3_priv.h"

/* ------------------------------------------------------------- Wi-Fi */

static lv_obj_t *wifi_page, *wifi_list, *wifi_state_lbl, *wifi_scan_btn_lbl;

void wifi_page_refresh(void);

static void wifi_cmd_done(const char *out, void *ud)
{
	LV_UNUSED(out);
	LV_UNUSED(ud);
	wifi_page_refresh();
}

static void wifi_join(const char *ssid, const char *psk)
{
	char q_ssid[160], q_psk[200], cmd[700];

	/* wpa_cli wants the SSID and passphrase in double quotes */
	snprintf(cmd, sizeof(cmd), "\"%s\"", ssid);
	shq(cmd, q_ssid, sizeof(q_ssid));
	if (psk) {
		snprintf(cmd, sizeof(cmd), "\"%s\"", psk);
		shq(cmd, q_psk, sizeof(q_psk));
	}
	snprintf(cmd, sizeof(cmd),
		 "n=$(wpa_cli -i wlan0 add_network | tail -1) && "
		 "wpa_cli -i wlan0 set_network $n ssid %s && "
		 "%s%s%s"
		 "wpa_cli -i wlan0 enable_network $n && wpa_cli -i wlan0 select_network $n && "
		 /* select_network disables every other network: all of them back
		  * on (it stays on this one), so each saved one joins by itself
		  * when it is in range, as iOS's do */
		 "sleep 5; wpa_cli -i wlan0 enable_network all; wpa_cli -i wlan0 save_config; sleep 1",
		 q_ssid,
		 psk ? "wpa_cli -i wlan0 set_network $n psk " : "wpa_cli -i wlan0 set_network $n key_mgmt NONE",
		 psk ? q_psk : "", " && ");
	label_set_if_changed(wifi_state_lbl, "Connecting...");
	job_run(cmd, wifi_page, wifi_cmd_done, NULL);
}

static lv_obj_t *pw_ta, *pw_scr;
static char pw_ssid[64];

static void pw_done_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_READY) {
		const char *pw = lv_textarea_get_text(pw_ta);

		if (strlen(pw) >= 8)
			wifi_join(pw_ssid, pw);
		go_back();
	} else if (code == LV_EVENT_CANCEL) {
		go_back();
	}
}

static void wifi_ask_password(const char *ssid)
{
	nav_mark(NULL, NULL);	/* (not reopened by a turn) */
	lv_obj_t *body;

	snprintf(pw_ssid, sizeof(pw_ssid), "%s", ssid);
	body = m3_page(ssid, &pw_scr);
	obj_track(&pw_scr);
	m3_label(body, "Password (at least 8 characters)", &lv_font_montserrat_14, M.on_surface_v);
	pw_ta = lv_textarea_create(body);
	obj_track(&pw_ta);
	lv_textarea_set_one_line(pw_ta, true);
	lv_textarea_set_password_mode(pw_ta, true);
	lv_obj_set_width(pw_ta, lv_pct(100));
	lv_obj_set_style_radius(pw_ta, 12, 0);
	lv_obj_set_style_anim_duration(pw_ta, 0, LV_PART_CURSOR);	/* (see the YouTube field) */
	lv_obj_add_event_cb(pw_ta, pw_done_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(pw_ta, pw_done_cb, LV_EVENT_CANCEL, NULL);
	push(pw_scr);
	kbd_attach(pw_ta, NULL, true);	/* the system keyboard, up */
}

struct wifi_net { char ssid[64]; int signal; bool secure; bool saved; int id; };
static struct wifi_net nets[24];
static int n_nets;

static void wifi_net_cb(lv_event_t *e)
{
	struct wifi_net *n = lv_event_get_user_data(e);
	char cmd[160];

	if (n->saved && n->id >= 0) {
		/* (then every saved network on again: see wifi_join) */
		snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 select_network %d; sleep 5; "
			 "wpa_cli -i wlan0 enable_network all; wpa_cli -i wlan0 save_config", n->id);
		label_set_if_changed(wifi_state_lbl, "Connecting...");
		job_run(cmd, wifi_page, wifi_cmd_done, NULL);
	} else if (n->secure) {
		wifi_ask_password(n->ssid);
	} else {
		wifi_join(n->ssid, NULL);
	}
}

static void wifi_forget_cb(lv_event_t *e)
{
	struct wifi_net *n = lv_event_get_user_data(e);
	char cmd[96];

	if (!n->saved || n->id < 0)
		return;
	snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 remove_network %d && wpa_cli -i wlan0 save_config", n->id);
	job_run(cmd, wifi_page, wifi_cmd_done, NULL);
}

/* saved networks first (the list is cut at 24), then by signal */
static int net_cmp(const void *a, const void *b)
{
	const struct wifi_net *x = a, *y = b;

	if (x->saved != y->saved)
		return y->saved - x->saved;
	return y->signal - x->signal;
}

void wifi_page_refresh(void);

/* what the list shows now (names, signal bands, flags), "" after a (re)open */
static char wifi_last_sig[24 * 72];

/* a list update put off while it scrolled */
static void wifi_relist_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	wifi_page_refresh();
}

/* scan_results lines: bssid freq signal flags ssid; list_networks: id ssid bssid flags */
static void wifi_lists_done(const char *out, void *ud)
{
	const char *p = out, *saved = strstr(out, "@@SAVED@@");
	int i;

	LV_UNUSED(ud);
	n_nets = 0;
	while (p && *p && (!saved || p < saved) && n_nets < (int)(sizeof(nets) / sizeof(nets[0]))) {
		char bssid[24], flags[128], ssid[64] = "";
		int freq, sig;

		if (sscanf(p, "%23s %d %d %127s %63[^\n]", bssid, &freq, &sig, flags, ssid) >= 4 &&
		    strchr(bssid, ':') && ssid[0]) {
			bool dup = false;

			for (i = 0; i < n_nets; i++)
				if (!strcmp(nets[i].ssid, ssid)) {
					if (sig > nets[i].signal)
						nets[i].signal = sig;
					dup = true;
				}
			if (!dup) {
				struct wifi_net *n = &nets[n_nets++];

				snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
				n->signal = sig;
				n->secure = strstr(flags, "WPA") || strstr(flags, "WEP");
				n->saved = false;
				n->id = -1;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	for (p = saved; p && *p; ) {
		int id;
		char ssid[64];

		if (sscanf(p, "%d\t%63[^\t\n]", &id, ssid) == 2) {
			for (i = 0; i < n_nets; i++)
				if (!strcmp(nets[i].ssid, ssid)) {
					nets[i].saved = true;
					nets[i].id = id;
				}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	qsort(nets, n_nets, sizeof(nets[0]), net_cmp);

	if (!wifi_list || !lv_obj_is_valid(wifi_list))
		return;
	/*
	 * Rebuilding 24 rows takes ~80 ms: not while the list is in use
	 * (scrolling, or touched in the last 1.5 s: try again shortly), and
	 * not at all when nothing shown has changed. The first fill is at once.
	 */
	{
		char sig[sizeof(wifi_last_sig)];
		size_t o = 0;

		if ((sc.owner && lv_obj_get_screen(sc.owner) == lv_obj_get_screen(wifi_list)) ||
		    (wifi_last_sig[0] && lv_display_get_inactive_time(NULL) < 1500)) {
			lv_timer_t *t = lv_timer_create(wifi_relist_cb, 600, NULL);

			lv_timer_set_repeat_count(t, 1);
			return;
		}
		sig[0] = 0;
		for (i = 0; i < n_nets && i < 24 && o < sizeof(sig) - 80; i++)
			o += snprintf(sig + o, sizeof(sig) - o, "%s|%d%d%d%d;", nets[i].ssid,
				      nets[i].signal > -55 ? 3 : nets[i].signal > -67 ? 2 :
				      nets[i].signal > -78 ? 1 : 0, nets[i].secure, nets[i].saved,
				      !strcmp(nets[i].ssid, wifi_ssid) && wifi_up);
		if (!strcmp(sig, wifi_last_sig) && n_nets) {
			if (wifi_scan_btn_lbl && lv_obj_is_valid(wifi_scan_btn_lbl))
				lv_label_set_text(wifi_scan_btn_lbl, "Scan");
			return;
		}
		memcpy(wifi_last_sig, sig, sizeof(sig));
	}
	lv_obj_clean(wifi_list);
	if (!n_nets)
		m3_label(wifi_list, "No networks found yet", &lv_font_montserrat_14, M.on_surface_v);
	/* the network in use first */
	for (i = 1; i < n_nets && wifi_up; i++)
		if (!strcmp(nets[i].ssid, wifi_ssid)) {
			struct wifi_net t = nets[i];

			memmove(&nets[1], &nets[0], i * sizeof(nets[0]));
			nets[0] = t;
			break;
		}
	/* the strongest 24 (sorted): more is only slower to build and scroll */
	for (i = 0; i < n_nets && i < 24; i++) {
		struct wifi_net *n = &nets[i];
		char sub[48];
		const char *bars = n->signal > -55 ? "Excellent" : n->signal > -67 ? "Good" :
				   n->signal > -78 ? "Fair" : "Weak";
		lv_obj_t *r;

		bool cur = !strcmp(n->ssid, wifi_ssid) && wifi_up;

		if (cur)
			snprintf(sub, sizeof(sub), "Connected, %s", bars);
		else
			snprintf(sub, sizeof(sub), "%s%s%s", bars, n->secure ? ", secured" : ", open",
				 n->saved ? ", saved" : "");
		r = m3_row(wifi_list, LV_SYMBOL_WIFI, n->ssid, sub, wifi_net_cb, n, NULL);
		if (cur)
			m3_row_current(r);
		if (n->saved)
			lv_obj_add_event_cb(r, wifi_forget_cb, LV_EVENT_LONG_PRESSED, n);
	}
	if (wifi_scan_btn_lbl && lv_obj_is_valid(wifi_scan_btn_lbl))
		lv_label_set_text(wifi_scan_btn_lbl, "Scan");
}

static void wifi_state_done(const char *out, void *ud)
{
	char ip[32], st[32];

	wifi_status_done(out, ud);
	if (!wifi_state_lbl || !lv_obj_is_valid(wifi_state_lbl))
		return;
	kv_get(out, "wpa_state", st, sizeof(st));
	ip_of("wlan0", ip, sizeof(ip));
	if (wifi_up) {
		char buf[128];

		snprintf(buf, sizeof(buf), "%s\n%s", wifi_ssid, ip);
		label_set_if_changed(wifi_state_lbl, buf);
	} else {
		label_set_if_changed(wifi_state_lbl, !wifi_shown_on() ? "Off" :
				     st[0] ? "Not connected" : "Wi-Fi is not running");
	}
}

void wifi_page_refresh(void)
{
	if (!wifi_page || !lv_obj_is_valid(wifi_page))
		return;
	job_run("wpa_cli -i wlan0 status", wifi_page, wifi_state_done, NULL);
	job_run("wpa_cli -i wlan0 scan_results; echo @@SAVED@@; wpa_cli -i wlan0 list_networks",
		wifi_page, wifi_lists_done, NULL);
}

static void wifi_scan_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (wifi_scan_btn_lbl)
		lv_label_set_text(wifi_scan_btn_lbl, "Scanning...");
	job_run("wpa_cli -i wlan0 scan; sleep 4", wifi_page, wifi_cmd_done, NULL);
}

static void wifi_switch_cb(lv_event_t *e)
{
	wifi_set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

void open_wifi(lv_event_t *e)
{
	nav_mark(open_wifi, NULL);
	lv_obj_t *body, *c, *b;

	LV_UNUSED(e);
	if (!have_tool("wpa_cli")) {
		body = m3_page("Wi-Fi", &wifi_page);
		obj_track(&wifi_page);
		m3_note(m3_card(body, M.surface_c), "wpa_supplicant is not installed.");
		push(wifi_page);
		return;
	}
	body = m3_page("Wi-Fi", &wifi_page);
	obj_track(&wifi_page);
	/* the switch on its own card ... */
	c = m3_card(body, M.primary_c);
	lv_obj_set_height(c, 64);
	lv_obj_align(m3_label(c, "Use Wi-Fi", &lv_font_montserrat_16, M.on_primary_c),
		     LV_ALIGN_LEFT_MID, 0, 0);
	if (have_tool("n72-wifi")) {
		wifi_switch = ios_switch(lv_switch_create(c));
		obj_track(&wifi_switch);
		lv_obj_align(wifi_switch, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_add_event_cb(wifi_switch, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
		switch_show(wifi_switch, wifi_shown_on());
	}
	/* ... and the connection below it */
	c = m3_card(body, M.surface_c);
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 2, 0);
	m3_label(c, "Connection", &lv_font_montserrat_14, M.on_surface_v);
	wifi_state_lbl = m3_label(c, "...", &lv_font_montserrat_16, M.on_surface);
	obj_track(&wifi_state_lbl);
	lv_obj_set_width(wifi_state_lbl, lv_pct(100));
	lv_label_set_long_mode(wifi_state_lbl, LV_LABEL_LONG_WRAP);

	m3_section(body, "Networks");
	wifi_last_sig[0] = 0;
	wifi_list = lv_obj_create(body);
	obj_track(&wifi_list);
	plain(wifi_list);
	lv_obj_set_width(wifi_list, lv_pct(100));
	lv_obj_set_height(wifi_list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(wifi_list, M.surface_c, 0);
	lv_obj_set_style_radius(wifi_list, 20, 0);
	lv_obj_set_style_pad_all(wifi_list, 6, 0);
	lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);
	m3_label(wifi_list, "Looking for networks...", &lv_font_montserrat_14, M.on_surface_v);
	b = m3_button(body, "Scan", false, wifi_scan_cb, NULL);
	wifi_scan_btn_lbl = lv_obj_get_child(b, 0);
	obj_track(&wifi_scan_btn_lbl);
	m3_note(body, "Long-press a saved network to forget it.");
	push(wifi_page);
	job_run("wpa_cli -i wlan0 scan; sleep 3", wifi_page, wifi_cmd_done, NULL);
	wifi_page_refresh();
}
