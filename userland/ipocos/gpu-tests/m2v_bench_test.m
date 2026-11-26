/* SPDX-License-Identifier: MIT
 * The G615's raw throughput through metal2vulkan over PanVK: how long a full-screen clear and a full-screen copy take.
 *
 * Trials 35-66: the desktop costs the GPU 4-5 ms per million pixels, five times the clock is not twice the speed, and a
 * 1080x2400 clear took ~6 ms (Codex, m2v_test): about 1.7 GB/s written, where the memory gives tens. This tells whether
 * the GPU's own memory path is slow (every case slow) or one kind of memory is (IOSurface pages vs the GPU's own
 * buffers), and what a command buffer's own cost is (one pass each vs ten passes each).
 *   clear  a 1125x2436 BGRA8 target cleared, nothing drawn (writes 11 MB)
 *   copy   CoreAnimation's seven-tap blur as a copy (offsets 0, weights summing to 1): reads 11 MB, writes 11 MB
 * each into/from the GPU's own texture and an IOSurface, 1 pass per command buffer and 10 per command buffer.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

id native_bridge_metal_open(const char *, const char *, uint64_t, void (*)(const char *, const char *));
size_t native_bridge_metal_status(char *, size_t);

enum { W = 1125, H = 2436, ROUNDS = 40 };

static void missing(const char *cls, const char *sel) { (void)cls; (void)sel; }
static void expired(int sig) { printf("m2v_bench_test: signal %d\n", sig); _exit(1); }

static double ms_since(uint64_t start)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)(mach_absolute_time() - start) * tb.numer / tb.denom / 1e6;
}

/* `rounds` passes into `target`, `per` passes per command buffer; each pass clears, and draws the copy from `source` if
 * there is one. Every command buffer committed, the last waited for. Milliseconds per pass. */
static double passes(id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline, id<MTLTexture> target, id<MTLTexture> source,
                     id<MTLBuffer> vb, id<MTLBuffer> matrix, id<MTLBuffer> uv, id<MTLBuffer> weights, id<MTLBuffer> edr,
                     id<MTLSamplerState> sampler, unsigned rounds, unsigned per)
{
    uint64_t start = mach_absolute_time();
    id<MTLCommandBuffer> last = nil;
    for (unsigned done = 0; done < rounds; done += per) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (unsigned p = 0; p < per; p++) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake((p & 1) ? 1 : 0, 0.5, 0, 1);
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
            if (source != nil) {
                [e setRenderPipelineState:pipeline];
                [e setVertexBuffer:vb offset:0 atIndex:0];
                [e setVertexBuffer:matrix offset:0 atIndex:2];
                [e setVertexBuffer:uv offset:0 atIndex:3];
                [e setFragmentBuffer:weights offset:0 atIndex:1];
                [e setFragmentBuffer:edr offset:0 atIndex:6];
                [e setFragmentTexture:source atIndex:3];
                [e setFragmentSamplerState:sampler atIndex:0];
                [e setViewport:(MTLViewport){0, 0, W, H, 0, 1}];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            }
            [e endEncoding];
        }
        [cb commit];
        last = cb;
    }
    [last waitUntilCompleted];
    if (last.status != MTLCommandBufferStatusCompleted) return -1;
    return ms_since(start) / rounds;
}

static void say(const char *what, double ms, double megabytes)
{
    if (ms < 0) { printf("m2v_bench_test: %-34s FAILED\n", what); return; }
    printf("m2v_bench_test: %-34s %7.2f ms a pass, %6.1f GB/s\n", what, ms, megabytes / ms);
}

