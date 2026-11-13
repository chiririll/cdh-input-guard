// SPDX-License-Identifier: GPL-2.0
/*
 * Create a virtual HID boot keyboard through uhid, hold it, then destroy it.
 * The input device shows up below /sys/devices/virtual/misc/uhid/.
 *
 *   -n NAME   device name (default "cdh-guard-test-uhid")
 *   -H MS     how long to keep the device (default 2000)
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const unsigned char rdesc[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,		/* Generic Desktop, Keyboard */
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,		/* modifiers */
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,		/* reserved byte */
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08,		/* LEDs */
	0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
	/* 6 keys, usages up to 0x66 (Keyboard Power -> KEY_POWER) so logind cares */
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x66,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x66, 0x81, 0x00,
	0xc0,
};

static void die(const char *what)
{
	fprintf(stderr, "uhid_kbd: %s: %s\n", what, strerror(errno));
	exit(1);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
	const char *name = "cdh-guard-test-uhid";
	long hold_ms = 2000, end;
	struct uhid_event ev;
	int fd, opt;

	while ((opt = getopt(argc, argv, "n:H:")) != -1) {
		switch (opt) {
		case 'n': name = optarg; break;
		case 'H': hold_ms = atol(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-n name] [-H ms]\n", argv[0]);
			return 2;
		}
	}

	fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open /dev/uhid");

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%s", name);
	memcpy(ev.u.create2.rd_data, rdesc, sizeof(rdesc));
	ev.u.create2.rd_size = sizeof(rdesc);
	ev.u.create2.bus = BUS_USB;
	ev.u.create2.vendor = 0x1209;
	ev.u.create2.product = 0xcd02;
	ev.u.create2.version = 1;
	if (write(fd, &ev, sizeof(ev)) < 0)
		die("UHID_CREATE2");

	printf("%s\n", name);
	fflush(stdout);

	/* Drain the kernel's events (start, open, LED reports) while waiting. */
	end = now_ms() + hold_ms;
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long left = end - now_ms();

		if (left <= 0)
			break;
		if (poll(&pfd, 1, left) > 0 && read(fd, &ev, sizeof(ev)) < 0 &&
		    errno != EINTR && errno != EAGAIN)
			die("read");
	}

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_DESTROY;
	if (write(fd, &ev, sizeof(ev)) < 0)
		die("UHID_DESTROY");
	close(fd);
	return 0;
}
