// Root signature serialization and deserialization: round trips through the
// exported D3D12 entry points, malformed input, and DXC interoperability.
#include "test_util.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// Containers produced by DXC (rootsig_1_0 / rootsig_1_1), checksums included.
// c2_1_0: RootConstants(4,b0), RootConstants(2,b2), DescriptorTable(CBV(b20), SRV(t0, 2)),
//   CBV(b21), SRV(t5), StaticSampler(s0). The hashed bytes leave 56 in the last block.
static const uint8_t kDxcRootSig10[] = {
  0x44, 0x58, 0x42, 0x43, 0x01, 0x63, 0x69, 0x59, 0x09, 0x26, 0x75, 0xe0,
  0x1a, 0x60, 0x4d, 0x0e, 0x1c, 0x47, 0x11, 0x83, 0x01, 0x00, 0x00, 0x00,
  0x0c, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
  0x52, 0x54, 0x53, 0x30, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x05, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x9c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x74, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
  0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x15, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x55, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xff, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// d_1_1: RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), a pixel-visible UAV table, a
// sampler table, a vertex-visible root UAV and a fully specified static sampler.
static const uint8_t kDxcRootSig11[] = {
  0x44, 0x58, 0x42, 0x43, 0x90, 0x9d, 0xa5, 0x13, 0x25, 0xec, 0x59, 0x15,
  0x98, 0xd3, 0x0d, 0x96, 0x2e, 0x9f, 0x87, 0xf4, 0x01, 0x00, 0x00, 0x00,
  0xe8, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
  0x52, 0x54, 0x53, 0x30, 0xbc, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x88, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x05, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x44, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x55, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3f, 0x08, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
  0x00, 0x00, 0x10, 0x41, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x05, 0x00, 0x00, 0x00
};

namespace {

constexpr UINT kAppend = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

// The test signature. Parameters: root constants, root CBV, a CBV/SRV/UAV table
// and a sampler table; one fully non-default static sampler.
const D3D12_DESCRIPTOR_RANGE1 kRanges11[] = {
    {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, 0},
    {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 2, 1, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, kAppend},
    {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 3, 2,
     D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
     7},
};
const D3D12_DESCRIPTOR_RANGE1 kSamplerRange11[] = {
    {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 8, 1, 3, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, kAppend},
};

D3D12_ROOT_PARAMETER1 make_param11(D3D12_ROOT_PARAMETER_TYPE type, D3D12_SHADER_VISIBILITY vis)
{
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = type;
    p.ShaderVisibility = vis;
    return p;
}

D3D12_STATIC_SAMPLER_DESC make_sampler()
{
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    s.MipLODBias = -0.75f;
    s.MaxAnisotropy = 7;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    s.MinLOD = 0.5f;
    s.MaxLOD = 12.25f;
    s.ShaderRegister = 5;
    s.RegisterSpace = 6;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return s;
}

// The description in both versions. The 1.0 flavor has no flags; the 1.1 flavor
// carries the flags above.
struct TestSignature {
    D3D12_STATIC_SAMPLER_DESC sampler = make_sampler();
    D3D12_ROOT_PARAMETER1 params11[4];
    D3D12_ROOT_PARAMETER params10[4];
    D3D12_DESCRIPTOR_RANGE ranges10[3];
    D3D12_DESCRIPTOR_RANGE sampler_range10[1];
    D3D12_ROOT_SIGNATURE_DESC1 desc11{};
    D3D12_ROOT_SIGNATURE_DESC desc10{};

    TestSignature()
    {
        params11[0] = make_param11(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, D3D12_SHADER_VISIBILITY_VERTEX);
        params11[0].Constants = {1, 2, 5};
        params11[1] = make_param11(D3D12_ROOT_PARAMETER_TYPE_CBV, D3D12_SHADER_VISIBILITY_PIXEL);
        params11[1].Descriptor = {4, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
        params11[2] = make_param11(D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_SHADER_VISIBILITY_ALL);
        params11[2].DescriptorTable = {3, kRanges11};
        params11[3] = make_param11(D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_SHADER_VISIBILITY_PIXEL);
        params11[3].DescriptorTable = {1, kSamplerRange11};
        desc11 = {4, params11, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};

        for (int i = 0; i < 3; i++) {
            const auto &r = kRanges11[i];
            ranges10[i] = {r.RangeType, r.NumDescriptors, r.BaseShaderRegister, r.RegisterSpace,
                           r.OffsetInDescriptorsFromTableStart};
        }
        const auto &sr = kSamplerRange11[0];
        sampler_range10[0] = {sr.RangeType, sr.NumDescriptors, sr.BaseShaderRegister, sr.RegisterSpace,
                              sr.OffsetInDescriptorsFromTableStart};
        for (int i = 0; i < 4; i++) {
            params10[i].ParameterType = params11[i].ParameterType;
            params10[i].ShaderVisibility = params11[i].ShaderVisibility;
        }
        params10[0].Constants = params11[0].Constants;
        params10[1].Descriptor = {4, 1};
        params10[2].DescriptorTable = {3, ranges10};
        params10[3].DescriptorTable = {1, sampler_range10};
        desc10 = {4, params10, 1, &sampler, desc11.Flags};
    }
};

std::vector<uint8_t> bytes_of(ID3DBlob *blob)
{
    const auto *p = static_cast<const uint8_t *>(blob->GetBufferPointer());
    return {p, p + blob->GetBufferSize()};
}

void check_sampler_eq(const D3D12_STATIC_SAMPLER_DESC &a, const D3D12_STATIC_SAMPLER_DESC &b)
{
    CHECK(a.Filter == b.Filter);
    CHECK(a.AddressU == b.AddressU);
    CHECK(a.AddressV == b.AddressV);
    CHECK(a.AddressW == b.AddressW);
    CHECK(a.MipLODBias == b.MipLODBias);
    CHECK(a.MaxAnisotropy == b.MaxAnisotropy);
    CHECK(a.ComparisonFunc == b.ComparisonFunc);
    CHECK(a.BorderColor == b.BorderColor);
    CHECK(a.MinLOD == b.MinLOD);
    CHECK(a.MaxLOD == b.MaxLOD);
    CHECK(a.ShaderRegister == b.ShaderRegister);
    CHECK(a.RegisterSpace == b.RegisterSpace);
    CHECK(a.ShaderVisibility == b.ShaderVisibility);
}

void check_range_eq(const D3D12_DESCRIPTOR_RANGE &a, const D3D12_DESCRIPTOR_RANGE &b)
{
    CHECK(a.RangeType == b.RangeType);
    CHECK(a.NumDescriptors == b.NumDescriptors);
    CHECK(a.BaseShaderRegister == b.BaseShaderRegister);
    CHECK(a.RegisterSpace == b.RegisterSpace);
    CHECK(a.OffsetInDescriptorsFromTableStart == b.OffsetInDescriptorsFromTableStart);
}

void check_range_eq(const D3D12_DESCRIPTOR_RANGE1 &a, const D3D12_DESCRIPTOR_RANGE1 &b)
{
    CHECK(a.RangeType == b.RangeType);
    CHECK(a.NumDescriptors == b.NumDescriptors);
    CHECK(a.BaseShaderRegister == b.BaseShaderRegister);
    CHECK(a.RegisterSpace == b.RegisterSpace);
    CHECK(a.Flags == b.Flags);
    CHECK(a.OffsetInDescriptorsFromTableStart == b.OffsetInDescriptorsFromTableStart);
}

// Compares two descriptions of the same version field by field. Descriptor
// flags are compared only for 1.1.
template <typename Desc, typename Param>
void check_desc_eq(const Desc &a, const Desc &b)
{
    CHECK(a.Flags == b.Flags);
    CHECK(a.NumParameters == b.NumParameters);
    CHECK(a.NumStaticSamplers == b.NumStaticSamplers);
    for (UINT i = 0; i < a.NumStaticSamplers; i++)
        check_sampler_eq(a.pStaticSamplers[i], b.pStaticSamplers[i]);
    for (UINT i = 0; i < a.NumParameters; i++) {
        const Param &p = a.pParameters[i], &q = b.pParameters[i];
        CHECK(p.ParameterType == q.ParameterType);
        CHECK(p.ShaderVisibility == q.ShaderVisibility);
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            CHECK(p.DescriptorTable.NumDescriptorRanges == q.DescriptorTable.NumDescriptorRanges);
            for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; j++)
                check_range_eq(p.DescriptorTable.pDescriptorRanges[j], q.DescriptorTable.pDescriptorRanges[j]);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            CHECK(p.Constants.ShaderRegister == q.Constants.ShaderRegister);
            CHECK(p.Constants.RegisterSpace == q.Constants.RegisterSpace);
            CHECK(p.Constants.Num32BitValues == q.Constants.Num32BitValues);
            break;
        default:
            CHECK(p.Descriptor.ShaderRegister == q.Descriptor.ShaderRegister);
            CHECK(p.Descriptor.RegisterSpace == q.Descriptor.RegisterSpace);
            if constexpr (std::is_same_v<Param, D3D12_ROOT_PARAMETER1>)
                CHECK(p.Descriptor.Flags == q.Descriptor.Flags);
            break;
        }
    }
}

void check_desc10_eq(const D3D12_ROOT_SIGNATURE_DESC &a, const D3D12_ROOT_SIGNATURE_DESC &b)
{
    check_desc_eq<D3D12_ROOT_SIGNATURE_DESC, D3D12_ROOT_PARAMETER>(a, b);
}

void check_desc11_eq(const D3D12_ROOT_SIGNATURE_DESC1 &a, const D3D12_ROOT_SIGNATURE_DESC1 &b)
{
    check_desc_eq<D3D12_ROOT_SIGNATURE_DESC1, D3D12_ROOT_PARAMETER1>(a, b);
}

// Expected 1.1 view of the 1.0 signature: 1.0 defaults for every flag.
void check_default_flags(const D3D12_ROOT_SIGNATURE_DESC1 &d)
{
    constexpr UINT kVolatileRange =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    for (UINT i = 0; i < d.NumParameters; i++) {
        const D3D12_ROOT_PARAMETER1 &p = d.pParameters[i];
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV)
            CHECK(p.Descriptor.Flags == D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
        if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; j++) {
            const D3D12_DESCRIPTOR_RANGE1 &r = p.DescriptorTable.pDescriptorRanges[j];
            if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
                CHECK(r.Flags == D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
            else
                CHECK(r.Flags == kVolatileRange);
        }
    }
}

// The same signature as `TestSignature::desc11`, with the 1.0 default flags.
struct DefaultFlagSignature {
    TestSignature sig;
    D3D12_DESCRIPTOR_RANGE1 ranges[3];
    D3D12_DESCRIPTOR_RANGE1 sampler_range[1];
    D3D12_ROOT_PARAMETER1 params[4];
    D3D12_ROOT_SIGNATURE_DESC1 desc{};

    DefaultFlagSignature()
    {
        std::memcpy(ranges, kRanges11, sizeof(ranges));
        std::memcpy(sampler_range, kSamplerRange11, sizeof(sampler_range));
        std::memcpy(params, sig.params11, sizeof(params));
        constexpr UINT kVolatile =
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        for (auto &r : ranges)
            r.Flags = D3D12_DESCRIPTOR_RANGE_FLAGS(kVolatile);
        params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        params[2].DescriptorTable.pDescriptorRanges = ranges;
        params[3].DescriptorTable.pDescriptorRanges = sampler_range;
        desc = sig.desc11;
        desc.pParameters = params;
    }
};

void test_roundtrip_11()
{
    TestSignature sig;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = sig.desc11;

    Com<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&versioned, blob.put(), error.put()));
    CHECK(!error.get());
    CHECK(blob->GetBufferSize() > 44);
    CHECK(std::memcmp(blob->GetBufferPointer(), "DXBC", 4) == 0);

    Com<ID3D12VersionedRootSignatureDeserializer> des;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           __uuidof(ID3D12VersionedRootSignatureDeserializer),
                                                           reinterpret_cast<void **>(des.put())));

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d = des->GetUnconvertedRootSignatureDesc();
    CHECK(d && d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
    check_desc11_eq(d->Desc_1_1, sig.desc11);

    CHECK_HR(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &d));
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
    check_desc11_eq(d->Desc_1_1, sig.desc11);

    // 1.1 -> 1.0 drops the flags.
    CHECK_HR(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &d));
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_0);
    check_desc10_eq(d->Desc_1_0, sig.desc10);

    CHECK(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_2, &d) == E_INVALIDARG);
    CHECK(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION(0), &d) == E_INVALIDARG);
    CHECK(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, nullptr) == E_INVALIDARG);

    // The non-versioned interface exposes the 1.0 form of the same blob.
    Com<ID3D12RootSignatureDeserializer> plain;
    CHECK_HR(D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  __uuidof(ID3D12RootSignatureDeserializer),
                                                  reinterpret_cast<void **>(plain.put())));
    check_desc10_eq(*plain->GetRootSignatureDesc(), sig.desc10);

    // The 1.0 serializer entry point converts a 1.1-targeted request down too.
    Com<ID3DBlob> blob10;
    CHECK_HR(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_0, blob10.put(), nullptr));
    CHECK(blob10->GetBufferSize() < blob->GetBufferSize());
}

