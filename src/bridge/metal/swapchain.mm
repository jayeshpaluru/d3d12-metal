// Swap chains: a CAMetalLayer per window and the pass that puts a back buffer on it.
//
// The application renders into ordinary textures (the DXGI back buffers); a
// present draws the back buffer onto the layer's next drawable with a fullscreen
// triangle. That one path converts between the back buffer format and the
// layer's (RGBA/BGRA order, sRGB encoding) and scales when the sizes differ.
#include "internal.h"

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

mtlb_result build_pipeline(Swapchain *swapchain, MTLPixelFormat pixel_format)
{
    if (!swapchain->library) {
        NSError *error = nil;
        swapchain->library = [swapchain->device->device newLibraryWithSource:@(kPresentSource) options:nil error:&error];
        if (!swapchain->library)
            return fail(MTLB_ERROR_COMPILE_FAILED, std::string("present shader: ") + error.localizedDescription.UTF8String);
    }
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [swapchain->library newFunctionWithName:@"present_vs"];
    desc.fragmentFunction = [swapchain->library newFunctionWithName:@"present_fs"];
    desc.colorAttachments[0].pixelFormat = pixel_format;
    NSError *error = nil;
    swapchain->pipeline = [swapchain->device->device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!swapchain->pipeline)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("present pipeline: ") + error.localizedDescription.UTF8String);
    return MTLB_OK;
}

void configure_layer(CAMetalLayer *layer, MTLPixelFormat pixel_format, uint32_t width, uint32_t height)
{
    layer.pixelFormat = pixel_format;
    layer.drawableSize = CGSizeMake(width, height);
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

mtlb_result encode_present(id<MTLCommandBuffer> command_buffer, Swapchain *swapchain, Texture *texture,
                           uint32_t sync_interval)
{
    const bool sync = sync_interval > 0;
    if (swapchain->display_sync.exchange(sync) != sync) {
        CAMetalLayer *layer = swapchain->layer;
        dispatch_async(dispatch_get_main_queue(), ^{ layer.displaySyncEnabled = sync; });
    }

    id<CAMetalDrawable> drawable = [swapchain->layer nextDrawable];
    if (!drawable) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            fprintf(stderr, "d3d12-metal: no drawable available, skipping presents\n");
        return MTLB_OK;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:swapchain->pipeline];
    [encoder setFragmentTexture:texture->texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command_buffer presentDrawable:drawable];
    return MTLB_OK;
}

} // namespace mtlb

extern "C" {

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

    auto *swapchain = new Swapchain();
    swapchain->device = device;
    swapchain->layer = layer;
    swapchain->format = static_cast<mtlb_format>(desc->format);
    if (mtlb_result result = build_pipeline(swapchain, pixel_format); result != MTLB_OK) {
        delete swapchain;
        return result;
    }
    const uint32_t drawables = desc->buffer_count >= 3 ? 3 : 2;
    run_on_main(^{
        layer.device = device->device;
        layer.framebufferOnly = YES;
        layer.maximumDrawableCount = drawables;
        layer.displaySyncEnabled = YES;
        configure_layer(layer, pixel_format, desc->width, desc->height);
    });
    *out = to_handle(swapchain);
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
    if (pixel_format != swapchain->layer.pixelFormat) {
        if (mtlb_result result = build_pipeline(swapchain, pixel_format); result != MTLB_OK)
            return result;
    }
    swapchain->format = static_cast<mtlb_format>(format);
    CAMetalLayer *layer = swapchain->layer;
    run_on_main(^{ configure_layer(layer, pixel_format, width, height); });
    return MTLB_OK;
}

} // extern "C"
