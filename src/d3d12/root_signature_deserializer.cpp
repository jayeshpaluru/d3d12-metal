#include "d3d12/root_signature_deserializer.h"

#include <new>

#include "common/com.h"
#include "d3d12/root_signature_blob.h"

namespace d3d12m {

namespace {

class RootSignatureDeserializer final : public RefCounted<ID3D12RootSignatureDeserializer> {
public:
    explicit RootSignatureDeserializer(ParsedRootSignature &&rs) : rs_(std::move(rs)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12RootSignatureDeserializer>(this, riid, out);
    }

    const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE GetRootSignatureDesc() override
    {
        return &rs_.desc10;
    }

private:
    ParsedRootSignature rs_;
};

class VersionedRootSignatureDeserializer final
    : public RefCounted<ID3D12VersionedRootSignatureDeserializer> {
public:
    explicit VersionedRootSignatureDeserializer(ParsedRootSignature &&rs) : rs_(std::move(rs))
    {
        v10_.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        v10_.Desc_1_0 = rs_.desc10;
        v11_.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        v11_.Desc_1_1 = rs_.desc11;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12VersionedRootSignatureDeserializer>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE GetRootSignatureDescAtVersion(
        D3D_ROOT_SIGNATURE_VERSION version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        *desc = nullptr;
        switch (version) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0: *desc = &v10_; return S_OK;
        case D3D_ROOT_SIGNATURE_VERSION_1_1: *desc = &v11_; return S_OK;
        default: return E_INVALIDARG;
        }
    }

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE GetUnconvertedRootSignatureDesc() override
    {
        return &rs_.original;
    }

private:
    ParsedRootSignature rs_;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v10_{};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v11_{};
};

// Hands out the interface `riid` of a new object and drops the creation
// reference.
template <typename T>
HRESULT make_deserializer(ParsedRootSignature &&rs, REFIID riid, void **out)
{
    T *obj = new (std::nothrow) T(std::move(rs));
    if (!obj)
        return E_OUTOFMEMORY;
    HRESULT hr = obj->QueryInterface(riid, out);
    obj->Release();
    return hr;
}

} // namespace

HRESULT create_root_signature_deserializer(const void *data, size_t size, REFIID riid, void **out)
{
    if (!out)
        return E_INVALIDARG;
    *out = nullptr;

    ParsedRootSignature rs;
    if (HRESULT hr = parse_root_signature(data, size, rs); FAILED(hr))
        return hr;

    if (riid == __uuidof(ID3D12RootSignatureDeserializer))
        return make_deserializer<RootSignatureDeserializer>(std::move(rs), riid, out);
    if (riid == __uuidof(ID3D12VersionedRootSignatureDeserializer))
        return make_deserializer<VersionedRootSignatureDeserializer>(std::move(rs), riid, out);
    return E_NOINTERFACE;
}

} // namespace d3d12m