void test_roundtrip_10()
{
    TestSignature sig;
    DefaultFlagSignature expected11;

    Com<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), error.put()));
    CHECK(!error.get());
    CHECK(std::memcmp(blob->GetBufferPointer(), "DXBC", 4) == 0);

    Com<ID3D12VersionedRootSignatureDeserializer> des;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           __uuidof(ID3D12VersionedRootSignatureDeserializer),
                                                           reinterpret_cast<void **>(des.put())));
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d = des->GetUnconvertedRootSignatureDesc();
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_0);
    check_desc10_eq(d->Desc_1_0, sig.desc10);

    // 1.0 -> 1.1 adds the volatile defaults.
    CHECK_HR(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &d));
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
    check_desc11_eq(d->Desc_1_1, expected11.desc);
    check_default_flags(d->Desc_1_1);

    CHECK_HR(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &d));
    check_desc10_eq(d->Desc_1_0, sig.desc10);

    // Asking the 1.0 serializer for a 1.1 blob converts the description up.
    Com<ID3DBlob> blob11;
    CHECK_HR(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_1, blob11.put(), nullptr));
    Com<ID3D12VersionedRootSignatureDeserializer> des11;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(blob11->GetBufferPointer(), blob11->GetBufferSize(),
                                                           __uuidof(ID3D12VersionedRootSignatureDeserializer),
                                                           reinterpret_cast<void **>(des11.put())));
    d = des11->GetUnconvertedRootSignatureDesc();
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
    check_desc11_eq(d->Desc_1_1, expected11.desc);
}

