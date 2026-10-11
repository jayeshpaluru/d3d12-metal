// SPDX-License-Identifier: LGPL-2.1-or-later
// Textures, shader resource views and samplers, checked by what a compute shader reads
// (tests/shaders/probe.hlsl): texture and buffer views of every kind, mip and slice ranges, component
// mappings, address modes, filters, null descriptors, descriptor copies, format reinterpretation,
// block-compressed data and texture-to-texture copies.
#include "probe.h"

namespace {

// ---- Texture data ----------------------------------------------------------------------------------

struct Rgba {
    uint8_t r, g, b, a;
};

// 4x4 texels, three mips: mip 0 varies per texel, mip 1 and 2 are flat.
ComPtr<ID3D12Resource> make_mip_texture(Gpu &gpu, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    ComPtr<ID3D12Resource> texture = gpu.texture(tex2d_desc(format, 4, 4, flags, 1, 3));
    Rgba mip0[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x)
            mip0[y * 4 + x] = {uint8_t(x * 60), uint8_t(y * 60), 100, 255};
    }
    const Rgba mip1[4] = {{200, 10, 10, 255}, {200, 10, 10, 255}, {200, 10, 10, 255}, {200, 10, 10, 255}};
    const Rgba mip2[1] = {{10, 200, 10, 255}};
    gpu.upload_texture(texture.Get(), 0, mip0, 4);
    gpu.upload_texture(texture.Get(), 1, mip1, 4);
    gpu.upload_texture(texture.Get(), 2, mip2, 4);
    return texture;
}

Float4 texel0(int x, int y)
{
    return rgba8(x * 60, y * 60, 100, 255);
}

float texel_center(int i)
{
    return (i + 0.5f) / 4.0f;
}

} // namespace

