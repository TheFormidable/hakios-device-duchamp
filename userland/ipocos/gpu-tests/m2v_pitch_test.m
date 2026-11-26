/* SPDX-License-Identifier: MIT
 * Does the Mali-G615 sample and render a linear BGRA8 image whose rows are 720 bytes apart (trial 82)?
 *
 * CoreAnimation's icons are 180x180 BGRA8 over the CPU's pages, rows 720 bytes apart. Mesa holds a linear image's rows
 * to 64 bytes on Mali v7+ (pan_linear_or_tiled_row_align_req; 16 for some YUV formats), so metal2vulkan copies each such
 * texture into an aligned shadow and back: ~18 internal 180x180 passes and ~40 copies a frame, each with its barrier
 * (trial 81). PanVK's PAN_LINEAR_ROW_ALIGN=N (tools/mesa26/panvk-linear-row-align.patch) relaxes the rule for explicit
 * linear layouts; this program says whether the hardware then reads and writes the right texels.
 *
 * Run without arguments: the cases as Mesa has it (the shadows: the reference), then itself again with
 * PAN_LINEAR_ROW_ALIGN=16 ("relaxed"). Each case compares every pixel.
 *   sample      a texture over a buffer, rows 720 apart, at offset 0, copied through CoreAnimation's blur (offsets 0)
 *   sample+16   the same at offset 16
 *   render      the copy drawn INTO a texture over a buffer, rows 720 apart; the CPU reads the buffer itself
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
id native_bridge_metal_open(const char *, const char *, uint64_t, void (*)(const char *, const char *));
size_t native_bridge_metal_status(char *, size_t);
uint64_t native_bridge_metal_first_problem(char *, size_t);
size_t native_bridge_metal_telemetry(char *, size_t);

enum { side = 180, pitch = 720 };
static const char *mode = "mesa";

static int result(int failed)
{
    char status[1024] = "", why[512] = "";
    native_bridge_metal_status(status, sizeof(status));
    native_bridge_metal_first_problem(why, sizeof(why));
    printf("m2v_pitch_test[%s]: status %s\n", mode, status);
    printf("m2v_pitch_test[%s]: first problem %s\n", mode, why);
    /* metal2vulkan's own count of where textures over host pages went: L inp (in place, at Metal's pitch) rf (refused:
     * shadowed for their pitch / for no driver image) sh (shadows) cp (copies in) bk (copied back) rt (render targets) */
    {
        static char table[8192];
        size_t n = native_bridge_metal_telemetry(table, sizeof(table));
        for (char *line = table, *end; n && line && *line; line = end ? end + 1 : NULL) {
            end = strchr(line, '\n');
            if (end) *end = 0;
            if (strstr(line, "L inp") || strstr(line, "TB n")) printf("m2v_pitch_test[%s]: telemetry %s\n", mode, line);
            if (!end) break;
        }
    }
    printf("m2v_pitch_test[%s]: %s\n", mode, failed ? "FAIL" : "PASS");
    return failed;
}
static void missing(const char *cls, const char *sel) { printf("m2v_pitch_test[%s]: missing %s %s\n", mode, cls, sel); }
static void expired(int sig) { printf("m2v_pitch_test[%s]: signal %d\n", mode, sig); result(1); _exit(1); }

/* Mesa's messages (mesa_loge: "WSI pitch not properly aligned") into the log, a line at a time */
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
            printf("m2v_pitch_test[%s] stderr: %s\n", mode, start);
            start = nl + 1;
        }
        used = strlen(start);
        memmove(buf, start, used);
        if (used == sizeof(buf) - 1) { printf("m2v_pitch_test[%s] stderr: %s\n", mode, buf); used = 0; }
    }
}

/* the pattern: B = x, G = y, R = (x * 7 + y * 3) & 255, A = 255 */
static void texel(unsigned x, unsigned y, uint8_t out[4])
{
    out[0] = (uint8_t)x; out[1] = (uint8_t)y; out[2] = (uint8_t)((x * 7 + y * 3) & 255); out[3] = 255;
}

