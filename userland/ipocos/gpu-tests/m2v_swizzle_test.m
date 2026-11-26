/* SPDX-License-Identifier: MIT
 * Does an R8 texture's swizzle survive metal2vulkan over PanVK on the G615?
 *
 * The lock screen's and the home screen's poster (CollectionsPoster) is made of 'L008' surfaces, luminance masks that
 * CoreAnimation samples as R8Unorm; one swirl of the wallpaper came out red where it is blue (trials 22-28), as a
 * luminance read (L, 0, 0, 1) instead of (L, L, L, 1) would. CoreAnimation's own seven-tap blur, its offsets all 0 and
 * its weights summing to 1, copies its input: each case samples a 64x64 R8 texture of 0x80 into BGRA8 and reads the
 * middle pixel back.
 *   plain   R8, no swizzle                    Metal reads (0.5, 0, 0, 1)
 *   swizzle R8, swizzle (R, R, R, 1)          Metal reads (0.5, 0.5, 0.5, 1)
 *   view    plain R8's view, swizzle (R,R,R,1) the same
 *   surface an 'L008' IOSurface, R8, swizzle   the same
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

id native_bridge_metal_open(const char *, const char *, uint64_t, void (*)(const char *, const char *));
size_t native_bridge_metal_status(char *, size_t);
uint64_t native_bridge_metal_first_problem(char *, size_t);

enum { side = 64 };

static int result(int failed)
{
    char status[1024] = "", why[512] = "";
    native_bridge_metal_status(status, sizeof(status));
    native_bridge_metal_first_problem(why, sizeof(why));
    printf("m2v_swizzle_test: status %s\n", status);
    printf("m2v_swizzle_test: first problem %s\n", why);
    printf("m2v_swizzle_test: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
static void missing(const char *cls, const char *sel) { printf("m2v_swizzle_test: missing %s %s\n", cls, sel); }
static void expired(int sig) { printf("m2v_swizzle_test: signal %d\n", sig); result(1); _exit(1); }

/* draw `src` through the copying blur into a fresh BGRA8 target; the middle pixel as R, G, B, A (0..255), or -1s */
static void copy_through_blur(id<MTLDevice> dev, id<MTLRenderPipelineState> pipeline, id<MTLTexture> src, int rgba[4])
{
    const float vertices[6][12] = {
        {-1,1,0,1, 0,0, 0,0, 1,1,1,1}, {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1},
        {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {1,-1,0,1, 1,1, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1}
    };
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float texmat[4] = {1,1,0,0}, edr = 1;
    /* AIR reflection: offset[8] at 0 (all 0: every tap reads the same texel), weight[2] at 64: 0.25 + 6 x 0.125 = 1 */
    float uniforms[24] = {0};
    uniforms[16] = 0.25f; uniforms[17] = uniforms[18] = uniforms[19] = 0.125f;
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = -1;
    id<MTLBuffer> vb = [dev newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    id<MTLBuffer> matrix = [dev newBufferWithBytes:identity length:sizeof(identity) options:MTLResourceStorageModeShared];
    id<MTLBuffer> uv = [dev newBufferWithBytes:texmat length:sizeof(texmat) options:MTLResourceStorageModeShared];
    id<MTLBuffer> weights = [dev newBufferWithBytes:uniforms length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    id<MTLBuffer> brightness = [dev newBufferWithBytes:&edr length:sizeof(edr) options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:side mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:td];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (!src || !vb || !matrix || !uv || !weights || !brightness || !dst || !sampler || !queue) return;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = dst;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 0);   /* blue, alpha 0: an undrawn pixel */
    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return;
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vb offset:0 atIndex:0];
    [encoder setVertexBuffer:matrix offset:0 atIndex:2];
    [encoder setVertexBuffer:uv offset:0 atIndex:3];
    [encoder setFragmentBuffer:weights offset:0 atIndex:1];
    [encoder setFragmentBuffer:brightness offset:0 atIndex:6];
    [encoder setFragmentTexture:src atIndex:3];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder setViewport:(MTLViewport){0, 0, side, side, 0, 1}];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        printf("m2v_swizzle_test: draw status %ld %s\n", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "none");
        return;
    }
    uint8_t pixels[side * side * 4] = {0};
    [dst getBytes:pixels bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
    const uint8_t *p = pixels + ((side / 2) * side + side / 2) * 4;   /* BGRA */
    rgba[0] = p[2]; rgba[1] = p[1]; rgba[2] = p[0]; rgba[3] = p[3];
}

static int check(const char *name, const int got[4], int r, int g, int b, int a)
{
    int right = abs(got[0] - r) <= 2 && abs(got[1] - g) <= 2 && abs(got[2] - b) <= 2 && abs(got[3] - a) <= 2;
    printf("m2v_swizzle_test: %-8s read (%d, %d, %d, %d), Metal reads (%d, %d, %d, %d): %s\n", name, got[0], got[1], got[2], got[3],
           r, g, b, a, right ? "right" : "WRONG");
    return right;
}

