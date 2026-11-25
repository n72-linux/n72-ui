/*
 * The system keyboard: one on-screen keyboard for the launcher's own text
 * fields and for every application, drawn by the launcher on the display's
 * layer 1 (the top layer, under the shade and the bars) at the bottom of
 * the screen.
 *
 * For an application the keys go out through a uinput keyboard of their
 * own ("n72-ui keyboard", US layout: a character becomes its key, with
 * Shift when it needs it), so anything that reads evdev keyboards gets
 * them. An application asks for the keyboard by writing "show", "hide" or
 * "toggle" (one a line) into the FIFO /run/n72-kbd (its path is in the
 * application's environment as N72_KBD), and /run/n72-kbd.state says
 * whether it is up and how tall ("shown HEIGHT WIDTH" in the screen's
 * pixels as it is turned, or "hidden"), so an application can keep its
 * text clear of it. The keyboard button on the bar pulled down over an
 * application brings it up for any application as well (one that does not
 * ask). For the launcher's text fields (kbd_attach)
 * the keys go straight into the field, and Enter is the field's
 * LV_EVENT_READY.
 *
 * Material 3, as Gboard draws it: rounded keys in a tonal surface, the
 * function keys in the secondary container, Enter in the primary colour.
 * A key commits when the finger lifts, on the key under it then (a finger
 * that rolls onto a neighbour types the neighbour, as on a phone); while
 * down, a bubble above the finger shows what it will type. Shift: once
 * for one capital, twice quickly for capitals. The top row's letters held
 * give their digit (hinted in the corner). Backspace and the arrows
 * repeat while held. A drag along the space bar moves the cursor. The
 * toolbar over the keys has Ctrl (for the next key), Esc, Tab and the
 * arrows for applications (terminals), the arrows for the launcher's
 * fields, and the key that puts the keyboard away.
 *
 * One object draws every key (no object per key): a press redraws two
 * keys and the bubble, nothing else.
 */

#include <linux/uinput.h>
#include <linux/kd.h>

#define KBD_FIFO	"/run/n72-kbd"
#define KBD_STATE	"/run/n72-kbd.state"

enum kbd_kind {
	KK_CHAR, KK_SHIFT, KK_BKSP, KK_ENTER, KK_SPACE, KK_PAGE,
	KK_LEFT, KK_RIGHT, KK_HIDE, KK_CTRL, KK_ESC, KK_TAB,
};

struct kbd_key {
	const char *lo, *up;	/* label and character(s); up: with Shift */
	uint8_t kind, w;	/* w: tenths of a row (10 = one letter key) */
	char hint;		/* held: this digit (top row) */
	uint8_t page;		/* KK_PAGE: to this page */
};

#define KROW_END { NULL, NULL, 0, 0, 0, 0 }
#define K(c, C, h) { c, C, KK_CHAR, 10, h, 0 }
#define S(c) { c, c, KK_CHAR, 10, 0, 0 }

/* Pages: 4 rows each, a row's widths add up to 100 (a lead-in where less). */
static const struct kbd_key kbd_letters[4][12] = {
	{ K("q", "Q", '1'), K("w", "W", '2'), K("e", "E", '3'), K("r", "R", '4'),
	  K("t", "T", '5'), K("y", "Y", '6'), K("u", "U", '7'), K("i", "I", '8'),
	  K("o", "O", '9'), K("p", "P", '0'), KROW_END },
	{ K("a", "A", 0), K("s", "S", 0), K("d", "D", 0), K("f", "F", 0),
	  K("g", "G", 0), K("h", "H", 0), K("j", "J", 0), K("k", "K", 0),
	  K("l", "L", 0), KROW_END },
	{ { LV_SYMBOL_UP, NULL, KK_SHIFT, 15, 0, 0 },
	  K("z", "Z", 0), K("x", "X", 0), K("c", "C", 0), K("v", "V", 0),
	  K("b", "B", 0), K("n", "N", 0), K("m", "M", 0),
	  { LV_SYMBOL_BACKSPACE, NULL, KK_BKSP, 15, 0, 0 }, KROW_END },
	{ { "?123", NULL, KK_PAGE, 15, 0, 1 }, S(","),
	  { "", " ", KK_SPACE, 50, 0, 0 }, S("."),
	  { LV_SYMBOL_NEW_LINE, NULL, KK_ENTER, 15, 0, 0 }, KROW_END },
};

static const struct kbd_key kbd_symbols[4][12] = {
	{ S("1"), S("2"), S("3"), S("4"), S("5"), S("6"), S("7"), S("8"), S("9"), S("0"), KROW_END },
	{ S("@"), S("#"), S("$"), S("_"), S("&"), S("-"), S("+"), S("("), S(")"), S("/"), KROW_END },
	{ { "=\\<", NULL, KK_PAGE, 15, 0, 2 },
	  S("*"), S("\""), S("'"), S(":"), S(";"), S("!"), S("?"),
	  { LV_SYMBOL_BACKSPACE, NULL, KK_BKSP, 15, 0, 0 }, KROW_END },
	{ { "ABC", NULL, KK_PAGE, 15, 0, 0 }, S(","),
	  { "", " ", KK_SPACE, 50, 0, 0 }, S("."),
	  { LV_SYMBOL_NEW_LINE, NULL, KK_ENTER, 15, 0, 0 }, KROW_END },
};

