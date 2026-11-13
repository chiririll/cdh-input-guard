// SPDX-License-Identifier: GPL-2.0
/*
 * Create a virtual keyboard through uinput, print its sysfs name (inputN),
 * optionally send key combos, then destroy it.
 *
 *   -n NAME   device name (default "cdh-guard-test-kbd")
 *   -w MS     wait after creating, before sending keys (default 1000)
 *   -H MS     wait after sending keys, before destroying (default 500)
 *   -t        type "cdhtest" + Enter (shows up at a tty login prompt if kbd is attached)
 *   -v        Ctrl+Alt+F2 (switches VT if kbd is attached)
 *   -r        Alt+SysRq+H (prints "sysrq: HELP" to dmesg if sysrq is attached)
 *   -d        Ctrl+Alt+Del (reboots the machine if kbd is attached and
 *             ctrl-alt-del.target is not masked; VM only)
 *   -p        KEY_POWER (powers the machine off if logind watches the device,
 *             that is if its event node is tagged "power-switch"; VM only)
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void die(const char *what)
{
	fprintf(stderr, "uinput_kbd: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void emit(int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		die("write");
}

static void key(int code, int value)
{
	emit(EV_KEY, code, value);
	emit(EV_SYN, SYN_REPORT, 0);
	msleep(20);
}

/* Press keys in order, release in reverse. */
static void combo(const int *keys, int n)
{
	int i;

	for (i = 0; i < n; i++)
		key(keys[i], 1);
	for (i = n - 1; i >= 0; i--)
		key(keys[i], 0);
	msleep(100);
}

#define COMBO(...) do { \
	const int k_[] = { __VA_ARGS__ }; \
	combo(k_, sizeof(k_) / sizeof(k_[0])); \
} while (0)

int main(int argc, char **argv)
{
	const char *name = "cdh-guard-test-kbd";
	long wait_ms = 1000, hold_ms = 500;
	int do_type = 0, do_vt = 0, do_sysrq = 0, do_cad = 0, do_power = 0;
	struct uinput_setup us;
	char sysname[64];
	int opt, k;

	while ((opt = getopt(argc, argv, "n:w:H:tvrdp")) != -1) {
		switch (opt) {
		case 'n': name = optarg; break;
		case 'w': wait_ms = atol(optarg); break;
		case 'H': hold_ms = atol(optarg); break;
		case 't': do_type = 1; break;
		case 'v': do_vt = 1; break;
		case 'r': do_sysrq = 1; break;
		case 'd': do_cad = 1; break;
		case 'p': do_power = 1; break;
		default:
			fprintf(stderr, "usage: %s [-n name] [-w ms] [-H ms] [-t] [-v] [-r] [-d] [-p]\n",
				argv[0]);
			return 2;
		}
	}

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		die("open /dev/uinput");

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		die("UI_SET_EVBIT");
	/* KEY_ESC..KEY_MICMUTE: a full keyboard, including Ctrl, Alt, Del, F-keys, SysRq. */
	for (k = KEY_ESC; k <= KEY_MICMUTE; k++)
		if (ioctl(fd, UI_SET_KEYBIT, k) < 0)
			die("UI_SET_KEYBIT");

	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor = 0x1209;
	us.id.product = 0xcd01;
	us.id.version = 1;
	snprintf(us.name, sizeof(us.name), "%s", name);

	if (ioctl(fd, UI_DEV_SETUP, &us) < 0)
		die("UI_DEV_SETUP");
	if (ioctl(fd, UI_DEV_CREATE) < 0)
		die("UI_DEV_CREATE");
	if (ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0)
		die("UI_GET_SYSNAME");

	printf("%s\n", sysname);
	fflush(stdout);

	msleep(wait_ms);

	if (do_type) {
		const int text[] = { KEY_C, KEY_D, KEY_H, KEY_T, KEY_E, KEY_S, KEY_T, KEY_ENTER };

		for (k = 0; k < (int)(sizeof(text) / sizeof(text[0])); k++) {
			key(text[k], 1);
			key(text[k], 0);
		}
	}
	if (do_vt)
		COMBO(KEY_LEFTCTRL, KEY_LEFTALT, KEY_F2);
	if (do_sysrq)
		COMBO(KEY_LEFTALT, KEY_SYSRQ, KEY_H);
	if (do_cad)
		COMBO(KEY_LEFTCTRL, KEY_LEFTALT, KEY_DELETE);
	if (do_power)
		COMBO(KEY_POWER);

	msleep(hold_ms);

	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}
