// Pipeline creation: DXIL -> Metal IR via libmetalirconverter, then the Metal
// render pipeline and depth-stencil state.
#include "internal.h"
#include "dxbc.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
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

// The converter settings every shader is compiled with (they are part of the disk cache key).
constexpr IRCompatibilityFlags kCompatibilityFlags = static_cast<IRCompatibilityFlags>(
    IRCompatibilityFlagBoundsCheck | IRCompatibilityFlagTextureMinLODClamp | IRCompatibilityFlagSamplerLODBias
    | IRCompatibilityFlagSampleNanToZero | IRCompatibilityFlagPositionInvariance);
constexpr uint32_t kCacheRevision = 2;  // bump when the conversion changes in a way the key does not capture

// One compiler per thread: creating one per stage per pipeline is wasteful. Pipelines with geometry or
// tessellation stages need a compiler of their own (the emulation option persists in it).
IRCompiler *thread_compiler(bool emulation = false)
{
    thread_local OwnedCompiler plain_compiler, emulation_compiler;
    OwnedCompiler &compiler = emulation ? emulation_compiler : plain_compiler;
    if (!compiler.ptr) {
        compiler.ptr = IRCompilerCreate();
        if (emulation)
            IRCompilerEnableGeometryAndTessellationEmulation(compiler.ptr, true);
        // Vertex shaders fetch their inputs through a separate stage-in function
        // (see get_stage_in) instead of Metal vertex fetch.
        IRCompilerSetStageInGenerationMode(compiler.ptr, IRStageInCodeGenerationModeUseSeparateStageInFunction);
        // D3D12 semantics the converter leaves off by default: out-of-bounds buffer and texture reads
        // return zero (and writes are dropped), the descriptors' min LOD clamp and the samplers' LOD bias
        // are applied, NaN coordinates sample as zero, and equal vertex shaders give equal positions.
        IRCompilerSetCompatibilityFlags(compiler.ptr, kCompatibilityFlags);
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

// How a stage is converted: for the geometry/tessellation emulation (a mesh pipeline made of these stages) the
// converter needs the option and the topology the pipeline draws with.
struct StageOptions {
    bool emulation = false;
    IRInputTopology topology = IRInputTopologyUndefined;
    uint32_t key() const { return emulation ? 0x100u | (static_cast<uint32_t>(topology) << 12) : 0; }
};

// Loads a converted shader's Metal function and reflection into `out`.
mtlb_result finish_stage(Device *device, dispatch_data_t bytecode, std::shared_ptr<IRShaderReflection> reflection,
                         IRShaderStage ir_stage, bool emulation, ShaderStage &out)
{
    const char *function_name = IRShaderReflectionGetEntryPointFunctionName(reflection.get());
    if (!function_name)
        return fail(MTLB_ERROR_COMPILE_FAILED, "shader reflection has no entry point");

    NSError *ns_error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithData:bytecode error:&ns_error];
    if (!library)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newLibraryWithData: ") + ns_error.localizedDescription.UTF8String);
    out.entry_name = function_name;
    if (emulation) {
        out.library = library;  // functions with constants: the runtime instantiates them
    } else {
        out.function = [library newFunctionWithName:[NSString stringWithUTF8String:function_name]];
        if (!out.function)
            return fail(MTLB_ERROR_COMPILE_FAILED, std::string("function not found in converted library: ") + function_name);
    }

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
        out.vertex_output_size = info.info_1_0.vertex_output_size_in_bytes;
        IRShaderReflectionReleaseVertexInfo(&info);
        out.reflection = std::move(reflection);
    } else if (ir_stage == IRShaderStageFragment) {
        out.reflection = std::move(reflection);  // the render target output types, at pipeline creation
    } else if (ir_stage == IRShaderStageGeometry) {
        IRVersionedGSInfo info;
        if (!IRShaderReflectionCopyGeometryInfo(reflection.get(), IRReflectionVersion_1_0, &info))
            return fail(MTLB_ERROR_COMPILE_FAILED, "geometry reflection unavailable");
        out.gs_max_input_primitives = info.info_1_0.max_input_primitives_per_mesh_threadgroup;
        out.gs_instance_count = std::max<uint32_t>(info.info_1_0.instance_count, 1);
        out.gs_input_primitive = static_cast<uint32_t>(info.info_1_0.input_primitive);
        out.gs_passthrough = info.info_1_0.is_passthrough;
        IRShaderReflectionReleaseGeometryInfo(&info);
    } else if (ir_stage == IRShaderStageHull) {
        IRVersionedHSInfo info;
        if (!IRShaderReflectionCopyHullInfo(reflection.get(), IRReflectionVersion_1_0, &info))
            return fail(MTLB_ERROR_COMPILE_FAILED, "hull reflection unavailable");
        out.hs_max_patches = info.info_1_0.max_patches_per_object_threadgroup;
        out.hs_max_object_threads = info.info_1_0.max_object_threads_per_patch;
        out.hs_input_control_points = info.info_1_0.input_control_point_count;
        out.hs_output_primitive = static_cast<uint32_t>(info.info_1_0.tessellator_output_primitive);
        out.hs_max_tess_factor = info.info_1_0.max_tessellation_factor;
        IRShaderReflectionReleaseHullInfo(&info);
    } else if (ir_stage == IRShaderStageDomain) {
        IRVersionedDSInfo info;
        if (!IRShaderReflectionCopyDomainInfo(reflection.get(), IRReflectionVersion_1_0, &info))
            return fail(MTLB_ERROR_COMPILE_FAILED, "domain reflection unavailable");
        out.ds_max_input_prims = info.info_1_0.max_input_prims_per_mesh_threadgroup;
        IRShaderReflectionReleaseDomainInfo(&info);
    }
    return MTLB_OK;
}

