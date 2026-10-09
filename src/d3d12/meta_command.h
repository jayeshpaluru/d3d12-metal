// SPDX-License-Identifier: LGPL-2.1-or-later
// ID3D12MetaCommand and the one meta command the layer offers: the DirectStorage GDeflate decompressor.
//
// DirectStorage 1.2 looks for a meta command with a fixed id (the one Nvidia, AMD and Intel drivers expose) and,
// when the driver has it, decompresses GDeflate requests on the GPU with it; else it falls back to a compute shader
// of its own, or the CPU. The id, the parameter lists and the structures are what DirectStorage and the drivers
// agree on (observed with EnumerateMetaCommandParameters on Windows drivers, and documented by vkd3d-proton).
// The layer runs it as two kernels of its own in the backend (src/bridge/metal/gdeflate.mm).
#pragma once

#include "bridge/mtlb_cmd.h"
#include "d3d12/object.h"

namespace d3d12m {

// {1bddd090-c47e-459c-8f81-42c9f97a5308}, "DirectStorage".
extern const GUID kDirectStorageMetaCommandId;

// The creation parameters (CREATION stage): version and format are 1, `max_streams` the streams per call.
struct DirectStorageCreateArgs {
    UINT64 version;
    UINT64 format;
    UINT64 max_streams;
    UINT64 flags;
};

// The execution parameters (EXECUTION stage), in the order of the structure DirectStorage fills.
struct DirectStorageExecArgs {
    D3D12_GPU_VIRTUAL_ADDRESS input_buffer;   // the compressed streams
    UINT64 input_size;
    D3D12_GPU_VIRTUAL_ADDRESS output_buffer;  // where they are decompressed to
    UINT64 output_size;
    D3D12_GPU_VIRTUAL_ADDRESS control_buffer; // dword stream count, then {input offset, output offset} per stream
    UINT64 control_size;
    D3D12_GPU_VIRTUAL_ADDRESS scratch_buffer;
    UINT64 scratch_size;
    UINT64 stream_count;
    D3D12_GPU_VIRTUAL_ADDRESS status_buffer;  // not written
    UINT64 status_size;
};

// What the query feature D3D12_FEATURE_QUERY_META_COMMAND takes and gives for this command. Their field names
// are guesses from the behaviour of DirectStorage; `format` is 1 (GDeflate).
struct DirectStorageQueryIn {
    UINT16 version;
    UINT16 stream_count;   // streams per call the application would like
    UINT32 reserved;
    UINT64 format;
};
struct DirectStorageQueryOut {
    UINT16 version;
    UINT16 max_stream_count;
    UINT32 reserved;
    UINT64 scratch_size;
    UINT64 reserved2;
};

// Most streams one call decompresses.
constexpr UINT kMaxGDeflateStreams = 1024;

// False when the meta command is hidden (D3D12METAL_GDEFLATE=0 or `gdeflate=0` in d3d12metal.conf): DirectStorage
// then uses its own decompression.
bool meta_commands_enabled();

HRESULT enumerate_meta_commands(UINT *count, D3D12_META_COMMAND_DESC *descs);
HRESULT enumerate_meta_command_parameters(REFGUID id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total_size,
                                          UINT *count, D3D12_META_COMMAND_PARAMETER_DESC *descs);
// D3D12_FEATURE_QUERY_META_COMMAND.
HRESULT query_meta_command(const D3D12_FEATURE_DATA_QUERY_META_COMMAND &query);

class MetaCommand final : public ChildImpl<ID3D12MetaCommand> {
public:
    static HRESULT create(Device *device, REFGUID id, UINT node_mask, const void *parameters, SIZE_T size, REFIID riid,
                          void **out);

    UINT max_streams() const { return max_streams_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12MetaCommand>(this, riid, out);
    }
    UINT64 STDMETHODCALLTYPE GetRequiredParameterResourceSize(D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT index) override;

private:
    explicit MetaCommand(Device *device) : ChildImpl(device) {}

    UINT max_streams_ = 0;
};

} // namespace d3d12m
