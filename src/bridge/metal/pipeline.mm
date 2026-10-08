// Pipeline creation: DXIL -> Metal IR via libmetalirconverter, then the Metal
// render pipeline and depth-stencil state.
#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
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

// One compiler per thread: creating one per stage per pipeline is wasteful.
IRCompiler *thread_compiler()
{
    thread_local OwnedCompiler compiler;
    if (!compiler.ptr) {
        compiler.ptr = IRCompilerCreate();
        // Vertex shaders fetch their inputs through a separate stage-in function
        // (see get_stage_in) instead of Metal vertex fetch.
        IRCompilerSetStageInGenerationMode(compiler.ptr, IRStageInCodeGenerationModeUseSeparateStageInFunction);
        // D3D12 semantics the converter leaves off by default: out-of-bounds buffer and texture reads
        // return zero (and writes are dropped), the descriptors' min LOD clamp and the samplers' LOD bias
        // are applied, NaN coordinates sample as zero, and equal vertex shaders give equal positions.
        IRCompilerSetCompatibilityFlags(compiler.ptr, static_cast<IRCompatibilityFlags>(
            IRCompatibilityFlagBoundsCheck | IRCompatibilityFlagTextureMinLODClamp | IRCompatibilityFlagSamplerLODBias
            | IRCompatibilityFlagSampleNanToZero | IRCompatibilityFlagPositionInvariance));
    }
    return compiler.ptr;
}

// FNV-1a, enough to tell shaders apart together with their size.
uint64_t hash_bytes(const void *data, uint64_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint64_t i = 0; i < size; ++i)
        hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 1099511628211ull;
    return hash;
}

// Converts one DXIL shader against `root_signature` and loads its Metal function.
mtlb_result convert_stage(Device *device, RootSignature *root_signature, const void *dxil, uint64_t size,
                          const char *entry, IRShaderStage ir_stage, ShaderStage &out)
{
    IRCompiler *compiler = thread_compiler();
    IRCompilerSetGlobalRootSignature(compiler, root_signature->ir);

    OwnedObject input(IRObjectCreateFromDXIL(static_cast<const uint8_t *>(dxil), size, IRBytecodeOwnershipNone));
    IRError *error = nullptr;
    OwnedObject output(IRCompilerAllocCompileAndLink(compiler, entry, input.ptr, &error));
    // The compiler outlives root signatures; do not leave it pointing at this one.
    IRCompilerSetGlobalRootSignature(compiler, nullptr);
    if (!output.ptr)
        return fail(MTLB_ERROR_COMPILE_FAILED, error_text(error, "DXIL conversion failed"));

    OwnedMetalLib metallib(IRMetalLibBinaryCreate());
    if (!IRObjectGetMetalLibBinary(output.ptr, ir_stage, metallib.ptr))
        return fail(MTLB_ERROR_COMPILE_FAILED, "converted shader has no metallib for the requested stage");

    std::shared_ptr<IRShaderReflection> reflection(IRShaderReflectionCreate(), IRShaderReflectionDestroy);
    if (!IRObjectGetReflection(output.ptr, ir_stage, reflection.get()))
        return fail(MTLB_ERROR_COMPILE_FAILED, "shader reflection unavailable");
    const char *function_name = IRShaderReflectionGetEntryPointFunctionName(reflection.get());

    NSError *ns_error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithData:IRMetalLibGetBytecodeData(metallib.ptr) error:&ns_error];
    if (!library)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newLibraryWithData: ") + ns_error.localizedDescription.UTF8String);
    out.function = [library newFunctionWithName:[NSString stringWithUTF8String:function_name]];
    if (!out.function)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("function not found in converted library: ") + function_name);

    if (ir_stage == IRShaderStageCompute) {
        IRVersionedCSInfo info;
        if (!IRShaderReflectionCopyComputeInfo(reflection.get(), IRReflectionVersion_1_0, &info))
            return fail(MTLB_ERROR_COMPILE_FAILED, "compute reflection unavailable");
        std::copy_n(info.info_1_0.tg_size, 3, out.threadgroup_size);
        IRShaderReflectionReleaseComputeInfo(&info);
    }
    if (ir_stage == IRShaderStageVertex) {
        IRVersionedVSInfo info;
        if (!IRShaderReflectionCopyVertexInfo(reflection.get(), IRReflectionVersion_1_0, &info))
            return fail(MTLB_ERROR_COMPILE_FAILED, "vertex reflection unavailable");
        out.num_vertex_inputs = static_cast<uint32_t>(info.info_1_0.num_vertex_inputs);
        IRShaderReflectionReleaseVertexInfo(&info);
        out.reflection = reflection;
    }
    return MTLB_OK;
}