dispatch_data_t bytecode_data(const std::vector<uint8_t> &bytes)
{
    return dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}

// The disk cache key of a converted shader: everything its conversion depends on.
CacheKey stage_key(RootSignature *root_signature, const void *dxil, uint64_t size, const char *entry, IRShaderStage ir_stage,
                   const StageOptions &options)
{
    Hasher h;
    h.update(std::string("stage"));
    h.update(DiskCache::converter_identity());
    h.update_value(kCacheRevision);
    h.update_value(static_cast<uint32_t>(kCompatibilityFlags));
    h.update_value(static_cast<uint32_t>(ir_stage));
    if (options.emulation)  // (the keys of the other stages stay as they were)
        h.update_value(options.key());
    h.update(std::string(entry ? entry : ""));
    h.update_value('\0');
    h.update(root_signature->blob_hash.data(), root_signature->blob_hash.size());
    h.update_value(size);
    h.update(dxil, size);
    return h.finish();
}

// Converts one DXIL shader against `root_signature` and loads its Metal function, or loads the converter's
// earlier output from the disk cache.
mtlb_result convert_stage(Device *device, RootSignature *root_signature, const void *dxil, uint64_t size,
                          const char *entry, IRShaderStage ir_stage, const StageOptions &options, ShaderStage &out)
{
    DiskCache &cache = DiskCache::instance();
    const bool use_cache = cache.enabled();
    CacheKey key{};
    if (use_cache) {
        key = stage_key(root_signature, dxil, size, entry, ir_stage, options);
        std::vector<uint8_t> metallib;
        std::string json;
        if (cache.load(CacheKind::Stage, key, metallib, json)) {
            std::shared_ptr<IRShaderReflection> reflection(IRShaderReflectionCreateFromJSON(json.c_str()), IRShaderReflectionDestroy);
            if (reflection && finish_stage(device, bytecode_data(metallib), reflection, ir_stage, options.emulation, out) == MTLB_OK) {
                out.cache_key = key;
                return MTLB_OK;
            }
            out.function = nil;  // an entry the converter or Metal rejects: rebuild it below
            out.reflection.reset();
            cache.stats().corrupt++;
        }
    }

    // Shader Model 4/5 bytecode becomes DXIL first (the cache above is keyed on the DXBC).
    const void *source = dxil;
    uint64_t source_size = size;
    std::vector<uint8_t> converted;
    if (is_dxbc_only(dxil, size)) {
        std::string dxbc_error;
        if (!dxbc_to_dxil(dxil, size, converted, dxbc_error))
            return fail(MTLB_ERROR_COMPILE_FAILED, dxbc_error);
        source = converted.data();
        source_size = converted.size();
    }

    IRCompiler *compiler = thread_compiler(options.emulation);
    if (options.emulation)
        IRCompilerSetInputTopology(compiler, options.topology);
    IRCompilerSetGlobalRootSignature(compiler, root_signature->ir);

    OwnedObject input(IRObjectCreateFromDXIL(static_cast<const uint8_t *>(source), source_size, IRBytecodeOwnershipNone));
    IRError *error = nullptr;
    OwnedObject output(IRCompilerAllocCompileAndLink(compiler, entry, input.ptr, &error));
    // The compiler outlives root signatures; do not leave it pointing at this one.
    IRCompilerSetGlobalRootSignature(compiler, nullptr);
    if (!output.ptr) {
        // D3D12METAL_DUMP_FAILED=<dir> keeps the shaders the converter rejects, for inspection with dxc -dumpbin.
        if (const char *dir = std::getenv("D3D12METAL_DUMP_FAILED")) {
            std::string path = std::string(dir) + "/failed_" + std::to_string(hash_bytes(dxil, size)) + ".dxil";
            if (FILE *f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(dxil, 1, size, f);
                std::fclose(f);
            }
        }
        return fail(MTLB_ERROR_COMPILE_FAILED, error_text(error, "DXIL conversion failed"));
    }

    OwnedMetalLib metallib(IRMetalLibBinaryCreate());
    if (!IRObjectGetMetalLibBinary(output.ptr, ir_stage, metallib.ptr))
        return fail(MTLB_ERROR_COMPILE_FAILED, "converted shader has no metallib for the requested stage");

    std::shared_ptr<IRShaderReflection> reflection(IRShaderReflectionCreate(), IRShaderReflectionDestroy);
    if (!IRObjectGetReflection(output.ptr, ir_stage, reflection.get()))
        return fail(MTLB_ERROR_COMPILE_FAILED, "shader reflection unavailable");
    out.cache_key = key;
    mtlb_result result = finish_stage(device, IRMetalLibGetBytecodeData(metallib.ptr), reflection, ir_stage, options.emulation, out);
    if (result == MTLB_OK && use_cache) {
        std::vector<uint8_t> bytes(IRMetalLibGetBytecodeSize(metallib.ptr));
        IRMetalLibGetBytecode(metallib.ptr, bytes.data());
        const char *json = IRShaderReflectionCopyJSONString(reflection.get());
        if (json) {
            cache.store(CacheKind::Stage, key, bytes.data(), bytes.size(), json);
            IRShaderReflectionReleaseString(json);
        }
    }
    return result;
}