void test_com_semantics()
{
    TestSignature sig;
    Com<ID3DBlob> blob;
    CHECK_HR(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr));

    Com<ID3D12VersionedRootSignatureDeserializer> des;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           __uuidof(ID3D12VersionedRootSignatureDeserializer),
                                                           reinterpret_cast<void **>(des.put())));
    Com<IUnknown> unk;
    CHECK_HR(des->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(unk.put())));
    CHECK(unk.get() == static_cast<IUnknown *>(des.get()));
    CHECK(des->AddRef() == 3);
    CHECK(des->Release() == 2);
    unk.reset();

    void *other = &sig;
    CHECK(des->QueryInterface(__uuidof(ID3D12RootSignatureDeserializer), &other) == E_NOINTERFACE);
    CHECK(other == nullptr);

    // Only the two deserializer interfaces may be requested.
    void *wrong = &sig;
    CHECK(D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                               __uuidof(ID3D12Device), &wrong) == E_NOINTERFACE);
    CHECK(wrong == nullptr);
    CHECK(FAILED(D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      __uuidof(ID3D12RootSignatureDeserializer), nullptr)));

    // The blob is a real COM object too.
    Com<IUnknown> blob_unk;
    CHECK_HR(blob->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(blob_unk.put())));
}

void test_malformed()
{
    TestSignature sig;
    Com<ID3DBlob> blob;
    CHECK_HR(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_1, blob.put(), nullptr));
    const std::vector<uint8_t> good = bytes_of(blob.get());

    auto parses = [](const std::vector<uint8_t> &b) {
        Com<ID3D12VersionedRootSignatureDeserializer> des;
        return D3D12CreateVersionedRootSignatureDeserializer(
            b.data(), b.size(), __uuidof(ID3D12VersionedRootSignatureDeserializer),
            reinterpret_cast<void **>(des.put()));
    };
    CHECK_HR(parses(good));

    // Every truncation fails.
    for (size_t len = 0; len < good.size(); len++)
        CHECK(FAILED(parses(std::vector<uint8_t>(good.begin(), good.begin() + len))));

    // Any single flipped byte is caught: by the checksum, or by the structure.
    for (size_t i = 0; i < good.size(); i++) {
        std::vector<uint8_t> bad = good;
        bad[i] ^= 0x5a;
        CHECK(FAILED(parses(bad)));
    }

    // A bare RTS0 payload is accepted, and its offsets are bounds-checked.
    constexpr size_t kPayload = 44; // 32-byte header, one part offset, 8-byte part header
    const std::vector<uint8_t> rts0(good.begin() + kPayload, good.end());
    CHECK_HR(parses(rts0));
    for (size_t field : {4, 8, 12, 16}) { // numParams, paramsOffset, numSamplers, samplersOffset
        std::vector<uint8_t> bad = rts0;
        const uint32_t huge = 0x7ffffff0;
        std::memcpy(&bad[field], &huge, 4);
        CHECK(FAILED(parses(bad)));
    }
    std::vector<uint8_t> bad_version = rts0;
    bad_version[0] = 9;
    CHECK(FAILED(parses(bad_version)));

    // Bad parameter type / payload offset inside the first parameter entry.
    std::vector<uint8_t> bad_type = rts0;
    bad_type[24] = 9;
    CHECK(FAILED(parses(bad_type)));
    std::vector<uint8_t> bad_payload = rts0;
    const uint32_t far = uint32_t(rts0.size());
    std::memcpy(&bad_payload[24 + 8], &far, 4);
    CHECK(FAILED(parses(bad_payload)));

    CHECK(FAILED(D3D12CreateRootSignatureDeserializer(nullptr, 0, __uuidof(ID3D12RootSignatureDeserializer),
                                                      reinterpret_cast<void **>(&bad_payload))));
}

