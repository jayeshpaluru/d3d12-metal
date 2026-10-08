// Swap chains: a CAMetalLayer per window and the pass that puts a back buffer on it.
//
// The application renders into ordinary textures (the DXGI back buffers); a
// present draws the back buffer onto the layer's next drawable with a fullscreen
// triangle. That one path converts between the back buffer format and the
// layer's (RGBA/BGRA order, sRGB encoding) and scales when the sizes differ.
#include "internal.h"

#import <ImageIO/ImageIO.h>

#include "layer_provider.h"

namespace {

using namespace mtlb;

LayerProvider g_layer_provider = nullptr;

const char *const kPresentSource = R"msl(
#include <metal_stdlib>
using namespace metal;

struct PresentVertex {
    float4 position [[position]];
    float2 uv;
};

vertex PresentVertex present_vs(uint vertex_id [[vertex_id]])
{
    float2 p = float2((vertex_id << 1) & 2, vertex_id & 2);
    PresentVertex out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

fragment float4 present_fs(PresentVertex in [[stage_in]], texture2d<float> source [[texture(0)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return source.sample(s, in.uv);
}
)msl";

// The layer format that shows a back buffer of `format` as the application meant it.
MTLPixelFormat layer_pixel_format(uint32_t format)
{
    switch (format) {
    case MTLB_FORMAT_R8G8B8A8_UNORM:
    case MTLB_FORMAT_R8G8B8A8_TYPELESS:
    case MTLB_FORMAT_B8G8R8A8_UNORM:
    case MTLB_FORMAT_B8G8R8A8_TYPELESS:
        return MTLPixelFormatBGRA8Unorm;
    case MTLB_FORMAT_R8G8B8A8_UNORM_SRGB:
    case MTLB_FORMAT_B8G8R8A8_UNORM_SRGB:
        return MTLPixelFormatBGRA8Unorm_sRGB;
    case MTLB_FORMAT_R10G10B10A2_UNORM:
        return MTLPixelFormatRGB10A2Unorm;
    case MTLB_FORMAT_R16G16B16A16_FLOAT:
        return MTLPixelFormatRGBA16Float;
    default:
        return MTLPixelFormatInvalid;
    }
}

// CALayer state is only safe to change on the main thread.
void run_on_main(void (^block)(void))
{
    if ([NSThread isMainThread])
        block();
    else
        dispatch_sync(dispatch_get_main_queue(), block);
}

// Builds the pass pipeline for drawables of `pixel_format`; nil (with fail() set) on error.
id<MTLRenderPipelineState> make_pipeline(Swapchain *swapchain, MTLPixelFormat pixel_format)
{
    if (!swapchain->library) {
        NSError *error = nil;
        swapchain->library = [swapchain->device->device newLibraryWithSource:@(kPresentSource) options:nil error:&error];
        if (!swapchain->library) {
            fail(MTLB_ERROR_COMPILE_FAILED, std::string("present shader: ") + error.localizedDescription.UTF8String);
            return nil;
        }
    }
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [swapchain->library newFunctionWithName:@"present_vs"];
    desc.fragmentFunction = [swapchain->library newFunctionWithName:@"present_fs"];
    desc.colorAttachments[0].pixelFormat = pixel_format;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [swapchain->device->device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline)
        fail(MTLB_ERROR_COMPILE_FAILED, std::string("present pipeline: ") + error.localizedDescription.UTF8String);
    return pipeline;
}

// Writes a BGRA8 texture as a PNG.
void write_png(id<MTLTexture> texture, const std::string &path)
{
    const NSUInteger width = texture.width, height = texture.height, pitch = width * 4;
    NSMutableData *pixels = [NSMutableData dataWithLength:pitch * height];
    [texture getBytes:pixels.mutableBytes bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.mutableBytes, width, height, 8, pitch, color_space,
                                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = context ? CGBitmapContextCreateImage(context) : nullptr;
    NSURL *url = [NSURL fileURLWithPath:@(path.c_str())];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (image && destination) {
        CGImageDestinationAddImage(destination, image, nullptr);
        ok = CGImageDestinationFinalize(destination);
    }
    fprintf(stderr, "d3d12-metal: %s present dump %s\n", ok ? "wrote" : "failed to write", path.c_str());
    if (destination)
        CFRelease(destination);
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(color_space);
}

void configure_layer(CAMetalLayer *layer, MTLPixelFormat pixel_format, uint32_t width, uint32_t height)
{
    layer.pixelFormat = pixel_format;
    layer.drawableSize = CGSizeMake(width, height);
}

// Draws `source` over all of `target` with the present pipeline.
void draw_present_pass(id<MTLCommandBuffer> command_buffer, id<MTLTexture> target,
                       id<MTLRenderPipelineState> pipeline, id<MTLTexture> source)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

} // namespace

namespace mtlb {

void set_layer_provider(LayerProvider provider)
{
    g_layer_provider = provider;
}

LayerProvider layer_provider()
{
    return g_layer_provider;
}

Drawable acquire_drawable(Swapchain *swapchain, uint32_t sync_interval)
{
    const bool sync = sync_interval > 0;
    if (swapchain->display_sync.exchange(sync) != sync) {
        CAMetalLayer *layer = swapchain->layer;
        dispatch_async(dispatch_get_main_queue(), ^{ layer.displaySyncEnabled = sync; });
    }

    Drawable result;
    {
        std::lock_guard<std::mutex> lock(swapchain->mutex);
        result.pipeline = swapchain->pipeline;
    }
    // Blocks while every drawable is in flight, which paces the application (and
    // for about a second when the window shows nothing); no lock may be held.
    result.drawable = [swapchain->layer nextDrawable];
    if (!result.drawable) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            fprintf(stderr, "d3d12-metal: no drawable available, skipping presents\n");
    }
    return result;
}

void encode_present(id<MTLCommandBuffer> command_buffer, Swapchain *swapchain, Texture *texture,
                    const Drawable &drawable)
{
    draw_present_pass(command_buffer, drawable.drawable.texture, drawable.pipeline, texture->texture);

    const uint64_t present = ++swapchain->presents;
    if (!swapchain->dump_path.empty() && present == swapchain->dump_frame) {
        // The same pass again into a texture the CPU can read.
        id<MTLTexture> target = drawable.drawable.texture;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target.pixelFormat
                                                                                       width:target.width
                                                                                      height:target.height
                                                                                   mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> capture = [swapchain->device->device newTextureWithDescriptor:td];
        draw_present_pass(command_buffer, capture, drawable.pipeline, texture->texture);
        const std::string path = swapchain->dump_path;
        [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) { write_png(capture, path); }];
    }
    [command_buffer presentDrawable:drawable.drawable];
}

} // namespace mtlb

extern "C" {

void mtlb_native_set_layer_provider(LayerProvider provider)
{
    set_layer_provider(provider);
}

mtlb_result mtlb_swapchain_create(mtlb_device handle, const mtlb_swapchain_desc *desc, mtlb_swapchain *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out || desc->width == 0 || desc->height == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    const MTLPixelFormat pixel_format = layer_pixel_format(desc->format);
    if (pixel_format == MTLPixelFormatInvalid)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported swap chain format " + std::to_string(desc->format));
    if (!g_layer_provider)
        return fail(MTLB_ERROR_UNSUPPORTED, "this build cannot present to windows");

    CAMetalLayer *layer = g_layer_provider(desc->window, device->device);
    if (!layer)
        return fail(MTLB_ERROR_DEVICE, "window " + std::to_string(desc->window) + " has no Metal layer");

    auto swapchain = std::make_unique<Swapchain>();
    swapchain->device = device;
    swapchain->layer = layer;
    swapchain->pixel_format = pixel_format;
    // The dump is a BGRA8 PNG: not available for the wider formats.
    if (const char *dump = getenv("D3D12METAL_DUMP_PRESENT");
        dump && pixel_format != MTLPixelFormatRGBA16Float && pixel_format != MTLPixelFormatRGB10A2Unorm) {
        const char *frame = getenv("D3D12METAL_DUMP_PRESENT_FRAME");
        swapchain->dump_path = dump;
        swapchain->dump_frame = frame ? strtoull(frame, nullptr, 10) : 30;
    }
    swapchain->pipeline = make_pipeline(swapchain.get(), pixel_format);
    if (!swapchain->pipeline)
        return MTLB_ERROR_COMPILE_FAILED;
    const uint32_t drawables = desc->buffer_count >= 3 ? 3 : 2;
    run_on_main(^{
        layer.device = device->device;
        layer.framebufferOnly = YES;
        layer.maximumDrawableCount = drawables;
        layer.displaySyncEnabled = YES;
        configure_layer(layer, pixel_format, desc->width, desc->height);
    });
    *out = to_handle(swapchain.release());
    return MTLB_OK;
}

void mtlb_swapchain_destroy(mtlb_swapchain handle)
{
    delete from_handle<Swapchain>(handle);
}

mtlb_result mtlb_swapchain_resize(mtlb_swapchain handle, uint32_t width, uint32_t height, uint32_t format)
{
    Swapchain *swapchain = from_handle<Swapchain>(handle);
    if (!swapchain || width == 0 || height == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    const MTLPixelFormat pixel_format = layer_pixel_format(format);
    if (pixel_format == MTLPixelFormatInvalid)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported swap chain format " + std::to_string(format));

    // Build the new pipeline first so that a failure changes nothing.
    id<MTLRenderPipelineState> pipeline = nil;
    {
        std::lock_guard<std::mutex> lock(swapchain->mutex);
        if (pixel_format != swapchain->pixel_format) {
            pipeline = make_pipeline(swapchain, pixel_format);
            if (!pipeline)
                return MTLB_ERROR_COMPILE_FAILED;
        }
    }
    CAMetalLayer *layer = swapchain->layer;
    run_on_main(^{ configure_layer(layer, pixel_format, width, height); });
    if (pipeline) {
        std::lock_guard<std::mutex> lock(swapchain->mutex);
        swapchain->pipeline = pipeline;
        swapchain->pixel_format = pixel_format;
    }
    return MTLB_OK;
}

} // extern "C"
