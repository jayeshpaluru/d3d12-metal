// SPDX-License-Identifier: LGPL-2.1-or-later
// Descriptors written by the front-end without a bridge call must equal what the backend would build:
// buffer views (compared with mtlb_buffer_view), sampler caching and sampler argument sanitising.
#include <cmath>
#include <cstring>
#include <vector>

#include "bridge/mtlb.h"
#include "test_util.h"

namespace {

mtlb_descriptor read_slot(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    mtlb_descriptor d;
    std::memcpy(&d, reinterpret_cast<const void *>(handle.ptr), sizeof(d));
    return d;
}

} // namespace

int main()
{
    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));

    constexpr UINT64 kSize = 4096;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kSize;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buffer;
    CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(buffer.GetAddressOf())));

    // The backend's own answer for an equivalent buffer.
    mtlb_device mdev = 0;
    CHECK(mtlb_device_create(0, &mdev) == MTLB_OK);
    mtlb_buffer mbuf = 0;
    mtlb_buffer_info binfo;
    CHECK(mtlb_buffer_create(mdev, kSize, MTLB_STORAGE_SHARED, &mbuf, &binfo) == MTLB_OK);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> dh;
    CHECK_HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dh.GetAddressOf())));
    const D3D12_CPU_DESCRIPTOR_HANDLE slot = dh->GetCPUDescriptorHandleForHeapStart();

    struct Case {
        UINT64 first, count;
        UINT stride;
        bool raw;
    };
    const Case cases[] = {{0, 16, 16, false}, {4, 8, 16, false}, {0, 1024, 4, false}, {10, 4000, 4, false},
                          {0, 1024, 0, true}, {16, 100, 0, true},  {0, 1u << 20, 4, false}};
    for (const Case &c : cases) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = c.raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.FirstElement = c.first;
        sv.Buffer.NumElements = static_cast<UINT>(c.count);
        sv.Buffer.StructureByteStride = c.stride;
        sv.Buffer.Flags = c.raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(buffer.Get(), &sv, slot);
        const mtlb_descriptor got = read_slot(slot);

        const UINT bytes = c.raw ? 4 : c.stride;
        mtlb_buffer_view_desc view = {};
        view.buffer = mbuf;
        view.offset = c.first * bytes;
        view.size = c.count * bytes;
        mtlb_descriptor want;
        CHECK(mtlb_buffer_view(&view, &want) == MTLB_OK);
        CHECK(got.metadata == want.metadata);
        CHECK(got.texture_id == want.texture_id);
        CHECK(got.gpu_address == buffer->GetGPUVirtualAddress() + view.offset);
        CHECK(want.gpu_address == binfo.gpu_address + view.offset);
    }

    // A view that starts past the end is refused (the descriptor becomes a null buffer, never a bad address).
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_UNKNOWN;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.FirstElement = 1u << 20;
        sv.Buffer.NumElements = 4;
        sv.Buffer.StructureByteStride = 16;
        device->CreateShaderResourceView(buffer.Get(), &sv, slot);
        const mtlb_descriptor got = read_slot(slot);
        CHECK(got.gpu_address < buffer->GetGPUVirtualAddress() || got.gpu_address >= buffer->GetGPUVirtualAddress() + kSize);
        CHECK((got.metadata & 0xffffffffu) == 0);
    }

    // Typed views, twice (the second answer comes from the cache), equal to the backend's.
    for (int round = 0; round < 2; ++round) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.FirstElement = 0;
        sv.Buffer.NumElements = 64;
        device->CreateShaderResourceView(buffer.Get(), &sv, slot);
        const mtlb_descriptor got = read_slot(slot);
        mtlb_buffer_view_desc view = {};
        view.buffer = mbuf;
        view.size = 64 * 16;
        view.format = 2;  // DXGI_FORMAT_R32G32B32A32_FLOAT
        view.num_elements = 64;
        mtlb_descriptor want;
        CHECK(mtlb_buffer_view(&view, &want) == MTLB_OK);
        CHECK(got.metadata == want.metadata);
        CHECK(got.texture_id != 0);
    }

    // Samplers: equal descriptions give equal entries, a different bias or filter does not.
    D3D12_DESCRIPTOR_HEAP_DESC sd = {};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sd.NumDescriptors = 8;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> sh;
    CHECK_HR(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(sh.GetAddressOf())));
    const UINT sinc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    auto sampler_slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = sh->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * sinc;
        return h;
    };
    D3D12_SAMPLER_DESC s = {};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MaxAnisotropy = 1;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&s, sampler_slot(0));
    device->CreateSampler(&s, sampler_slot(1));
    s.MipLODBias = 0.5f;
    device->CreateSampler(&s, sampler_slot(2));
    s.MipLODBias = 0;
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    device->CreateSampler(&s, sampler_slot(3));
    const mtlb_descriptor s0 = read_slot(sampler_slot(0)), s1 = read_slot(sampler_slot(1)), s2 = read_slot(sampler_slot(2)),
                          s3 = read_slot(sampler_slot(3));
    CHECK(std::memcmp(&s0, &s1, sizeof(s0)) == 0);
    CHECK(std::memcmp(&s0, &s2, sizeof(s0)) != 0);
    CHECK(std::memcmp(&s0, &s3, sizeof(s0)) != 0);
    CHECK(s0.gpu_address != 0 || s0.texture_id != 0 || s0.metadata != 0);

    // Hostile arguments are clamped, not passed to Metal: they must give a usable sampler, equal to the clamped one.
    D3D12_SAMPLER_DESC bad = {};
    bad.Filter = D3D12_FILTER_ANISOTROPIC;
    bad.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(99);
    bad.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(0);
    bad.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(-1);
    bad.MaxAnisotropy = 100000;
    bad.MipLODBias = NAN;
    bad.MinLOD = -INFINITY;
    bad.MaxLOD = NAN;
    bad.BorderColor[0] = NAN;
    device->CreateSampler(&bad, sampler_slot(4));
    D3D12_SAMPLER_DESC clamped = {};
    clamped.Filter = D3D12_FILTER_ANISOTROPIC;
    clamped.AddressU = clamped.AddressV = clamped.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    clamped.MaxAnisotropy = 16;
    clamped.MaxLOD = 1000.0f;
    device->CreateSampler(&clamped, sampler_slot(5));
    const mtlb_descriptor b4 = read_slot(sampler_slot(4)), b5 = read_slot(sampler_slot(5));
    CHECK(b4.gpu_address != 0 || b4.texture_id != 0 || b4.metadata != 0);
    CHECK(std::memcmp(&b4, &b5, sizeof(b4)) == 0);

    mtlb_buffer_destroy(mbuf);
    mtlb_device_destroy(mdev);
    std::printf("test_descriptors: OK\n");
    return 0;
}