void test_serialize_errors()
{
    TestSignature sig;
    Com<ID3DBlob> blob, error;

    CHECK(D3D12SerializeRootSignature(nullptr, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr) == E_INVALIDARG);
    CHECK(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_0, nullptr, nullptr) ==
          E_INVALIDARG);
    CHECK(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION(0), blob.put(), error.put()) ==
          E_INVALIDARG);
    CHECK(!blob.get());
    CHECK(error.get() && error->GetBufferSize() > 0);
    CHECK(D3D12SerializeRootSignature(&sig.desc10, D3D_ROOT_SIGNATURE_VERSION_1_2, blob.put(), nullptr) ==
          E_INVALIDARG);

    D3D12_ROOT_SIGNATURE_DESC bad = sig.desc10;
    D3D12_ROOT_PARAMETER bad_params[4];
    std::memcpy(bad_params, sig.params10, sizeof(bad_params));
    bad.pParameters = bad_params;

    bad_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE(9);
    CHECK(D3D12SerializeRootSignature(&bad, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), error.put()) ==
          E_INVALIDARG);
    CHECK(!blob.get() && error.get());
    bad_params[0] = sig.params10[0];

    bad_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY(42);
    CHECK(D3D12SerializeRootSignature(&bad, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr) == E_INVALIDARG);
    bad_params[0] = sig.params10[0];

    bad_params[2].DescriptorTable.pDescriptorRanges = nullptr;
    CHECK(D3D12SerializeRootSignature(&bad, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr) == E_INVALIDARG);
    bad_params[2] = sig.params10[2];

    D3D12_DESCRIPTOR_RANGE bad_range = sig.ranges10[0];
    bad_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE(7);
    bad_params[2].DescriptorTable.pDescriptorRanges = &bad_range;
    bad_params[2].DescriptorTable.NumDescriptorRanges = 1;
    CHECK(D3D12SerializeRootSignature(&bad, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr) == E_INVALIDARG);

    bad = sig.desc10;
    bad.pParameters = nullptr;
    CHECK(D3D12SerializeRootSignature(&bad, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr) == E_INVALIDARG);
    bad = sig.desc10;
    bad.pStaticSamplers = nullptr;
    CHECK(D3D12SerializeRootSignature(&bad, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr) == E_INVALIDARG);

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION(5);
    CHECK(D3D12SerializeVersionedRootSignature(&versioned, blob.put(), error.put()) == E_INVALIDARG);
    CHECK(error.get() && error->GetBufferSize() > 0);
    CHECK(D3D12SerializeVersionedRootSignature(nullptr, blob.put(), nullptr) == E_INVALIDARG);

    // Empty signatures and tables without ranges are allowed.
    D3D12_ROOT_SIGNATURE_DESC empty{};
    CHECK_HR(D3D12SerializeRootSignature(&empty, D3D_ROOT_SIGNATURE_VERSION_1_1, blob.put(), nullptr));
    Com<ID3D12RootSignatureDeserializer> des;
    CHECK_HR(D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  __uuidof(ID3D12RootSignatureDeserializer),
                                                  reinterpret_cast<void **>(des.put())));
    CHECK(des->GetRootSignatureDesc()->NumParameters == 0);

    D3D12_ROOT_PARAMETER table{};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    D3D12_ROOT_SIGNATURE_DESC with_table{1, &table, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    CHECK_HR(D3D12SerializeRootSignature(&with_table, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.put(), nullptr));
}

