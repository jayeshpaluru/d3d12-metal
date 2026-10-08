#include "d3d12/root_signature_blob.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace d3d12m {

namespace {

static_assert(std::endian::native == std::endian::little, "blob I/O assumes a little-endian host");
// Static samplers are 13 consecutive 32-bit fields on the wire, the same
// layout as D3D12_STATIC_SAMPLER_DESC, so they are copied as a whole.
static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC) == 52);

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
           uint32_t(uint8_t(d)) << 24;
}

constexpr uint32_t kDxbc = fourcc('D', 'X', 'B', 'C');
constexpr uint32_t kRts0 = fourcc('R', 'T', 'S', '0');

// DXBC container: fourcc, 16-byte hash, version, total size, part count, then
// the part offset table. Each part is a fourcc, a size and the payload.
constexpr size_t kContainerHeaderSize = 32;
constexpr size_t kHashOffset = 4;
constexpr size_t kHashSize = 16;
constexpr size_t kHashedOffset = 20; // The hash covers everything from here on.
constexpr size_t kPartHeaderSize = 8;

// RTS0 payload: version, numParameters, offsetToParameters, numStaticSamplers,
// offsetToStaticSamplers, flags.
constexpr size_t kRts0HeaderSize = 24;
constexpr size_t kParameterEntrySize = 12; // type, visibility, payload offset
constexpr size_t kSamplerSize = sizeof(D3D12_STATIC_SAMPLER_DESC);

// Bounds the memory a malformed blob can make the parser allocate.
constexpr size_t kMaxParsedRanges = 1 << 16;

// ---------------------------------------------------------------------------
// DXBC checksum: MD5 with a non-standard final padding.

uint32_t md5_constant(int i)
{
    static const std::array<uint32_t, 64> table = [] {
        std::array<uint32_t, 64> t{};
        for (int k = 0; k < 64; k++)
            t[k] = uint32_t(std::fabs(std::sin(k + 1.0)) * 4294967296.0);
        return t;
    }();
    return table[i];
}

