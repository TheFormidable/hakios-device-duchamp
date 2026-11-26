/* SPDX-License-Identifier: MIT
 * hal_hide: our audio device in place of Apple's HAL plug-ins, when the root is the UFS system image (trial 103).
 *
 * Trial 102: mediaserverd sent, without pause, an XPC connection's message to com.apple.BTAudioHALPlugin.xpc (CoreAudio's
 * Bluetooth HAL plug-in, which cannot run on this phone), and launchd answered each: 120-125% of a core at rest, always.
 * The image's init hides Apple's HAL plug-ins behind /redwood/hal (only HakiosAudio.driver) when it mounts a flash root
 * (userland/src/init.c, flash_root); here the root is the system image on UFS and that never ran. Run from etc/rc before
 * launchd (and so before mediaserverd), it binds the same directory over the same place.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

int main(void)
{
	struct stat st;

	/* an empty /redwood/hal would leave mediaserverd no audio device at all */
	if (stat("/redwood/hal/HakiosAudio.driver", &st) != 0) {
		printf("hal_hide: /redwood/hal has no HakiosAudio.driver; Apple's plug-ins left as they are\n");
		return 1;
	}
	if (mount("bindfs", "/System/Library/Audio/Plug-Ins/HAL", MNT_RDONLY, (void *)"/redwood/hal") != 0) {
		printf("hal_hide: bindfs /redwood/hal on /System/Library/Audio/Plug-Ins/HAL: %s\n", strerror(errno));
		return 1;
	}
	printf("hal_hide: /System/Library/Audio/Plug-Ins/HAL is /redwood/hal (our audio device, not Apple's plug-ins)\n");
	return 0;
}