static const struct kbd_key kbd_more[4][12] = {
	{ S("~"), S("`"), S("|"), S("\\"), S("^"), S("%"), S("="), S("<"), S(">"), S("*"), KROW_END },
	{ S("{"), S("}"), S("["), S("]"), S("$"), S("#"), S("@"), S("&"), S("+"), S("-"), KROW_END },
	{ { "?123", NULL, KK_PAGE, 15, 0, 1 },
	  S("_"), S("\""), S("'"), S(":"), S(";"), S("!"), S("?"),
	  { LV_SYMBOL_BACKSPACE, NULL, KK_BKSP, 15, 0, 0 }, KROW_END },
	{ { "ABC", NULL, KK_PAGE, 15, 0, 0 }, S(","),
	  { "", " ", KK_SPACE, 50, 0, 0 }, S("."),
	  { LV_SYMBOL_NEW_LINE, NULL, KK_ENTER, 15, 0, 0 }, KROW_END },
};

static const struct kbd_key (*const kbd_pages[3])[12] = { kbd_letters, kbd_symbols, kbd_more };

/* The toolbar: for applications, and for the launcher's own fields. */
static const struct kbd_key kbd_tb_app[] = {
	{ "Ctrl", NULL, KK_CTRL, 0, 0, 0 }, { "Esc", NULL, KK_ESC, 0, 0, 0 },
	{ "Tab", NULL, KK_TAB, 0, 0, 0 }, { LV_SYMBOL_LEFT, NULL, KK_LEFT, 0, 0, 0 },
	{ LV_SYMBOL_RIGHT, NULL, KK_RIGHT, 0, 0, 0 }, KROW_END,
};
static const struct kbd_key kbd_tb_ui[] = {
	{ LV_SYMBOL_LEFT, NULL, KK_LEFT, 0, 0, 0 },
	{ LV_SYMBOL_RIGHT, NULL, KK_RIGHT, 0, 0, 0 }, KROW_END,
};
static const struct kbd_key kbd_tb_hide = { LV_SYMBOL_DOWN, NULL, KK_HIDE, 0, 0, 0 };


struct kbd kbd = { .down = -1, .ui_fd = -1, .fifo_fd = -1, .fifo_wr = -1 };


/* ------------------------------------------------------------ output */

static const uint16_t kbd_letter_codes[26] = {
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

static const struct { char c; uint16_t code; bool shift; } kbd_punct[] = {
	{ ' ', KEY_SPACE, false }, { '-', KEY_MINUS, false }, { '=', KEY_EQUAL, false },
	{ '[', KEY_LEFTBRACE, false }, { ']', KEY_RIGHTBRACE, false },
	{ '\\', KEY_BACKSLASH, false }, { ';', KEY_SEMICOLON, false },
	{ '\'', KEY_APOSTROPHE, false }, { '`', KEY_GRAVE, false }, { ',', KEY_COMMA, false },
	{ '.', KEY_DOT, false }, { '/', KEY_SLASH, false },
	{ '!', KEY_1, true }, { '@', KEY_2, true }, { '#', KEY_3, true }, { '$', KEY_4, true },
	{ '%', KEY_5, true }, { '^', KEY_6, true }, { '&', KEY_7, true }, { '*', KEY_8, true },
	{ '(', KEY_9, true }, { ')', KEY_0, true }, { '_', KEY_MINUS, true },
	{ '+', KEY_EQUAL, true }, { '{', KEY_LEFTBRACE, true }, { '}', KEY_RIGHTBRACE, true },
	{ '|', KEY_BACKSLASH, true }, { ':', KEY_SEMICOLON, true }, { '"', KEY_APOSTROPHE, true },
	{ '~', KEY_GRAVE, true }, { '<', KEY_COMMA, true }, { '>', KEY_DOT, true },
	{ '?', KEY_SLASH, true },
};

/* The key for a character on a US keyboard; 0 when there is none. */
static uint16_t kbd_code(char c, bool *shift)
{
	*shift = false;
	if (c >= 'a' && c <= 'z')
		return kbd_letter_codes[c - 'a'];
	if (c >= 'A' && c <= 'Z') {
		*shift = true;
		return kbd_letter_codes[c - 'A'];
	}
	if (c >= '1' && c <= '9')
		return KEY_1 + (c - '1');
	if (c == '0')
		return KEY_0;
	for (size_t i = 0; i < sizeof(kbd_punct) / sizeof(kbd_punct[0]); i++)
		if (kbd_punct[i].c == c) {
			*shift = kbd_punct[i].shift;
			return kbd_punct[i].code;
		}
	return 0;
}

/*
 * The uinput keyboard: made at start (kbd_init), so an application that
 * looks for its keyboards when it starts finds it; again here if that
 * failed.
 */
static bool kbd_uinput(void)
{
	struct uinput_setup us = { .id = { BUS_VIRTUAL, 0x6e72, 0x6b62, 1 } };
	int fd;

	if (kbd.ui_fd >= 0)
		return true;
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	for (int code = KEY_ESC; code <= KEY_SLASH; code++)	/* the main block */
		ioctl(fd, UI_SET_KEYBIT, code);
	ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_LEFT);
	ioctl(fd, UI_SET_KEYBIT, KEY_RIGHT);
	ioctl(fd, UI_SET_KEYBIT, KEY_UP);
	ioctl(fd, UI_SET_KEYBIT, KEY_DOWN);
	snprintf(us.name, sizeof(us.name), "n72-ui keyboard");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return false;
	}
	kbd.ui_fd = fd;
	return true;
}