// Returns the converted stage from the device cache, converting it on a miss.
mtlb_result get_stage(Device *device, RootSignature *root_signature, const void *dxil, uint64_t size,
                      const char *entry, IRShaderStage ir_stage, std::shared_ptr<const ShaderStage> &out,
                      const StageOptions &options = {})
{
    const ShaderKey key{hash_bytes(dxil, size), size, root_signature->id, static_cast<uint32_t>(ir_stage) | options.key(),
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
    mtlb_result result = convert_stage(device, root_signature, dxil, size, entry, ir_stage, options, *stage);
    if (result != MTLB_OK)
        return result;
    std::lock_guard<std::mutex> lock(device->shaders_mutex);
    out = device->shaders.emplace(key, std::move(stage)).first->second;  // keeps the first on a race
    return MTLB_OK;
}

// The format of the same size and channels with the other kind of shader output (integer for float and normalised
// formats, float or normalised for integer ones); Invalid where there is none.
MTLPixelFormat opposite_kind_format(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatR8Unorm: return MTLPixelFormatR8Uint;
    case MTLPixelFormatRG8Unorm: return MTLPixelFormatRG8Uint;
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Uint;
    case MTLPixelFormatRGB10A2Unorm: return MTLPixelFormatRGB10A2Uint;
    case MTLPixelFormatR16Float: case MTLPixelFormatR16Unorm: return MTLPixelFormatR16Uint;
    case MTLPixelFormatRG16Float: case MTLPixelFormatRG16Unorm: return MTLPixelFormatRG16Uint;
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm: return MTLPixelFormatRGBA16Uint;
    case MTLPixelFormatR32Float: return MTLPixelFormatR32Uint;
    case MTLPixelFormatRG32Float: return MTLPixelFormatRG32Uint;
    case MTLPixelFormatRGBA32Float: return MTLPixelFormatRGBA32Uint;
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: return MTLPixelFormatR8Unorm;
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint: return MTLPixelFormatRG8Unorm;
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatRGB10A2Uint: return MTLPixelFormatRGB10A2Unorm;
    case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: return MTLPixelFormatR16Float;
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: return MTLPixelFormatRG16Float;
    case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: return MTLPixelFormatRGBA16Float;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: return MTLPixelFormatR32Float;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: return MTLPixelFormatRG32Float;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: return MTLPixelFormatRGBA32Float;
    default: return MTLPixelFormatInvalid;
    }
}

