/* SPDX-License-Identifier: MIT
 * Opt-in fault capture for compositor bring-up. Signal messages go directly to
 * the kernel ring: an asynchronous stderr relay can die before forwarding them.
 */
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
static uintptr_t cache_slide;
static const char *process;

static void say(const char *line)
{
    sysctlbyname("kern.hakios.say", NULL, NULL, (void *)line, strlen(line));
}

static void address(const char *what, uintptr_t value)
{
    char line[512];
    Dl_info info = {0};
    dladdr((void *)value, &info);
    snprintf(line, sizeof(line), "metal_fault: %s[%d] %s %#llx unslid %#llx %s +%#llx (%s)\n",
             process, getpid(), what, (unsigned long long)value,
             (unsigned long long)(value - cache_slide), info.dli_fname ?: "?",
             (unsigned long long)(value - (uintptr_t)info.dli_fbase), info.dli_sname ?: "?");
    say(line);
}

static void exception(id object)
{
    id name = ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("name"));
    id reason = ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("reason"));
    const char *n = ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String"));
    const char *r = ((const char *(*)(id, SEL))objc_msgSend)(reason, sel_registerName("UTF8String"));
    char line[768];
    snprintf(line, sizeof(line), "metal_fault: %s[%d] uncaught %s: %s\n", process, getpid(), n ?: "?", r ?: "?");
    say(line);
}

static void fault(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    char line[256];
    snprintf(line, sizeof(line), "metal_fault: %s[%d] signal %d at %p\n", process, getpid(), sig,
             info ? info->si_addr : NULL);
    say(line);
    if (uc) {
        address("pc", uc->uc_mcontext->__ss.__pc);
        address("lr", uc->uc_mcontext->__ss.__lr);
        uintptr_t fp = uc->uc_mcontext->__ss.__fp, sp = uc->uc_mcontext->__ss.__sp;
        for (unsigned n = 0; n < 20 && fp >= sp && fp - sp < 1024 * 1024 && !(fp & 15); ++n) {
            const uintptr_t *frame = (const uintptr_t *)fp;
            address("frame", frame[1]);
            uintptr_t next = frame[0];
            if (next <= fp) break;
            fp = next;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void hakios_metal_fault_install(void)
{
    char args[2048] = "", line[256]; size_t size = sizeof(args) - 1, cache_size = 0;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) || !strstr(args, "metal_watch=1")) return;
    process = getprogname();
    uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&cache_size);
    if (cache) cache_slide = cache - 0x180000000ull;
    struct sigaction action = {.sa_sigaction=fault, .sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) sigaction(signals[i], &action, NULL);
    void (*set_exception_handler)(void (*)(id)) = dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
    if (set_exception_handler) set_exception_handler(exception);
    snprintf(line, sizeof(line), "metal_fault: installed in %s[%d], cache slide %#llx\n", process, getpid(),
             (unsigned long long)cache_slide);
    say(line);
}

__attribute__((constructor)) static void install(void) { hakios_metal_fault_install(); }