/* draw `src` through the copying blur into `dst` (side x side) */
static int copy_through_blur(id<MTLDevice> dev, id<MTLRenderPipelineState> pipeline, id<MTLTexture> src, id<MTLTexture> dst)
{
    const float vertices[6][12] = {
        {-1,1,0,1, 0,0, 0,0, 1,1,1,1}, {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1},
        {1,1,0,1, 1,0, 0,0, 1,1,1,1}, {1,-1,0,1, 1,1, 0,0, 1,1,1,1}, {-1,-1,0,1, 0,1, 0,0, 1,1,1,1}
    };
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float texmat[4] = {1,1,0,0}, edr = 1;
    float uniforms[24] = {0};
    uniforms[16] = 0.25f; uniforms[17] = uniforms[18] = uniforms[19] = 0.125f;
    id<MTLBuffer> vb = [dev newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    id<MTLBuffer> matrix = [dev newBufferWithBytes:identity length:sizeof(identity) options:MTLResourceStorageModeShared];
    id<MTLBuffer> uv = [dev newBufferWithBytes:texmat length:sizeof(texmat) options:MTLResourceStorageModeShared];
    id<MTLBuffer> weights = [dev newBufferWithBytes:uniforms length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    id<MTLBuffer> brightness = [dev newBufferWithBytes:&edr length:sizeof(edr) options:MTLResourceStorageModeShared];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (!src || !dst || !vb || !matrix || !uv || !weights || !brightness || !sampler || !queue) return 0;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = dst;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 0);
    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return 0;
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
        printf("m2v_pitch_test[%s]: draw status %ld %s\n", mode, (long)cb.status, cb.error.localizedDescription.UTF8String ?: "none");
        return 0;
    }
    return 1;
}

/* every pixel of `pixels` (BGRA, rows `row` bytes apart) against the pattern as the source `kind` holds it, copied through the blur
 * into BGRA8: 0 BGRA8 (bytes back as they were), 1 RGBA8 (R and B swapped on the way), 2 A8 (black, its alpha the pattern's R) */
static int compare_kind(const char *name, const uint8_t *pixels, size_t row, int kind)
{
    unsigned wrong = 0, fx = 0, fy = 0;
    uint8_t first[4] = {0}, want[4] = {0}, expect[4], t[4];
    for (unsigned y = 0; y < side; y++) {
        for (unsigned x = 0; x < side; x++) {
            const uint8_t *p = pixels + y * row + x * 4;
            texel(x, y, t);
            if (kind == 0) memcpy(expect, t, 4);
            else if (kind == 1) { expect[0] = t[2]; expect[1] = t[1]; expect[2] = t[0]; expect[3] = t[3]; }
            else { expect[0] = expect[1] = expect[2] = 0; expect[3] = t[2]; }
            int bad = 0;
            for (int c = 0; c < 4; c++) if (abs((int)p[c] - (int)expect[c]) > 1) bad = 1;
            if (bad && wrong++ == 0) { fx = x; fy = y; memcpy(first, p, 4); memcpy(want, expect, 4); }
        }
    }
    if (wrong)
        printf("m2v_pitch_test[%s]: %-16s WRONG: %u of %u pixels; first at (%u, %u): BGRA %u %u %u %u, want %u %u %u %u\n", mode, name,
               wrong, side * side, fx, fy, first[0], first[1], first[2], first[3], want[0], want[1], want[2], want[3]);
    else
        printf("m2v_pitch_test[%s]: %-16s right: all %u pixels\n", mode, name, side * side);
    return wrong == 0;
}
static int compare(const char *name, const uint8_t *pixels, size_t row) { return compare_kind(name, pixels, row, 0); }