// True for the pixel formats whose shader outputs are integers (uint4 / int4).
bool is_integer_pixel_format(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGB10A2Uint:
        return true;
    default:
        return false;
    }
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
mtlb_result get_stage_in(Device *device, ShaderStage &vs, const mtlb_pipeline_desc &desc, id<MTLFunction> *out,
                         bool emulation = false, id<MTLLibrary> *out_library = nullptr)
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
    if (emulation) {
        auto emulated = vs.emulation_stage_ins.find(key);
        if (emulated != vs.emulation_stage_ins.end()) {
            *out_library = emulated->second;
            return MTLB_OK;
        }
    } else {
        auto it = vs.stage_ins.find(key);
        if (it != vs.stage_ins.end()) {
            *out = it->second;
            return MTLB_OK;
        }
    }
    DiskCache &cache = DiskCache::instance();
    const bool use_cache = cache.enabled();
    CacheKey disk_key{};
    dispatch_data_t bytecode = nil;
    OwnedMetalLib metallib(IRMetalLibBinaryCreate());
    if (use_cache) {
        Hasher h;
        h.update(std::string("stagein"));
        h.update(DiskCache::converter_identity());
        h.update_value(kCacheRevision);
        h.update(vs.cache_key.data(), vs.cache_key.size());
        h.update_value(emulation);
        h.update(key);  // the layout
        disk_key = h.finish();
        std::vector<uint8_t> cached;
        std::string unused;
        if (cache.load(CacheKind::StageIn, disk_key, cached, unused))
            bytecode = bytecode_data(cached);
    }
    // A cached entry that Metal rejects (intact on disk, but not a usable library) is discarded and rebuilt.
    id<MTLFunction> function = nil;
    id<MTLLibrary> stage_in_library = nil;
    bool from_cache = false;
    NSError *ns_error = nil;
    if (bytecode != nil) {
        id<MTLLibrary> library = [device->device newLibraryWithData:bytecode error:&ns_error];
        function = library ? [library newFunctionWithName:library.functionNames.firstObject] : nil;
        if (function) {
            from_cache = true;
            stage_in_library = library;
        } else {
            cache.discard(CacheKind::StageIn, disk_key);
            ns_error = nil;
        }
    }
    if (!function) {
        if (!IRMetalLibSynthesizeStageInFunction(thread_compiler(emulation), vs.reflection.get(), &layout, metallib.ptr))
            return fail(MTLB_ERROR_COMPILE_FAILED, "stage-in function synthesis failed for the input layout");
        id<MTLLibrary> library = [device->device newLibraryWithData:IRMetalLibGetBytecodeData(metallib.ptr) error:&ns_error];
        if (!library)
            return fail(MTLB_ERROR_COMPILE_FAILED, std::string("stage-in library: ") + ns_error.localizedDescription.UTF8String);
        function = [library newFunctionWithName:library.functionNames.firstObject];
        if (!function)
            return fail(MTLB_ERROR_COMPILE_FAILED, "stage-in function missing from its library");
        stage_in_library = library;
    }
    if (use_cache && !from_cache) {
        std::vector<uint8_t> bytes(IRMetalLibGetBytecodeSize(metallib.ptr));
        IRMetalLibGetBytecode(metallib.ptr, bytes.data());
        cache.store(CacheKind::StageIn, disk_key, bytes.data(), bytes.size(), std::string());
    }
    if (emulation) {
        *out_library = stage_in_library;
        vs.emulation_stage_ins.emplace(std::move(key), stage_in_library);
        return MTLB_OK;
    }
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

// The render targets (bit i: SV_Target i) a pixel shader declares in the output signature of its DXIL container. All of
// them when the container cannot be read: a target counted as written only decides whether its type is checked.
uint32_t dxil_written_targets(const void *dxil, uint64_t size)
{
    constexpr uint32_t kAll = 0xff;
    if (!dxil || size < 32)
        return kAll;
    const auto *bytes = static_cast<const uint8_t *>(dxil);
    auto u32 = [&](uint64_t offset) {
        uint32_t v;
        std::memcpy(&v, bytes + offset, 4);
        return v;
    };
    if (u32(0) != 0x43425844u)  // 'DXBC'
        return kAll;
    const uint32_t parts = u32(28);
    if (32 + uint64_t(parts) * 4 > size)
        return kAll;
    for (uint32_t i = 0; i < parts; ++i) {
        const uint64_t at = u32(32 + i * 4);
        if (at + 8 > size)
            return kAll;
        if (u32(at) != 0x3147534Fu)  // 'OSG1'
            continue;
        const uint64_t end = std::min<uint64_t>(at + 8 + u32(at + 4), size);
        const uint64_t data = at + 8;
        if (data + 8 > end)
            return kAll;
        const uint32_t count = u32(data), first = u32(data + 4);
        constexpr uint64_t kElement = 32;  // DxilProgramSignatureElement
        if (first < 8 || data + first + uint64_t(count) * kElement > end)
            return kAll;
        uint32_t written = 0;
        for (uint32_t e = 0; e < count; ++e) {
            const uint64_t element = data + first + e * kElement;
            const uint32_t semantic_index = u32(element + 8), system_value = u32(element + 12);
            if (system_value == 64 && semantic_index < 8)  // D3D_NAME_TARGET
                written |= 1u << semantic_index;
        }
        return written;
    }
    return kAll;
}

// The attachment formats and blend state of a pipeline, shared by plain and emulated (mesh) pipelines.
struct Attachments {
    std::array<MTLPixelFormat, MTLB_MAX_RENDER_TARGETS> color_view_formats{};
    MTLPixelFormat depth_format = MTLPixelFormatInvalid, stencil_format = MTLPixelFormatInvalid;
};

