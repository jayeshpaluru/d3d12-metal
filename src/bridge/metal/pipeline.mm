// Pipeline creation: DXIL -> Metal IR via libmetalirconverter, then the Metal
// render pipeline and depth-stencil state.
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

namespace {

using namespace mtlb;

// Releases Metal shader converter objects when leaving scope.
template <typename T, void (*Destroy)(T *)>
struct Owned {
    T *ptr = nullptr;
    Owned() = default;
    explicit Owned(T *p) : ptr(p) {}
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
    ~Owned()
    {
        if (ptr)
            Destroy(ptr);
    }
};

using OwnedRootSignature = Owned<IRRootSignature, IRRootSignatureDestroy>;
using OwnedCompiler = Owned<IRCompiler, IRCompilerDestroy>;
using OwnedObject = Owned<IRObject, IRObjectDestroy>;
using OwnedMetalLib = Owned<IRMetalLibBinary, IRMetalLibBinaryDestroy>;
using OwnedReflection = Owned<IRShaderReflection, IRShaderReflectionDestroy>;

std::string error_text(IRError *error, const char *what)
{
    std::string text = std::string(what) + " (code " + std::to_string(error ? IRErrorGetCode(error) : 0) + ")";
    if (error)
        IRErrorDestroy(error);
    return text;
}

struct VertexInput {
    std::string name;  // lower-case semantic + index, e.g. "position0"
    uint8_t attribute_index;
};

struct Stage {
    id<MTLFunction> function = nil;
    std::vector<VertexInput> vertex_inputs;
};

// Converts one DXIL shader against `root_signature` and loads its Metal function.
mtlb_result build_stage(id<MTLDevice> device, const IRRootSignature *root_signature, const void *dxil,
                        uint64_t size, const char *entry, IRShaderStage ir_stage, Stage &out)
{
    OwnedCompiler compiler(IRCompilerCreate());
    IRCompilerSetGlobalRootSignature(compiler.ptr, root_signature);

    OwnedObject input(IRObjectCreateFromDXIL(static_cast<const uint8_t *>(dxil), size, IRBytecodeOwnershipNone));
    IRError *error = nullptr;
    OwnedObject output(IRCompilerAllocCompileAndLink(compiler.ptr, entry, input.ptr, &error));
    if (!output.ptr)
        return fail(MTLB_ERROR_COMPILE_FAILED, error_text(error, "DXIL conversion failed"));

    OwnedMetalLib metallib(IRMetalLibBinaryCreate());
    if (!IRObjectGetMetalLibBinary(output.ptr, ir_stage, metallib.ptr))
        return fail(MTLB_ERROR_COMPILE_FAILED, "converted shader has no metallib for the requested stage");

    OwnedReflection reflection(IRShaderReflectionCreate());
    if (!IRObjectGetReflection(output.ptr, ir_stage, reflection.ptr))
        return fail(MTLB_ERROR_COMPILE_FAILED, "shader reflection unavailable");
    const char *function_name = IRShaderReflectionGetEntryPointFunctionName(reflection.ptr);

    NSError *ns_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:IRMetalLibGetBytecodeData(metallib.ptr) error:&ns_error];
    if (!library)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newLibraryWithData: ") + ns_error.localizedDescription.UTF8String);
    out.function = [library newFunctionWithName:[NSString stringWithUTF8String:function_name]];
    if (!out.function)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("function not found in converted library: ") + function_name);

    if (ir_stage == IRShaderStageVertex) {
        IRVersionedVSInfo info;
        if (!IRShaderReflectionCopyVertexInfo(reflection.ptr, IRReflectionVersion_1_0, &info))
            return fail(MTLB_ERROR_COMPILE_FAILED, "vertex reflection unavailable");
        for (size_t i = 0; i < info.info_1_0.num_vertex_inputs; ++i) {
            const IRVertexInputInfo_1_0 &input_info = info.info_1_0.vertex_inputs[i];
            out.vertex_inputs.push_back({input_info.name, input_info.attributeIndex});
        }
        IRShaderReflectionReleaseVertexInfo(&info);
    }
    return MTLB_OK;
}

