/* SPDX-License-Identifier: MIT
 * Execute CoreAnimation's real seven-tap blur with a known impulse image.
 * Uses the ordinary metallib/pipeline/texture Metal APIs and checks every pixel.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

id native_bridge_metal_open(const char *, const char *, uint64_t, void (*)(const char *, const char *));
size_t native_bridge_metal_status(char *, size_t);
uint64_t native_bridge_metal_first_problem(char *, size_t);

static int result(int failed)
{
    char status[1024] = "", why[512] = "";
    native_bridge_metal_status(status, sizeof(status));
    native_bridge_metal_first_problem(why, sizeof(why));
    printf("m2v_blur_test: status %s\n", status);
    printf("m2v_blur_test: first problem %s\n", why);
    printf("m2v_blur_test: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
static void missing(const char *cls, const char *sel) { printf("m2v_blur_test: missing %s %s\n", cls, sel); }
static void expired(int sig) { printf("m2v_blur_test: signal %d\n", sig); result(1); _exit(1); }

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
        id<MTLDevice> dev = native_bridge_metal_open("/redwood/lib/libvulkan_panfrost.dylib",
                                                   "/private/var/tmp/m2v-blur", 0, missing);
        if (!dev) return result(1);
        printf("m2v_blur_test: device %s\n", dev.name.UTF8String);
        NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/redwood/share/ca-share/QuartzCore-default.metallib"] error:&error];
        if (!lib) { printf("m2v_blur_test: library %s\n", error.localizedDescription.UTF8String); return result(1); }
        MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
        layout.layouts[0].stride = 48;
        layout.attributes[0].format = MTLVertexFormatFloat4;
        layout.attributes[0].offset = 0; layout.attributes[0].bufferIndex = 0;
        layout.attributes[1].format = MTLVertexFormatFloat2;
        layout.attributes[1].offset = 16; layout.attributes[1].bufferIndex = 0;
        layout.attributes[3].format = MTLVertexFormatFloat4;
        layout.attributes[3].offset = 32; layout.attributes[3].bufferIndex = 0;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"downsample_blur_vert_lpf"];
        pd.fragmentFunction = [lib newFunctionWithName:@"downsample_blur_2_frag_lpf"];
        pd.vertexDescriptor = layout;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) { printf("m2v_blur_test: pipeline %s\n", error.localizedDescription.UTF8String); return result(1); }

        enum { side = 64 };
        const float vertices[6][12] = {
            {-1,1,0,1, 0,0, 0,0, 1,1,1,1}, {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1},
            {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {1,-1,0,1, 1,1, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1}
        };
        const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const float texmat[4] = {1,1,0,0}, edr = 1;
        /* AIR reflection: offset[8] at 0, weight[2] at 64, total 96 bytes. */
        float uniforms[24] = {0};
        for (unsigned i = 0; i < 3; ++i) uniforms[i * 2] = (float)(i + 1) / side;
        uniforms[16] = 0.25f; uniforms[17] = uniforms[18] = uniforms[19] = 0.125f;
        id<MTLBuffer> vb = [dev newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
        id<MTLBuffer> matrix = [dev newBufferWithBytes:identity length:sizeof(identity) options:MTLResourceStorageModeShared];
        id<MTLBuffer> uv = [dev newBufferWithBytes:texmat length:sizeof(texmat) options:MTLResourceStorageModeShared];
        id<MTLBuffer> weights = [dev newBufferWithBytes:uniforms length:sizeof(uniforms) options:MTLResourceStorageModeShared];
        id<MTLBuffer> brightness = [dev newBufferWithBytes:&edr length:sizeof(edr) options:MTLResourceStorageModeShared];
        if (!vb || !matrix || !uv || !weights || !brightness) return result(1);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:side mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> src = [dev newTextureWithDescriptor:td], dst = [dev newTextureWithDescriptor:td];
        if (!src || !dst) return result(1);
        uint8_t input[side * side * 4] = {0};
        for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
            unsigned at = (y * side + x) * 4;
            input[at] = input[at + 1] = input[at + 2] = x == side / 2 ? 255 : 0;
            input[at + 3] = 255;
        }
        [src replaceRegion:MTLRegionMake2D(0,0,side,side) mipmapLevel:0 withBytes:input bytesPerRow:side * 4];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!sampler || !queue) return result(1);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = dst;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1,0,0,1);
        id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return result(1);
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vb offset:0 atIndex:0];
        [encoder setVertexBuffer:matrix offset:0 atIndex:2];
        [encoder setVertexBuffer:uv offset:0 atIndex:3];
        [encoder setFragmentBuffer:weights offset:0 atIndex:1];
        [encoder setFragmentBuffer:brightness offset:0 atIndex:6];
        [encoder setFragmentTexture:src atIndex:3];
        [encoder setFragmentSamplerState:sampler atIndex:0];
        [encoder setViewport:(MTLViewport){0,0,side,side,0,1}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
        printf("m2v_blur_test: draw status %ld %s\n", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "none");
        if (cb.status != MTLCommandBufferStatusCompleted) return result(1);
        uint8_t pixels[side * side * 4] = {0};
        [dst getBytes:pixels bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0,0,side,side) mipmapLevel:0];
        unsigned good = 0, spread = 0;
        for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
            const uint8_t *p = pixels + (y * side + x) * 4;
            unsigned distance = abs((int)x - side / 2);
            int expected = distance == 0 ? 64 : distance <= 3 ? 32 : 0;
            int right = p[3] == 255;
            for (unsigned c = 0; c < 3; ++c) right &= abs((int)p[c] - expected) <= 1;
            good += right; spread += x != side / 2 && p[0] > 0;
        }
        printf("m2v_blur_test: CoreAnimation blur %u/4096 pixels correct, %u/384 pixels spread outside input column\n", good, spread);
        return result(good != side * side || spread != 6 * side);
    }
}