mtlb_result fill_attachments(const mtlb_pipeline_desc &desc, const ShaderStage *ps,
                             MTLRenderPipelineColorAttachmentDescriptorArray *color_attachments, Attachments *out)
{
    const mtlb_pipeline_desc *const d = &desc;
    // Bit i is set when the fragment shader writes integers to render target i; written_outputs, when it writes target i at
    // all (a target it leaves alone keeps the view of its own format).
    uint32_t integer_outputs = 0, written_outputs = ps ? dxil_written_targets(d->ps_dxil, d->ps_size) : 0;
    if (ps) {
        IRVersionedFSInfo info;
        if (IRShaderReflectionCopyFragmentInfo(ps->reflection.get(), IRReflectionVersion_1_0, &info)) {
            integer_outputs = info.info_1_0.rt_index_int;
            IRShaderReflectionReleaseFragmentInfo(&info);
        }
    }
    auto &color_view_formats = out->color_view_formats;
    for (uint32_t i = 0; i < d->num_render_targets; ++i) {
        if (d->rtv_formats[i] == MTLB_FORMAT_UNKNOWN)
            continue;
        MTLPixelFormat format = to_pixel_format(d->rtv_formats[i]);
        if (format == MTLPixelFormatInvalid)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported render target format " + std::to_string(d->rtv_formats[i]));
        // D3D12 tolerates a shader output of another type than the render target (the result is undefined, and
        // games do it with the output masked); Metal refuses the pipeline. The target is written through a view of
        // the other kind instead (the bits land as they are); where no such view exists, the pipeline is refused.
        if (ps && ((written_outputs >> i) & 1) && ((integer_outputs >> i) & 1) != (is_integer_pixel_format(format) ? 1u : 0u)) {
            const MTLPixelFormat other = opposite_kind_format(format);
            if (other != MTLPixelFormatInvalid) {
                color_view_formats[i] = other;
                format = other;
            }
        }
        const mtlb_render_target_blend &blend = d->blend[d->independent_blend ? i : 0];
        MTLRenderPipelineColorAttachmentDescriptor *ca = color_attachments[i];
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

    // D24_UNORM_S8_UINT is a 32-bit depth with 8-bit stencil here (docs/STATUS.md).
    if (d->dsv_format != MTLB_FORMAT_UNKNOWN) {
        out->depth_format = to_texture_pixel_format(d->dsv_format, true);
        if (out->depth_format == MTLPixelFormatInvalid)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported depth-stencil format " + std::to_string(d->dsv_format));
        if (out->depth_format == MTLPixelFormatDepth32Float_Stencil8)
            out->stencil_format = out->depth_format;
    }
    return MTLB_OK;
}

// True when the DXIL container's runtime info (PSV0 part) lists a UAV binding. A container that cannot be parsed counts
// as using one: the answer only decides how conservatively passes are merged.
bool dxil_binds_uav(const void *dxil, uint64_t size)
{
    if (!dxil || size < 32)
        return size != 0;
    const auto *bytes = static_cast<const uint8_t *>(dxil);
    auto u32 = [&](uint64_t offset) {
        uint32_t v;
        std::memcpy(&v, bytes + offset, 4);
        return v;
    };
    if (u32(0) != 0x43425844u)  // 'DXBC'
        return true;
    const uint32_t parts = u32(28);
    if (32 + uint64_t(parts) * 4 > size)
        return true;
    for (uint32_t i = 0; i < parts; ++i) {
        const uint64_t at = u32(32 + i * 4);
        if (at + 8 > size)
            return true;
        if (u32(at) != 0x30565350u)  // 'PSV0'
            continue;
        const uint64_t end = std::min<uint64_t>(at + 8 + u32(at + 4), size);
        uint64_t cursor = at + 8;
        if (cursor + 4 > end)
            return true;
        cursor += 4 + uint64_t(u32(cursor));  // the runtime info
        if (cursor + 4 > end)
            return true;
        const uint32_t resources = u32(cursor);
        cursor += 4;
        if (!resources)
            return false;
        if (cursor + 4 > end)
            return true;
        const uint32_t stride = u32(cursor);
        cursor += 4;
        if (stride < 16 || cursor + uint64_t(resources) * stride > end)
            return true;
        for (uint32_t r = 0; r < resources; ++r) {
            if (u32(cursor + uint64_t(r) * stride) >= 6)  // PSVResourceType: UAVTyped and above
                return true;
        }
        return false;
    }
    return true;
}

// True when any stage of the description binds a UAV.
bool desc_binds_uav(const mtlb_pipeline_desc &d)
{
    return dxil_binds_uav(d.vs_dxil, d.vs_size) || dxil_binds_uav(d.ps_dxil, d.ps_size) || dxil_binds_uav(d.gs_dxil, d.gs_size)
           || dxil_binds_uav(d.hs_dxil, d.hs_size) || dxil_binds_uav(d.ds_dxil, d.ds_size);
}

IRInputTopology to_input_topology(uint32_t type)
{
    switch (type) {
    case MTLB_TOPOLOGY_TYPE_POINT: return IRInputTopologyPoint;
    case MTLB_TOPOLOGY_TYPE_LINE: return IRInputTopologyLine;
    case MTLB_TOPOLOGY_TYPE_PATCH: return IRInputTopologyPatch;
    default: return IRInputTopologyTriangle;
    }
}

// Builds the pipeline of a description with geometry and/or tessellation stages out of mesh shaders.
mtlb_result create_emulated_pipeline(Device *device, RootSignature *root_signature, const mtlb_pipeline_desc &desc,
                                     mtlb_pipeline *out)
{
    const bool tessellation = desc.hs_size != 0;
    if (desc.topology_type == MTLB_TOPOLOGY_TYPE_PATCH && !tessellation)
        return fail(MTLB_ERROR_UNSUPPORTED, "a patch topology without a hull shader");
    if (tessellation && desc.topology_type != MTLB_TOPOLOGY_TYPE_PATCH)
        return fail(MTLB_ERROR_UNSUPPORTED, "a hull shader needs the patch topology type");
    const StageOptions options{true, to_input_topology(desc.topology_type)};

    std::shared_ptr<const ShaderStage> vs, ps, gs, hs, ds;
    mtlb_result result = get_stage(device, root_signature, desc.vs_dxil, desc.vs_size, desc.vs_entry, IRShaderStageVertex, vs, options);
    if (result == MTLB_OK && desc.ps_size)
        result = get_stage(device, root_signature, desc.ps_dxil, desc.ps_size, desc.ps_entry, IRShaderStageFragment, ps, options);
    if (result == MTLB_OK && desc.gs_size)
        result = get_stage(device, root_signature, desc.gs_dxil, desc.gs_size, nullptr, IRShaderStageGeometry, gs, options);
    if (result == MTLB_OK && tessellation) {
        result = get_stage(device, root_signature, desc.hs_dxil, desc.hs_size, nullptr, IRShaderStageHull, hs, options);
        if (result == MTLB_OK)
            result = get_stage(device, root_signature, desc.ds_dxil, desc.ds_size, nullptr, IRShaderStageDomain, ds, options);
    }
    if (result != MTLB_OK)
        return result;
    if (!ps)
        return fail(MTLB_ERROR_UNSUPPORTED, "a pipeline with geometry or tessellation stages needs a pixel shader");

    auto emulated = std::make_unique<EmulatedPipeline>();
    emulated->tessellation = tessellation;
    emulated->vertex = vs->library;
    emulated->vertex_name = vs->entry_name;
    emulated->fragment = ps->library;
    emulated->fragment_name = ps->entry_name;
    if (gs) {
        emulated->geometry = gs->library;
        emulated->geometry_name = gs->entry_name;
    }
    if (tessellation) {
        emulated->hull = hs->library;
        emulated->domain = ds->library;
    }
    // The emulated pipelines always link a stage-in function; one for a shader that reads no vertex inputs (it pulls
    // its data from buffers) is built for an empty layout.
    mtlb_pipeline_desc no_inputs;
    const mtlb_pipeline_desc *layout_desc = &desc;
    if (!vs->num_vertex_inputs) {
        no_inputs = desc;
        no_inputs.num_input_elements = 0;
        layout_desc = &no_inputs;
    }
    id<MTLFunction> unused = nil;
    id<MTLLibrary> stage_in = nil;
    result = get_stage_in(device, const_cast<ShaderStage &>(*vs), *layout_desc, &unused, true, &stage_in);
    if (result != MTLB_OK)
        return result;
    emulated->stage_in = stage_in;

    if (tessellation) {
        const uint32_t output_primitive = hs->hs_output_primitive;
        IRRuntimeTessellationPipelineConfig &c = emulated->ts_config;
        c.outputPrimitiveType = static_cast<IRRuntimeTessellatorOutputPrimitive>(output_primitive);
        c.vsOutputSizeInBytes = vs->vertex_output_size;
        c.gsMaxInputPrimitivesPerMeshThreadgroup = gs ? gs->gs_max_input_primitives : ds->ds_max_input_prims;
        c.hsMaxPatchesPerObjectThreadgroup = hs->hs_max_patches;
        c.hsInputControlPointCount = hs->hs_input_control_points;
        c.hsMaxObjectThreadsPerThreadgroup = hs->hs_max_object_threads;
        c.hsMaxTessellationFactor = hs->hs_max_tess_factor;
        c.gsInstanceCount = gs ? gs->gs_instance_count : 1;
        emulated->patch_control_points = hs->hs_input_control_points;
        if (!gs) {  // the domain library's own pass-through geometry shader
            emulated->geometry_name = output_primitive == IRRuntimeTessellatorOutputPoint ? kIRPointPassthroughGeometryShader
                                      : output_primitive == IRRuntimeTessellatorOutputLine ? kIRLinePassthroughGeometryShader
                                                                                           : kIRTrianglePassthroughGeometryShader;
        }
    } else {
        emulated->gs_config = {vs->vertex_output_size, gs->gs_max_input_primitives};
    }

    MTLMeshRenderPipelineDescriptor *md = [MTLMeshRenderPipelineDescriptor new];
    md.rasterSampleCount = desc.sample_count ? desc.sample_count : 1;
    Attachments attachments;
    result = fill_attachments(desc, ps.get(), md.colorAttachments, &attachments);
    if (result != MTLB_OK)
        return result;
    if (attachments.depth_format != MTLPixelFormatInvalid)
        md.depthAttachmentPixelFormat = attachments.depth_format;
    if (attachments.stencil_format != MTLPixelFormatInvalid)
        md.stencilAttachmentPixelFormat = attachments.stencil_format;

    NSError *ns_error = nil;
    id<MTLRenderPipelineState> state = build_emulated_state(device, *emulated, md, &ns_error);
    if (!state)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("emulated pipeline: ") + (ns_error ? ns_error.localizedDescription.UTF8String : "failed"));

    id<MTLDepthStencilState> depth_stencil = get_depth_stencil(device, desc, true);
    id<MTLDepthStencilState> depth_stencil_off = get_depth_stencil(device, desc, false);
    if (!depth_stencil || !depth_stencil_off)
        return fail(MTLB_ERROR_COMPILE_FAILED, "newDepthStencilState failed");
    auto *pipeline = new Pipeline();
    pipeline->device = device;
    pipeline->state = state;
    pipeline->emulated = std::move(emulated);
    pipeline->mesh_descriptor = md;
    pipeline->writes_uav = desc_binds_uav(desc);
    pipeline->depth_format = attachments.depth_format;
    pipeline->stencil_format = attachments.stencil_format;
    pipeline->color_view_formats = attachments.color_view_formats;
    pipeline->depth_stencil = depth_stencil;
    pipeline->depth_stencil_off = depth_stencil_off;
    pipeline->cull_mode = desc.cull_mode == MTLB_CULL_FRONT ? MTLCullModeFront
                          : desc.cull_mode == MTLB_CULL_BACK ? MTLCullModeBack : MTLCullModeNone;
    pipeline->winding = desc.front_counter_clockwise ? MTLWindingCounterClockwise : MTLWindingClockwise;
    pipeline->fill_mode = desc.fill_mode == MTLB_FILL_WIREFRAME ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    pipeline->depth_clip = desc.depth_clip_enable ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
    pipeline->depth_bias = static_cast<float>(desc.depth_bias);
    pipeline->slope_scaled_depth_bias = desc.slope_scaled_depth_bias;
    pipeline->depth_bias_clamp = desc.depth_bias_clamp;
    *out = to_handle(pipeline);
    return MTLB_OK;
}

} // namespace

