// Texture copies with sub-rectangles in both directions, and the footprints
// that describe the buffer side of them.
#include <cstring>
#include <vector>

#include "test_context.h"

namespace {

constexpr UINT kSize = 16;

struct Rgba {
    uint8_t r, g, b, a;
};

D3D12_RESOURCE_DESC texture_desc(UINT size, DXGI_FORMAT format, UINT mips)
{
    return CD3DX12_RESOURCE_DESC::Tex2D(format, size, size, 1, static_cast<UINT16>(mips));
}

void check_footprints(TestContext &ctx)
{
    // Block-compressed mips round up to whole blocks: BC1 2x2 is stored as 4x4.
    D3D12_RESOURCE_DESC bc1 = texture_desc(8, DXGI_FORMAT_BC1_UNORM, 4);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[4] = {};
    UINT rows[4] = {};
    UINT64 row_sizes[4] = {}, total = 0;
    ctx.device->GetCopyableFootprints(&bc1, 0, 4, 0, layouts, rows, row_sizes, &total);
    CHECK(layouts[0].Footprint.Width == 8 && layouts[0].Footprint.Height == 8);
    CHECK(layouts[2].Footprint.Width == 4 && layouts[2].Footprint.Height == 4);  // 2x2 mip
    CHECK(layouts[3].Footprint.Width == 4 && layouts[3].Footprint.Height == 4);  // 1x1 mip
    CHECK(rows[0] == 2 && row_sizes[0] == 16);
    CHECK(rows[3] == 1 && row_sizes[3] == 8);
    CHECK(layouts[0].Footprint.RowPitch == 256);
    for (const auto &layout : layouts)
        CHECK(layout.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);

    // Buffers: one subresource, row pitch aligned to 256, offsets relative to the base.
    const CD3DX12_RESOURCE_DESC buffer = CD3DX12_RESOURCE_DESC::Buffer(1000);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT row_count = 0;
    UINT64 row_size = 0;
    ctx.device->GetCopyableFootprints(&buffer, 0, 1, 24, &layout, &row_count, &row_size, &total);
    CHECK(layout.Offset == 24);
    CHECK(layout.Footprint.Format == DXGI_FORMAT_UNKNOWN);
    CHECK(layout.Footprint.Width == 1000 && layout.Footprint.Height == 1 && layout.Footprint.Depth == 1);
    CHECK(layout.Footprint.RowPitch == 1024);
    CHECK(row_count == 1 && row_size == 1000 && total == 1000);
}

// Uploads with block-rounded footprints: a 2x2 mip is reported as 4x4 and a
// destination origin leaves less room than the footprint covers. Neither may
// copy past the texture.
void check_bc1_small_mips(TestContext &ctx)
{
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = texture_desc(8, DXGI_FORMAT_BC1_UNORM, 4);
    ComPtr<ID3D12Resource> texture;
    CHECK_HR(ctx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[4] = {};
    UINT64 total = 0;
    ctx.device->GetCopyableFootprints(&desc, 0, 4, 0, layouts, nullptr, nullptr, &total);

    std::vector<uint8_t> data(total);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i * 13 + 1);
    ComPtr<ID3D12Resource> upload = ctx.create_upload_buffer(data.data(), total);
    ComPtr<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, total);

    ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
    for (UINT mip = 0; mip < 4; ++mip) {
        const CD3DX12_TEXTURE_COPY_LOCATION dst(texture.Get(), mip), src(upload.Get(), layouts[mip]);
        // Mip 0 goes to the lower right quadrant, so the 8x8 footprint overhangs.
        const UINT origin = mip == 0 ? 4 : 0;
        list->CopyTextureRegion(&dst, origin, origin, 0, &src, nullptr);
    }
    // Read mips 1 to 3 back (mip 0 only holds one quadrant).
    for (UINT mip = 1; mip < 4; ++mip) {
        const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), layouts[mip]), src(texture.Get(), mip);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.Get());

    void *mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    for (UINT mip = 1; mip < 4; ++mip) {
        // Each of these mips is a single block of 8 bytes at the start of its footprint.
        const uint8_t *got = static_cast<uint8_t *>(mapped) + layouts[mip].Offset;
        CHECK(std::memcmp(got, data.data() + layouts[mip].Offset, 8) == 0);
    }
    readback->Unmap(0, nullptr);
}

} // namespace

