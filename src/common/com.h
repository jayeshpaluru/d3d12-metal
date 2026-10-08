// Small helpers for implementing COM objects.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "common/d3d12_uuids.h"

namespace d3d12m {

// Internal keep-alive for objects the layer itself holds on to (a recorded command list names its pipelines and
// resources until it is reset). It does not change the COM reference count the application sees: when the
// application's last reference goes away the object only dies once the internal references are gone too.
class InternalRefCounted {
public:
    virtual void add_internal_ref() = 0;
    virtual void release_internal_ref() = 0;
    // Takes an internal reference unless the object is already being destroyed.
    virtual bool try_add_internal_ref() = 0;

protected:
    ~InternalRefCounted() = default;
};

// Implements IUnknown reference counting for the most-derived interface `I`. One atomic word holds both counts:
// the application's COM references in the low half, internal references in the high half; the object is deleted
// when the whole word reaches zero.
// The final class provides QueryInterface (see query_interfaces below).
template <typename I>
class RefCounted : public I, public InternalRefCounted {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(counts_.fetch_add(1) + 1); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const uint64_t after = counts_.fetch_sub(1) - 1;
        if (after == 0)
            delete this;
        return static_cast<ULONG>(after & 0xffffffffu);
    }

    void add_internal_ref() override { counts_.fetch_add(kInternal); }
    void release_internal_ref() override
    {
        if (counts_.fetch_sub(kInternal) - kInternal == 0)
            delete this;
    }
    bool try_add_internal_ref() override
    {
        uint64_t n = counts_.load();
        while (n != 0) {
            if (counts_.compare_exchange_weak(n, n + kInternal))
                return true;
        }
        return false;
    }

protected:
    virtual ~RefCounted() = default;

private:
    static constexpr uint64_t kInternal = uint64_t(1) << 32;
    std::atomic<uint64_t> counts_{1};
};

// Private identification of this layer's own objects. Application-supplied COM
// pointers may be wrappers (overlays, proxies) or MSVC objects, so they are
// never downcast with RTTI or a blind static_cast. Each final class answers
// QueryInterface for internal_iid<Class>() with its own `Class *`; a foreign
// object (or a wrapper that does not forward) answers E_NOINTERFACE.
template <typename T>
struct InternalTag {
    static inline const char tag = 0;
};

template <typename T>
GUID internal_iid()
{
    // The address of a per-class variable makes the IID unique per class and
    // impossible for a foreign object to answer by accident.
    GUID iid = {0x6d3d1200, 0x6d65, 0x7461, {0, 0, 0, 0, 0, 0, 0, 0}};
    const uintptr_t address = reinterpret_cast<uintptr_t>(&InternalTag<T>::tag);
    static_assert(sizeof(address) <= 8, "pointer fits the GUID tail");
    std::memcpy(iid.Data4, &address, sizeof(address));
    return iid;
}

// Answers QueryInterface for `self` with the first interface in Is whose IID
// matches `riid`. Every interface must be a base of Derived, so one object
// serves its whole inheritance chain (IUnknown ... most-derived interface).
template <typename... Is, typename Derived>
HRESULT query_interfaces(Derived *self, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    void *found = nullptr;
    if (riid == internal_iid<Derived>())
        found = self;
    else
    (void)((riid == __uuidof(Is) ? (found = static_cast<Is *>(self), true) : false) || ...);
    *out = found;
    if (!found)
        return E_NOINTERFACE;
    self->AddRef();
    return S_OK;
}

// Returns the object behind an application-supplied COM pointer as `T` if it
// is one of this layer's, else nullptr. The caller's own reference keeps it
// alive, so the reference taken by the query is dropped at once.
template <typename T, typename P>
T *ours(P *p)
{
    if (!p)
        return nullptr;
    void *object = nullptr;
    if (FAILED(p->QueryInterface(internal_iid<T>(), &object)))
        return nullptr;
    static_cast<IUnknown *>(static_cast<T *>(object))->Release();
    return static_cast<T *>(object);
}

// Hands a newly created object (holding its creation reference) to the caller
// as `riid`: queries the interface, then drops the creation reference. The
// object is destroyed when the query fails.
template <typename T>
HRESULT hand_out(T *object, REFIID riid, void **out)
{
    HRESULT hr = object->QueryInterface(riid, out);
    object->Release();
    return hr;
}

// Releases a COM pointer and clears it.
template <typename T>
void safe_release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace d3d12m