id<MTLRenderPipelineState> mtlb::build_emulated_state(Device *device, const EmulatedPipeline &e,
                                                      MTLMeshRenderPipelineDescriptor *descriptor, NSError **error)
{
    MTLMeshRenderPipelineDescriptor *base = [descriptor copy];
    if (e.tessellation) {
        IRGeometryTessellationEmulationPipelineDescriptor d = {};
        d.stageInLibrary = e.stage_in;
        d.vertexLibrary = e.vertex;
        d.vertexFunctionName = e.vertex_name.c_str();
        d.hullLibrary = e.hull;
        d.hullFunctionName = "irconverter_hull_shader";
        d.domainLibrary = e.domain;
        d.domainFunctionName = "irconverter_dxil_domain_shader";
        d.geometryLibrary = e.geometry;
        d.geometryFunctionName = e.geometry_name.c_str();
        d.fragmentLibrary = e.fragment;
        d.fragmentFunctionName = e.fragment_name.c_str();
        d.basePipelineDescriptor = base;
        d.pipelineConfig = e.ts_config;
        return IRRuntimeNewGeometryTessellationEmulationPipeline(device->device, &d, error);
    }
    IRGeometryEmulationPipelineDescriptor d = {};
    d.stageInLibrary = e.stage_in;
    d.vertexLibrary = e.vertex;
    d.vertexFunctionName = e.vertex_name.c_str();
    d.geometryLibrary = e.geometry;
    d.geometryFunctionName = e.geometry_name.c_str();
    d.fragmentLibrary = e.fragment;
    d.fragmentFunctionName = e.fragment_name.c_str();
    d.basePipelineDescriptor = base;
    d.pipelineConfig = e.gs_config;
    return IRRuntimeNewGeometryEmulationPipeline(device->device, &d, error);
}