int main(void)
{
    signal(SIGALRM, expired); signal(SIGABRT, expired); signal(SIGSEGV, expired); signal(SIGBUS, expired);
    alarm(150);
    for (unsigned n = 0; n < 45; ++n) {
        io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
        if (service) { IOObjectRelease(service); break; }
        if (n == 44) { printf("m2v_bench_test: no GPU\n"); return 1; }
        sleep(1);
    }
    @autoreleasepool {
        id<MTLDevice> dev = native_bridge_metal_open("/redwood/lib/libvulkan_panfrost.dylib", "/private/var/tmp/m2v-bench", 0, missing);
        if (!dev) { printf("m2v_bench_test: no device\n"); return 1; }
        NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/redwood/share/ca-share/QuartzCore-default.metallib"] error:&error];
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
        id<MTLRenderPipelineState> pipeline = lib ? [dev newRenderPipelineStateWithDescriptor:pd error:&error] : nil;
        if (!pipeline) { printf("m2v_bench_test: pipeline %s\n", error.localizedDescription.UTF8String); return 1; }

        const float vertices[6][12] = {
            {-1,1,0,1, 0,0, 0,0, 1,1,1,1}, {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1},
            {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {1,-1,0,1, 1,1, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1}
        };
        const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const float texmat[4] = {1,1,0,0}, edr = 1;
        float uniforms[24] = {0};
        uniforms[16] = 0.25f; uniforms[17] = uniforms[18] = uniforms[19] = 0.125f;   /* offsets 0: a copy */
        id<MTLBuffer> vb = [dev newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
        id<MTLBuffer> matrix = [dev newBufferWithBytes:identity length:sizeof(identity) options:MTLResourceStorageModeShared];
        id<MTLBuffer> uv = [dev newBufferWithBytes:texmat length:sizeof(texmat) options:MTLResourceStorageModeShared];
        id<MTLBuffer> weights = [dev newBufferWithBytes:uniforms length:sizeof(uniforms) options:MTLResourceStorageModeShared];
        id<MTLBuffer> bright = [dev newBufferWithBytes:&edr length:sizeof(edr) options:MTLResourceStorageModeShared];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> own = [dev newTextureWithDescriptor:td], own2 = [dev newTextureWithDescriptor:td];
        NSDictionary *props = @{(id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
                                (id)kIOSurfacePixelFormat: @(0x42475241) /* 'BGRA' */};
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        MTLTextureDescriptor *sdesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        sdesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> onSurface = surface ? [dev newTextureWithDescriptor:sdesc iosurface:surface plane:0] : nil;
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!own || !own2 || !onSurface || !queue) { printf("m2v_bench_test: no textures (own %d surface %d)\n", own != nil, onSurface != nil); return 1; }

        const double mb = (double)W * H * 4 / 1e6;   /* MB a full-screen target */
        printf("m2v_bench_test: %ux%u BGRA8, %.1f MB a target, %u passes each\n", W, H, mb, ROUNDS);
        /* a first round warms the pipelines and lets the governor see the load */
        passes(queue, pipeline, own, own2, vb, matrix, uv, weights, bright, sampler, ROUNDS, 10);
        say("clear, own texture, 1 pass/cb", passes(queue, pipeline, own, nil, vb, matrix, uv, weights, bright, sampler, ROUNDS, 1), mb);
        say("clear, own texture, 10 passes/cb", passes(queue, pipeline, own, nil, vb, matrix, uv, weights, bright, sampler, ROUNDS, 10), mb);
        say("clear, IOSurface, 10 passes/cb", passes(queue, pipeline, onSurface, nil, vb, matrix, uv, weights, bright, sampler, ROUNDS, 10), mb);
        say("copy own -> own, 1 pass/cb", passes(queue, pipeline, own, own2, vb, matrix, uv, weights, bright, sampler, ROUNDS, 1), 2 * mb);
        say("copy own -> own, 10 passes/cb", passes(queue, pipeline, own, own2, vb, matrix, uv, weights, bright, sampler, ROUNDS, 10), 2 * mb);
        say("copy IOSurface -> own, 10 passes/cb", passes(queue, pipeline, own, onSurface, vb, matrix, uv, weights, bright, sampler, ROUNDS, 10), 2 * mb);
        say("copy own -> IOSurface, 10 passes/cb", passes(queue, pipeline, onSurface, own, vb, matrix, uv, weights, bright, sampler, ROUNDS, 10), 2 * mb);
        char status[600] = "";
        native_bridge_metal_status(status, sizeof(status));
        printf("m2v_bench_test: status %.200s\n", status);
        printf("m2v_bench_test: DONE\n");
        return 0;
    }
}
