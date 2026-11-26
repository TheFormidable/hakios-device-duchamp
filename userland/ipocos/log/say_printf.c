/* SPDX-License-Identifier: MIT
 * See say_printf.h. */
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

int hakios_say_printf(const char *format, ...)
{
	char line[480];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) return n;
	fputs(line, stdout);
	fflush(stdout);
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	return n;
}

static int err_pipe[2];
static void *forward(void *unused)
{
	char buf[400];
	size_t used = 0;
	(void)unused;
	for (;;) {
		ssize_t n = read(err_pipe[0], buf + used, sizeof(buf) - 1 - used);
		if (n <= 0) return NULL;
		used += (size_t)n;
		buf[used] = 0;
		char *start = buf, *nl;
		while ((nl = strchr(start, '\n'))) { *nl = 0; hakios_say_printf("[stderr] %s\n", start); start = nl + 1; }
		used = strlen(start);
		memmove(buf, start, used);
		if (used == sizeof(buf) - 1) { hakios_say_printf("[stderr] %s\n", buf); used = 0; }
	}
}

__attribute__((constructor)) static void say_printf_start(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setenv("HAKIOS_VK_LOG", "1", 0);   /* PanVK's own errors, with their reasons (panvk_instance.c) */
	if (pipe(err_pipe) == 0 && dup2(err_pipe[1], 2) == 2) {
		pthread_t t;
		pthread_create(&t, NULL, forward, NULL);
		pthread_detach(t);
	}
	fprintf(stderr, "say_printf: stderr reaches the log\n");
}