void md5_transform(uint32_t state[4], const uint32_t m[16])
{
    static constexpr int kShift[4][4] = {
        {7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        switch (i / 16) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d); g = (7 * i) % 16; break;
        }
        f += a + md5_constant(i) + m[g];
        a = d;
        d = c;
        c = b;
        b += std::rotl(f, kShift[i / 16][i % 4]);
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// Computes the container hash of the `size` bytes at `data`.
std::array<uint8_t, kHashSize> dxbc_hash(const uint8_t *data, size_t size)
{
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint32_t block[16];

    size_t pos = 0;
    for (; size - pos >= 64; pos += 64) {
        std::memcpy(block, data + pos, 64);
        md5_transform(state, block);
    }

    // The tail carries the bit count in its first word and a tag in its last.
    const size_t rest = size - pos;
    const uint32_t bits = uint32_t(size) * 8;
    const uint32_t tag = uint32_t(size) * 2 | 1;
    std::memset(block, 0, sizeof(block));
    if (rest >= 56) {
        std::memcpy(block, data + pos, rest);
        reinterpret_cast<uint8_t *>(block)[rest] = 0x80;
        md5_transform(state, block);
        std::memset(block, 0, sizeof(block));
        block[0] = bits;
    } else {
        block[0] = bits;
        auto *bytes = reinterpret_cast<uint8_t *>(block) + 4;
        std::memcpy(bytes, data + pos, rest);
        bytes[rest] = 0x80;
    }
    block[15] = tag;
    md5_transform(state, block);

    std::array<uint8_t, kHashSize> digest;
    std::memcpy(digest.data(), state, kHashSize);
    return digest;
}

// ---------------------------------------------------------------------------
// Shared helpers.

HRESULT fail(std::string *error, const char *message)
{
    if (error)
        *error = message;
    return E_INVALIDARG;
}

// Reads an enum-typed field of application data as its raw value. Applications
// may pass out-of-range values, and loading those as enums is undefined.
template <typename T>
uint32_t raw(const T &field)
{
    static_assert(sizeof(T) == sizeof(uint32_t));
    uint32_t value;
    std::memcpy(&value, &field, sizeof(value));
    return value;
}

bool valid_parameter_type(uint32_t type) { return type <= D3D12_ROOT_PARAMETER_TYPE_UAV; }
bool valid_visibility(uint32_t visibility) { return visibility <= D3D12_SHADER_VISIBILITY_MESH; }
bool valid_range_type(uint32_t type) { return type <= D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; }

// Flags a 1.0 range has when seen as a 1.1 range.
D3D12_DESCRIPTOR_RANGE_FLAGS default_range_flags(uint32_t range_type)
{
    if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
        return D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    return D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
}

D3D12_DESCRIPTOR_RANGE1 to_range1(const D3D12_DESCRIPTOR_RANGE &r)
{
    return {r.RangeType, r.NumDescriptors, r.BaseShaderRegister, r.RegisterSpace,
            default_range_flags(r.RangeType), r.OffsetInDescriptorsFromTableStart};
}

D3D12_DESCRIPTOR_RANGE1 to_range1(const D3D12_DESCRIPTOR_RANGE1 &r) { return r; }

// The converted parameter's table pointer stays null; finalize() links it.
D3D12_ROOT_PARAMETER1 to_param1(const D3D12_ROOT_PARAMETER &p)
{
    D3D12_ROOT_PARAMETER1 q{};
    q.ParameterType = p.ParameterType;
    q.ShaderVisibility = p.ShaderVisibility;
    switch (p.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        q.DescriptorTable.NumDescriptorRanges = p.DescriptorTable.NumDescriptorRanges;
        break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        q.Constants = p.Constants;
        break;
    default:
        q.Descriptor = {p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace,
                        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        break;
    }
    return q;
}

D3D12_ROOT_PARAMETER1 to_param1(const D3D12_ROOT_PARAMETER1 &p)
{
    D3D12_ROOT_PARAMETER1 q = p;
    if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        q.DescriptorTable.pDescriptorRanges = nullptr;
    return q;
}

// Builds the 1.0 view, links every table to its ranges and fills in the
// descriptions. params11, ranges11 (in parameter order) and samplers must be
// complete; `version` is the version the signature was stored in.
void finalize(ParsedRootSignature &rs, D3D_ROOT_SIGNATURE_VERSION version,
              D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    rs.params10.resize(rs.params11.size());
    rs.ranges10.resize(rs.ranges11.size());

    size_t next_range = 0;
    for (size_t i = 0; i < rs.params11.size(); i++) {
        D3D12_ROOT_PARAMETER1 &p = rs.params11[i];
        D3D12_ROOT_PARAMETER &q = rs.params10[i];
        q.ParameterType = p.ParameterType;
        q.ShaderVisibility = p.ShaderVisibility;
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            const UINT n = p.DescriptorTable.NumDescriptorRanges;
            for (size_t j = next_range; j < next_range + n; j++) {
                const D3D12_DESCRIPTOR_RANGE1 &r = rs.ranges11[j];
                rs.ranges10[j] = {r.RangeType, r.NumDescriptors, r.BaseShaderRegister, r.RegisterSpace,
                                  r.OffsetInDescriptorsFromTableStart};
            }
            p.DescriptorTable.pDescriptorRanges = n ? &rs.ranges11[next_range] : nullptr;
            q.DescriptorTable = {n, n ? &rs.ranges10[next_range] : nullptr};
            next_range += n;
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            q.Constants = p.Constants;
            break;
        default:
            q.Descriptor = {p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace};
            break;
        }
    }

    const UINT num_params = UINT(rs.params11.size());
    const UINT num_samplers = UINT(rs.samplers.size());
    const D3D12_STATIC_SAMPLER_DESC *samplers = rs.samplers.empty() ? nullptr : rs.samplers.data();
    rs.desc10 = {num_params, rs.params10.empty() ? nullptr : rs.params10.data(), num_samplers, samplers,
                 flags};
    rs.desc11 = {num_params, rs.params11.empty() ? nullptr : rs.params11.data(), num_samplers, samplers,
                 flags};

    rs.original.Version = version;
    if (version == D3D_ROOT_SIGNATURE_VERSION_1_0)
        rs.original.Desc_1_0 = rs.desc10;
    else
        rs.original.Desc_1_1 = rs.desc11;
}

// Copies and validates a user description (1.0 or 1.1) into `rs`.
template <typename Desc>
HRESULT adopt(const Desc &desc, D3D_ROOT_SIGNATURE_VERSION version, ParsedRootSignature &rs,
              std::string *error)
{
    if (desc.NumParameters && !desc.pParameters)
        return fail(error, "NumParameters is non-zero but pParameters is null");
    if (desc.NumStaticSamplers && !desc.pStaticSamplers)
        return fail(error, "NumStaticSamplers is non-zero but pStaticSamplers is null");

    for (UINT i = 0; i < desc.NumParameters; i++) {
        const auto &p = desc.pParameters[i];
        if (!valid_parameter_type(raw(p.ParameterType)))
            return fail(error, "invalid root parameter type");
        if (!valid_visibility(raw(p.ShaderVisibility)))
            return fail(error, "invalid root parameter shader visibility");
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            const auto &table = p.DescriptorTable;
            if (table.NumDescriptorRanges && !table.pDescriptorRanges)
                return fail(error, "NumDescriptorRanges is non-zero but pDescriptorRanges is null");
            for (UINT j = 0; j < table.NumDescriptorRanges; j++) {
                if (!valid_range_type(raw(table.pDescriptorRanges[j].RangeType)))
                    return fail(error, "invalid descriptor range type");
                rs.ranges11.push_back(to_range1(table.pDescriptorRanges[j]));
            }
        }
        rs.params11.push_back(to_param1(p));
    }

    for (UINT i = 0; i < desc.NumStaticSamplers; i++) {
        if (!valid_visibility(raw(desc.pStaticSamplers[i].ShaderVisibility)))
            return fail(error, "invalid static sampler shader visibility");
        rs.samplers.push_back(desc.pStaticSamplers[i]);
    }

    finalize(rs, version, desc.Flags);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Serialization.

class Writer {
public:
    size_t size() const { return bytes_.size(); }
    std::vector<uint8_t> take() { return std::move(bytes_); }

    void u32(uint32_t v) { append(&v, 4); }
    void append(const void *data, size_t n)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        bytes_.insert(bytes_.end(), p, p + n);
    }
    void patch(size_t at, uint32_t v) { std::memcpy(&bytes_[at], &v, 4); }

private:
    std::vector<uint8_t> bytes_;
};