MTLBlendFactor to_blend_factor(uint32_t blend)
{
    switch (blend) {
    case MTLB_BLEND_ZERO: return MTLBlendFactorZero;
    case MTLB_BLEND_ONE: return MTLBlendFactorOne;
    case MTLB_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
    case MTLB_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case MTLB_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case MTLB_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case MTLB_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case MTLB_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case MTLB_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
    case MTLB_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case MTLB_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
    case MTLB_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
    case MTLB_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
    case MTLB_BLEND_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case MTLB_BLEND_INV_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case MTLB_BLEND_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case MTLB_BLEND_INV_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation to_blend_op(uint32_t op)
{
    switch (op) {
    case MTLB_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
    case MTLB_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case MTLB_BLEND_OP_MIN: return MTLBlendOperationMin;
    case MTLB_BLEND_OP_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLColorWriteMask to_write_mask(uint32_t mask)
{
    MTLColorWriteMask result = MTLColorWriteMaskNone;
    if (mask & 1) result |= MTLColorWriteMaskRed;
    if (mask & 2) result |= MTLColorWriteMaskGreen;
    if (mask & 4) result |= MTLColorWriteMaskBlue;
    if (mask & 8) result |= MTLColorWriteMaskAlpha;
    return result;
}

MTLCompareFunction to_compare(uint32_t func)
{
    switch (func) {
    case MTLB_COMPARE_NEVER: return MTLCompareFunctionNever;
    case MTLB_COMPARE_LESS: return MTLCompareFunctionLess;
    case MTLB_COMPARE_EQUAL: return MTLCompareFunctionEqual;
    case MTLB_COMPARE_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case MTLB_COMPARE_GREATER: return MTLCompareFunctionGreater;
    case MTLB_COMPARE_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case MTLB_COMPARE_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation to_stencil_op(uint32_t op)
{
    switch (op) {
    case MTLB_STENCIL_OP_ZERO: return MTLStencilOperationZero;
    case MTLB_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
    case MTLB_STENCIL_OP_INCR_SAT: return MTLStencilOperationIncrementClamp;
    case MTLB_STENCIL_OP_DECR_SAT: return MTLStencilOperationDecrementClamp;
    case MTLB_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
    case MTLB_STENCIL_OP_INCR: return MTLStencilOperationIncrementWrap;
    case MTLB_STENCIL_OP_DECR: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLStencilDescriptor *to_stencil(const mtlb_stencil_face &face, uint32_t read_mask, uint32_t write_mask)
{
    MTLStencilDescriptor *s = [MTLStencilDescriptor new];
    s.stencilCompareFunction = to_compare(face.func);
    s.stencilFailureOperation = to_stencil_op(face.fail_op);
    s.depthFailureOperation = to_stencil_op(face.depth_fail_op);
    s.depthStencilPassOperation = to_stencil_op(face.pass_op);
    s.readMask = read_mask;
    s.writeMask = write_mask;
    return s;
}

std::string lowercase_key(const char *semantic, uint32_t index)
{
    std::string key(semantic);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    return key + std::to_string(index);
}

// Maps the D3D12 input layout onto Metal vertex fetch attributes, using the
// shader's reflected input names to find each element's attribute slot.
mtlb_result build_vertex_descriptor(const mtlb_pipeline_desc &desc, const Stage &vs, MTLVertexDescriptor **out)
{
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < desc.num_input_elements; ++i) {
        const mtlb_input_element &e = desc.input_elements[i];
        MTLVertexFormat format = to_vertex_format(e.format);
        if (format == MTLVertexFormatInvalid || e.input_slot >= MTLB_MAX_VERTEX_BUFFERS)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported input element " + std::string(e.semantic_name));

        const std::string key = lowercase_key(e.semantic_name, e.semantic_index);
        auto input = std::find_if(vs.vertex_inputs.begin(), vs.vertex_inputs.end(),
                                  [&](const VertexInput &v) { return v.name == key; });
        if (input == vs.vertex_inputs.end())
            continue;  // not consumed by the vertex shader

        const NSUInteger buffer_index = kIRVertexBufferBindPoint + e.input_slot;
        MTLVertexAttributeDescriptor *attribute = vd.attributes[kIRStageInAttributeStartIndex + input->attribute_index];
        attribute.format = format;
        attribute.offset = e.byte_offset;
        attribute.bufferIndex = buffer_index;

        // The stride arrives with IASetVertexBuffers, so it is set per draw.
        MTLVertexBufferLayoutDescriptor *layout = vd.layouts[buffer_index];
        layout.stride = MTLBufferLayoutStrideDynamic;
        if (e.input_class == MTLB_INPUT_PER_INSTANCE) {
            layout.stepFunction = MTLVertexStepFunctionPerInstance;
            layout.stepRate = e.step_rate ? e.step_rate : 1;
        } else {
            layout.stepFunction = MTLVertexStepFunctionPerVertex;
        }
    }
    *out = vd;
    return MTLB_OK;
}

MTLPrimitiveTopologyClass to_topology_class(uint32_t type)
{
    switch (type) {
    case MTLB_TOPOLOGY_TYPE_POINT: return MTLPrimitiveTopologyClassPoint;
    case MTLB_TOPOLOGY_TYPE_LINE: return MTLPrimitiveTopologyClassLine;
    default: return MTLPrimitiveTopologyClassTriangle;
    }
}

} // namespace

extern "C" mtlb_result mtlb_pipeline_create(mtlb_device handle, const mtlb_pipeline_desc *desc, mtlb_pipeline *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out || !desc->vs_dxil || !desc->vs_size || !desc->root_signature
        || desc->num_render_targets > MTLB_MAX_RENDER_TARGETS
        || desc->num_input_elements > MTLB_MAX_INPUT_ELEMENTS)
        return MTLB_ERROR_INVALID_ARGUMENT;

    RootSignature *root_signature = from_handle<RootSignature>(desc->root_signature);
    if (!root_signature)
        return MTLB_ERROR_INVALID_ARGUMENT;

    Stage vs, ps;
    mtlb_result result = build_stage(device->device, root_signature->ir, desc->vs_dxil, desc->vs_size,
                                     desc->vs_entry, IRShaderStageVertex, vs);
    if (result != MTLB_OK)
        return result;
    if (desc->ps_dxil && desc->ps_size) {
        result = build_stage(device->device, root_signature->ir, desc->ps_dxil, desc->ps_size,
                             desc->ps_entry, IRShaderStageFragment, ps);
        if (result != MTLB_OK)
            return result;
    }

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs.function;
    pd.fragmentFunction = ps.function;
    pd.rasterizationEnabled = ps.function != nil;
    pd.rasterSampleCount = desc->sample_count ? desc->sample_count : 1;
    pd.inputPrimitiveTopology = to_topology_class(desc->topology_type);

    MTLVertexDescriptor *vertex_descriptor = nil;
    result = build_vertex_descriptor(*desc, vs, &vertex_descriptor);
    if (result != MTLB_OK)
        return result;
    pd.vertexDescriptor = vertex_descriptor;

    for (uint32_t i = 0; i < desc->num_render_targets; ++i) {
        if (desc->rtv_formats[i] == MTLB_FORMAT_UNKNOWN)
            continue;
        MTLPixelFormat format = to_pixel_format(desc->rtv_formats[i]);
        if (format == MTLPixelFormatInvalid)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported render target format " + std::to_string(desc->rtv_formats[i]));
        const mtlb_render_target_blend &blend = desc->blend[desc->independent_blend ? i : 0];
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        ca.pixelFormat = format;
        ca.writeMask = to_write_mask(blend.write_mask);
        ca.blendingEnabled = blend.blend_enable != 0;
        ca.sourceRGBBlendFactor = to_blend_factor(blend.src_blend);
        ca.destinationRGBBlendFactor = to_blend_factor(blend.dest_blend);
        ca.rgbBlendOperation = to_blend_op(blend.blend_op);
        ca.sourceAlphaBlendFactor = to_blend_factor(blend.src_blend_alpha);
        ca.destinationAlphaBlendFactor = to_blend_factor(blend.dest_blend_alpha);
        ca.alphaBlendOperation = to_blend_op(blend.blend_op_alpha);
    }

    if (desc->dsv_format != MTLB_FORMAT_UNKNOWN) {
        mtlb_format_info info;
        MTLPixelFormat format = to_pixel_format(desc->dsv_format);
        if (format == MTLPixelFormatInvalid || mtlb_format_get_info(static_cast<mtlb_format>(desc->dsv_format), &info) != MTLB_OK)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported depth format " + std::to_string(desc->dsv_format));
        if (info.flags & MTLB_FORMAT_FLAG_DEPTH)
            pd.depthAttachmentPixelFormat = format;
        if (info.flags & MTLB_FORMAT_FLAG_STENCIL)
            pd.stencilAttachmentPixelFormat = format;
    }

    NSError *ns_error = nil;
    id<MTLRenderPipelineState> state = [device->device newRenderPipelineStateWithDescriptor:pd error:&ns_error];
    if (!state)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newRenderPipelineState: ") + ns_error.localizedDescription.UTF8String);

    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = desc->depth_enable ? to_compare(desc->depth_func) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = desc->depth_enable && desc->depth_write_enable;
    if (desc->stencil_enable) {
        dd.frontFaceStencil = to_stencil(desc->front_face, desc->stencil_read_mask, desc->stencil_write_mask);
        dd.backFaceStencil = to_stencil(desc->back_face, desc->stencil_read_mask, desc->stencil_write_mask);
    }
    id<MTLDepthStencilState> depth_stencil = [device->device newDepthStencilStateWithDescriptor:dd];
    if (!depth_stencil)
        return fail(MTLB_ERROR_COMPILE_FAILED, "newDepthStencilState failed");

    auto *pipeline = new Pipeline();
    pipeline->state = state;
    pipeline->depth_stencil = depth_stencil;
    pipeline->cull_mode = desc->cull_mode == MTLB_CULL_FRONT ? MTLCullModeFront
                          : desc->cull_mode == MTLB_CULL_BACK ? MTLCullModeBack : MTLCullModeNone;
    pipeline->winding = desc->front_counter_clockwise ? MTLWindingCounterClockwise : MTLWindingClockwise;
    pipeline->fill_mode = desc->fill_mode == MTLB_FILL_WIREFRAME ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    pipeline->depth_clip = desc->depth_clip_enable ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
    pipeline->depth_bias = static_cast<float>(desc->depth_bias);
    pipeline->slope_scaled_depth_bias = desc->slope_scaled_depth_bias;
    pipeline->depth_bias_clamp = desc->depth_bias_clamp;
    *out = to_handle(pipeline);
    return MTLB_OK;
}

extern "C" mtlb_result mtlb_root_signature_create(mtlb_device handle, const void *blob, uint64_t size,
                                                   mtlb_root_signature *out, mtlb_root_signature_layout *layout)
{
    if (!from_handle<Device>(handle) || !blob || !out || !layout)
        return MTLB_ERROR_INVALID_ARGUMENT;

    IRError *error = nullptr;
    IRVersionedRootSignatureDescriptor *rs_desc = IRVersionedRootSignatureDescriptorCreateFromBlob(
        static_cast<const uint8_t *>(blob), static_cast<uint32_t>(size), &error);
    if (!rs_desc)
        return fail(MTLB_ERROR_COMPILE_FAILED, error_text(error, "root signature rejected by the shader converter"));
    const uint32_t num_parameters = rs_desc->version == IRRootSignatureVersion_1_0 ? rs_desc->desc_1_0.NumParameters
                                                                                   : rs_desc->desc_1_1.NumParameters;
    OwnedRootSignature root_signature(IRRootSignatureCreateFromDescriptor(rs_desc, &error));
    IRVersionedRootSignatureDescriptorRelease(rs_desc);
    if (!root_signature.ptr)
        return fail(MTLB_ERROR_COMPILE_FAILED, error_text(error, "root signature rejected by the shader converter"));

    // The converter reports one top-level argument buffer entry per root
    // parameter, in parameter order.
    const size_t count = IRRootSignatureGetResourceCount(root_signature.ptr);
    if (count != num_parameters || count > MTLB_MAX_ROOT_PARAMETERS)
        return fail(MTLB_ERROR_UNSUPPORTED, "unexpected root signature resource count " + std::to_string(count));
    std::vector<IRResourceLocation> locations(count);
    IRRootSignatureGetResourceLocations(root_signature.ptr, locations.data());

    *layout = {};
    layout->num_parameters = static_cast<uint32_t>(count);
    uint64_t end = 0;
    for (size_t i = 0; i < count; ++i) {
        layout->parameters[i] = {locations[i].topLevelOffset, static_cast<uint32_t>(locations[i].sizeBytes)};
        end = std::max<uint64_t>(end, uint64_t(locations[i].topLevelOffset) + locations[i].sizeBytes);
    }
    layout->argument_buffer_size = static_cast<uint32_t>((end + 7) & ~uint64_t(7));

    *out = to_handle(new RootSignature{root_signature.ptr});
    root_signature.ptr = nullptr;  // owned by the handle now
    return MTLB_OK;
}

extern "C" void mtlb_root_signature_destroy(mtlb_root_signature handle)
{
    RootSignature *root_signature = from_handle<RootSignature>(handle);
    if (!root_signature)
        return;
    IRRootSignatureDestroy(root_signature->ir);
    delete root_signature;
}

extern "C" void mtlb_pipeline_destroy(mtlb_pipeline handle)
{
    delete from_handle<mtlb::Pipeline>(handle);
}