// Returns the converted stage from the device cache, converting it on a miss.
mtlb_result get_stage(Device *device, RootSignature *root_signature, const void *dxil, uint64_t size,
                      const char *entry, IRShaderStage ir_stage, std::shared_ptr<const ShaderStage> &out)
{
    const ShaderKey key{hash_bytes(dxil, size), size, root_signature->id, static_cast<uint32_t>(ir_stage),
                        entry ? entry : ""};
    {
        std::lock_guard<std::mutex> lock(device->shaders_mutex);
        auto it = device->shaders.find(key);
        if (it != device->shaders.end()) {
            out = it->second;
            return MTLB_OK;
        }
    }
    auto stage = std::make_shared<ShaderStage>();
    mtlb_result result = convert_stage(device, root_signature, dxil, size, entry, ir_stage, *stage);
    if (result != MTLB_OK)
        return result;
    std::lock_guard<std::mutex> lock(device->shaders_mutex);
    out = device->shaders.emplace(key, std::move(stage)).first->second;  // keeps the first on a race
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

// Render passes never attach depth-stencil yet (DSV milestone). Until they do,
// pipelines declare no depth or stencil format and depth-stencil state stays off.
constexpr bool kDepthStencilSupported = false;

// Returns the shared depth-stencil state for the description's relevant fields.
id<MTLDepthStencilState> get_depth_stencil(Device *device, const mtlb_pipeline_desc &desc, bool enabled)
{
    const bool depth_enable = enabled && desc.depth_enable, stencil_enable = enabled && desc.stencil_enable;
    DepthStencilKey key{};
    key[0] = depth_enable ? desc.depth_func : MTLB_COMPARE_ALWAYS;
    key[1] = depth_enable && desc.depth_write_enable;
    if (stencil_enable) {
        key[2] = 1;
        key[3] = desc.stencil_read_mask;
        key[4] = desc.stencil_write_mask;
        const mtlb_stencil_face *faces[2] = {&desc.front_face, &desc.back_face};
        for (int i = 0; i < 2; ++i) {
            key[5 + i * 4] = faces[i]->fail_op;
            key[6 + i * 4] = faces[i]->depth_fail_op;
            key[7 + i * 4] = faces[i]->pass_op;
            key[8 + i * 4] = faces[i]->func;
        }
    }

    std::lock_guard<std::mutex> lock(device->depth_stencil_mutex);
    auto it = device->depth_stencil_states.find(key);
    if (it != device->depth_stencil_states.end())
        return it->second;

    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = to_compare(key[0]);
    dd.depthWriteEnabled = key[1] != 0;
    if (stencil_enable) {
        dd.frontFaceStencil = to_stencil(desc.front_face, desc.stencil_read_mask, desc.stencil_write_mask);
        dd.backFaceStencil = to_stencil(desc.back_face, desc.stencil_read_mask, desc.stencil_write_mask);
    }
    id<MTLDepthStencilState> state = [device->device newDepthStencilStateWithDescriptor:dd];
    if (state)
        device->depth_stencil_states.emplace(key, state);
    return state;
}

// Synthesizes (or fetches from the stage's cache) the stage-in function that
// feeds the vertex shader from the D3D12 input layout. The function reads the
// vertex buffers and their strides at draw time from the IRRuntimeVertexBuffers
// table the replay binds, so strides stay dynamic.
mtlb_result get_stage_in(Device *device, ShaderStage &vs, const mtlb_pipeline_desc &desc, id<MTLFunction> *out)
{
    IRVersionedInputLayoutDescriptor layout = {};
    layout.version = IRInputLayoutDescriptorVersion_1;
    IRInputLayoutDescriptor1 &il = layout.desc_1_0;
    il.numElements = desc.num_input_elements;

    std::string key;  // the layout, serialized
    for (uint32_t i = 0; i < desc.num_input_elements; ++i) {
        const mtlb_input_element &e = desc.input_elements[i];
        if (to_vertex_format(e.format) == MTLVertexFormatInvalid || e.input_slot >= MTLB_MAX_VERTEX_BUFFERS)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported input element " + std::string(e.semantic_name));
        il.semanticNames[i] = e.semantic_name;
        il.inputElementDescs[i] = {e.semantic_index, static_cast<IRFormat>(e.format), e.input_slot, e.byte_offset,
                                   e.input_class == MTLB_INPUT_PER_INSTANCE ? e.step_rate : 0,
                                   e.input_class == MTLB_INPUT_PER_INSTANCE ? IRInputClassificationPerInstanceData
                                                                            : IRInputClassificationPerVertexData};
        key.append(reinterpret_cast<const char *>(&e), sizeof(e));
    }

    std::lock_guard<std::mutex> lock(vs.stage_in_mutex);
    auto it = vs.stage_ins.find(key);
    if (it != vs.stage_ins.end()) {
        *out = it->second;
        return MTLB_OK;
    }
    OwnedMetalLib metallib(IRMetalLibBinaryCreate());
    if (!IRMetalLibSynthesizeStageInFunction(thread_compiler(), vs.reflection.get(), &layout, metallib.ptr))
        return fail(MTLB_ERROR_COMPILE_FAILED, "stage-in function synthesis failed for the input layout");
    NSError *ns_error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithData:IRMetalLibGetBytecodeData(metallib.ptr) error:&ns_error];
    if (!library)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("stage-in library: ") + ns_error.localizedDescription.UTF8String);
    id<MTLFunction> function = [library newFunctionWithName:library.functionNames.firstObject];
    if (!function)
        return fail(MTLB_ERROR_COMPILE_FAILED, "stage-in function missing from its library");
    vs.stage_ins.emplace(std::move(key), function);
    *out = function;
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

    std::shared_ptr<const ShaderStage> vs, ps;
    mtlb_result result = get_stage(device, root_signature, desc->vs_dxil, desc->vs_size, desc->vs_entry,
                                   IRShaderStageVertex, vs);
    if (result != MTLB_OK)
        return result;
    if (desc->ps_dxil && desc->ps_size) {
        result = get_stage(device, root_signature, desc->ps_dxil, desc->ps_size, desc->ps_entry,
                           IRShaderStageFragment, ps);
        if (result != MTLB_OK)
            return result;
    }

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs->function;
    pd.fragmentFunction = ps ? ps->function : nil;
    pd.rasterizationEnabled = ps != nullptr;
    pd.rasterSampleCount = desc->sample_count ? desc->sample_count : 1;
    pd.inputPrimitiveTopology = to_topology_class(desc->topology_type);

    if (vs->num_vertex_inputs) {
        id<MTLFunction> stage_in = nil;
        result = get_stage_in(device, const_cast<ShaderStage &>(*vs), *desc, &stage_in);
        if (result != MTLB_OK)
            return result;
        MTLLinkedFunctions *linked = [MTLLinkedFunctions new];
        linked.functions = @[ stage_in ];
        pd.vertexLinkedFunctions = linked;
    }

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

    if (!kDepthStencilSupported
        && (desc->dsv_format != MTLB_FORMAT_UNKNOWN || desc->depth_enable || desc->stencil_enable)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            std::fprintf(stderr, "d3d12-metal: depth-stencil is not supported yet, pipelines render without it\n");
    }

    NSError *ns_error = nil;
    id<MTLRenderPipelineState> state = [device->device newRenderPipelineStateWithDescriptor:pd error:&ns_error];
    if (!state)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newRenderPipelineState: ") + ns_error.localizedDescription.UTF8String);

    id<MTLDepthStencilState> depth_stencil = get_depth_stencil(device, *desc, kDepthStencilSupported);
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