int main()
{
    Gpu gpu;
    Probe probe(gpu);

    // ---- 2D textures: point sampling, Load, mip ranges --------------------------------------------------
    {
        ComPtr<ID3D12Resource> texture = make_mip_texture(gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture2D.MipLevels = UINT_MAX;
        probe.set_srv(0, texture.Get(), desc);

        std::vector<Float4> coords;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x)
                coords.push_back({texel_center(x), texel_center(y), 0, 0});
        }
        std::vector<Float4> got = probe.run(0, coords);
        for (int i = 0; i < 16; ++i)
            expect_float4("point sample mip 0", got[i], texel0(i % 4, i / 4));

        expect_float4("sample mip 1", probe.one(0, {0.5f, 0.5f, 0, 1}), rgba8(200, 10, 10, 255));
        expect_float4("sample mip 2", probe.one(0, {0.5f, 0.5f, 0, 2}), rgba8(10, 200, 10, 255));
        expect_float4("Load mip 0", probe.one(4, {2, 3, 0, 0}), texel0(2, 3));
        expect_float4("Load mip 1", probe.one(4, {1, 1, 0, 1}), rgba8(200, 10, 10, 255));

        // Only mips 1 and 2: lod 0 of the view is mip 1 of the texture.
        desc.Texture2D.MostDetailedMip = 1;
        desc.Texture2D.MipLevels = 2;
        probe.set_srv(0, texture.Get(), desc);
        expect_float4("view starting at mip 1", probe.one(0, {0.5f, 0.5f, 0, 0}), rgba8(200, 10, 10, 255));
        expect_float4("view starting at mip 1, lod 1", probe.one(0, {0.5f, 0.5f, 0, 1}), rgba8(10, 200, 10, 255));

        // ResourceMinLODClamp goes into the descriptor (checked here only for being accepted: the shader
        // converter applies it to Sample, which a compute shader cannot use without derivatives).
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = 3;
        desc.Texture2D.ResourceMinLODClamp = 1.0f;
        probe.set_srv(0, texture.Get(), desc);
        desc.Texture2D.ResourceMinLODClamp = 0.0f;
        probe.set_srv(0, texture.Get(), desc);

        // The default view (a null description) covers the whole texture.
        gpu.device->CreateShaderResourceView(texture.Get(), nullptr, gpu.cpu_handle(probe.srv_heap.Get(), 0));
        expect_float4("default view", probe.one(0, {texel_center(1), texel_center(2), 0, 0}), texel0(1, 2));
    }

    // ---- Filters and address modes ------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> texture = make_mip_texture(gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture2D.MipLevels = 3;
        probe.set_srv(0, texture.Get(), desc);

        // Linear: halfway between two texels.
        probe.set_sampler(sampler_desc(D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
        expect_float4("linear", probe.one(0, {0.5f, texel_center(0), 0, 0}), rgba8(90, 0, 100, 255));
        probe.set_sampler(sampler_desc(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
        expect_float4("point at the same place", probe.one(0, {0.51f, texel_center(0), 0, 0}), texel0(2, 0));

        const float row = texel_center(1);
        const struct {
            D3D12_TEXTURE_ADDRESS_MODE mode;
            float u;
            Float4 expected;
            const char *name;
        } cases[] = {
            {D3D12_TEXTURE_ADDRESS_MODE_WRAP, 1.375f, texel0(1, 1), "wrap"},
            {D3D12_TEXTURE_ADDRESS_MODE_WRAP, -0.125f, texel0(3, 1), "wrap negative"},
            {D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1.5f, texel0(3, 1), "clamp"},
            {D3D12_TEXTURE_ADDRESS_MODE_CLAMP, -0.5f, texel0(0, 1), "clamp negative"},
            {D3D12_TEXTURE_ADDRESS_MODE_MIRROR, 1.125f, texel0(3, 1), "mirror"},
            {D3D12_TEXTURE_ADDRESS_MODE_MIRROR, 1.375f, texel0(2, 1), "mirror 2"},
            {D3D12_TEXTURE_ADDRESS_MODE_BORDER, -0.25f, {0, 0, 0, 0}, "border"},
            {D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.375f, texel0(1, 1), "border inside"},
            {D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, -0.125f, texel0(0, 1), "mirror once"},
        };
        for (const auto &c : cases) {
            probe.set_sampler(sampler_desc(D3D12_FILTER_MIN_MAG_MIP_POINT, c.mode));
            expect_float4(c.name, probe.one(0, {c.u, row, 0, 0}), c.expected);
        }
        // Opaque white border.
        D3D12_SAMPLER_DESC white = sampler_desc(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        white.BorderColor[0] = white.BorderColor[1] = white.BorderColor[2] = white.BorderColor[3] = 1.0f;
        probe.set_sampler(white);
        expect_float4("white border", probe.one(0, {-0.25f, row, 0, 0}), {1, 1, 1, 1});
        // Opaque black border.
        D3D12_SAMPLER_DESC black = sampler_desc(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        black.BorderColor[3] = 1.0f;
        probe.set_sampler(black);
        expect_float4("black border", probe.one(0, {-0.25f, row, 0, 0}), {0, 0, 0, 1});
        probe.set_sampler(sampler_desc(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
    }

    // ---- Texture arrays and cube maps ---------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> array = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, D3D12_RESOURCE_FLAG_NONE, 6, 1));
        const Rgba faces[6] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
                               {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255}};
        for (UINT slice = 0; slice < 6; ++slice) {
            const Rgba flat[4] = {faces[slice], faces[slice], faces[slice], faces[slice]};
            gpu.upload_texture(array.Get(), slice, flat, 4);
        }

        // An array view over slices 1 and 2: array index 0 is slice 1.
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2DARRAY, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = 1;
        desc.Texture2DArray.ArraySize = 2;
        probe.set_srv(1, array.Get(), desc);
        expect_float4("array slice 0 of view", probe.one(1, {0.5f, 0.5f, 0, 0}), rgba8(0, 255, 0, 255));
        expect_float4("array slice 1 of view", probe.one(1, {0.5f, 0.5f, 1, 0}), rgba8(0, 0, 255, 255));

        // The same texture as a cube map; directions per D3D12 face order +X -X +Y -Y +Z -Z.
        D3D12_SHADER_RESOURCE_VIEW_DESC cube = srv_desc(D3D12_SRV_DIMENSION_TEXTURECUBE, DXGI_FORMAT_R8G8B8A8_UNORM);
        cube.TextureCube.MipLevels = 1;
        probe.set_srv(2, array.Get(), cube);
        const Float4 directions[6] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
        const std::vector<Float4> got = probe.run(2, std::vector<Float4>(directions, directions + 6));
        for (int i = 0; i < 6; ++i)
            expect_float4("cube face", got[i], rgba8(faces[i].r, faces[i].g, faces[i].b, 255));
    }

    // ---- 3D textures ----------------------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> volume = gpu.texture(
            texture_desc(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 4, 1, D3D12_RESOURCE_FLAG_NONE));
        std::vector<Rgba> voxels(64);
        for (int z = 0; z < 4; ++z) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x)
                    voxels[(z * 4 + y) * 4 + x] = {uint8_t(x * 60), uint8_t(y * 60), uint8_t(z * 60), 255};
            }
        }
        gpu.upload_texture(volume.Get(), 0, voxels.data(), 4);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture3D.MipLevels = 1;
        probe.set_srv(3, volume.Get(), desc);
        expect_float4("3D texel", probe.one(3, {texel_center(1), texel_center(2), texel_center(3), 0}), rgba8(60, 120, 180, 255));
        expect_float4("3D texel 2", probe.one(3, {texel_center(3), texel_center(0), texel_center(2), 0}), rgba8(180, 0, 120, 255));
    }

    // ---- Component mapping -----------------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> texture = make_mip_texture(gpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture2D.MipLevels = 1;
        const float u = texel_center(2), v = texel_center(1);  // texel (2, 1) = (120, 60, 100, 255)
        // Mapping fields: 0-3 take a component, 4 forces 0, 5 forces 1.
        auto mapping = [](int r, int g, int b, int a) {
            return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a);
        };
        desc.Shader4ComponentMapping = mapping(2, 1, 0, 3);
        probe.set_srv(0, texture.Get(), desc);
        expect_float4("BGRA swizzle", probe.one(0, {u, v, 0, 0}), rgba8(100, 60, 120, 255));
        desc.Shader4ComponentMapping = mapping(0, 0, 0, 5);
        probe.set_srv(0, texture.Get(), desc);
        expect_float4("red broadcast, alpha one", probe.one(0, {u, v, 0, 0}), rgba8(120, 120, 120, 255));
        desc.Shader4ComponentMapping = mapping(4, 1, 4, 5);
        probe.set_srv(0, texture.Get(), desc);
        expect_float4("forced zeros", probe.one(0, {u, v, 0, 0}), rgba8(0, 60, 0, 255));
    }

    // ---- Null descriptors ------------------------------------------------------------------------------------
    {
        probe.clear_views();
        expect_float4("null 2D", probe.one(0, {0.5f, 0.5f, 0, 0}), {0, 0, 0, 0});
        expect_float4("null 3D", probe.one(3, {0.5f, 0.5f, 0.5f, 0}), {0, 0, 0, 0});
        expect_float4("null cube", probe.one(2, {1, 0, 0, 0}), {0, 0, 0, 0});
        expect_float4("null array", probe.one(1, {0.5f, 0.5f, 0, 0}), {0, 0, 0, 0});
        expect_float4("null typed buffer", probe.one(5, {3, 0, 0, 0}), {0, 0, 0, 0});
        expect_float4("null raw buffer", probe.one(6, {3, 0, 0, 0}), {0, 0, 0, 0});
        expect_float4("null structured buffer", probe.one(7, {3, 0, 0, 0}), {0, 0, 0, 0});
    }

    // ---- Buffer views ----------------------------------------------------------------------------------------
    {
        std::vector<uint16_t> shorts(64);
        for (size_t i = 0; i < shorts.size(); ++i)
            shorts[i] = static_cast<uint16_t>(i * 3 + 1);
        ComPtr<ID3D12Resource> typed = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, shorts.size() * 2);
        ComPtr<ID3D12Resource> staging = gpu.upload_buffer(shorts.data(), shorts.size() * 2);
        gpu.run([&](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(typed.Get(), 0, staging.Get(), 0, shorts.size() * 2); });

        // A typed view that starts at element 8 (byte 16). Texture buffers must start at a multiple of
        // 16 bytes and the shader converter has no way to skip elements, so that is the smallest start
        // other than 0 (see docs/STATUS.md).
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R16_UINT);
        desc.Buffer.FirstElement = 8;
        desc.Buffer.NumElements = 20;
        probe.set_srv(4, typed.Get(), desc);
        std::vector<Float4> coords;
        for (int i = 0; i < 20; ++i)
            coords.push_back({float(i), 0, 0, 0});
        std::vector<Float4> got = probe.run(5, coords);
        for (int i = 0; i < 20; ++i)
            CHECK_EQ(int(got[i].x), (i + 8) * 3 + 1);

        // Raw: ByteAddressBuffer, from element (dword) 2.
        std::vector<uint32_t> dwords(32);
        for (size_t i = 0; i < dwords.size(); ++i)
            dwords[i] = 1000 + static_cast<uint32_t>(i);
        ComPtr<ID3D12Resource> raw = gpu.upload_buffer(dwords.data(), dwords.size() * 4);
        D3D12_SHADER_RESOURCE_VIEW_DESC raw_desc = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32_TYPELESS);
        raw_desc.Buffer.FirstElement = 2;
        raw_desc.Buffer.NumElements = 16;
        raw_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        probe.set_srv(5, raw.Get(), raw_desc);
        got = probe.run(6, {{0, 0, 0, 0}, {4, 0, 0, 0}});
        uint32_t bits[4];
        std::memcpy(bits, &got[0], 16);
        CHECK_EQ(bits[0], 1002u);
        CHECK_EQ(bits[3], 1005u);
        std::memcpy(bits, &got[1], 16);
        CHECK_EQ(bits[0], 1006u);

        // Structured buffer of float4, from element 1.
        std::vector<Float4> elements(8);
        for (int i = 0; i < 8; ++i)
            elements[i] = {float(i), float(i) * 2, float(i) * 3, 1};
        ComPtr<ID3D12Resource> structured = gpu.upload_buffer(elements.data(), elements.size() * 16);
        D3D12_SHADER_RESOURCE_VIEW_DESC s_desc = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN);
        s_desc.Buffer.FirstElement = 1;
        s_desc.Buffer.NumElements = 7;
        s_desc.Buffer.StructureByteStride = 16;
        probe.set_srv(6, structured.Get(), s_desc);
        got = probe.run(7, {{0, 0, 0, 0}, {5, 0, 0, 0}, {7, 0, 0, 0}});
        expect_float4("structured 0", got[0], elements[1], 0.0001f);
        expect_float4("structured 5", got[1], elements[6], 0.0001f);
        // Past the view: bounds-checked, reads zero.
        expect_float4("structured past the end", got[2], {0, 0, 0, 0}, 0.0001f);
        probe.clear_views();
    }

    // ---- Format reinterpretation -----------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_TYPELESS, 2, 2));
        const Rgba grey[4] = {{128, 128, 128, 255}, {128, 128, 128, 255}, {128, 128, 128, 255}, {128, 128, 128, 255}};
        gpu.upload_texture(texture.Get(), 0, grey, 4);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture2D.MipLevels = 1;
        probe.set_srv(0, texture.Get(), desc);
        expect_float4("typeless as UNORM", probe.one(0, {0.5f, 0.5f, 0, 0}), rgba8(128, 128, 128, 255));
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        probe.set_srv(0, texture.Get(), desc);
        expect_float4("typeless as sRGB", probe.one(0, {0.5f, 0.5f, 0, 0}), {0.2158f, 0.2158f, 0.2158f, 1.0f}, 0.01f);
    }

    // ---- Other formats -----------------------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> float_texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_FLOAT, 2, 2));
        const float values[4] = {0.25f, 0.5f, 0.75f, 1.5f};
        gpu.upload_texture(float_texture.Get(), 0, values, 4);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT);
        desc.Texture2D.MipLevels = 1;
        probe.set_srv(0, float_texture.Get(), desc);
        expect_float4("R32_FLOAT", probe.one(0, {0.75f, 0.75f, 0, 0}), {1.5f, 0, 0, 1}, 0.0001f);
        expect_float4("R32_FLOAT 2", probe.one(0, {0.25f, 0.75f, 0, 0}), {0.75f, 0, 0, 1}, 0.0001f);

        ComPtr<ID3D12Resource> two_channel = gpu.texture(tex2d_desc(DXGI_FORMAT_R16G16_UNORM, 1, 1));
        const uint16_t rg[2] = {65535, 32768};
        gpu.upload_texture(two_channel.Get(), 0, rg, 4);
        desc.Format = DXGI_FORMAT_R16G16_UNORM;
        probe.set_srv(0, two_channel.Get(), desc);
        expect_float4("R16G16_UNORM", probe.one(0, {0.5f, 0.5f, 0, 0}), {1.0f, 0.5f, 0, 1}, 0.001f);
        probe.clear_views();
    }

    // ---- Block-compressed data --------------------------------------------------------------------------------
    {
        // 8x8 BC1: four blocks of solid colour at mip 0, one block at each of the smaller mips.
        ComPtr<ID3D12Resource> bc1 = gpu.texture(tex2d_desc(DXGI_FORMAT_BC1_UNORM, 8, 8, D3D12_RESOURCE_FLAG_NONE, 1, 4));
        auto block = [](uint16_t color) {
            std::vector<uint8_t> b(8, 0);
            b[0] = b[2] = color & 0xff;
            b[1] = b[3] = color >> 8;  // both endpoints equal: every texel is the endpoint
            return b;
        };
        const uint16_t colors[4] = {0xf800, 0x07e0, 0x001f, 0xffe0};  // red, green, blue, yellow (RGB565)
        std::vector<uint8_t> mip0;
        for (uint16_t color : colors) {
            const std::vector<uint8_t> b = block(color);
            mip0.insert(mip0.end(), b.begin(), b.end());
        }
        gpu.upload_texture(bc1.Get(), 0, mip0.data(), 8);
        const std::vector<uint8_t> grey = block(0x8410);
        gpu.upload_texture(bc1.Get(), 1, grey.data(), 8);
        gpu.upload_texture(bc1.Get(), 2, grey.data(), 8);
        gpu.upload_texture(bc1.Get(), 3, grey.data(), 8);

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_BC1_UNORM);
        desc.Texture2D.MipLevels = 4;
        probe.set_srv(0, bc1.Get(), desc);
        const std::vector<Float4> got = probe.run(0, {{0.25f, 0.25f, 0, 0}, {0.75f, 0.25f, 0, 0}, {0.25f, 0.75f, 0, 0},
                                                      {0.75f, 0.75f, 0, 0}, {0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 3}});
        expect_float4("BC1 red", got[0], {1, 0, 0, 1});
        expect_float4("BC1 green", got[1], {0, 1, 0, 1});
        expect_float4("BC1 blue", got[2], {0, 0, 1, 1});
        expect_float4("BC1 yellow", got[3], {1, 1, 0, 1});
        expect_float4("BC1 mip 1", got[4], {0.5f, 0.5f, 0.5f, 1}, 0.03f);
        expect_float4("BC1 mip 3 (1x1)", got[5], {0.5f, 0.5f, 0.5f, 1}, 0.03f);
        probe.clear_views();
    }

    // ---- Descriptor copies -----------------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> texture = make_mip_texture(gpu);
        ComPtr<ID3D12DescriptorHeap> staging = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, false);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.Texture2D.MipLevels = 3;
        gpu.device->CreateShaderResourceView(texture.Get(), &desc, gpu.cpu_handle(staging.Get(), 2));
        gpu.device->CopyDescriptorsSimple(1, gpu.cpu_handle(probe.srv_heap.Get(), 0), gpu.cpu_handle(staging.Get(), 2),
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        expect_float4("CopyDescriptorsSimple", probe.one(0, {texel_center(3), texel_center(3), 0, 0}), texel0(3, 3));

        // CopyDescriptors with ranges of different sizes.
        gpu.device->CreateShaderResourceView(texture.Get(), &desc, gpu.cpu_handle(staging.Get(), 0));
        gpu.device->CreateShaderResourceView(texture.Get(), &desc, gpu.cpu_handle(staging.Get(), 1));
        probe.clear_views();
        const D3D12_CPU_DESCRIPTOR_HANDLE dst[2] = {gpu.cpu_handle(probe.srv_heap.Get(), 0), gpu.cpu_handle(probe.srv_heap.Get(), 3)};
        const UINT dst_sizes[2] = {1, 1};
        const D3D12_CPU_DESCRIPTOR_HANDLE src[1] = {gpu.cpu_handle(staging.Get(), 0)};
        const UINT src_sizes[1] = {2};
        gpu.device->CopyDescriptors(2, dst, dst_sizes, 1, src, src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        expect_float4("CopyDescriptors", probe.one(0, {texel_center(2), texel_center(0), 0, 0}), texel0(2, 0));
        probe.clear_views();
    }

    // ---- Texture copies -----------------------------------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> source = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, D3D12_RESOURCE_FLAG_NONE, 1, 2));
        ComPtr<ID3D12Resource> dest = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, D3D12_RESOURCE_FLAG_NONE, 1, 2));
        std::vector<Rgba> pattern(64), zeros(64, Rgba{0, 0, 0, 0});
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x)
                pattern[y * 8 + x] = {uint8_t(x * 30), uint8_t(y * 30), 7, 255};
        }
        const std::vector<Rgba> mip1(16, Rgba{9, 9, 9, 9});
        gpu.upload_texture(source.Get(), 0, pattern.data(), 4);
        gpu.upload_texture(source.Get(), 1, mip1.data(), 4);
        gpu.upload_texture(dest.Get(), 0, zeros.data(), 4);
        gpu.upload_texture(dest.Get(), 1, zeros.data(), 4);

        // A box of the source goes to another place of the destination.
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            const D3D12_TEXTURE_COPY_LOCATION dst = subresource_location(dest.Get(), 0);
            const D3D12_TEXTURE_COPY_LOCATION src = subresource_location(source.Get(), 0);
            const D3D12_BOX box = {2, 3, 0, 6, 5, 1};
            list->CopyTextureRegion(&dst, 1, 4, 0, &src, &box);
        });
        Image image = gpu.read_texture(dest.Get(), 0, 4);
        expect_pixel("copied texel (1,4)", image.pixel(1, 4), {60, 90, 7, 255});
        expect_pixel("copied texel (4,5)", image.pixel(4, 5), {150, 120, 7, 255}, 0);  // (5,4) of the source
        expect_pixel("outside the box", image.pixel(0, 0), {0, 0, 0, 0});
        expect_pixel("outside the box 2", image.pixel(5, 4), {0, 0, 0, 0});

        // The whole resource, mips included.
        gpu.run([&](ID3D12GraphicsCommandList *list) { list->CopyResource(dest.Get(), source.Get()); });
        image = gpu.read_texture(dest.Get(), 0, 4);
        expect_pixel("CopyResource mip 0", image.pixel(5, 6), {150, 180, 7, 255}, 0);
        image = gpu.read_texture(dest.Get(), 1, 4);
        expect_pixel("CopyResource mip 1", image.pixel(2, 2), {9, 9, 9, 9}, 0);

        // Between different formats of one family.
        ComPtr<ID3D12Resource> srgb = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 8, 8, D3D12_RESOURCE_FLAG_NONE, 1, 2));
        gpu.run([&](ID3D12GraphicsCommandList *list) { list->CopyResource(srgb.Get(), source.Get()); });
        image = gpu.read_texture(srgb.Get(), 0, 4);
        expect_pixel("copy into an sRGB texture keeps the bits", image.pixel(5, 6), {150, 180, 7, 255}, 0);
    }

    std::printf("p_textures: PASS\n");
    return 0;
}