// Accepts containers produced by the DirectX Shader Compiler and reproduces
// them byte for byte (checksum included).
void test_dxc_interop()
{
    Com<ID3D12VersionedRootSignatureDeserializer> des;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(kDxcRootSig10, sizeof(kDxcRootSig10),
                                                           __uuidof(ID3D12VersionedRootSignatureDeserializer),
                                                           reinterpret_cast<void **>(des.put())));
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d = des->GetUnconvertedRootSignatureDesc();
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_0);
    const D3D12_ROOT_SIGNATURE_DESC &r = d->Desc_1_0;
    CHECK(r.NumParameters == 5 && r.NumStaticSamplers == 1);
    CHECK(r.pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    CHECK(r.pParameters[0].Constants.ShaderRegister == 0 && r.pParameters[0].Constants.Num32BitValues == 4);
    CHECK(r.pParameters[1].Constants.ShaderRegister == 2 && r.pParameters[1].Constants.Num32BitValues == 2);
    CHECK(r.pParameters[2].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    CHECK(r.pParameters[2].DescriptorTable.NumDescriptorRanges == 2);
    const D3D12_DESCRIPTOR_RANGE *ranges = r.pParameters[2].DescriptorTable.pDescriptorRanges;
    CHECK(ranges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && ranges[0].BaseShaderRegister == 20);
    CHECK(ranges[0].OffsetInDescriptorsFromTableStart == kAppend);
    CHECK(ranges[1].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && ranges[1].NumDescriptors == 2);
    CHECK(r.pParameters[3].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV);
    CHECK(r.pParameters[3].Descriptor.ShaderRegister == 21);
    CHECK(r.pParameters[4].ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV);
    CHECK(r.pParameters[4].Descriptor.ShaderRegister == 5);
    CHECK(r.pStaticSamplers[0].ShaderRegister == 0);
    CHECK(r.pStaticSamplers[0].Filter == D3D12_FILTER_ANISOTROPIC);

    Com<ID3DBlob> again;
    CHECK_HR(D3D12SerializeVersionedRootSignature(d, again.put(), nullptr));
    CHECK(bytes_of(again.get()) == std::vector<uint8_t>(kDxcRootSig10, kDxcRootSig10 + sizeof(kDxcRootSig10)));

    // The 1.1 container.
    Com<ID3D12VersionedRootSignatureDeserializer> des11;
    CHECK_HR(D3D12CreateVersionedRootSignatureDeserializer(kDxcRootSig11, sizeof(kDxcRootSig11),
                                                           __uuidof(ID3D12VersionedRootSignatureDeserializer),
                                                           reinterpret_cast<void **>(des11.put())));
    d = des11->GetUnconvertedRootSignatureDesc();
    CHECK(d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
    const D3D12_ROOT_SIGNATURE_DESC1 &s = d->Desc_1_1;
    CHECK(s.Flags == D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    CHECK(s.NumParameters == 3 && s.NumStaticSamplers == 1);
    CHECK(s.pParameters[0].ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL);
    const D3D12_DESCRIPTOR_RANGE1 &uav = s.pParameters[0].DescriptorTable.pDescriptorRanges[0];
    CHECK(uav.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && uav.NumDescriptors == UINT(-1));
    CHECK(uav.Flags == D3D12_DESCRIPTOR_RANGE_FLAG_NONE && uav.OffsetInDescriptorsFromTableStart == kAppend);
    const D3D12_DESCRIPTOR_RANGE1 &smp = s.pParameters[1].DescriptorTable.pDescriptorRanges[0];
    CHECK(smp.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && smp.NumDescriptors == 4);
    CHECK(smp.BaseShaderRegister == 1 && smp.OffsetInDescriptorsFromTableStart == 10);
    CHECK(s.pParameters[2].ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV);
    CHECK(s.pParameters[2].Descriptor.ShaderRegister == 3 && s.pParameters[2].Descriptor.RegisterSpace == 2);
    CHECK(s.pParameters[2].ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX);
    const D3D12_STATIC_SAMPLER_DESC &ss = s.pStaticSamplers[0];
    CHECK(ss.Filter == D3D12_FILTER_ANISOTROPIC && ss.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    CHECK(ss.AddressV == D3D12_TEXTURE_ADDRESS_MODE_WRAP && ss.MipLODBias == 1.5f && ss.MaxAnisotropy == 8);
    CHECK(ss.ComparisonFunc == D3D12_COMPARISON_FUNC_LESS);
    CHECK(ss.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);
    CHECK(ss.MinLOD == 1.0f && ss.MaxLOD == 9.0f && ss.ShaderRegister == 2 && ss.RegisterSpace == 3);
    CHECK(ss.ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL);

    CHECK_HR(D3D12SerializeVersionedRootSignature(d, again.put(), nullptr));
    CHECK(bytes_of(again.get()) == std::vector<uint8_t>(kDxcRootSig11, kDxcRootSig11 + sizeof(kDxcRootSig11)));

    // A corrupted DXC container is rejected by the checksum.
    std::vector<uint8_t> bad(kDxcRootSig11, kDxcRootSig11 + sizeof(kDxcRootSig11));
    bad[bad.size() - 1] ^= 1;
    Com<ID3D12VersionedRootSignatureDeserializer> none;
    CHECK(FAILED(D3D12CreateVersionedRootSignatureDeserializer(
        bad.data(), bad.size(), __uuidof(ID3D12VersionedRootSignatureDeserializer),
        reinterpret_cast<void **>(none.put()))));
}

} // namespace

int main()
{
    test_roundtrip_11();
    test_roundtrip_10();
    test_com_semantics();
    test_malformed();
    test_serialize_errors();
    test_dxc_interop();
    std::puts("test_rootsig: ok");
    return 0;
}