static void kbd_emit(uint16_t code, int value)
{
	struct input_event ev[2] = {
		{ .type = EV_KEY, .code = code, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	if (write(kbd.ui_fd, ev, sizeof(ev)) < 0)
		fprintf(stderr, "n72-ui: keyboard: %s\n", strerror(errno));
}

/* One key to the application, with Shift and Ctrl around it as wanted. */
static void kbd_tap(uint16_t code, bool shift)
{
	bool ctrl = kbd.ctrl;

	if (!code || !kbd_uinput())
		return;
	if (ctrl)
		kbd_emit(KEY_LEFTCTRL, 1);
	if (shift)
		kbd_emit(KEY_LEFTSHIFT, 1);
	kbd_emit(code, 1);
	kbd_emit(code, 0);
	if (shift)
		kbd_emit(KEY_LEFTSHIFT, 0);
	if (ctrl) {
		kbd_emit(KEY_LEFTCTRL, 0);
		kbd.ctrl = false;	/* (for one key) */
		if (kbd.obj)
			lv_obj_invalidate(kbd.obj);
	}
}

static void kbd_type(const char *s)
{
	if (kbd.ta) {
		lv_textarea_add_text(kbd.ta, s);
		return;
	}
	for (; *s; s++) {
		bool shift;
		uint16_t code = kbd_code(*s, &shift);

		kbd_tap(code, shift);
	}
}

/* What a function key does. */
static void kbd_do(int kind)
{
	if (kbd.ta) {
		switch (kind) {
		case KK_BKSP:	lv_textarea_delete_char(kbd.ta); break;
		case KK_LEFT:	lv_textarea_cursor_left(kbd.ta); break;
		case KK_RIGHT:	lv_textarea_cursor_right(kbd.ta); break;
		case KK_ENTER:	lv_obj_send_event(kbd.ta, LV_EVENT_READY, NULL); break;
		}
		return;
	}
	switch (kind) {
	case KK_BKSP:	kbd_tap(KEY_BACKSPACE, false); break;
	case KK_LEFT:	kbd_tap(KEY_LEFT, false); break;
	case KK_RIGHT:	kbd_tap(KEY_RIGHT, false); break;
	case KK_ENTER:	kbd_tap(KEY_ENTER, false); break;
	case KK_ESC:	kbd_tap(KEY_ESC, false); break;
	case KK_TAB:	kbd_tap(KEY_TAB, false); break;
	}
}

/* ------------------------------------------------------------ layout */

/* Sizes for the screen as it is turned. */
static void kbd_metrics(void)
{
	bool land = W > H;

	kbd.pad = 4;
	kbd.gap = land ? 5 : 5;
	kbd.key_h = land ? 30 : 40;
	kbd.tb_h = land ? 26 : 30;
	kbd.w = W;
	kbd.h = kbd.pad + kbd.tb_h + 2 + 4 * kbd.key_h + 3 * (land ? 4 : 7) + kbd.pad + 2;
}

static int32_t kbd_row_gap(void)
{
	return W > H ? 4 : 7;
}

static void kbd_add(const struct kbd_key *k, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
	if (kbd.nkeys >= KBD_NKEYS)
		return;
	kbd.keys[kbd.nkeys].k = k;
	kbd.keys[kbd.nkeys].a = (lv_area_t){ x1, y1, x2, y2 };
	kbd.nkeys++;
}

/* Where every key of the page (and the toolbar) is. */
static void kbd_layout(void)
{
	const struct kbd_key (*rows)[12] = kbd_pages[kbd.page];
	int32_t inner = kbd.w - 2 * kbd.pad, unit_x, y, x;
	const struct kbd_key *tb = kbd.ta ? kbd_tb_ui : kbd_tb_app;

	kbd.nkeys = 0;
	/* the toolbar: chips from the left, Hide at the right */
	y = kbd.pad;
	x = kbd.pad;
	for (; tb->lo; tb++) {
		int32_t cw = tb->kind == KK_LEFT || tb->kind == KK_RIGHT ? 40 : 48;

		kbd_add(tb, x, y, x + cw - 1, y + kbd.tb_h - 1);
		x += cw + 4;
	}
	kbd_add(&kbd_tb_hide, kbd.w - kbd.pad - 48, y, kbd.w - kbd.pad - 1, y + kbd.tb_h - 1);

	/* the rows: 100 units across, a key's gap taken from its width */
	y = kbd.pad + kbd.tb_h + 2;
	unit_x = inner;		/* (per 100 units) */
	for (int r = 0; r < 4; r++) {
		int units = 0, lead;

		for (const struct kbd_key *k = rows[r]; k->lo; k++)
			units += k->w;
		lead = (100 - units) / 2;	/* (a shorter row centred) */
		x = lead;
		for (const struct kbd_key *k = rows[r]; k->lo; k++) {
			int32_t x1 = kbd.pad + x * unit_x / 100 + kbd.gap / 2;
			int32_t x2 = kbd.pad + (x + k->w) * unit_x / 100 - (kbd.gap + 1) / 2 - 1;

			kbd_add(k, x1, y, x2, y + kbd.key_h - 1);
			x += k->w;
		}
		y += kbd.key_h + kbd_row_gap();
	}
}

/*
 * The key for a touch at (x, y) (relative to the object): the one it is
 * in, or the nearest in its band (the gaps belong to the keys beside them).
 */
static int kbd_hit(int32_t x, int32_t y)
{
	int best = -1;
	int32_t bd = INT32_MAX;
	int32_t half = kbd_row_gap() / 2 + 1;

	for (int i = 0; i < kbd.nkeys; i++) {
		lv_area_t *a = &kbd.keys[i].a;
		int32_t dx, dy;

		if (y < a->y1 - half || y > a->y2 + half)
			continue;
		dx = x < a->x1 ? a->x1 - x : x > a->x2 ? x - a->x2 : 0;
		dy = y < a->y1 ? a->y1 - y : y > a->y2 ? y - a->y2 : 0;
		if (dx + dy < bd) {
			bd = dx + dy;
			best = i;
		}
	}
	if (best >= 0 && bd > kbd.gap * 3)	/* (far from every key: the toolbar's empty middle) */
		return -1;
	return best;
}

/* ------------------------------------------------------------ drawing */

static bool kbd_is_letter_page(void)
{
	return kbd.page == 0;
}

static const char *kbd_label(const struct kbd_key *k)
{
	if (k->kind == KK_CHAR && kbd_is_letter_page() && kbd.shift && k->up)
		return k->up;
	return k->lo;
}

/* The bubble over a pressed character key (relative to the object). */
static lv_area_t kbd_bubble_area(int i)
{
	lv_area_t *a = &kbd.keys[i].a;
	int32_t kw = lv_area_get_width(a), bw = LV_MAX(kw + 14, 44), bh = kbd.key_h + 10;
	lv_area_t b;

	b.x1 = a->x1 + kw / 2 - bw / 2;
	b.x1 = LV_CLAMP(2, b.x1, kbd.w - 2 - bw);
	b.x2 = b.x1 + bw - 1;
	b.y2 = a->y1 - 4;
	b.y1 = b.y2 - bh + 1;
	return b;
}

static bool kbd_has_bubble(int i)
{
	return i >= 0 && kbd.keys[i].k->kind == KK_CHAR;
}

static void kbd_key_colors(const struct kbd_key *k, bool pressed, lv_color_t *bg, lv_color_t *fg,
			   lv_opa_t *opa)
{
	*opa = LV_OPA_COVER;
	switch (k->kind) {
	case KK_ENTER:
		*bg = M.primary;
		*fg = M.on_primary;
		break;
	case KK_SHIFT:
		if (kbd.shift == 2) {
			*bg = M.primary;
			*fg = M.on_primary;
		} else if (kbd.shift == 1) {
			*bg = M.primary_c;
			*fg = M.on_primary_c;
		} else {
			*bg = M.secondary_c;
			*fg = M.on_secondary_c;
		}
		break;
	case KK_BKSP: case KK_PAGE:
		*bg = M.secondary_c;
		*fg = M.on_secondary_c;
		break;
	case KK_CTRL:
		*bg = kbd.ctrl ? M.secondary_c : M.surface_c;
		*fg = kbd.ctrl ? M.on_secondary_c : M.on_surface_v;
		*opa = kbd.ctrl ? LV_OPA_COVER : LV_OPA_TRANSP;
		break;
	case KK_LEFT: case KK_RIGHT: case KK_HIDE: case KK_ESC: case KK_TAB:
		*bg = M.surface_c;
		*fg = M.on_surface_v;
		*opa = LV_OPA_TRANSP;
		break;
	default:		/* the characters and space */
		*bg = conf.dark ? M.surface_c_highest : M.surface;
		*fg = M.on_surface;
		break;
	}
	if (pressed) {		/* the pressed state layer: on-colour at 16% */
		*bg = lv_color_mix(*fg, *bg, LV_OPA_20);
		*opa = LV_OPA_COVER;
	}
}

static const lv_font_t *kbd_font(const struct kbd_key *k, const char *label)
{
	if (k->kind == KK_CHAR)
		return &lv_font_montserrat_20;
	if (k->kind == KK_PAGE || k->kind == KK_CTRL || k->kind == KK_ESC || k->kind == KK_TAB)
		return &lv_font_montserrat_14;
	LV_UNUSED(label);
	return &lv_font_montserrat_20;	/* the symbols */
}

static void kbd_draw_text(lv_layer_t *layer, const char *txt, const lv_font_t *font,
			  lv_color_t color, const lv_area_t *box)
{
	lv_draw_label_dsc_t d;
	lv_area_t a = *box;
	int32_t th = lv_font_get_line_height(font);

	lv_draw_label_dsc_init(&d);
	d.text = txt;
	d.font = font;
	d.color = color;
	d.align = LV_TEXT_ALIGN_CENTER;
	a.y1 += (lv_area_get_height(box) - th) / 2;
	a.y2 = a.y1 + th - 1;
	lv_draw_label(layer, &d, &a);
}

static void kbd_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t c;

	lv_obj_get_coords(kbd.obj, &c);
	for (int i = 0; i < kbd.nkeys; i++) {
		const struct kbd_key *k = kbd.keys[i].k;
		const char *label = kbd_label(k);
		lv_area_t a = kbd.keys[i].a;
		lv_color_t bg, fg;
		lv_opa_t opa;
		bool pressed = i == kbd.down && !(kbd_has_bubble(i));

		lv_area_move(&a, c.x1, c.y1);
		kbd_key_colors(k, pressed || (i == kbd.down && k->kind == KK_SPACE), &bg, &fg, &opa);
		if (opa) {
			lv_draw_rect_dsc_t r;

			lv_draw_rect_dsc_init(&r);
			r.bg_color = bg;
			r.bg_opa = opa;
			r.radius = k->kind == KK_ENTER ? LV_MIN(kbd.key_h / 2, 16) :
				   k->w ? 8 : LV_RADIUS_CIRCLE;
			lv_draw_rect(layer, &r, &a);
		}
		if (*label)
			kbd_draw_text(layer, label, kbd_font(k, label), fg, &a);
		if (k->kind == KK_SHIFT && kbd.shift == 2) {	/* locked: a bar under the arrow */
			lv_draw_rect_dsc_t r;
			lv_area_t u = { a.x1 + lv_area_get_width(&a) / 2 - 7, a.y2 - 8,
					a.x1 + lv_area_get_width(&a) / 2 + 6, a.y2 - 7 };

			lv_draw_rect_dsc_init(&r);
			r.bg_color = fg;
			r.radius = 1;
			lv_draw_rect(layer, &r, &u);
		}
		if (k->hint) {			/* the digit it gives when held */
			char h[2] = { k->hint, 0 };
			lv_area_t ha = { a.x2 - 8, a.y1 + 1, a.x2 - 2, a.y1 + 14 };
			lv_draw_label_dsc_t d;

			lv_draw_label_dsc_init(&d);
			d.text = h;
			d.text_local = true;
			d.font = &lv_font_montserrat_12;
			d.color = M.on_surface_v;
			d.opa = LV_OPA_70;
			lv_draw_label(layer, &d, &ha);
		}
	}
}

/* The bubble: over everything, past the object's top for the top row. */
static void kbd_draw_post(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	const struct kbd_key *k;
	lv_draw_rect_dsc_t r;
	lv_area_t c, b;
	char txt[2] = { 0, 0 };

	if (!kbd_has_bubble(kbd.down))
		return;
	k = kbd.keys[kbd.down].k;
	lv_obj_get_coords(kbd.obj, &c);
	b = kbd_bubble_area(kbd.down);
	lv_area_move(&b, c.x1, c.y1);
	lv_draw_rect_dsc_init(&r);
	r.bg_color = M.secondary_c;
	r.radius = 12;
	lv_draw_rect(layer, &r, &b);
	if (kbd.held && k->hint)
		txt[0] = k->hint;
	kbd_draw_text(layer, txt[0] ? txt : kbd_label(k), &lv_font_montserrat_28,
		      M.on_secondary_c, &b);
}

/* Redraw one key (and its bubble). */
static void kbd_inval_key(int i)
{
	lv_area_t c, a;

	if (i < 0 || !kbd.obj)
		return;
	lv_obj_get_coords(kbd.obj, &c);
	a = kbd.keys[i].a;
	if (kbd_has_bubble(i)) {
		lv_area_t b = kbd_bubble_area(i);

		a.y1 = b.y1;
		a.x1 = LV_MIN(a.x1, b.x1);
		a.x2 = LV_MAX(a.x2, b.x2);
	}
	lv_area_move(&a, c.x1, c.y1);
	lv_obj_invalidate_area(kbd.obj, &a);
}

/* ------------------------------------------------------------ touch */

static void kbd_stop_timers(void)
{
	if (kbd.rep) {
		lv_timer_delete(kbd.rep);
		kbd.rep = NULL;
	}
	if (kbd.hold) {
		lv_timer_delete(kbd.hold);
		kbd.hold = NULL;
	}
}

static void kbd_rep_cb(lv_timer_t *t)
{
	if (kbd.down < 0) {
		kbd_stop_timers();
		return;
	}
	lv_timer_set_period(t, 50);	/* (after the first, quicker) */
	kbd_do(kbd.keys[kbd.down].k->kind);
}

static void kbd_hold_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	kbd.hold = NULL;
	if (kbd.down >= 0 && kbd.keys[kbd.down].k->hint) {
		kbd.held = true;
		kbd_inval_key(kbd.down);
	}
}

