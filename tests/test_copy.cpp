// Buffer copies: upload -> default -> readback.
#include <cstring>
#include <vector>

#include "test_context.h"

int main()
{
    TestContext ctx;
    constexpr UINT64 kSize = 4096;

    Com<ID3D12Resource> upload = ctx.create_buffer(D3D12_HEAP_TYPE_UPLOAD, kSize);
    Com<ID3D12Resource> gpu = ctx.create_buffer(D3D12_HEAP_TYPE_DEFAULT, kSize);
    Com<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, kSize);
    CHECK(upload->GetGPUVirtualAddress() != 0);
    CHECK(upload->GetDesc().Width == kSize);

    std::vector<uint8_t> pattern(kSize);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<uint8_t>(i * 7 + 3);
    void *mapped = nullptr;
    CHECK_HR(upload->Map(0, nullptr, &mapped));
    std::memcpy(mapped, pattern.data(), kSize);
    upload->Unmap(0, nullptr);

    Com<ID3D12CommandAllocator> allocator;
    CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
    Com<ID3D12GraphicsCommandList> list;
    CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                           IID_PPV_ARGS(list.put())));
    list->CopyBufferRegion(gpu.get(), 0, upload.get(), 0, kSize);
    list->CopyResource(readback.get(), gpu.get());
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.get());

    CHECK_HR(readback->Map(0, nullptr, &mapped));
    CHECK(std::memcmp(mapped, pattern.data(), kSize) == 0);
    readback->Unmap(0, nullptr);

    // The list can be recorded again after Reset.
    CHECK_HR(allocator->Reset());
    CHECK_HR(list->Reset(allocator.get(), nullptr));
    list->CopyBufferRegion(readback.get(), 16, upload.get(), 0, 64);
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.get());
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    CHECK(std::memcmp(static_cast<uint8_t *>(mapped) + 16, pattern.data(), 64) == 0);
    readback->Unmap(0, nullptr);

    return 0;
}
