/*
 * vkey: type on a Wayland compositor's virtual keyboard with REAL key
 * codes. wtype invents a keymap where 'r' may be key 1; programs that
 * read raw scan codes (FS-UAE) then see the wrong keys. vkey uploads a
 * plain US keymap and sends each character's real evdev code.
 *
 * Usage: vkey [-d ms] ARG...
 *   ARG "text"    types the text (\n = Return, \t = Tab)
 *   -k NAME       one key: Return Escape Up Down Left Right BackSpace
 *                 Tab Help Delete F1..F10
 *   -c X          Ctrl+X
 *   -s ms         sleep
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "vk.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *mgr;
static struct zwp_virtual_keyboard_v1 *kbd;
static struct wl_display *dpy;
static int delay_ms = 40;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
	usleep(ms * 1000);
}

#define MOD_SHIFT 1
#define MOD_CTRL  4

static void press(int code, int mods)
{
	zwp_virtual_keyboard_v1_modifiers(kbd, mods, 0, 0, 0);
	if (mods & MOD_SHIFT)
		zwp_virtual_keyboard_v1_key(kbd, now_ms(), 42, 1);
	if (mods & MOD_CTRL)
		zwp_virtual_keyboard_v1_key(kbd, now_ms(), 29, 1);
	wl_display_roundtrip(dpy);
	sleep_ms(delay_ms / 2);
	zwp_virtual_keyboard_v1_key(kbd, now_ms(), code, 1);
	wl_display_roundtrip(dpy);
	sleep_ms(delay_ms);
	zwp_virtual_keyboard_v1_key(kbd, now_ms(), code, 0);
	if (mods & MOD_CTRL)
		zwp_virtual_keyboard_v1_key(kbd, now_ms(), 29, 0);
	if (mods & MOD_SHIFT)
		zwp_virtual_keyboard_v1_key(kbd, now_ms(), 42, 0);
	zwp_virtual_keyboard_v1_modifiers(kbd, 0, 0, 0, 0);
	wl_display_roundtrip(dpy);
	sleep_ms(delay_ms);
}

/* US layout: evdev code and whether Shift is needed */
static const char *row_plain[] = {
	/* code: chars */
	"\x02" "1", "\x03" "2", "\x04" "3", "\x05" "4", "\x06" "5", "\x07" "6",
	"\x08" "7", "\x09" "8", "\x0a" "9", "\x0b" "0", "\x0c" "-", "\x0d" "=",
	"\x10" "q", "\x11" "w", "\x12" "e", "\x13" "r", "\x14" "t", "\x15" "y",
	"\x16" "u", "\x17" "i", "\x18" "o", "\x19" "p", "\x1a" "[", "\x1b" "]",
	"\x1e" "a", "\x1f" "s", "\x20" "d", "\x21" "f", "\x22" "g", "\x23" "h",
	"\x24" "j", "\x25" "k", "\x26" "l", "\x27" ";", "\x28" "'", "\x29" "`",
	"\x2b" "\\", "\x2c" "z", "\x2d" "x", "\x2e" "c", "\x2f" "v", "\x30" "b",
	"\x31" "n", "\x32" "m", "\x33" ",", "\x34" ".", "\x35" "/", "\x39" " ",
	NULL
};
static const char shifted_of[] = "1!2@3#4$5%6^7&8*9(0)-_=+[{]};:'\"`~\\|,<.>/?";

static int lookup(char c, int *mods)
{
	int i;
	*mods = 0;
	if (c >= 'A' && c <= 'Z') {
		*mods = MOD_SHIFT;
		c = c - 'A' + 'a';
	} else {
		for (i = 1; shifted_of[i]; i += 2)
			if (shifted_of[i] == c) {
				*mods = MOD_SHIFT;
				c = shifted_of[i - 1];
				break;
			}
	}
	if (c == '\n')
		return 28;
	if (c == '\t')
		return 15;
	for (i = 0; row_plain[i]; i++)
		if (row_plain[i][1] == c)
			return (unsigned char)row_plain[i][0];
	return -1;
}

static const struct { const char *n; int code; } names[] = {
	{ "Return", 28 }, { "Escape", 1 }, { "BackSpace", 14 }, { "Tab", 15 },
	{ "Up", 103 }, { "Down", 108 }, { "Left", 105 }, { "Right", 106 },
	{ "Delete", 111 }, { "Help", 138 },
	{ "F1", 59 }, { "F2", 60 }, { "F3", 61 }, { "F4", 62 }, { "F5", 63 },
	{ "F6", 64 }, { "F7", 65 }, { "F8", 66 }, { "F9", 67 }, { "F10", 68 },
	{ NULL, 0 }
};

static void upload_keymap(void)
{
	struct xkb_context *ctx = xkb_context_new(0);
	struct xkb_rule_names rn = { "evdev", "pc105", "us", "", "" };
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &rn, 0);
	char *s = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(s) + 1;
	int fd = memfd_create("vkey-keymap", 0);

	if (write(fd, s, len) != (ssize_t)len) {
		perror("keymap");
		exit(1);
	}
	zwp_virtual_keyboard_v1_keymap(kbd, XKB_KEYMAP_FORMAT_TEXT_V1, fd, len);
	wl_display_roundtrip(dpy);
	close(fd);
	free(s);
}

int main(int argc, char **argv)
{
	int i;

	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "vkey: no Wayland display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !mgr) {
		fprintf(stderr, "vkey: no seat or virtual keyboard support\n");
		return 1;
	}
	kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(mgr, seat);
	upload_keymap();
	/* A brand new keyboard can lose its first key while the compositor
	   and the client switch to its keymap: settle first */
	wl_display_roundtrip(dpy);
	sleep_ms(150);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int mods, code, j;

		if (!strcmp(a, "-d") && i + 1 < argc) {
			delay_ms = atoi(argv[++i]);
		} else if (!strcmp(a, "-s") && i + 1 < argc) {
			sleep_ms(atoi(argv[++i]));
		} else if (!strcmp(a, "-k") && i + 1 < argc) {
			a = argv[++i];
			for (j = 0; names[j].n; j++)
				if (!strcmp(names[j].n, a))
					break;
			if (!names[j].n) {
				fprintf(stderr, "vkey: unknown key %s\n", a);
				return 1;
			}
			press(names[j].code, 0);
		} else if (!strcmp(a, "-c") && i + 1 < argc) {
			code = lookup(argv[++i][0], &mods);
			if (code >= 0)
				press(code, MOD_CTRL);
		} else {
			for (; *a; a++) {
				code = lookup(*a, &mods);
				if (code < 0) {
					fprintf(stderr, "vkey: cannot type '%c'\n", *a);
					continue;
				}
				press(code, mods);
			}
		}
	}
	wl_display_roundtrip(dpy);
	return 0;
}