extern "C" void mtlb_device_test_fail_next_pipeline(mtlb_device handle, mtlb_result result)
{
    if (Device *device = from_handle<Device>(handle))
        device->test_fail_pipeline.store(result);
}

extern "C" mtlb_result mtlb_pipeline_create(mtlb_device handle, const mtlb_pipeline_desc *desc, mtlb_pipeline *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out || !desc->vs_dxil || !desc->vs_size || !desc->root_signature
        || desc->num_render_targets > MTLB_MAX_RENDER_TARGETS
        || desc->num_input_elements > MTLB_MAX_INPUT_ELEMENTS)
        return MTLB_ERROR_INVALID_ARGUMENT;
    stat_add(kStatPipelineAttempts);
    if (const int injected = device->test_fail_pipeline.exchange(0))
        return fail(static_cast<mtlb_result>(injected), "injected pipeline failure");

    RootSignature *root_signature = from_handle<RootSignature>(desc->root_signature);
    if (!root_signature)
        return MTLB_ERROR_INVALID_ARGUMENT;
    if (desc->gs_size || desc->hs_size)
        return create_emulated_pipeline(device, root_signature, *desc, out);

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
    // A depth-only pipeline has no fragment function but still rasterizes.
    pd.rasterizationEnabled = ps != nullptr || desc->dsv_format != MTLB_FORMAT_UNKNOWN;
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

    Attachments attachments;
    result = fill_attachments(*desc, ps.get(), pd.colorAttachments, &attachments);
    if (result != MTLB_OK)
        return result;
    if (attachments.depth_format != MTLPixelFormatInvalid)
        pd.depthAttachmentPixelFormat = attachments.depth_format;
    if (attachments.stencil_format != MTLPixelFormatInvalid)
        pd.stencilAttachmentPixelFormat = attachments.stencil_format;
    const MTLPixelFormat depth_format = attachments.depth_format, stencil_format = attachments.stencil_format;

    NSError *ns_error = nil;
    id<MTLRenderPipelineState> state = [device->device newRenderPipelineStateWithDescriptor:pd error:&ns_error];
    if (!state)
        return fail(MTLB_ERROR_COMPILE_FAILED, std::string("newRenderPipelineState: ") + ns_error.localizedDescription.UTF8String);

    id<MTLDepthStencilState> depth_stencil = get_depth_stencil(device, *desc, true);
    id<MTLDepthStencilState> depth_stencil_off = get_depth_stencil(device, *desc, false);
    if (!depth_stencil || !depth_stencil_off)
        return fail(MTLB_ERROR_COMPILE_FAILED, "newDepthStencilState failed");

    auto *pipeline = new Pipeline();
    pipeline->device = device;
    pipeline->state = state;
    pipeline->descriptor = pd;
    pipeline->writes_uav = desc_binds_uav(*desc);
    pipeline->depth_format = depth_format;
    pipeline->stencil_format = stencil_format;
    pipeline->color_view_formats = attachments.color_view_formats;
    pipeline->depth_stencil = depth_stencil;
    pipeline->depth_stencil_off = depth_stencil_off;
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