static void kbd_set_down(int i)
{
	if (i == kbd.down)
		return;
	kbd_inval_key(kbd.down);
	kbd.down = i;
	kbd.held = false;
	if (kbd.hold) {
		lv_timer_delete(kbd.hold);
		kbd.hold = NULL;
	}
	if (i >= 0 && kbd.keys[i].k->hint) {
		kbd.hold = lv_timer_create(kbd_hold_cb, 380, NULL);
		lv_timer_set_repeat_count(kbd.hold, 1);
	}
	kbd_inval_key(i);
}

static void kbd_set_page(int page)
{
	kbd.page = page;
	kbd.shift = 0;
	kbd_layout();
	lv_obj_invalidate(kbd.obj);
}

void kbd_hide(void);

/* A key's work when the finger lifts on it. */
static void kbd_commit(int i)
{
	const struct kbd_key *k = kbd.keys[i].k;

	switch (k->kind) {
	case KK_CHAR:
		if (kbd.held && k->hint) {
			char d[2] = { k->hint, 0 };

			kbd_type(d);
		} else {
			kbd_type(kbd_label(k));
		}
		if (kbd.shift == 1 && kbd_is_letter_page()) {
			kbd.shift = 0;
			lv_obj_invalidate(kbd.obj);
		}
		break;
	case KK_SPACE:
		kbd_type(" ");
		/* back to the letters after a symbol and a space, as phones do */
		if (kbd.page)
			kbd_set_page(0);
		break;
	case KK_ENTER: case KK_ESC: case KK_TAB:
		kbd_do(k->kind);
		break;
	case KK_PAGE:
		kbd_set_page(k->page);
		break;
	case KK_CTRL:
		kbd.ctrl = !kbd.ctrl;
		lv_obj_invalidate(kbd.obj);
		break;
	case KK_HIDE:
		if (kbd.ta)
			lv_obj_send_event(kbd.ta, LV_EVENT_CANCEL, NULL);
		kbd_hide();
		break;
	}
}

