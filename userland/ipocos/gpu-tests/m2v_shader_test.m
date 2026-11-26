/* SPDX-License-Identifier: MIT
 * Physical Metal -> AIR/SPIR-V -> PanVK shader execution test.
 * The bridge's public, owned runtime_examples AIR fixture is used only by
 * this diagnostic. All dispatch/draw operations use the Metal object API.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

id native_bridge_metal_open(const char *, const char *, uint64_t,
                           void (*)(const char *, const char *));
id native_bridge_metal_library_with_air(const void *, size_t);
size_t native_bridge_metal_status(char *, size_t);
uint64_t native_bridge_metal_first_problem(char *, size_t);

static void missing(const char *cls, const char *sel)
{
    printf("m2v_shader_test: missing -[%s %s]\n", cls, sel);
}

static int result(int failed)
{
    char status[1024] = "", why[512] = "";
    native_bridge_metal_status(status, sizeof(status));
    native_bridge_metal_first_problem(why, sizeof(why));
    printf("m2v_shader_test: status %s\n", status);
    printf("m2v_shader_test: first problem: %s\n", why);
    printf("m2v_shader_test: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}

static void expired(int sig)
{
    printf("m2v_shader_test: stopped by signal %d\n", sig);
    result(1);
    _exit(1);
}

static int completed(id<MTLCommandBuffer> cb, const char *operation)
{
    [cb commit];
    [cb waitUntilCompleted];
    printf("m2v_shader_test: %s status %ld error %s\n", operation,
           (long)cb.status, cb.error.localizedDescription.UTF8String ?: "none");
    return cb != nil && cb.status == MTLCommandBufferStatusCompleted;
}

int main(void)
{
    signal(SIGALRM, expired);
    signal(SIGABRT, expired);
    signal(SIGSEGV, expired);
    signal(SIGBUS, expired);
    alarm(120);
    for (unsigned n = 0; n < 60; ++n) {
        io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
        if (gpu) { IOObjectRelease(gpu); break; }
        if (n == 59) { printf("m2v_shader_test: no HakiosCSF\n"); return 1; }
        sleep(1);
    }
    mkdir("/private/var/tmp", 0777);
    mkdir("/private/var/tmp/m2v-shader", 0700);
    @autoreleasepool {
        id<MTLDevice> dev = native_bridge_metal_open(
            "/redwood/lib/libvulkan_panfrost.dylib", "/private/var/tmp/m2v-shader", 0, missing);
        if (!dev) return result(1);
        printf("m2v_shader_test: device %s\n", dev.name.UTF8String);
        NSData *air = [NSData dataWithContentsOfFile:@"/redwood/fw/runtime_examples.air"];
        if (!air) { printf("m2v_shader_test: missing owned AIR fixture\n"); return result(1); }
        id<MTLLibrary> lib = native_bridge_metal_library_with_air(air.bytes, air.length);
        if (!lib) return result(1);
        printf("m2v_shader_test: AIR library opened\n");
        NSError *error = nil;
        id<MTLFunction> function = [lib newFunctionWithName:@"scale_and_offset"];
        id<MTLComputePipelineState> compute = function ? [dev newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!compute) {
            printf("m2v_shader_test: compute pipeline refused: %s\n", error.localizedDescription.UTF8String ?: "no function");
            return result(1);
        }
        enum { count = 1024, side = 64 };
        float input[count], scale = 2.0f;
        for (unsigned i = 0; i < count; ++i) input[i] = (float)i;
        id<MTLBuffer> src = [dev newBufferWithBytes:input length:sizeof(input) options:MTLResourceStorageModeShared];
        id<MTLBuffer> dst = [dev newBufferWithLength:sizeof(input) options:MTLResourceStorageModeShared];
        id<MTLBuffer> factor = [dev newBufferWithBytes:&scale length:sizeof(scale) options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!src || !dst || !factor || !queue || !dst.contents) return result(1);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        if (!ce) return result(1);
        [ce setComputePipelineState:compute];
        [ce setBuffer:src offset:0 atIndex:0];
        [ce setBuffer:dst offset:0 atIndex:1];
        [ce setBuffer:factor offset:0 atIndex:2];
        [ce dispatchThreadgroups:MTLSizeMake(count / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        if (!completed(cb, "compute shader")) return result(1);
        unsigned good = 0;
        for (unsigned i = 0; i < count; ++i) good += ((const float *)dst.contents)[i] == 3.0f * i;
        printf("m2v_shader_test: compute %u/%u expected floats\n", good, count);
        if (good != count) return result(1);

        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"colored_vertex"];
        pd.fragmentFunction = [lib newFunctionWithName:@"colored_fragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) {
            printf("m2v_shader_test: render pipeline refused: %s\n", error.localizedDescription.UTF8String ?: "unknown");
            return result(1);
        }
        const float positions[][2] = {{-1,1},{0,1},{-1,0},{0,1},{0,0},{-1,0}};
        float colors[6][4];
        for (unsigned i = 0; i < 6; ++i) {
            colors[i][0] = 1.0f; colors[i][1] = 0.2f; colors[i][2] = 0.0f; colors[i][3] = 1.0f;
        }
        id<MTLBuffer> pos = [dev newBufferWithBytes:positions length:sizeof(positions) options:MTLResourceStorageModeShared];
        id<MTLBuffer> col = [dev newBufferWithBytes:colors length:sizeof(colors) options:MTLResourceStorageModeShared];
        if (!pos || !col) return result(1);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:side height:side mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        NSDictionary *properties = @{(id)kIOSurfaceWidth:@(side), (id)kIOSurfaceHeight:@(side),
                                     (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@((uint32_t)'RGBA')};
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        id<MTLTexture> targets[2] = {[dev newTextureWithDescriptor:td],
                                  surface ? [dev newTextureWithDescriptor:td iosurface:surface plane:0] : nil};
        if (!targets[0] || !targets[1]) return result(1);
        int failed = 0;
        for (unsigned t = 0; t < 2; ++t) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = targets[t];
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
            cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
            if (!re) return result(1);
            [re setRenderPipelineState:pipeline];
            [re setVertexBuffer:pos offset:0 atIndex:0];
            [re setVertexBuffer:col offset:0 atIndex:1];
            [re setViewport:(MTLViewport){0, 0, side, side, 0, 1}];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [re endEncoding];
            if (!completed(cb, t ? "IOSurface shader draw" : "shared texture shader draw")) return result(1);
            uint8_t pixels[side * side * 4] = {0};
            if (t) {
                IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
                const uint8_t *base = IOSurfaceGetBaseAddress(surface);
                size_t row = IOSurfaceGetBytesPerRow(surface);
                for (unsigned y = 0; y < side; ++y) memcpy(pixels + y * side * 4, base + y * row, side * 4);
                IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
            } else {
                [targets[t] getBytes:pixels bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0,0,side,side) mipmapLevel:0];
            }
            unsigned exact = 0, orange = 0, blue = 0;
            for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
                const uint8_t *p = pixels + (y * side + x) * 4;
                int inside = x < side / 2 && y < side / 2;
                int o = p[0] == 255 && p[1] == 51 && p[2] == 0 && p[3] == 255;
                int b = p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255;
                orange += o; blue += b; exact += inside ? o : b;
            }
            printf("m2v_shader_test: %s %u/%u exact pixels (%u orange, %u blue)\n",
                   t ? "IOSurface" : "shared texture", exact, side * side, orange, blue);
            failed |= exact != side * side;
        }
        if (surface) CFRelease(surface);
        return result(failed);
    }
}