std::vector<uint8_t> build_rts0(const ParsedRootSignature &rs, bool v11)
{
    Writer w;
    w.u32(v11 ? D3D_ROOT_SIGNATURE_VERSION_1_1 : D3D_ROOT_SIGNATURE_VERSION_1_0);
    w.u32(uint32_t(rs.params11.size()));
    w.u32(kRts0HeaderSize);
    w.u32(uint32_t(rs.samplers.size()));
    const size_t sampler_offset_at = w.size();
    w.u32(0);
    w.u32(rs.desc11.Flags);

    // Parameter table, with payload offsets patched in as the payloads follow.
    const size_t table_at = w.size();
    for (const D3D12_ROOT_PARAMETER1 &p : rs.params11) {
        w.u32(p.ParameterType);
        w.u32(p.ShaderVisibility);
        w.u32(0);
    }

    for (size_t i = 0; i < rs.params11.size(); i++) {
        const D3D12_ROOT_PARAMETER1 &p = rs.params11[i];
        w.patch(table_at + i * kParameterEntrySize + 8, uint32_t(w.size()));
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            // The ranges directly follow the {count, offset} payload.
            const uint32_t ranges_at = uint32_t(w.size()) + 8;
            w.u32(p.DescriptorTable.NumDescriptorRanges);
            w.u32(ranges_at);
            for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; j++) {
                const D3D12_DESCRIPTOR_RANGE1 &r = p.DescriptorTable.pDescriptorRanges[j];
                w.u32(r.RangeType);
                w.u32(r.NumDescriptors);
                w.u32(r.BaseShaderRegister);
                w.u32(r.RegisterSpace);
                if (v11)
                    w.u32(r.Flags);
                w.u32(r.OffsetInDescriptorsFromTableStart);
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            w.u32(p.Constants.ShaderRegister);
            w.u32(p.Constants.RegisterSpace);
            w.u32(p.Constants.Num32BitValues);
            break;
        default:
            w.u32(p.Descriptor.ShaderRegister);
            w.u32(p.Descriptor.RegisterSpace);
            if (v11)
                w.u32(p.Descriptor.Flags);
            break;
        }
    }

    w.patch(sampler_offset_at, uint32_t(w.size()));
    for (const D3D12_STATIC_SAMPLER_DESC &s : rs.samplers)
        w.append(&s, kSamplerSize);
    return w.take();
}