extern "C" mtlb_result mtlb_compute_pipeline_create(mtlb_device handle, const mtlb_compute_pipeline_desc *desc,
                                                    mtlb_pipeline *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out || !desc->cs_dxil || !desc->cs_size || !desc->root_signature)
        return MTLB_ERROR_INVALID_ARGUMENT;
    RootSignature *root_signature = from_handle<RootSignature>(desc->root_signature);
    if (!root_signature)
        return MTLB_ERROR_INVALID_ARGUMENT;

    std::shared_ptr<const ShaderStage> cs;
    mtlb_result result = get_stage(device, root_signature, desc->cs_dxil, desc->cs_size, desc->cs_entry,
                                   IRShaderStageCompute, cs);
    if (result != MTLB_OK)
        return result;

    NSError *ns_error = nil;
    id<MTLComputePipelineState> state = [device->device newComputePipelineStateWithFunction:cs->function error:&ns_error];
    if (!state)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newComputePipelineState: ") + ns_error.localizedDescription.UTF8String);
    auto *pipeline = new Pipeline();
    pipeline->compute = state;
    pipeline->threadgroup_size = MTLSizeMake(cs->threadgroup_size[0], cs->threadgroup_size[1], cs->threadgroup_size[2]);
    *out = to_handle(pipeline);
    return MTLB_OK;
}

extern "C" mtlb_result mtlb_root_signature_create(mtlb_device handle, const void *blob, uint64_t size,
                                                   mtlb_root_signature *out, mtlb_root_signature_layout *layout)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !blob || !out || !layout)
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

    static std::atomic<uint64_t> next_id{1};
    *out = to_handle(new RootSignature{device, next_id++, root_signature.ptr});
    root_signature.ptr = nullptr;  // owned by the handle now
    return MTLB_OK;
}

extern "C" void mtlb_root_signature_destroy(mtlb_root_signature handle)
{
    RootSignature *root_signature = from_handle<RootSignature>(handle);
    if (!root_signature)
        return;
    {
        // Shaders converted against this root signature can no longer be reused.
        Device *device = root_signature->device;
        std::lock_guard<std::mutex> lock(device->shaders_mutex);
        for (auto it = device->shaders.begin(); it != device->shaders.end();)
            it = std::get<2>(it->first) == root_signature->id ? device->shaders.erase(it) : std::next(it);
    }
    IRRootSignatureDestroy(root_signature->ir);
    delete root_signature;
}

extern "C" void mtlb_pipeline_destroy(mtlb_pipeline handle)
{
    delete from_handle<mtlb::Pipeline>(handle);
}