int main()
{
    TestContext ctx;
    check_footprints(ctx);
    check_bc1_small_mips(ctx);

    const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = texture_desc(kSize, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    ComPtr<ID3D12Resource> texture;
    CHECK_HR(ctx.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 total = 0;
    ctx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    const UINT pitch = footprint.Footprint.RowPitch;

    // Upload 1: every texel is (x, y, 0, 255).
    std::vector<uint8_t> gradient(total), solid(total);
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            const Rgba g = {uint8_t(x), uint8_t(y), 0, 255};
            const Rgba s = {200, 100, 50, 255};
            std::memcpy(&gradient[y * pitch + x * 4], &g, 4);
            std::memcpy(&solid[y * pitch + x * 4], &s, 4);
        }
    }
    auto make_upload = [&](const std::vector<uint8_t> &data) {
        ComPtr<ID3D12Resource> buffer = ctx.create_buffer(D3D12_HEAP_TYPE_UPLOAD, total);
        void *mapped = nullptr;
        CHECK_HR(buffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data.data(), data.size());
        buffer->Unmap(0, nullptr);
        return buffer;
    };
    ComPtr<ID3D12Resource> gradient_upload = make_upload(gradient);
    ComPtr<ID3D12Resource> solid_upload = make_upload(solid);
    ComPtr<ID3D12Resource> readback_full = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, total);
    ComPtr<ID3D12Resource> readback_region = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, total);

    auto buffer_location = [&](ID3D12Resource *buffer) { return CD3DX12_TEXTURE_COPY_LOCATION(buffer, footprint); };
    const CD3DX12_TEXTURE_COPY_LOCATION texture_location(texture.Get(), 0);

    ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();

    // Whole gradient first, then the 4x4 block (2..6, 2..6) of the solid image
    // lands at texture (8, 8).
    CD3DX12_TEXTURE_COPY_LOCATION src = buffer_location(gradient_upload.Get());
    list->CopyTextureRegion(&texture_location, 0, 0, 0, &src, nullptr);
    src = buffer_location(solid_upload.Get());
    const D3D12_BOX solid_box = {2, 2, 0, 6, 6, 1};
    list->CopyTextureRegion(&texture_location, 8, 8, 0, &src, &solid_box);

    // Whole texture back, and texture region (8, 8)-(12, 12) into the readback
    // image at (2, 3).
    CD3DX12_TEXTURE_COPY_LOCATION dst = buffer_location(readback_full.Get());
    list->CopyTextureRegion(&dst, 0, 0, 0, &texture_location, nullptr);
    dst = buffer_location(readback_region.Get());
    const D3D12_BOX texture_box = {8, 8, 0, 12, 12, 1};
    list->CopyTextureRegion(&dst, 2, 3, 0, &texture_location, &texture_box);
    CHECK_HR(list->Close());
    ctx.execute_and_wait(list.Get());

    auto pixel = [&](ID3D12Resource *readback, UINT x, UINT y) {
        void *mapped = nullptr;
        CHECK_HR(readback->Map(0, nullptr, &mapped));
        Rgba p;
        std::memcpy(&p, static_cast<uint8_t *>(mapped) + y * pitch + x * 4, 4);
        readback->Unmap(0, nullptr);
        return p;
    };
    auto expect = [](const char *what, Rgba p, Rgba e) {
        if (std::memcmp(&p, &e, 4) != 0) {
            std::fprintf(stderr, "%s: got (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n", what, p.r, p.g, p.b, p.a, e.r,
                         e.g, e.b, e.a);
            std::exit(1);
        }
    };

    const Rgba solid_px = {200, 100, 50, 255};
    expect("outside the patch, left", pixel(readback_full.Get(), 7, 8), {7, 8, 0, 255});
    expect("patch origin", pixel(readback_full.Get(), 8, 8), solid_px);
    expect("patch last texel", pixel(readback_full.Get(), 11, 11), solid_px);
    expect("outside the patch, right", pixel(readback_full.Get(), 12, 8), {12, 8, 0, 255});
    expect("outside the patch, below", pixel(readback_full.Get(), 8, 12), {8, 12, 0, 255});

    expect("region origin", pixel(readback_region.Get(), 2, 3), solid_px);
    expect("region last texel", pixel(readback_region.Get(), 5, 6), solid_px);
    expect("region left of origin", pixel(readback_region.Get(), 1, 3), {0, 0, 0, 0});
    expect("region right of end", pixel(readback_region.Get(), 6, 3), {0, 0, 0, 0});
    expect("region above origin", pixel(readback_region.Get(), 2, 2), {0, 0, 0, 0});
    return 0;
}