std::vector<uint8_t> wrap_in_container(const std::vector<uint8_t> &rts0)
{
    const uint32_t part_offset = kContainerHeaderSize + 4;
    const uint32_t total = part_offset + kPartHeaderSize + uint32_t(rts0.size());

    Writer w;
    w.u32(kDxbc);
    const uint8_t no_hash[kHashSize] = {};
    w.append(no_hash, sizeof(no_hash));
    w.u32(1); // Container version.
    w.u32(total);
    w.u32(1); // Part count.
    w.u32(part_offset);
    w.u32(kRts0);
    w.u32(uint32_t(rts0.size()));
    w.append(rts0.data(), rts0.size());

    std::vector<uint8_t> out = w.take();
    const auto hash = dxbc_hash(out.data() + kHashedOffset, out.size() - kHashedOffset);
    std::memcpy(out.data() + kHashOffset, hash.data(), hash.size());
    return out;
}

// ---------------------------------------------------------------------------
// Parsing.

class Reader {
public:
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

    // True when `count` elements of `stride` bytes starting at `offset` lie
    // inside the buffer. Both factors come from 32-bit fields, so no overflow.
    bool fits(uint64_t offset, uint64_t count, uint64_t stride) const
    {
        return offset <= size_ && count * stride <= size_ - offset;
    }

    // Reads `count` 32-bit words at `offset`.
    bool read(uint64_t offset, void *dst, size_t count) const
    {
        if (!fits(offset, count, 4))
            return false;
        std::memcpy(dst, data_ + offset, count * 4);
        return true;
    }

private:
    const uint8_t *data_;
    size_t size_;
};

// Reads the payload of one root parameter and appends it to `rs`.
HRESULT parse_parameter(const Reader &r, bool v11, uint32_t type, uint32_t visibility,
                        uint32_t payload, ParsedRootSignature &rs)
{
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE(type);
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY(visibility);

    switch (type) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        uint32_t t[2];
        if (!r.read(payload, t, 2))
            return E_INVALIDARG;
        const uint32_t count = t[0], ranges_at = t[1];
        const size_t range_size = v11 ? 24 : 20;
        if (!r.fits(ranges_at, count, range_size) || count > kMaxParsedRanges - rs.ranges11.size())
            return E_INVALIDARG;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t w[6];
            if (!r.read(ranges_at + i * range_size, w, range_size / 4) || !valid_range_type(w[0]))
                return E_INVALIDARG;
            D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE(w[0]), w[1], w[2], w[3],
                                          default_range_flags(w[0]), w[4]};
            if (v11) {
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAGS(w[4]);
                range.OffsetInDescriptorsFromTableStart = w[5];
            }
            rs.ranges11.push_back(range);
        }
        p.DescriptorTable.NumDescriptorRanges = count;
        break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
        uint32_t w[3];
        if (!r.read(payload, w, 3))
            return E_INVALIDARG;
        p.Constants = {w[0], w[1], w[2]};
        break;
    }
    default: {
        uint32_t w[3];
        if (!r.read(payload, w, v11 ? 3 : 2))
            return E_INVALIDARG;
        p.Descriptor = {w[0], w[1],
                        v11 ? D3D12_ROOT_DESCRIPTOR_FLAGS(w[2]) : D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        break;
    }
    }
    rs.params11.push_back(p);
    return S_OK;
}