static void kbd_touch(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p;
	lv_area_t c;
	int i;

	if (!indev)
		return;
	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(kbd.obj, &c);
	p.x -= c.x1;
	p.y -= c.y1;

	if (code == LV_EVENT_PRESSED) {
		kbd.cursor = false;
		kbd.px = kbd.last_x = p.x;
		i = kbd_hit(p.x, p.y);
		kbd_set_down(i);
		if (i < 0)
			return;
		switch (kbd.keys[i].k->kind) {
		case KK_BKSP: case KK_LEFT: case KK_RIGHT:	/* now, and again while held */
			kbd_do(kbd.keys[i].k->kind);
			kbd.rep = lv_timer_create(kbd_rep_cb, 420, NULL);
			break;
		case KK_SHIFT:		/* at once: a second tap soon after locks */
			if (kbd.shift == 1 && lv_tick_elaps(kbd.shift_t) < 350)
				kbd.shift = 2;
			else
				kbd.shift = kbd.shift ? 0 : 1;
			kbd.shift_t = lv_tick_get();
			lv_obj_invalidate(kbd.obj);
			break;
		}
	} else if (code == LV_EVENT_PRESSING) {
		if (kbd.down < 0)
			return;
		if (kbd.keys[kbd.down].k->kind == KK_SPACE &&
		    (kbd.cursor || LV_ABS(p.x - kbd.px) > 14)) {
			int32_t step = 14;	/* the space bar dragged: the cursor */

			kbd.cursor = true;
			while (p.x - kbd.last_x >= step) {
				kbd_do(KK_RIGHT);
				kbd.last_x += step;
			}
			while (kbd.last_x - p.x >= step) {
				kbd_do(KK_LEFT);
				kbd.last_x -= step;
			}
			return;
		}
		if (kbd_has_bubble(kbd.down)) {	/* a rolling finger: the key under it now */
			i = kbd_hit(p.x, p.y);
			if (i >= 0 && kbd_has_bubble(i))
				kbd_set_down(i);
		} else if (kbd_hit(p.x, p.y) != kbd.down) {
			kbd_stop_timers();	/* off a function key: nothing, no more repeats */
			kbd_set_down(-1);
		}
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		i = kbd.down;
		kbd_stop_timers();
		if (i >= 0 && code == LV_EVENT_RELEASED && !kbd.cursor) {
			int kind = kbd.keys[i].k->kind;

			if (kind != KK_BKSP && kind != KK_LEFT && kind != KK_RIGHT && kind != KK_SHIFT)
				kbd_commit(i);
		}
		kbd.cursor = false;
		if (kbd.obj)
			kbd_set_down(-1);
	}
}

