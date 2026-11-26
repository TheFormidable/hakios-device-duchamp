/* SPDX-License-Identifier: MIT
 * The first shader on the POCO X6 Pro's Mali-G615: vkCmdFillBuffer through Mesa's PanVK, which on Mali v10+ fills
 * with a compute kernel (libpan's panlib_fill) run by the shader cores. The CPU then checks every word.
 *
 *   vk_fill_test [seconds to wait for HakiosCSF]
 *
 * No Vulkan loader on Darwin: the driver is opened directly and asked for its entry points.
 * Every step is said before it is made, so that a hang names its place.
 */
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
#define WORDS 16384u
#define VALUE 0x53414d49u

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
static void say(const char *step) { logf_("vk_fill_test: %s\n", step); }

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
			logf_("vk_fill_test [mesa]: %s\n", start);
			start = nl + 1;
		}
		used = strlen(start);
		memmove(buf, start, used);
		if (used == sizeof(buf) - 1) { logf_("vk_fill_test [mesa]: %s\n", buf); used = 0; }
	}
}

/* A crash says where: the signal, the faulting address, the PC and the frames, each named by its image and symbol. */
static void crashed(int sig, siginfo_t *info, void *uc_)
{
	ucontext_t *uc = uc_;
	void *frames[32];
	int n = backtrace(frames, 32);
#if defined(__arm64__)
	logf_("vk_fill_test: CRASH signal %d at address %p, pc %#llx lr %#llx\n", sig, info ? info->si_addr : NULL,
	      uc ? (unsigned long long)uc->uc_mcontext->__ss.__pc : 0ull, uc ? (unsigned long long)uc->uc_mcontext->__ss.__lr : 0ull);
	if (uc) frames[0] = (void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
#else
	(void)uc;
	logf_("vk_fill_test: CRASH signal %d at address %p\n", sig, info ? info->si_addr : NULL);
#endif
	for (int i = 0; i < n; i++) {
		Dl_info d;
		if (dladdr(frames[i], &d) && d.dli_fname)
			logf_("vk_fill_test:   #%d %s +%#lx (%s +%#lx)\n", i, d.dli_sname ? d.dli_sname : "?",
			      d.dli_saddr ? (unsigned long)((char *)frames[i] - (char *)d.dli_saddr) : 0ul,
			      strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname, (unsigned long)((char *)frames[i] - (char *)d.dli_fbase));
		else
			logf_("vk_fill_test:   #%d %p\n", i, frames[i]);
	}
	_exit(128 + sig);
}

/* What the kernel says about the GPU, as Mesa will see it */
static void report_device(void)
{
	int fd = drmOpenWithType("panthor", NULL, DRM_NODE_RENDER);
	if (fd < 0) { logf_("vk_fill_test: no connection for the device report\n"); return; }
	struct drm_panthor_gpu_info g = {0};
	struct drm_panthor_csif_info c = {0};
	struct drm_panthor_dev_query q = { .type = DRM_PANTHOR_DEV_QUERY_GPU_INFO, .size = sizeof(g), .pointer = (uintptr_t)&g };
	if (!drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q)) {
		logf_("vk_fill_test: gpu_id %#x rev %#x csf_id %#x l2 %#x tiler %#x mem %#x mmu %#x\n", g.gpu_id, g.gpu_rev, g.csf_id,
		      g.l2_features, g.tiler_features, g.mem_features, g.mmu_features);
		logf_("vk_fill_test: thread %#x max_threads %u wg %u barrier %u coherency %#x/%u core_features %#x as %#x\n",
		      g.thread_features, g.max_threads, g.thread_max_workgroup_size, g.thread_max_barrier_size, g.coherency_features,
		      g.selected_coherency, g.core_features, g.as_present);
		logf_("vk_fill_test: texture %#x %#x %#x %#x shader %#llx l2 %#llx tiler %#llx gpu_features %#llx\n", g.texture_features[0],
		      g.texture_features[1], g.texture_features[2], g.texture_features[3], (unsigned long long)g.shader_present,
		      (unsigned long long)g.l2_present, (unsigned long long)g.tiler_present, (unsigned long long)g.gpu_features);
	}
	q = (struct drm_panthor_dev_query){ .type = DRM_PANTHOR_DEV_QUERY_CSIF_INFO, .size = sizeof(c), .pointer = (uintptr_t)&c };
	if (!drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q))
		logf_("vk_fill_test: csif groups %u streams %u regs %u scoreboards %u unpreserved %u\n", c.csg_slot_count, c.cs_slot_count,
		      c.cs_reg_count, c.scoreboard_slot_count, c.unpreserved_cs_reg_count);
	drmClose(fd);
}

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("vk_fill_test: %s FAILED: %d\n", #call, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
	int wait_s = argc > 1 ? atoi(argv[1]) : 120;
	setvbuf(stdout, NULL, _IOLBF, 0);
	setenv("PANVK_DEBUG", "startup", 0);
	setenv("MESA_LOG_LEVEL", "info", 0);
	if (pipe(err_pipe) == 0 && dup2(err_pipe[1], 2) == 2) {
		pthread_t t;
		pthread_create(&t, NULL, forward_stderr, NULL);
		pthread_detach(t);
	}

	say("waiting for the kernel's HakiosCSF service (mali=3 mali_steps>=5 csf_transport=1)");
	for (int s = 0;; s++) {
		io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
		if (service) { IOObjectRelease(service); printf("vk_fill_test: HakiosCSF after %d s\n", s); break; }
		if (s >= wait_s) { say("no HakiosCSF: the GPU was not published, nothing tried"); return 2; }
		sleep(1);
	}

	struct sigaction sa = { .sa_sigaction = crashed, .sa_flags = SA_SIGINFO };
	sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL); sigaction(SIGABRT, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
	report_device();
	say("dlopen " DRIVER);
	void *driver = dlopen(DRIVER, RTLD_NOW | RTLD_LOCAL);
	if (!driver) { printf("vk_fill_test: dlopen: %s\n", dlerror()); return 1; }
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(driver, "vk_icdGetInstanceProcAddr");
	if (!gipa) { say("no vk_icdGetInstanceProcAddr"); return 1; }
#define I(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
	VkInstance instance = VK_NULL_HANDLE;
	I(vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_fill_test", .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	say("vkCreateInstance");
	CHECK(vkCreateInstance(&ici, NULL, &instance));
	I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
	I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);

	uint32_t count = 1;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	say("vkEnumeratePhysicalDevices");
	VkResult er = vkEnumeratePhysicalDevices(instance, &count, &gpu);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !count) { printf("vk_fill_test: no physical device (%d, %u)\n", er, count); return 1; }
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(gpu, &props);
	printf("vk_fill_test: device \"%s\", Vulkan %u.%u.%u, driver %#x\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
	       VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion);

	uint32_t families = 8;
	VkQueueFamilyProperties qf[8];
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, qf);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < families; i++)
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
	if (family == UINT32_MAX) { say("no compute queue"); return 1; }
	printf("vk_fill_test: queue family %u flags %#x\n", family, qf[family].queueFlags);

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice device = VK_NULL_HANDLE;
	say("vkCreateDevice (VM, tiler heap, queue group)");
	CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
	D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory);
	D(vkMapMemory); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkCmdFillBuffer);
	D(vkEndCommandBuffer); D(vkCreateFence); D(vkQueueSubmit); D(vkWaitForFences); D(vkInvalidateMappedMemoryRanges);
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WORDS * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buffer;
	CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, buffer, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = i; break; }
	if (type == UINT32_MAX) { say("no host-visible memory type"); return 1; }
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type };
	VkDeviceMemory memory;
	say("vkAllocateMemory + map");
	CHECK(vkAllocateMemory(device, &mai, NULL, &memory));
	CHECK(vkBindBufferMemory(device, buffer, memory, 0));
	volatile uint32_t *words = NULL;
	CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words));
	for (uint32_t i = 0; i < WORDS; i++) words[i] = 0xdeadbeefu;   /* the canary the GPU must overwrite */

	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
	VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	say("recording vkCmdFillBuffer");
	CHECK(vkBeginCommandBuffer(cmd, &cbi));
	vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, VALUE);
	CHECK(vkEndCommandBuffer(cmd));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(device, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	say("vkQueueSubmit");
	CHECK(vkQueueSubmit(queue, 1, &si, fence));
	say("vkWaitForFences (5 s)");
	CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
	VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memory, .size = VK_WHOLE_SIZE };
	vkInvalidateMappedMemoryRanges(device, 1, &range);

	uint32_t good = 0, canary = 0;
	for (uint32_t i = 0; i < WORDS; i++) { good += words[i] == VALUE; canary += words[i] == 0xdeadbeefu; }
	printf("vk_fill_test: %u of %u words hold %#x, %u still the canary, first %#x last %#x\n", good, WORDS, VALUE, canary, words[0], words[WORDS - 1]);
	say(good == WORDS ? "SHADER PASSED: the G615's shader cores filled the buffer (Vulkan, PanVK)" : "FAILED: the buffer is not what the GPU should have written");
	return good == WORDS ? 0 : 3;
}
