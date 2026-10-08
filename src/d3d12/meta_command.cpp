#include "d3d12/meta_command.h"

#include <algorithm>
#include <cstddef>

#include "common/config.h"
#include "d3d12/device.h"

namespace d3d12m {

const GUID kDirectStorageMetaCommandId = {0x1bddd090, 0xc47e, 0x459c, {0x8f, 0x81, 0x42, 0xc9, 0xf9, 0x7a, 0x53, 0x08}};

namespace {

constexpr WCHAR kCommandName[] = L"DirectStorage";

struct ParameterInfo {
    D3D12_META_COMMAND_PARAMETER_STAGE stage;
    D3D12_META_COMMAND_PARAMETER_DESC desc;
};

#define CREATION_PARAMETER(name, field)                                                                  \
    {D3D12_META_COMMAND_PARAMETER_STAGE_CREATION,                                                        \
     {L##name, D3D12_META_COMMAND_PARAMETER_TYPE_UINT64, D3D12_META_COMMAND_PARAMETER_FLAG_INPUT,        \
      D3D12_RESOURCE_STATE_COMMON, static_cast<UINT>(offsetof(DirectStorageCreateArgs, field))}}
#define EXEC_VALUE(name, field)                                                                          \
    {D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,                                                       \
     {L##name, D3D12_META_COMMAND_PARAMETER_TYPE_UINT64, D3D12_META_COMMAND_PARAMETER_FLAG_INPUT,        \
      D3D12_RESOURCE_STATE_COMMON, static_cast<UINT>(offsetof(DirectStorageExecArgs, field))}}
#define EXEC_ADDRESS(name, field, flag)                                                                  \
    {D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,                                                       \
     {L##name, D3D12_META_COMMAND_PARAMETER_TYPE_GPU_VIRTUAL_ADDRESS, flag,                              \
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, static_cast<UINT>(offsetof(DirectStorageExecArgs, field))}}

// What EnumerateMetaCommandParameters returns on Windows drivers, in their order.
const ParameterInfo kParameters[] = {
    CREATION_PARAMETER("Version", version),
    CREATION_PARAMETER("Format", format),
    CREATION_PARAMETER("MaxStreams", max_streams),
    CREATION_PARAMETER("Flags", flags),
    EXEC_ADDRESS("InputBuffer", input_buffer, D3D12_META_COMMAND_PARAMETER_FLAG_INPUT),
    EXEC_VALUE("InputBufferSize", input_size),
    EXEC_ADDRESS("OutputBuffer", output_buffer, D3D12_META_COMMAND_PARAMETER_FLAG_OUTPUT),
    EXEC_VALUE("OutputBufferSize", output_size),
    EXEC_ADDRESS("ControlBuffer", control_buffer, D3D12_META_COMMAND_PARAMETER_FLAG_INPUT),
    EXEC_VALUE("ControlBufferSize", control_size),
    EXEC_ADDRESS("ScratchBuffer", scratch_buffer, D3D12_META_COMMAND_PARAMETER_FLAG_OUTPUT),
    EXEC_VALUE("ScratchBufferSize", scratch_size),
    EXEC_VALUE("StreamCount", stream_count),
    EXEC_ADDRESS("StatusBuffer", status_buffer, D3D12_META_COMMAND_PARAMETER_FLAG_OUTPUT),
    EXEC_VALUE("StatusBufferSize", status_size),
};

// Index of the scratch buffer among the execution parameters.
constexpr UINT kScratchParameter = 6;

UINT scratch_streams(UINT64 requested)
{
    return static_cast<UINT>(std::min<UINT64>(requested, kMaxGDeflateStreams));
}

} // namespace

bool meta_commands_enabled()
{
    static const bool enabled = [] {
        const char *value = config_get("GDEFLATE");
        return !value || !*value || *value != '0';
    }();
    return enabled;
}

HRESULT enumerate_meta_commands(UINT *count, D3D12_META_COMMAND_DESC *descs)
{
    if (!count)
        return E_INVALIDARG;
    const UINT available = meta_commands_enabled() ? 1 : 0;
    if (descs && available && *count >= 1) {
        D3D12_META_COMMAND_DESC desc = {};
        desc.Id = kDirectStorageMetaCommandId;
        desc.Name = kCommandName;
        desc.InitializationDirtyState = static_cast<D3D12_GRAPHICS_STATES>(0);
        desc.ExecutionDirtyState = D3D12_GRAPHICS_STATE_COMPUTE_ROOT_SIGNATURE | D3D12_GRAPHICS_STATE_PIPELINE_STATE;
        descs[0] = desc;
    }
    *count = available;
    return S_OK;
}

HRESULT enumerate_meta_command_parameters(REFGUID id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_size,
                                          UINT *count, D3D12_META_COMMAND_PARAMETER_DESC *descs)
{
    if (!meta_commands_enabled() || !(id == kDirectStorageMetaCommandId))
        return E_INVALIDARG;
    const UINT capacity = count ? *count : 0;
    UINT found = 0, size = 0;
    for (const ParameterInfo &parameter : kParameters) {
        if (parameter.stage != stage)
            continue;
        if (descs && found < capacity)
            descs[found] = parameter.desc;
        ++found;
        size = std::max<UINT>(size, parameter.desc.StructureOffset + sizeof(UINT64));
    }
    if (count)
        *count = found;
    if (total_size)
        *total_size = size;
    return S_OK;
}

HRESULT query_meta_command(const D3D12_FEATURE_DATA_QUERY_META_COMMAND &query)
{
    if ((query.QueryInputDataSizeInBytes && !query.pQueryInputData)
        || (query.QueryOutputDataSizeInBytes && !query.pQueryOutputData))
        return E_INVALIDARG;
    if (!meta_commands_enabled() || !(query.CommandId == kDirectStorageMetaCommandId)) {
        D3D12M_LOG("QUERY_META_COMMAND for an unsupported meta command");
        return E_INVALIDARG;
    }
    // Excess output bytes are not written.
    if (query.QueryInputDataSizeInBytes < sizeof(DirectStorageQueryIn)
        || query.QueryOutputDataSizeInBytes < sizeof(DirectStorageQueryOut))
        return E_INVALIDARG;
    DirectStorageQueryIn in;
    std::memcpy(&in, query.pQueryInputData, sizeof(in));
    DirectStorageQueryOut out = {};
    if (in.format == 1) {
        const UINT streams = std::min<UINT>(kMaxGDeflateStreams, in.stream_count);
        out.version = 1;
        out.max_stream_count = static_cast<UINT16>(streams);
        out.scratch_size = mtlb_gdeflate_scratch_size(streams);
    }
    std::memcpy(query.pQueryOutputData, &out, sizeof(out));
    return S_OK;
}

HRESULT MetaCommand::create(Device *device, REFGUID id, UINT, const void *parameters, SIZE_T size, REFIID riid,
                            void **out)
{
    if (!out)
        return E_POINTER;
    if (!meta_commands_enabled() || !(id == kDirectStorageMetaCommandId)) {
        D3D12M_LOG("CreateMetaCommand: unsupported meta command");
        return E_INVALIDARG;
    }
    if (!parameters || size < sizeof(DirectStorageCreateArgs))
        return E_INVALIDARG;
    DirectStorageCreateArgs args;
    std::memcpy(&args, parameters, sizeof(args));
    if (args.version != 1 || args.format != 1) {
        D3D12M_LOG("CreateMetaCommand: DirectStorage version %llu format %llu is not supported",
                   static_cast<unsigned long long>(args.version), static_cast<unsigned long long>(args.format));
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (args.flags)
        D3D12M_LOG("CreateMetaCommand: unknown flags 0x%llx ignored", static_cast<unsigned long long>(args.flags));
    auto *command = new MetaCommand(device);
    command->max_streams_ = scratch_streams(args.max_streams);
    return hand_out(command, riid, out);
}

HRESULT Device::EnumerateMetaCommands(UINT *count, D3D12_META_COMMAND_DESC *descs)
{
    D3D12M_TRACED_BEGIN
    return enumerate_meta_commands(count, descs);
    D3D12M_TRACED_END(count, descs)
}

HRESULT Device::EnumerateMetaCommandParameters(REFGUID id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_size,
                                               UINT *count, D3D12_META_COMMAND_PARAMETER_DESC *descs)
{
    D3D12M_TRACED_BEGIN
    return enumerate_meta_command_parameters(id, stage, total_size, count, descs);
    D3D12M_TRACED_END(id, stage, total_size, count, descs)
}

HRESULT Device::CreateMetaCommand(REFGUID id, UINT node_mask, const void *parameters, SIZE_T size, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return MetaCommand::create(this, id, node_mask, parameters, size, riid, out);
    D3D12M_TRACED_END(id, node_mask, parameters, size, riid, out)
}

UINT64 MetaCommand::GetRequiredParameterResourceSize(D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT index)
{
    D3D12M_TRACE(stage, index);
    if (stage == D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION && index == kScratchParameter)
        return mtlb_gdeflate_scratch_size(max_streams_);
    return 0;
}

} // namespace d3d12m