static void kbd_ext(lv_event_t *e)
{
	lv_event_set_ext_draw_size(e, kbd.key_h + 16);	/* the top row's bubbles */
}

/* ------------------------------------------------------------ showing */

/* Tell applications where the keyboard is. */
static void kbd_state_write(void)
{
	FILE *f = fopen(KBD_STATE ".new", "w");

	if (!f)
		return;
	if (kbd.shown && !kbd.ta)
		fprintf(f, "shown %d %d\n", (int)kbd.h, (int)kbd.w);
	else
		fprintf(f, "hidden\n");
	fclose(f);
	rename(KBD_STATE ".new", KBD_STATE);
}

static void kbd_y(void *obj, int32_t v)
{
	lv_obj_set_y(obj, v);
}

static void kbd_gone(lv_anim_t *a)
{
	lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
	app_dormant_sync();
}

bool kbd_is_open(void)
{
	return kbd.shown || (kbd.obj && !lv_obj_has_flag(kbd.obj, LV_OBJ_FLAG_HIDDEN)) ||
	       psl_sliding(kbd.obj);
}

static void kbd_build(void)
{
	if (kbd.obj) {
		psl_stop(kbd.obj);
		lv_obj_delete(kbd.obj);
	}
	kbd_metrics();
	kbd.obj = lv_obj_create(lv_layer_top());
	plain(kbd.obj);
	lv_obj_set_size(kbd.obj, kbd.w, kbd.h);
	lv_obj_set_pos(kbd.obj, 0, H);
	lv_obj_set_style_bg_color(kbd.obj, M.surface_c, 0);
	lv_obj_set_style_bg_opa(kbd.obj, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(kbd.obj, 0, 0);
	lv_obj_remove_flag(kbd.obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_flag(kbd.obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(kbd.obj, kbd_draw, LV_EVENT_DRAW_MAIN_END, NULL);
	lv_obj_add_event_cb(kbd.obj, kbd_draw_post, LV_EVENT_DRAW_POST_END, NULL);
	lv_obj_add_event_cb(kbd.obj, kbd_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_add_event_cb(kbd.obj, kbd_touch, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kbd.obj, kbd_touch, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(kbd.obj, kbd_touch, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(kbd.obj, kbd_touch, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_move_to_index(kbd.obj, 0);	/* under the shade, the bars, the pop-ups */
	lv_obj_refresh_ext_draw_size(kbd.obj);
	kbd.down = -1;
	kbd_layout();
}

/* iOS's keyboard: 300 ms ease-in-out both ways (UIInputViewAnimationStyle) */
#define KBD_SLIDE_MS	300

/*
 * Up (iOS: 300 ms, ease-in-out, as a picture: psl_start), for a field (@vis_cb: its
 * page, told) or for the application (@ta NULL).
 */
void kbd_show_for(lv_obj_t *ta, void (*vis_cb)(bool shown, int32_t h))
{
	lv_anim_t a;

	if (kbd.shown && kbd.ta == ta)
		return;
	if (!ta && app_dormant)
		app_dormant_wake(false);
	if (kbd.ta && kbd.ta != ta) {
		lv_obj_remove_state(kbd.ta, LV_STATE_FOCUSED);
		if (kbd.vis_cb)
			kbd.vis_cb(false, 0);
	}
	kbd.ta = ta;
	kbd.vis_cb = ta ? vis_cb : NULL;
	if (!kbd.obj || kbd.w != W)
		kbd_build();
	kbd.page = 0;
	kbd.shift = 0;
	kbd.ctrl = false;
	kbd_layout();
	lv_obj_invalidate(kbd.obj);
	lv_obj_move_to_index(kbd.obj, 0);
	lv_obj_remove_flag(kbd.obj, LV_OBJ_FLAG_HIDDEN);
	if (ta)
		lv_obj_add_state(ta, LV_STATE_FOCUSED);
	lv_anim_delete(kbd.obj, kbd_y);
	if (!kbd.shown) {
		if (!psl_start(kbd.obj, H - kbd.h, KBD_SLIDE_MS, ios_path_ease_in_out, false)) {
			lv_anim_init(&a);
			lv_anim_set_var(&a, kbd.obj);
			lv_anim_set_exec_cb(&a, kbd_y);
			lv_anim_set_values(&a, lv_obj_get_y(kbd.obj), H - kbd.h);
			lv_anim_set_duration(&a, KBD_SLIDE_MS);
			lv_anim_set_path_cb(&a, ios_path_ease_in_out);
			lv_anim_start(&a);
		}
	} else {
		psl_stop(kbd.obj);
		lv_obj_set_y(kbd.obj, H - kbd.h);
	}
	kbd.shown = true;
	if (kbd.ta && kbd.vis_cb)
		kbd.vis_cb(true, kbd.h);
	kbd_state_write();
}

/* Down (the same slide), and hidden. @anim false: at once. */
void kbd_hide_now(bool anim)
{
	lv_anim_t a;
	lv_obj_t *ta = kbd.ta;

	if (!kbd.shown)
		return;
	kbd.shown = false;
	kbd_stop_timers();
	kbd.down = -1;
	kbd.ctrl = false;
	if (ta) {
		lv_obj_remove_state(ta, LV_STATE_FOCUSED);
		if (kbd.vis_cb)
			kbd.vis_cb(false, 0);
	}
	kbd.ta = NULL;
	kbd.vis_cb = NULL;
	lv_anim_delete(kbd.obj, kbd_y);
	if (anim && psl_start(kbd.obj, H, KBD_SLIDE_MS, ios_path_ease_in_out, true)) {
		;
	} else if (anim) {
		lv_anim_init(&a);
		lv_anim_set_var(&a, kbd.obj);
		lv_anim_set_exec_cb(&a, kbd_y);
		lv_anim_set_values(&a, lv_obj_get_y(kbd.obj), H);
		lv_anim_set_duration(&a, KBD_SLIDE_MS);
		lv_anim_set_path_cb(&a, ios_path_ease_in_out);
		lv_anim_set_completed_cb(&a, kbd_gone);
		lv_anim_start(&a);
	} else {
		psl_stop(kbd.obj);
		lv_obj_set_y(kbd.obj, H);
		lv_obj_add_flag(kbd.obj, LV_OBJ_FLAG_HIDDEN);
	}
	kbd_state_write();
	if (!anim)
		app_dormant_sync();
}

void kbd_hide(void)
{
	kbd_hide_now(true);
}

/* Does a touch at (x, y) (the screen's) belong to the keyboard? */
static bool kbd_wants(int x, int y)
{
	return kbd.shown && (in_obj(kbd.obj, x, y) || psl_covers(kbd.obj, x, y));
}

/* ---------------------------------------------------- the launcher's fields */

static void kbd_ta_deleted(lv_event_t *e)
{
	if (kbd.ta == lv_event_get_target(e)) {
		kbd.vis_cb = NULL;	/* (its page is going too) */
		kbd.ta = NULL;
		kbd_hide_now(false);
	}
}

static void kbd_ta_clicked(lv_event_t *e)
{
	kbd_show_for(lv_event_get_target(e), (void (*)(bool, int32_t))lv_event_get_user_data(e));
}

/*
 * The keyboard for one of the launcher's text fields: up now when @show,
 * and whenever the field is tapped. @vis_cb (may be NULL) hears when it
 * comes and goes, with its height, so the page can make room. Enter is
 * the field's LV_EVENT_READY; the keyboard's Hide, LV_EVENT_CANCEL.
 */
void kbd_attach(lv_obj_t *ta, void (*vis_cb)(bool shown, int32_t h), bool show)
{
	lv_obj_add_event_cb(ta, kbd_ta_clicked, LV_EVENT_CLICKED, (void *)vis_cb);
	lv_obj_add_event_cb(ta, kbd_ta_deleted, LV_EVENT_DELETE, NULL);
	if (show)
		kbd_show_for(ta, vis_cb);
}

/* ---------------------------------------------------- applications */

static void kbd_show_app(void)
{
	if (app_pid > 0)
		kbd_show_for(NULL, NULL);
}

/* The app bar's keyboard button. */
void kbd_toggle_app(void)
{
	if (kbd.shown && !kbd.ta)
		kbd_hide();
	else
		kbd_show_app();
}

/* "show", "hide", "toggle" from /run/n72-kbd */
static void kbd_fifo_ev(int fd)
{
	char buf[128];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);

	if (n <= 0)
		return;
	buf[n] = 0;
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strcmp(l, "show"))
			kbd_show_app();
		else if (!strcmp(l, "hide") && !kbd.ta)
			kbd_hide();
		else if (!strcmp(l, "toggle")) {
			if (kbd.shown && !kbd.ta)
				kbd_hide();
			else
				kbd_show_app();
		}
	}
}

void kbd_init(void)
{
	unlink(KBD_FIFO);
	if (mkfifo(KBD_FIFO, 0666) == 0)
		chmod(KBD_FIFO, 0666);	/* (whatever the umask) */
	kbd.fifo_fd = open(KBD_FIFO, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (kbd.fifo_fd >= 0) {
		kbd.fifo_wr = open(KBD_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		ui_watch(kbd.fifo_fd, kbd_fifo_ev);
	}
	/*
	 * The console's keyboard off: the kernel's console takes every
	 * keyboard's keys, this one's too, into the shell on the screen's
	 * console (tty1, behind the launcher), which would have run what an
	 * application was typed. Applications read the keys from evdev,
	 * which this does not touch (as X and Weston do it).
	 */
	{
		int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);

		if (fd >= 0) {
			if (ioctl(fd, KDSKBMODE, K_OFF) < 0)
				fprintf(stderr, "n72-ui: console keyboard: %s\n", strerror(errno));
			close(fd);
		}
	}
	kbd_uinput();
	kbd_state_write();
}

/* The application is gone, the screen locked: no keyboard for it. */
void kbd_app_gone(void)
{
	if (kbd.shown && !kbd.ta)
		kbd_hide_now(false);
}

/* The screen turned: the keyboard at the new size, up if it was. */
void kbd_turned(void)
{
	lv_obj_t *ta = kbd.ta;
	void (*cb)(bool, int32_t) = kbd.vis_cb;
	bool was = kbd.shown;

	if (!kbd.obj || kbd.w == W)
		return;		/* (none yet, or this size already) */
	kbd.shown = false;
	kbd_build();
	if (was) {
		kbd.ta = ta;
		kbd.vis_cb = cb;
		kbd.shown = true;	/* (no slide: it was up) */
		lv_obj_remove_flag(kbd.obj, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_y(kbd.obj, H - kbd.h);
		if (ta && cb)
			cb(true, kbd.h);
		kbd_state_write();
	}
}