static void telemetry(void)
{
    static char table[8192];
    size_t n = native_bridge_metal_telemetry(table, sizeof(table));
    for (char *line = table, *end; n && line && *line; line = end ? end + 1 : NULL) {
        end = strchr(line, '\n');
        if (end) *end = 0;
        if (strstr(line, "L inp")) printf("m2v_pitch_test[%s]: telemetry %s\n", mode, line);
        if (!end) break;
    }
}

/* (trial 89) a texture of `format` over a no-copy buffer (the caller's own pages, as CoreAnimation's images are), rows `pitch`
 * apart from `offset`, copied through the blur into a BGRA8 target and compared */
static int sample_nocopy(id<MTLDevice> dev, id<MTLRenderPipelineState> pipeline, const char *name, MTLPixelFormat format, int kind,
                         unsigned bpp, unsigned row, unsigned offset)
{
    size_t length = ((size_t)offset + (size_t)row * side + 16383) & ~(size_t)16383;
    void *pages = NULL;
    if (posix_memalign(&pages, 16384, length)) return 0;
    memset(pages, 0, length);
    for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++) {
        uint8_t t[4];
        texel(x, y, t);
        uint8_t *at = (uint8_t *)pages + offset + (size_t)y * row + (size_t)x * bpp;
        if (kind == 2) at[0] = t[2]; else memcpy(at, t, 4);
    }
    id<MTLBuffer> buffer = [dev newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:side height:side mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [buffer newTextureWithDescriptor:d offset:offset bytesPerRow:row];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:side mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:td];
    int right = 0;
    if (!src) printf("m2v_pitch_test[%s]: %-16s no texture over the buffer\n", mode, name);
    else if (copy_through_blur(dev, pipeline, src, dst)) {
        uint8_t *out = calloc(side * side, 4);
        [dst getBytes:out bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
        right = compare_kind(name, out, side * 4, kind);
        free(out);
    }
    /* (the pages stay: the buffer has no deallocator, and a texture may still hold them) */
    return right;
}

