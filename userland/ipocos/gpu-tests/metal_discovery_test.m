/* SPDX-License-Identifier: MIT
 * Exercise Metal.framework discovery, independently of the linked bridge tests.
 * The temporary process label selects the plugin's existing compositor-only
 * visibility policy. It does not turn this process into the real window server.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static void expired(int sig)
{
    printf("metal_discovery_test: FAIL signal %d\n", sig);
    _exit(1);
}

int main(void)
{
    signal(SIGALRM, expired); signal(SIGABRT, expired); signal(SIGSEGV, expired);
    alarm(90);
    @autoreleasepool {
        io_service_t service = 0;
        for (unsigned i = 0; i < 45 && !service; ++i) {
            service = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
            io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
            if (!gpu && service) { IOObjectRelease(service); service = 0; }
            if (gpu) IOObjectRelease(gpu);
            if (!service) sleep(1);
        }
        if (!service) { printf("metal_discovery_test: FAIL no IOAcceleratorES\n"); return 1; }
        CFTypeRef name = IORegistryEntryCreateCFProperty(service, CFSTR("MetalPluginName"), NULL, 0);
        CFTypeRef cls = IORegistryEntryCreateCFProperty(service, CFSTR("MetalPluginClassName"), NULL, 0);
        printf("metal_discovery_test: registry plugin %s class %s\n",
               [(__bridge id)name description].UTF8String ?: "nil", [(__bridge id)cls description].UTF8String ?: "nil");
        if (!name || CFGetTypeID(name) != CFStringGetTypeID()) return 1;
        NSString *path = [NSString stringWithFormat:@"/System/Library/Extensions/%@.bundle", (__bridge NSString *)name];
        printf("metal_discovery_test: bundle path %s normalized %s\n", path.UTF8String, path.stringByStandardizingPath.UTF8String);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:path];
        if (![bundle loadAndReturnError:&error]) {
            printf("metal_discovery_test: FAIL load: %s\n", error.localizedDescription.UTF8String ?: "no bundle"); return 1;
        }
        printf("metal_discovery_test: bundle loaded; diagnostic PID %d uses compositor visibility policy\n", getpid());
        setprogname("backboardd");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        printf("metal_discovery_test: framework device %s class %s\n", device.name.UTF8String ?: "nil",
               device ? object_getClassName(device) : "nil");
        if (!device || strcmp(object_getClassName(device), "NativeBridgeMetalDevice")) {
            printf("metal_discovery_test: FAIL framework did not select the Vulkan bridge\n"); return 1;
        }
        id<MTLBuffer> buffer = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!buffer || !buffer.contents || !queue) return 1;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit fillBuffer:buffer range:NSMakeRange(0, 1024) value:0x39];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        unsigned good = 0;
        for (unsigned i = 0; i < 256; ++i) good += ((uint32_t *)buffer.contents)[i] == 0x39393939;
        printf("metal_discovery_test: framework GPU fill %u/256 words, status %ld\n", good, (long)command.status);
        int failed = good != 256 || command.status != MTLCommandBufferStatusCompleted;
        printf("metal_discovery_test: %s\n", failed ? "FAIL" : "PASS");
        if (name) CFRelease(name); if (cls) CFRelease(cls); IOObjectRelease(service);
        return failed;
    }
}