id<MTLRenderPipelineState> mtlb::Pipeline::state_for(MTLPixelFormat depth, MTLPixelFormat stencil)
{
    if (depth == depth_format && stencil == stencil_format)
        return state;
    const uint64_t key = (uint64_t(depth) << 32) | uint64_t(stencil);
    std::lock_guard<std::mutex> lock(variants_mutex);
    auto it = variants.find(key);
    if (it != variants.end())
        return it->second;
    NSError *error = nil;
    id<MTLRenderPipelineState> variant;
    if (emulated) {
        MTLMeshRenderPipelineDescriptor *copy = [mesh_descriptor copy];
        copy.depthAttachmentPixelFormat = depth;
        copy.stencilAttachmentPixelFormat = stencil;
        variant = build_emulated_state(device, *emulated, copy, &error);
    } else {
        MTLRenderPipelineDescriptor *copy = [descriptor copy];
        copy.depthAttachmentPixelFormat = depth;
        copy.stencilAttachmentPixelFormat = stencil;
        variant = [device->device newRenderPipelineStateWithDescriptor:copy error:&error];
    }
    if (!variant) {
        fail(MTLB_ERROR_COMPILE_FAILED, std::string("pipeline variant for another depth-stencil format: ") + error.localizedDescription.UTF8String);
        return nil;
    }
    variants.emplace(key, variant);
    return variant;
}

extern "C" mtlb_result mtlb_compute_pipeline_create(mtlb_device handle, const mtlb_compute_pipeline_desc *desc,
                                                    mtlb_pipeline *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out || !desc->cs_dxil || !desc->cs_size || !desc->root_signature)
        return MTLB_ERROR_INVALID_ARGUMENT;
    stat_add(kStatPipelineAttempts);
    if (const int injected = device->test_fail_pipeline.exchange(0))
        return fail(static_cast<mtlb_result>(injected), "injected pipeline failure");
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

    Hasher blob_hasher;
    blob_hasher.update(blob, size);
    static std::atomic<uint64_t> next_id{1};
    *out = to_handle(new RootSignature{device, next_id++, root_signature.ptr, blob_hasher.finish()});
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
