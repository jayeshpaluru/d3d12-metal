// SPDX-License-Identifier: LGPL-2.1-or-later
// Misuse the layer must survive without crashing or writing where it should not: CPU descriptor handles that
// are not inside a heap of the right type, render target views of subresources that do not exist, views whose
// resources are destroyed before use, and objects released while a command list still names them.
#include <cstring>
#include <vector>

#include "test_context.h"

namespace {

constexpr D3D12_CPU_DESCRIPTOR_HANDLE kBogus[] = {{0}, {16}, {0xdeadbeef}, {~size_t(0) - 7}};

bool all_zero(const void *data, size_t size)
{
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        if (p[i])
            return false;
    return true;
}

} // namespace

int main()
{
    TestContext ctx;
    ID3D12Device *device = ctx.device.Get();

    auto make_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible = false) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {type, count, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ComPtr<ID3D12DescriptorHeap> heap;
        CHECK_HR(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())));
        return heap;
    };
    ComPtr<ID3D12DescriptorHeap> view_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, true);
    ComPtr<ID3D12DescriptorHeap> sampler_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, true);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4);
    ComPtr<ID3D12DescriptorHeap> dsv_heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4);
    const UINT view_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto at = [](ID3D12DescriptorHeap *heap, UINT index, UINT increment) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(index) * increment;
        return h;
    };

    ComPtr<ID3D12Resource> buffer = ctx.create_buffer(D3D12_HEAP_TYPE_DEFAULT, 4096);
    const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC rt_desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 2, 3, 1, 0,
                                                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ComPtr<ID3D12Resource> target;
    CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             nullptr, IID_PPV_ARGS(target.GetAddressOf())));

    // ---- handles that are not descriptors: nothing may be written or crash ----------------------------------------
    // A buffer on the stack stands in for "memory that is not a heap": it must stay untouched.
    alignas(16) uint8_t guard[256] = {};
    const D3D12_CPU_DESCRIPTOR_HANDLE on_stack = {reinterpret_cast<size_t>(guard)};
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> bad(std::begin(kBogus), std::end(kBogus));
    bad.push_back(on_stack);
    bad.push_back({view_heap->GetCPUDescriptorHandleForHeapStart().ptr + 4 * view_inc});     // one past the end
    bad.push_back({view_heap->GetCPUDescriptorHandleForHeapStart().ptr + 3 * view_inc + 5});  // not on a boundary
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 16;
    srv.Buffer.StructureByteStride = 16;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 16;
    uav.Buffer.StructureByteStride = 16;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {buffer->GetGPUVirtualAddress(), 256};
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    for (const D3D12_CPU_DESCRIPTOR_HANDLE &handle : bad) {
        device->CreateShaderResourceView(buffer.Get(), &srv, handle);
        device->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav, handle);
        device->CreateConstantBufferView(&cbv, handle);
        device->CreateSampler(&sampler, handle);
        device->CreateRenderTargetView(target.Get(), nullptr, handle);
        device->CreateDepthStencilView(target.Get(), nullptr, handle);
        device->CopyDescriptorsSimple(1, handle, at(view_heap.Get(), 0, view_inc), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CopyDescriptorsSimple(1, at(view_heap.Get(), 0, view_inc), handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT one = 1;
        device->CopyDescriptors(1, &handle, &one, 1, &handle, &one, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    CHECK(all_zero(guard, sizeof(guard)));

    // A handle of the wrong heap type, and a range that runs past the end of its heap, are refused too.
    uint8_t sampler_before[24 * 4];
    std::memcpy(sampler_before, reinterpret_cast<void *>(sampler_heap->GetCPUDescriptorHandleForHeapStart().ptr), sizeof(sampler_before));
    device->CreateConstantBufferView(&cbv, sampler_heap->GetCPUDescriptorHandleForHeapStart());   // a sampler slot
    device->CreateSampler(&sampler, view_heap->GetCPUDescriptorHandleForHeapStart());              // a view slot
    CHECK(std::memcmp(sampler_before, reinterpret_cast<void *>(sampler_heap->GetCPUDescriptorHandleForHeapStart().ptr),
                      sizeof(sampler_before)) == 0);
    CHECK(all_zero(reinterpret_cast<void *>(view_heap->GetCPUDescriptorHandleForHeapStart().ptr), 24));
    device->CopyDescriptorsSimple(8, at(view_heap.Get(), 0, view_inc), at(view_heap.Get(), 0, view_inc),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);  // 8 > the 4 the heap holds

    // Legitimate writes in the same heaps still work.
    device->CreateConstantBufferView(&cbv, at(view_heap.Get(), 3, view_inc));
    CHECK(!all_zero(reinterpret_cast<void *>(at(view_heap.Get(), 3, view_inc).ptr), 24));
    device->CopyDescriptorsSimple(1, at(view_heap.Get(), 0, view_inc), at(view_heap.Get(), 3, view_inc),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(std::memcmp(reinterpret_cast<void *>(at(view_heap.Get(), 0, view_inc).ptr),
                      reinterpret_cast<void *>(at(view_heap.Get(), 3, view_inc).ptr), 24) == 0);

    // A heap that is gone: its old handles are refused (the address may even be reused by a new heap).
    D3D12_CPU_DESCRIPTOR_HANDLE stale;
    {
        ComPtr<ID3D12DescriptorHeap> temporary = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
        stale = temporary->GetCPUDescriptorHandleForHeapStart();
        device->CreateConstantBufferView(&cbv, stale);  // fine while the heap lives
    }
    // (no write: the memory is freed; this only checks that the lookup refuses it instead of dereferencing)
    device->CopyDescriptorsSimple(1, at(view_heap.Get(), 1, view_inc), stale, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ---- render target and depth-stencil views of subresources that do not exist -----------------------------------
    // The texture has 2 slices and 3 mips.
    auto rtv_slot_empty = [&](UINT index) {
        return all_zero(reinterpret_cast<void *>(at(rtv_heap.Get(), index, rtv_inc).ptr), 24);
    };
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv.Texture2DArray = {2, 1, 1, 0};  // mip 2, slice 1: the last subresource
    device->CreateRenderTargetView(target.Get(), &rtv, at(rtv_heap.Get(), 0, rtv_inc));
    CHECK(!rtv_slot_empty(0));
    rtv.Texture2DArray = {3, 0, 1, 0};  // mip 3 does not exist
    device->CreateRenderTargetView(target.Get(), &rtv, at(rtv_heap.Get(), 1, rtv_inc));
    CHECK(rtv_slot_empty(1));
    rtv.Texture2DArray = {0, 2, 1, 0};  // slice 2 does not exist
    device->CreateRenderTargetView(target.Get(), &rtv, at(rtv_heap.Get(), 2, rtv_inc));
    CHECK(rtv_slot_empty(2));
    rtv.Texture2DArray = {0, 0, 1, 0};
    // A depth-stencil view of a render target texture, and a render target view of a buffer, are refused as well.
    device->CreateDepthStencilView(target.Get(), nullptr, at(dsv_heap.Get(), 0, rtv_inc));
    CHECK(all_zero(reinterpret_cast<void *>(at(dsv_heap.Get(), 0, rtv_inc).ptr), 24));
    device->CreateRenderTargetView(buffer.Get(), nullptr, at(rtv_heap.Get(), 3, rtv_inc));
    CHECK(rtv_slot_empty(3));

    // Using the refused views draws nothing and crashes nothing.
    {
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        const float red[4] = {1, 0, 0, 1};
        for (UINT i = 1; i < 4; ++i) {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = at(rtv_heap.Get(), i, rtv_inc);
            list->ClearRenderTargetView(h, red, 0, nullptr);
            list->OMSetRenderTargets(1, &h, FALSE, nullptr);
        }
        for (const D3D12_CPU_DESCRIPTOR_HANDLE &handle : bad) {
            list->ClearRenderTargetView(handle, red, 0, nullptr);
            list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            list->OMSetRenderTargets(1, &handle, TRUE, &handle);
        }
        CHECK_HR(list->Close());
        ctx.execute_and_wait(list.Get());
    }

    // ---- views and objects that go away before the list runs -------------------------------------------------------
    {
        ComPtr<ID3D12Resource> doomed;
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1, 1, 0,
                                                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(doomed.GetAddressOf())));
        device->CreateRenderTargetView(doomed.Get(), nullptr, at(rtv_heap.Get(), 0, rtv_inc));
        doomed.Reset();  // the view is stale now
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        const float red[4] = {1, 0, 0, 1};
        const D3D12_CPU_DESCRIPTOR_HANDLE h = at(rtv_heap.Get(), 0, rtv_inc);
        list->ClearRenderTargetView(h, red, 0, nullptr);
        list->OMSetRenderTargets(1, &h, FALSE, nullptr);
        CHECK_HR(list->Close());
        ctx.execute_and_wait(list.Get());
    }

    {
        // Recorded, then released by the application, then executed: the list keeps the memory alive without
        // changing the reference counts the application sees (Release() reports 0, as on any driver).
        ComPtr<ID3D12Resource> src = ctx.create_upload_buffer("source data 0123", 16);
        ComPtr<ID3D12Resource> dst = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, 16);
        ID3D12Resource *raw_src = src.Get();
        raw_src->AddRef();
        const ULONG before_src = raw_src->Release();

        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        list->CopyBufferRegion(dst.Get(), 0, src.Get(), 0, 16);
        raw_src->AddRef();
        CHECK(raw_src->Release() == before_src);  // recording takes no COM reference
        CHECK_HR(list->Close());

        CHECK(src.Detach()->Release() == 0);  // the application lets go of the source
        ctx.execute_and_wait(list.Get());     // the list still copies from it
        void *mapped = nullptr;
        CHECK_HR(dst->Map(0, nullptr, &mapped));
        CHECK(std::memcmp(mapped, "source data 0123", 16) == 0);
        dst->Unmap(0, nullptr);

        // Reset gives the internal references back (the source is freed at last); the list is usable again.
        CHECK_HR(ctx.allocators.back()->Reset());
        CHECK_HR(list->Reset(ctx.allocators.back().Get(), nullptr));
        CHECK_HR(list->Close());
        dst->AddRef();
        CHECK(dst->Release() == 1);
    }

    {
        // A pipeline state released after SetPipelineState and before execution.
        ComPtr<ID3D12RootSignature> signature;
        {
            D3D12_ROOT_SIGNATURE_DESC desc = {};
            desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> blob, error;
            CHECK_HR(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf()));
            CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature.GetAddressOf())));
        }
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        list->SetGraphicsRootSignature(signature.Get());
        CHECK(signature.Detach()->Release() == 0);  // the list's hold on it is not a COM reference
        CHECK_HR(list->Close());
        ctx.execute_and_wait(list.Get());
    }

    // ---- overlapping descriptor copies behave like memmove, shadow view info included ---------------------------------
    {
        const CD3DX12_HEAP_PROPERTIES default_props(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC uav_desc = CD3DX12_RESOURCE_DESC::Buffer(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> uav_buffer;
        CHECK_HR(device->CreateCommittedResource(&default_props, D3D12_HEAP_FLAG_NONE, &uav_desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(uav_buffer.GetAddressOf())));
        ComPtr<ID3D12DescriptorHeap> heap = make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, true);
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC all = {};  // slot 4 views the whole buffer: it zeroes it between checks
            all.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            all.Buffer.NumElements = 16;
            all.Buffer.StructureByteStride = 4;
            device->CreateUnorderedAccessView(uav_buffer.Get(), nullptr, &all, at(heap.Get(), 4, view_inc));
        }
        // Slot i views i + 1 dwords, so a clear through a slot reveals which view the slot holds.
        for (UINT i = 0; i < 3; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements = i + 1;
            d.Buffer.StructureByteStride = 4;
            device->CreateUnorderedAccessView(uav_buffer.Get(), nullptr, &d, at(heap.Get(), i, view_inc));
        }
        // Shift up by one inside the heap: 0,1,2 -> 1,2,3 (the destination overlaps the source).
        device->CopyDescriptorsSimple(3, at(heap.Get(), 1, view_inc), at(heap.Get(), 0, view_inc), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cleared_dwords = [&](UINT slot) {
            ComPtr<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, 64);
            ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
            const CD3DX12_RESOURCE_BARRIER uav_barrier = CD3DX12_RESOURCE_BARRIER::UAV(uav_buffer.Get());
            const UINT zeros[4] = {0, 0, 0, 0}, ones[4] = {0xAB, 0xAB, 0xAB, 0xAB};
            D3D12_GPU_DESCRIPTOR_HANDLE all_gpu = heap->GetGPUDescriptorHandleForHeapStart();
            all_gpu.ptr += size_t(4) * view_inc;
            list->ClearUnorderedAccessViewUint(all_gpu, at(heap.Get(), 4, view_inc), uav_buffer.Get(), zeros, 0, nullptr);
            list->ResourceBarrier(1, &uav_barrier);
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += size_t(slot) * view_inc;
            list->ClearUnorderedAccessViewUint(gpu, at(heap.Get(), slot, view_inc), uav_buffer.Get(), ones, 0, nullptr);
            list->ResourceBarrier(1, &uav_barrier);
            list->CopyResource(readback.Get(), uav_buffer.Get());
            CHECK_HR(list->Close());
            ctx.execute_and_wait(list.Get());
            void *mapped = nullptr;
            CHECK_HR(readback->Map(0, nullptr, &mapped));
            UINT count = 0;
            for (UINT i = 0; i < 16; ++i)
                count += static_cast<const uint32_t *>(mapped)[i] == 0xAB ? 1 : 0;
            readback->Unmap(0, nullptr);
            return count;
        };
        CHECK(cleared_dwords(1) == 1u);  // old slot 0
        CHECK(cleared_dwords(2) == 2u);  // old slot 1
        CHECK(cleared_dwords(3) == 3u);  // old slot 2 (a forward copy would have given slot 3 the first view)
        // And back down: 1,2,3 -> 0,1,2.
        device->CopyDescriptorsSimple(3, at(heap.Get(), 0, view_inc), at(heap.Get(), 1, view_inc), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CHECK(cleared_dwords(0) == 1u);
        CHECK(cleared_dwords(2) == 3u);
    }

    std::printf("test_hardening: OK\n");
    return 0;
}
