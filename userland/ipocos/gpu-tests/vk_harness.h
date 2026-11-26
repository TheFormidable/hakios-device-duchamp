/* SPDX-License-Identifier: MIT
 * What the GPU tests share: their lines go to the log that comes back from the phone (kern.hakios.say), Mesa's stderr
 * with them, a crash names its frames, and the kernel's description of the GPU is said first.
 * Define HARNESS_NAME before including.
 */
#ifndef VK_HARNESS_H
#define VK_HARNESS_H
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include "xf86drm.h"
#include "drm-uapi/panthor_drm.h"

#ifndef DRIVER
#define DRIVER "/redwood/lib/libvulkan_panfrost.dylib"
#endif

/* User space's console does not reach the log that comes back from the phone; the kernel's does (kern.hakios.say). */
static void logf_(const char *format, ...)
{
	char line[480];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) return;
	fputs(line, stdout);
	fflush(stdout);
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
}
#define printf logf_
static void say(const char *step) { logf_(HARNESS_NAME ": %s\n", step); }

/* Mesa's own messages (mesa_loge, PANVK_DEBUG=startup) go to stderr: into the same log, a line at a time */
static int err_pipe[2];
static void *forward_stderr(void *unused)
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
		while ((nl = strchr(start, '\n'))) {
			*nl = 0;
			logf_(HARNESS_NAME " [mesa]: %s\n", start);
			start = nl + 1;
		}
		used = strlen(start);
		memmove(buf, start, used);
		if (used == sizeof(buf) - 1) { logf_(HARNESS_NAME " [mesa]: %s\n", buf); used = 0; }
	}
}

/* A crash says where: the signal, the faulting address, the PC and the frames, each named by its image and symbol. */
static void crashed(int sig, siginfo_t *info, void *uc_)
{
	ucontext_t *uc = uc_;
	void *frames[32];
	int n = backtrace(frames, 32);
#if defined(__arm64__)
	logf_(HARNESS_NAME ": CRASH signal %d at address %p, pc %#llx lr %#llx\n", sig, info ? info->si_addr : NULL,
	      uc ? (unsigned long long)uc->uc_mcontext->__ss.__pc : 0ull, uc ? (unsigned long long)uc->uc_mcontext->__ss.__lr : 0ull);
	if (uc) frames[0] = (void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
#else
	(void)uc;
	logf_(HARNESS_NAME ": CRASH signal %d at address %p\n", sig, info ? info->si_addr : NULL);
#endif
	for (int i = 0; i < n; i++) {
		Dl_info d;
		if (dladdr(frames[i], &d) && d.dli_fname)
			logf_(HARNESS_NAME ":   #%d %s +%#lx (%s +%#lx)\n", i, d.dli_sname ? d.dli_sname : "?",
			      d.dli_saddr ? (unsigned long)((char *)frames[i] - (char *)d.dli_saddr) : 0ul,
			      strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname, (unsigned long)((char *)frames[i] - (char *)d.dli_fbase));
		else
			logf_(HARNESS_NAME ":   #%d %p\n", i, frames[i]);
	}
	_exit(128 + sig);
}

/* What the kernel says about the GPU, as Mesa will see it */
static void report_device(void)
{
	int fd = drmOpenWithType("panthor", NULL, DRM_NODE_RENDER);
	if (fd < 0) { logf_(HARNESS_NAME ": no connection for the device report\n"); return; }
	struct drm_panthor_gpu_info g = {0};
	struct drm_panthor_csif_info c = {0};
	struct drm_panthor_dev_query q = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO, .size = sizeof(g), .pointer = (uintptr_t)&g };
	if (!drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q)) {
		logf_(HARNESS_NAME ": gpu_id %#x rev %#x csf_id %#x l2 %#x tiler %#x mem %#x mmu %#x\n", g.gpu_id, g.gpu_rev, g.csf_id,
		      g.l2_features, g.tiler_features, g.mem_features, g.mmu_features);
		logf_(HARNESS_NAME ": thread %#x max_threads %u wg %u barrier %u coherency %#x/%u core_features %#x as %#x\n",
		      g.thread_features, g.max_threads, g.thread_max_workgroup_size, g.thread_max_barrier_size, g.coherency_features,
		      g.selected_coherency, g.core_features, g.as_present);
		logf_(HARNESS_NAME ": texture %#x %#x %#x %#x shader %#llx l2 %#llx tiler %#llx gpu_features %#llx\n", g.texture_features[0],
		      g.texture_features[1], g.texture_features[2], g.texture_features[3], (unsigned long long)g.shader_present,
		      (unsigned long long)g.l2_present, (unsigned long long)g.tiler_present, (unsigned long long)g.gpu_features);
	}
	q = (struct drm_panthor_dev_query){ .type = DRM_PANTHOR_DEV_QUERY_CSIF_INFO, .size = sizeof(c), .pointer = (uintptr_t)&c };
	if (!drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q))
		logf_(HARNESS_NAME ": csif groups %u streams %u regs %u scoreboards %u unpreserved %u\n", c.csg_slot_count, c.cs_slot_count,
		      c.cs_reg_count, c.scoreboard_slot_count, c.unpreserved_cs_reg_count);
	drmClose(fd);
}

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf(HARNESS_NAME ": %s FAILED: %d\n", #call, r_); return 1; } } while (0)


/* The common start: logging, crash report, wait for HakiosCSF, the device report, the driver opened. */
static PFN_vkGetInstanceProcAddr harness_start(int wait_s)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setenv("PANVK_DEBUG", "startup", 0);
	if (pipe(err_pipe) == 0 && dup2(err_pipe[1], 2) == 2) {
		pthread_t t;
		pthread_create(&t, NULL, forward_stderr, NULL);
		pthread_detach(t);
	}
	struct sigaction sa = { .sa_sigaction = crashed, .sa_flags = SA_SIGINFO };
	sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL); sigaction(SIGABRT, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
	for (int s = 0;; s++) {
		io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
		if (service) { IOObjectRelease(service); logf_(HARNESS_NAME ": HakiosCSF after %d s\n", s); break; }
		if (s >= wait_s) { logf_(HARNESS_NAME ": no HakiosCSF: the GPU was not published, nothing tried\n"); return NULL; }
		sleep(1);
	}
	report_device();
	void *driver = dlopen(DRIVER, RTLD_NOW | RTLD_LOCAL);
	if (!driver) { logf_(HARNESS_NAME ": dlopen: %s\n", dlerror()); return NULL; }
	return (PFN_vkGetInstanceProcAddr)dlsym(driver, "vk_icdGetInstanceProcAddr");
}
#endif
