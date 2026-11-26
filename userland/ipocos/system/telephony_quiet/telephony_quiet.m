/* SPDX-License-Identifier: MIT
 * libhakios.telephony_quiet.dylib, loaded into SpringBoard (etc/inject.conf).
 *
 * HakiOS has no modem and no CommCenter. SpringBoard's telephony manager asks CommCenter anyway, gets
 * "CTServerConnection returned error: (2, 5)" ((os/kern) failure), and -[SBTelephonyManager _serverConnectionDidError:]
 * schedules -[SBTelephonyManager _handleTelephonyDaemonRestart] with -performSelector:afterDelay:, which runs updateCalls
 * and updateSpringBoard, asks CommCenter again, fails again: about once a second for as long as the phone is on (659 times
 * in the first 12 minutes of a boot, 17 Nov). Every round resets the idle timer ("updateIdleTimerForReason:
 * IdleTimerReset"), so iOS never dimmed or locked by itself, and the loop kept a core busy.
 *
 * Here a restart is let through until three errors have come within 30 s; after that it is skipped while errors keep
 * coming, and the loop ends because nothing schedules another. 30 s without an error lets the next restart through
 * again (a telephony daemon that really restarted). Selectors as in iOS 16.7 (20H392): checked at load, nothing is
 * replaced when they are missing.
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/sysctl.h>

#define ERRORS_TO_QUIET 3
#define QUIET_WINDOW_S  30.0

static void say(NSString *text)
{
	const char *line = [[text stringByAppendingString:@"\n"] UTF8String];
	sysctlbyname("kern.hakios.say", NULL, NULL, (void *)line, strlen(line));
}

static IMP orig_error, orig_restart;
static _Atomic unsigned errors_in_window, skipped, passed;
static _Atomic double window_start = 0, last_error = 0;

typedef struct { int domain, error; } hakios_cterror;   /* CTError, by value ("{?=ii}") */

static void quiet_error(id self, SEL sel, hakios_cterror error)
{
	double now = CFAbsoluteTimeGetCurrent();
	if (now - atomic_load(&window_start) > QUIET_WINDOW_S) {
		atomic_store(&window_start, now);
		atomic_store(&errors_in_window, 0);
	}
	atomic_fetch_add(&errors_in_window, 1);
	atomic_store(&last_error, now);
	((void (*)(id, SEL, hakios_cterror))orig_error)(self, sel, error);
}

static void quiet_restart(id self, SEL sel)
{
	double now = CFAbsoluteTimeGetCurrent();
	if (atomic_load(&errors_in_window) >= ERRORS_TO_QUIET && now - atomic_load(&last_error) < QUIET_WINDOW_S) {
		unsigned n = atomic_fetch_add(&skipped, 1);
		if (n == 0 || n % 100 == 0)
			say([NSString stringWithFormat:@"hakios: telephony: no CommCenter here: restart %u skipped (%u errors in %.0f s; %u restarts let through)",
			    n + 1, atomic_load(&errors_in_window), QUIET_WINDOW_S, atomic_load(&passed)]);
		return;
	}
	atomic_fetch_add(&passed, 1);
	((void (*)(id, SEL))orig_restart)(self, sel);
}

static void install(int late)
{
	Class cls = objc_getClass("SBTelephonyManager");
	if (!cls && !late) {                    /* SpringBoard's classes may not be registered yet: try again from its run loop */
		dispatch_async(dispatch_get_main_queue(), ^{ install(1); });
		return;
	}
	Method error = cls ? class_getInstanceMethod(cls, sel_registerName("_serverConnectionDidError:")) : NULL;
	Method restart = cls ? class_getInstanceMethod(cls, sel_registerName("_handleTelephonyDaemonRestart")) : NULL;
	if (!error || !restart || strcmp(method_getTypeEncoding(error) ?: "", "v24@0:8{?=ii}16") != 0 ||
	    strcmp(method_getTypeEncoding(restart) ?: "", "v16@0:8") != 0) {
		say([NSString stringWithFormat:@"hakios: telephony: SBTelephonyManager %s, error handler %s, restart %s: left alone",
		    cls ? "found" : "missing", error ? method_getTypeEncoding(error) : "missing",
		    restart ? method_getTypeEncoding(restart) : "missing"]);
		return;
	}
	orig_error = method_setImplementation(error, (IMP)quiet_error);
	orig_restart = method_setImplementation(restart, (IMP)quiet_restart);
	say(@"hakios: telephony: restarts after repeated CommCenter errors are skipped (idle timer can run)");
}

__attribute__((constructor)) static void telephony_quiet_init(void)
{
	install(0);
}