int main(void)
{
    signal(SIGALRM, expired); signal(SIGABRT, expired); signal(SIGSEGV, expired); signal(SIGBUS, expired);
    alarm(100);
    for (unsigned n = 0; n < 45; ++n) {
        io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
        if (service) { IOObjectRelease(service); break; }
        if (n == 44) return result(1);
        sleep(1);
    }
    @autoreleasepool {
        id<MTLDevice> dev = native_bridge_metal_open("/redwood/lib/libvulkan_panfrost.dylib", "/private/var/tmp/m2v-swizzle", 0, missing);
        if (!dev) return result(1);
        NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/redwood/share/ca-share/QuartzCore-default.metallib"] error:&error];
        if (!lib) { printf("m2v_swizzle_test: library %s\n", error.localizedDescription.UTF8String); return result(1); }
        MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
        layout.layouts[0].stride = 48;
        layout.attributes[0].format = MTLVertexFormatFloat4; layout.attributes[0].offset = 0; layout.attributes[0].bufferIndex = 0;
        layout.attributes[1].format = MTLVertexFormatFloat2; layout.attributes[1].offset = 16; layout.attributes[1].bufferIndex = 0;
        layout.attributes[3].format = MTLVertexFormatFloat4; layout.attributes[3].offset = 32; layout.attributes[3].bufferIndex = 0;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"downsample_blur_vert_lpf"];
        pd.fragmentFunction = [lib newFunctionWithName:@"downsample_blur_2_frag_lpf"];
        pd.vertexDescriptor = layout;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) { printf("m2v_swizzle_test: pipeline %s\n", error.localizedDescription.UTF8String); return result(1); }

        const MTLTextureSwizzleChannels luminance = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                                                 MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        uint8_t texels[side * side];
        memset(texels, 0x80, sizeof(texels));
        int got[4], right = 0;

        MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:side height:side mipmapped:NO];
        rd.storageMode = MTLStorageModeShared; rd.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> plain = [dev newTextureWithDescriptor:rd];
        [plain replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0 withBytes:texels bytesPerRow:side];
        copy_through_blur(dev, pipeline, plain, got);
        right += check("plain", got, 128, 0, 0, 255);

        rd.swizzle = luminance;
        id<MTLTexture> swizzled = [dev newTextureWithDescriptor:rd];
        [swizzled replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0 withBytes:texels bytesPerRow:side];
        copy_through_blur(dev, pipeline, swizzled, got);
        right += check("swizzle", got, 128, 128, 128, 255);

        id<MTLTexture> view = [plain newTextureViewWithPixelFormat:MTLPixelFormatR8Unorm textureType:MTLTextureType2D
                                                           levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:luminance];
        copy_through_blur(dev, pipeline, view, got);
        right += check("view", got, 128, 128, 128, 255);

        NSDictionary *properties = @{(id)kIOSurfaceWidth: @(side), (id)kIOSurfaceHeight: @(side), (id)kIOSurfaceBytesPerElement: @1,
                                     (id)kIOSurfaceBytesPerRow: @(side), (id)kIOSurfacePixelFormat: @(0x4c303038) /* 'L008' */};
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (surface) {
            IOSurfaceLock(surface, 0, NULL);
            uint8_t *base = IOSurfaceGetBaseAddress(surface);
            for (size_t y = 0; y < side; ++y) memset(base + y * IOSurfaceGetBytesPerRow(surface), 0x80, side);
            IOSurfaceUnlock(surface, 0, NULL);
            MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:side height:side mipmapped:NO];
            sd.usage = MTLTextureUsageShaderRead; sd.swizzle = luminance;
            id<MTLTexture> onSurface = [dev newTextureWithDescriptor:sd iosurface:surface plane:0];
            copy_through_blur(dev, pipeline, onSurface, got);
        } else {
            got[0] = got[1] = got[2] = got[3] = -1;
            printf("m2v_swizzle_test: no IOSurface\n");
        }
        right += check("surface", got, 128, 128, 128, 255);

        /* ASTC 4x4 LDR (format 204), what icons and glyphs of the system's asset catalogs are: the App Library's
         * folder thumbnails, the camera button and the TV icon came out as white or black squares. Every block a
         * void-extent block of one colour: bits 0-8 0x1fc, bit 9 LDR, bits 10-11 set, the extents all ones (none), then
         * R, G, B, A as 16-bit UNORM: (1, 0x8080/0xffff, 0, 1), which Metal reads as (255, 128, 0, 255). */
        static const uint8_t block[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x80, 0x00, 0x00, 0xff, 0xff};
        uint8_t blocks[(side / 4) * (side / 4) * 16];
        for (size_t i = 0; i < sizeof(blocks); i += 16) memcpy(blocks + i, block, 16);
        MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:side height:side mipmapped:NO];
        ad.storageMode = MTLStorageModeShared; ad.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> astc = [dev newTextureWithDescriptor:ad];
        [astc replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0 withBytes:blocks bytesPerRow:(side / 4) * 16];
        copy_through_blur(dev, pipeline, astc, got);
        right += check("astc", got, 255, 128, 0, 255);
        return result(right != 5);
    }
}