static int run(void)
{
    signal(SIGALRM, expired); signal(SIGABRT, expired); signal(SIGSEGV, expired); signal(SIGBUS, expired);
    alarm(200);
    if (pipe(err_pipe) == 0 && dup2(err_pipe[1], 2) == 2) {
        pthread_t t;
        pthread_create(&t, NULL, forward_stderr, NULL);
        pthread_detach(t);
    }
    for (unsigned n = 0; n < 45; ++n) {
        io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosCSF"));
        if (service) { IOObjectRelease(service); break; }
        if (n == 44) return result(1);
        sleep(1);
    }
    printf("m2v_pitch_test[%s]: PAN_LINEAR_ROW_ALIGN=%s\n", mode, getenv("PAN_LINEAR_ROW_ALIGN") ?: "(unset)");
    @autoreleasepool {
        char cache[128];
        snprintf(cache, sizeof(cache), "/private/var/tmp/m2v-pitch-%s", mode);
        id<MTLDevice> dev = native_bridge_metal_open("/redwood/lib/libvulkan_panfrost.dylib", cache, 0, missing);
        if (!dev) return result(1);
        NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@"/redwood/share/ca-share/QuartzCore-default.metallib"] error:&error];
        if (!lib) { printf("m2v_pitch_test[%s]: library %s\n", mode, error.localizedDescription.UTF8String); return result(1); }
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
        if (!pipeline) { printf("m2v_pitch_test[%s]: pipeline %s\n", mode, error.localizedDescription.UTF8String); return result(1); }

        int right = 0;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:side mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        uint8_t *aligned = calloc(side * side, 4);   /* the reference's and the reads' rows, tight */

        /* sample, at offsets 0 and 16: the pattern in a buffer at rows 720 apart, the texture over it */
        for (unsigned offset = 0; offset <= 16; offset += 16) {
            id<MTLBuffer> buffer = [dev newBufferWithLength:offset + (size_t)pitch * side + 4096 options:MTLResourceStorageModeShared];
            uint8_t *bytes = (uint8_t *)buffer.contents + offset;
            for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++) texel(x, y, bytes + y * pitch + x * 4);
            MTLTextureDescriptor *bd = [td copy];
            bd.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> src = [buffer newTextureWithDescriptor:bd offset:offset bytesPerRow:pitch];
            id<MTLTexture> dst = [dev newTextureWithDescriptor:td];
            const char *name = offset ? "sample+16" : "sample";
            if (!src) { printf("m2v_pitch_test[%s]: %-10s no texture over the buffer\n", mode, name); continue; }
            if (!copy_through_blur(dev, pipeline, src, dst)) continue;
            [dst getBytes:aligned bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
            right += compare(name, aligned, side * 4);
        }

        /* render: the pattern copied INTO a texture over a buffer at rows 720 apart; the CPU reads the buffer */
        {
            id<MTLTexture> src = [dev newTextureWithDescriptor:td];
            for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++) texel(x, y, aligned + (y * side + x) * 4);
            [src replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0 withBytes:aligned bytesPerRow:side * 4];
            id<MTLBuffer> buffer = [dev newBufferWithLength:(size_t)pitch * side + 4096 options:MTLResourceStorageModeShared];
            memset(buffer.contents, 0x55, buffer.length);
            id<MTLTexture> dst = [buffer newTextureWithDescriptor:td offset:0 bytesPerRow:pitch];
            if (!dst) printf("m2v_pitch_test[%s]: render     no texture over the buffer\n", mode);
            else if (copy_through_blur(dev, pipeline, src, dst)) right += compare("render", buffer.contents, pitch);
        }
        free(aligned);

        /* (trial 89) no-copy pages, as CoreAnimation's: first a warm-up texture so that metal2vulkan's check of the first move
         * of an image over such pages answers (until then every texture is a shadow), then each format and offset */
        sample_nocopy(dev, pipeline, "warm-up", MTLPixelFormatBGRA8Unorm, 0, 4, pitch, 0);
        sleep(6);
        telemetry();
        int cases = 0, good = 0;
        good += sample_nocopy(dev, pipeline, "nocopy bgra 720", MTLPixelFormatBGRA8Unorm, 0, 4, pitch, 0); cases++;
        good += sample_nocopy(dev, pipeline, "nocopy bgra 768", MTLPixelFormatBGRA8Unorm, 0, 4, 768, 0); cases++;
        good += sample_nocopy(dev, pipeline, "nocopy bgra +16", MTLPixelFormatBGRA8Unorm, 0, 4, pitch, 16); cases++;
        good += sample_nocopy(dev, pipeline, "nocopy rgba 720", MTLPixelFormatRGBA8Unorm, 1, 4, pitch, 0); cases++;
        good += sample_nocopy(dev, pipeline, "nocopy rgba 768", MTLPixelFormatRGBA8Unorm, 1, 4, 768, 0); cases++;
        good += sample_nocopy(dev, pipeline, "nocopy a8 192", MTLPixelFormatA8Unorm, 2, 1, 192, 0); cases++;
        good += sample_nocopy(dev, pipeline, "nocopy a8 256", MTLPixelFormatA8Unorm, 2, 1, 256, 0); cases++;
        telemetry();
        printf("m2v_pitch_test[%s]: no-copy cases %d of %d right\n", mode, good, cases);
        return result(right != 3 || good != cases);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "relaxed") == 0) {
        mode = "relaxed";
        return run();
    }
    int failed = run();
    /* again with the rows held to 16 bytes, in a process of its own: PanVK reads the variable once */
    setenv("PAN_LINEAR_ROW_ALIGN", "16", 1);
    char *args[] = { argv[0], "relaxed", NULL };
    pid_t pid;
    int status = 0;
    if (posix_spawn(&pid, argv[0], NULL, NULL, args, environ) == 0) waitpid(pid, &status, 0);
    else printf("m2v_pitch_test: could not run the relaxed case\n");
    return failed;
}
