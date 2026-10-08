/* winrt_shim.dll: a stand-in for the Windows.Foundation.Metadata.ApiInformation runtime class. Wine's wintypes.dll
 * (11.x) implements every IApiInformationStatics method as a stub that returns E_NOTIMPL, and C++/WinRT turns that
 * into an uncaught exception: Spider-Man.exe asks about GraphicsCaptureSession.IsBorderRequired while it starts, throws
 * hresult_not_implemented and dies before it creates a window. This answers every query "not present", which sends
 * the caller down its fallback path as on a Windows without the API. tools/install-game.sh points the class at this
 * DLL in the prefix's registry (HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId). */
#include <windows.h>

typedef void *hstr_t;   /* HSTRING: never read here */

/* IApiInformationStatics {997439fe-f681-4a11-b416-c13a47e8ba36}; IActivationFactory {00000035-0000-0000-c000-000000000046} */
static const GUID iid_statics = {0x997439fe, 0xf681, 0x4a11, {0xb4, 0x16, 0xc1, 0x3a, 0x47, 0xe8, 0xba, 0x36}};
static const GUID iid_factory = {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID iid_inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
static const GUID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

typedef struct object {
    const void *const *vtbl;   /* the vtable of the interface this pointer is for */
} object;

/* Two static COM objects, one per interface; reference counts are only bookkeeping. */
static const void *const factory_vtbl[];
static const void *const statics_vtbl[];
static object factory_object = {factory_vtbl};
static object statics_object = {statics_vtbl};

static HRESULT WINAPI query_interface(void *iface, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &iid_unknown) || IsEqualGUID(riid, &iid_inspectable) || IsEqualGUID(riid, &iid_factory)) {
        *out = &factory_object;
    } else if (IsEqualGUID(riid, &iid_statics)) {
        *out = &statics_object;
    } else {
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

static ULONG WINAPI add_ref(void *iface) { return 2; }
static ULONG WINAPI release(void *iface) { return 1; }

static HRESULT WINAPI get_iids(void *iface, ULONG *count, GUID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI get_runtime_class_name(void *iface, hstr_t *name)
{
    *name = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI get_trust_level(void *iface, int *level)
{
    *level = 0;   /* BaseTrust */
    return S_OK;
}

static HRESULT WINAPI activate_instance(void *iface, void **instance)
{
    *instance = NULL;
    return E_NOTIMPL;   /* ApiInformation is a static class */
}

/* Every query answers FALSE; the signatures differ only in the arguments before the result pointer. */
static HRESULT WINAPI is_present_1(void *iface, hstr_t a, BOOLEAN *value) { *value = FALSE; return S_OK; }
static HRESULT WINAPI is_present_2(void *iface, hstr_t a, hstr_t b, BOOLEAN *value) { *value = FALSE; return S_OK; }
static HRESULT WINAPI is_present_arity(void *iface, hstr_t a, hstr_t b, UINT32 n, BOOLEAN *value) { *value = FALSE; return S_OK; }
static HRESULT WINAPI is_contract_major(void *iface, hstr_t a, UINT16 major, BOOLEAN *value) { *value = FALSE; return S_OK; }
static HRESULT WINAPI is_contract_minor(void *iface, hstr_t a, UINT16 major, UINT16 minor, BOOLEAN *value) { *value = FALSE; return S_OK; }

static const void *const factory_vtbl[] = {
    query_interface, add_ref, release, get_iids, get_runtime_class_name, get_trust_level, activate_instance,
};

static const void *const statics_vtbl[] = {
    query_interface, add_ref, release, get_iids, get_runtime_class_name, get_trust_level,
    is_present_1,       /* IsTypePresent */
    is_present_2,       /* IsMethodPresent */
    is_present_arity,   /* IsMethodPresentWithArity */
    is_present_2,       /* IsEventPresent */
    is_present_2,       /* IsPropertyPresent */
    is_present_2,       /* IsReadOnlyPropertyPresent */
    is_present_2,       /* IsWriteablePropertyPresent */
    is_present_2,       /* IsEnumNamedValuePresent */
    is_contract_major,  /* IsApiContractPresentByMajor */
    is_contract_minor,  /* IsApiContractPresentByMajorAndMinor */
};

HRESULT WINAPI DllGetActivationFactory(hstr_t class_id, void **factory)
{
    *factory = &factory_object;
    return S_OK;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    return TRUE;
}