HRESULT parse_rts0(const uint8_t *data, size_t size, ParsedRootSignature &out)
{
    const Reader r(data, size);
    uint32_t h[6];
    if (!r.read(0, h, 6))
        return E_INVALIDARG;
    const uint32_t version = h[0], num_params = h[1], params_at = h[2];
    const uint32_t num_samplers = h[3], samplers_at = h[4], flags = h[5];
    if (version != D3D_ROOT_SIGNATURE_VERSION_1_0 && version != D3D_ROOT_SIGNATURE_VERSION_1_1)
        return E_INVALIDARG;
    if (!r.fits(params_at, num_params, kParameterEntrySize) ||
        !r.fits(samplers_at, num_samplers, kSamplerSize))
        return E_INVALIDARG;

    ParsedRootSignature rs;
    const bool v11 = version == D3D_ROOT_SIGNATURE_VERSION_1_1;
    for (uint32_t i = 0; i < num_params; i++) {
        uint32_t e[3];
        if (!r.read(params_at + uint64_t(i) * kParameterEntrySize, e, 3) || !valid_parameter_type(e[0]) ||
            !valid_visibility(e[1]))
            return E_INVALIDARG;
        if (HRESULT hr = parse_parameter(r, v11, e[0], e[1], e[2], rs); FAILED(hr))
            return hr;
    }

    rs.samplers.resize(num_samplers);
    for (uint32_t i = 0; i < num_samplers; i++) {
        D3D12_STATIC_SAMPLER_DESC &s = rs.samplers[i];
        if (!r.read(samplers_at + uint64_t(i) * kSamplerSize, &s, kSamplerSize / 4) ||
            !valid_visibility(s.ShaderVisibility))
            return E_INVALIDARG;
    }

    finalize(rs, D3D_ROOT_SIGNATURE_VERSION(version), D3D12_ROOT_SIGNATURE_FLAGS(flags));
    out = std::move(rs);
    return S_OK;
}

// Finds the RTS0 part of a DXBC container after verifying its structure and
// checksum.
HRESULT find_rts0_part(const uint8_t *data, size_t size, const uint8_t *&part, size_t &part_size)
{
    uint32_t tail[3]; // container version, total size, part count
    if (size < kContainerHeaderSize || !Reader(data, size).read(kHashedOffset, tail, 3))
        return E_INVALIDARG;
    const uint32_t total = tail[1], num_parts = tail[2];
    if (tail[0] != 1 || total > size || total < kContainerHeaderSize)
        return E_INVALIDARG;

    // An all-zero hash marks an unsigned container.
    const uint8_t *stored = data + kHashOffset;
    if (!std::all_of(stored, stored + kHashSize, [](uint8_t b) { return b == 0; })) {
        const auto computed = dxbc_hash(data + kHashedOffset, total - kHashedOffset);
        if (std::memcmp(stored, computed.data(), kHashSize) != 0)
            return E_INVALIDARG;
    }

    const Reader c(data, total);
    if (!c.fits(kContainerHeaderSize, num_parts, 4))
        return E_INVALIDARG;
    for (uint32_t i = 0; i < num_parts; i++) {
        uint32_t offset, header[2]; // part offset; fourcc, part size
        if (!c.read(kContainerHeaderSize + i * 4, &offset, 1) || !c.read(offset, header, 2) ||
            !c.fits(uint64_t(offset) + kPartHeaderSize, header[1], 1))
            return E_INVALIDARG;
        if (header[0] == kRts0) {
            part = data + offset + kPartHeaderSize;
            part_size = header[1];
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

} // namespace

HRESULT serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc,
                                 uint32_t target_version,
                                 std::vector<uint8_t> &out, std::string *error)
{
    if (target_version != D3D_ROOT_SIGNATURE_VERSION_1_0 && target_version != D3D_ROOT_SIGNATURE_VERSION_1_1)
        return fail(error, "unsupported target root signature version");

    try {
        ParsedRootSignature rs;
        HRESULT hr;
        switch (raw(desc.Version)) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0:
            hr = adopt(desc.Desc_1_0, desc.Version, rs, error);
            break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1:
            hr = adopt(desc.Desc_1_1, desc.Version, rs, error);
            break;
        default:
            return fail(error, "unsupported root signature version");
        }
        if (FAILED(hr))
            return hr;
        out = wrap_in_container(build_rts0(rs, target_version == D3D_ROOT_SIGNATURE_VERSION_1_1));
    } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT parse_root_signature(const void *blob, size_t size, ParsedRootSignature &out)
{
    if (!blob)
        return E_INVALIDARG;
    const auto *data = static_cast<const uint8_t *>(blob);
    try {
        uint32_t magic = 0;
        if (size >= 4)
            std::memcpy(&magic, data, 4);
        if (magic != kDxbc)
            return parse_rts0(data, size, out);

        const uint8_t *part;
        size_t part_size;
        if (HRESULT hr = find_rts0_part(data, size, part, part_size); FAILED(hr))
            return hr;
        return parse_rts0(part, part_size, out);
    } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
    }
}

} // namespace d3d12m
