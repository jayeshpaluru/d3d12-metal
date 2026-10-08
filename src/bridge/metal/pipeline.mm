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

// Loads a converted shader's Metal function and reflection into `out`.
mtlb_result finish_stage(Device *device, dispatch_data_t bytecode, std::shared_ptr<IRShaderReflection> reflection,
                         IRShaderStage ir_stage, ShaderStage &out)
{
    const char *function_name = IRShaderReflectionGetEntryPointFunctionName(reflection.get());
    if (!function_name)
        return fail(MTLB_ERROR_COMPILE_FAILED, "shader reflection has no entry point");

    NSError *ns_error = nil;
    id<MTLLibrary> library = [device->device newLibraryWithData:bytecode error:&ns_error];
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
        out.reflection = std::move(reflection);
    } else if (ir_stage == IRShaderStageFragment) {
        out.reflection = std::move(reflection);  // the render target output types, at pipeline creation
    }
    return MTLB_OK;
}

dispatch_data_t bytecode_data(const std::vector<uint8_t> &bytes)
{
    return dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}

// The disk cache key of a converted shader: everything its conversion depends on.
CacheKey stage_key(RootSignature *root_signature, const void *dxil, uint64_t size, const char *entry, IRShaderStage ir_stage)
{
    Hasher h;
    h.update(std::string("stage"));
    h.update(DiskCache::converter_identity());
    h.update_value(kCacheRevision);
    h.update_value(static_cast<uint32_t>(kCompatibilityFlags));
    h.update_value(static_cast<uint32_t>(ir_stage));
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
                          const char *entry, IRShaderStage ir_stage, ShaderStage &out)
{
    DiskCache &cache = DiskCache::instance();
    const bool use_cache = cache.enabled();
    CacheKey key{};
    if (use_cache) {
        key = stage_key(root_signature, dxil, size, entry, ir_stage);
        std::vector<uint8_t> metallib;
        std::string json;
        if (cache.load(CacheKind::Stage, key, metallib, json)) {
            std::shared_ptr<IRShaderReflection> reflection(IRShaderReflectionCreateFromJSON(json.c_str()), IRShaderReflectionDestroy);
            if (reflection && finish_stage(device, bytecode_data(metallib), reflection, ir_stage, out) == MTLB_OK) {
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

    IRCompiler *compiler = thread_compiler();
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
    mtlb_result result = finish_stage(device, IRMetalLibGetBytecodeData(metallib.ptr), reflection, ir_stage, out);
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
        h.update(key);  // the layout
        disk_key = h.finish();
        std::vector<uint8_t> cached;
        std::string unused;
        if (cache.load(CacheKind::StageIn, disk_key, cached, unused))
            bytecode = bytecode_data(cached);
    }
    // A cached entry that Metal rejects (intact on disk, but not a usable library) is discarded and rebuilt.
    id<MTLFunction> function = nil;
    bool from_cache = false;
    NSError *ns_error = nil;
    if (bytecode != nil) {
        id<MTLLibrary> library = [device->device newLibraryWithData:bytecode error:&ns_error];
        function = library ? [library newFunctionWithName:library.functionNames.firstObject] : nil;
        if (function) {
            from_cache = true;
        } else {
            cache.discard(CacheKind::StageIn, disk_key);
            ns_error = nil;
        }
    }
    if (!function) {
        if (!IRMetalLibSynthesizeStageInFunction(thread_compiler(), vs.reflection.get(), &layout, metallib.ptr))
            return fail(MTLB_ERROR_COMPILE_FAILED, "stage-in function synthesis failed for the input layout");
        id<MTLLibrary> library = [device->device newLibraryWithData:IRMetalLibGetBytecodeData(metallib.ptr) error:&ns_error];
        if (!library)
            return fail(MTLB_ERROR_COMPILE_FAILED, std::string("stage-in library: ") + ns_error.localizedDescription.UTF8String);
        function = [library newFunctionWithName:library.functionNames.firstObject];
        if (!function)
            return fail(MTLB_ERROR_COMPILE_FAILED, "stage-in function missing from its library");
    }
    if (use_cache && !from_cache) {
        std::vector<uint8_t> bytes(IRMetalLibGetBytecodeSize(metallib.ptr));
        IRMetalLibGetBytecode(metallib.ptr, bytes.data());
        cache.store(CacheKind::StageIn, disk_key, bytes.data(), bytes.size(), std::string());
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

} // namespace

extern "C" mtlb_result mtlb_pipeline_create(mtlb_device handle, const mtlb_pipeline_desc *desc, mtlb_pipeline *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out || !desc->vs_dxil || !desc->vs_size || !desc->root_signature
        || desc->num_render_targets > MTLB_MAX_RENDER_TARGETS
        || desc->num_input_elements > MTLB_MAX_INPUT_ELEMENTS)
        return MTLB_ERROR_INVALID_ARGUMENT;
    stat_add(kStatPipelineAttempts);

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

    // Bit i is set when the fragment shader writes integers to render target i.
    uint32_t integer_outputs = 0;
    if (ps) {
        IRVersionedFSInfo info;
        if (IRShaderReflectionCopyFragmentInfo(ps->reflection.get(), IRReflectionVersion_1_0, &info)) {
            integer_outputs = info.info_1_0.rt_index_int;
            IRShaderReflectionReleaseFragmentInfo(&info);
        }
    }
    std::array<MTLPixelFormat, MTLB_MAX_RENDER_TARGETS> color_view_formats{};
    for (uint32_t i = 0; i < desc->num_render_targets; ++i) {
        if (desc->rtv_formats[i] == MTLB_FORMAT_UNKNOWN)
            continue;
        MTLPixelFormat format = to_pixel_format(desc->rtv_formats[i]);
        if (format == MTLPixelFormatInvalid)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported render target format " + std::to_string(desc->rtv_formats[i]));
        // D3D12 tolerates a shader output of another type than the render target (the result is undefined, and
        // games do it with the output masked); Metal refuses the pipeline. The target is written through a view of
        // the other kind instead (the bits land as they are); where no such view exists, the pipeline is refused.
        if (ps && ((integer_outputs >> i) & 1) != (is_integer_pixel_format(format) ? 1u : 0u)) {
            const MTLPixelFormat other = opposite_kind_format(format);
            if (other != MTLPixelFormatInvalid) {
                color_view_formats[i] = other;
                format = other;
            }
        }
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

    // D24_UNORM_S8_UINT is a 32-bit depth with 8-bit stencil here (docs/STATUS.md).
    MTLPixelFormat depth_format = MTLPixelFormatInvalid, stencil_format = MTLPixelFormatInvalid;
    if (desc->dsv_format != MTLB_FORMAT_UNKNOWN) {
        depth_format = to_texture_pixel_format(desc->dsv_format, true);
        if (depth_format == MTLPixelFormatInvalid)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported depth-stencil format " + std::to_string(desc->dsv_format));
        pd.depthAttachmentPixelFormat = depth_format;
        if (depth_format == MTLPixelFormatDepth32Float_Stencil8) {
            stencil_format = depth_format;
            pd.stencilAttachmentPixelFormat = stencil_format;
        }
    }

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
    pipeline->depth_format = depth_format;
    pipeline->stencil_format = stencil_format;
    pipeline->color_view_formats = color_view_formats;
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
    MTLRenderPipelineDescriptor *copy = [descriptor copy];
    copy.depthAttachmentPixelFormat = depth;
    copy.stencilAttachmentPixelFormat = stencil;
    NSError *error = nil;
    id<MTLRenderPipelineState> variant = [device->device newRenderPipelineStateWithDescriptor:copy error:&error];
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
