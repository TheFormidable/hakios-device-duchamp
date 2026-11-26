/* SPDX-License-Identifier: MIT
 * hakios_ptpd: the phone's still-image camera over USB (interface 0 of "PTP + Apple Mobile Device", kernel usb=2,
 * /dev/hakios-ptp), what ptpd is on an iPhone: the camera roll's files under /var/mobile/Media/DCIM, read only, for
 * Photos and Image Capture on a Mac, Explorer and Photos on Windows, gphoto2 and file managers on Linux.
 * Started from /redwood/etc/rc.jobs. Protocol: ptp_core.h.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "ptp_core.h"

#define HAKIOS_USB_END _IO('h', 1)
#define DCIM "/var/mobile/Media/DCIM"

static int usb = -1;
static struct ptp ptp;
static uint64_t write_errors;

static void say(const char *format, ...)
{
	char line[300];
	va_list ap;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
}

/* stable handles: a path keeps the handle it was first given */
static char *paths[PTP_MAX_OBJECTS];
static uint32_t path_count;

static uint32_t handle_for(const char *path)
{
	for (uint32_t i = 0; i < path_count; i++)
		if (strcmp(paths[i], path) == 0)
			return i + 1;
	if (path_count >= PTP_MAX_OBJECTS)
		return 0;
	paths[path_count] = strdup(path);
	return ++path_count;
}

static uint16_t format_for(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot)
		return PTP_FORMAT_UNDEFINED;
	if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))
		return PTP_FORMAT_JPEG;
	if (!strcasecmp(dot, ".png"))
		return PTP_FORMAT_PNG;
	if (!strcasecmp(dot, ".gif"))
		return PTP_FORMAT_GIF;
	if (!strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff"))
		return PTP_FORMAT_TIFF;
	if (!strcasecmp(dot, ".mov"))
		return PTP_FORMAT_QUICKTIME;
	if (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v"))
		return PTP_FORMAT_MPEG;
	return PTP_FORMAT_UNDEFINED;      /* HEIC and the rest: still copied */
}

static void fill(struct ptp_object *o, uint32_t handle, uint32_t parent, uint16_t format, const char *name,
    const struct stat *st)
{
	memset(o, 0, sizeof(*o));
	o->handle = handle;
	o->parent = parent;
	o->format = format;
	o->size = format == PTP_FORMAT_ASSOCIATION ? 0 : (uint64_t)st->st_size;
	strlcpy(o->name, name, sizeof(o->name));
	struct tm tm;
	gmtime_r(&st->st_mtime, &tm);
	strftime(o->date, sizeof(o->date), "%Y%m%dT%H%M%S", &tm);
}

/* DCIM at the root, its folders (100APPLE, ...) under it, their files under those */
static int scan(void *ctx, struct ptp_object *objects, int max)
{
	(void)ctx;
	int n = 0;
	struct stat st;
	if (stat(DCIM, &st) != 0 || max < 1)
		return 0;
	uint32_t dcim = handle_for(DCIM);
	fill(&objects[n++], dcim, 0, PTP_FORMAT_ASSOCIATION, "DCIM", &st);
	DIR *top = opendir(DCIM);
	struct dirent *d;
	while (top && (d = readdir(top)) && n < max) {
		if (d->d_name[0] == '.')
			continue;
		char folder[1024];
		snprintf(folder, sizeof(folder), "%s/%s", DCIM, d->d_name);
		if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		uint32_t fh = handle_for(folder);
		if (!fh)
			break;
		fill(&objects[n++], fh, dcim, PTP_FORMAT_ASSOCIATION, d->d_name, &st);
		DIR *inner = opendir(folder);
		struct dirent *e;
		while (inner && (e = readdir(inner)) && n < max) {
			if (e->d_name[0] == '.')
				continue;
			char file[1280];
			snprintf(file, sizeof(file), "%s/%s", folder, e->d_name);
			if (stat(file, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			uint32_t h = handle_for(file);
			if (!h)
				break;
			fill(&objects[n++], h, fh, format_for(e->d_name), e->d_name, &st);
		}
		if (inner)
			closedir(inner);
	}
	if (top)
		closedir(top);
	struct statfs fs;
	if (statfs("/private/var", &fs) == 0) {
		ptp.capacity = (uint64_t)fs.f_blocks * fs.f_bsize;
		ptp.free_bytes = (uint64_t)fs.f_bavail * fs.f_bsize;
	}
	return n;
}

static long read_object(void *ctx, const struct ptp_object *object, uint64_t offset, uint8_t *buffer, size_t length)
{
	(void)ctx;
	static int fd = -1;
	static uint32_t open_handle;
	if (object->handle == 0 || object->handle > path_count)
		return -1;
	if (fd < 0 || open_handle != object->handle) {
		if (fd >= 0)
			close(fd);
		fd = open(paths[object->handle - 1], O_RDONLY);
		open_handle = object->handle;
		if (fd < 0)
			return -1;
	}
	return (long)pread(fd, buffer, length, (off_t)offset);
}

static void out(void *ctx, const uint8_t *bytes, size_t length)
{
	(void)ctx;
	while (length) {
		ssize_t w = write(usb, bytes, length);
		if (w <= 0) {
			write_errors++;
			return;
		}
		bytes += w;
		length -= (size_t)w;
	}
}

static void end(void *ctx)
{
	(void)ctx;
	if (ioctl(usb, HAKIOS_USB_END) != 0)
		write_errors++;
}

static const struct ptp_ops ops = {scan, read_object, out, end};

static char *copy_answer(const char *key)
{
	static void *mg;
	static CFTypeRef (*answer)(CFStringRef);
	if (!mg && (mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY)))
		answer = (CFTypeRef (*)(CFStringRef))dlsym(mg, "MGCopyAnswer");
	if (!answer)
		return strdup("");
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef v = answer(k);
	CFRelease(k);
	char text[128] = "";
	if (v && CFGetTypeID(v) == CFStringGetTypeID())
		CFStringGetCString(v, text, sizeof(text), kCFStringEncodingUTF8);
	if (v)
		CFRelease(v);
	return strdup(text);
}

int main(void)
{
	for (int i = 0; i < 300 && (usb = open("/dev/hakios-ptp", O_RDWR)) < 0; i++)
		usleep(100000);
	if (usb < 0) {
		say("hakios_ptpd: no /dev/hakios-ptp (boot-arg usb=2?): %s\n", strerror(errno));
		return 1;
	}
	ptp_init(&ptp, &ops, NULL);
	ptp.model = "iPhone";
	ptp.version = copy_answer("ProductVersion");
	ptp.serial = copy_answer("UniqueDeviceID");
	struct ptp_object probe[1];
	say("hakios_ptpd: the camera serves %s (%d objects at the root level now)\n", DCIM, scan(NULL, probe, 1));
	static uint8_t buffer[16384];
	uint64_t said_commands = 0;
	time_t last = 0;
	for (;;) {
		ssize_t n = read(usb, buffer, sizeof(buffer));
		if (n < 0) {
			usleep(20000);
			continue;
		}
		ptp_input(&ptp, buffer, (size_t)n);
		time_t now = time(NULL);
		if (ptp.commands != said_commands && now - last >= 5) {
			say("hakios_ptpd: %llu commands, %llu bytes out, %d objects, session %u, %llu write errors\n",
			    ptp.commands, ptp.bytes_out, ptp.count, ptp.session, write_errors);
			said_commands = ptp.commands;
			last = now;
		}
	}
}
