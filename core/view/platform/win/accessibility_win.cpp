// Windows UI Automation accessibility provider.
//
// Phase 1 (earlier): HWND + root + UiaClientsAreListening probe.
//
// Phase 2 (this slice, #247):
// - IRawElementProviderSimple on the root (host provider) so UIA clients
//   can discover the process. WM_GETOBJECT handler returns this via
//   UiaReturnRawElementProvider.
// - Event-raising helpers for value / focus / structure / name changes.
//   These short-circuit when no assistive tech is attached
//   (UiaClientsAreListening() == FALSE) — the cheap path matters because
//   widgets call them on every parameter nudge.
//
// Phase 3 (follow-up): IRawElementProviderFragment on per-widget
// providers so the tree is fully walkable — currently the host provider
// alone advertises the process to screen readers, but widget-level
// providers are the next layer.

#ifdef _WIN32

#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/platform/win32_sane.hpp>  // brings in ObjBase.h + oleidl.h
#include <UIAutomation.h>
#include <atomic>

namespace pulp::view {

// Map Pulp AccessRole to Windows UIA Control Type IDs.
// Full list: https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-controltype-ids
static long access_role_to_uia_type(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return 50015;  // UIA_SliderControlTypeId
        case View::AccessRole::toggle: return 50000;  // UIA_ButtonControlTypeId
        case View::AccessRole::label:  return 50020;  // UIA_TextControlTypeId
        case View::AccessRole::group:  return 50026;  // UIA_GroupControlTypeId
        case View::AccessRole::meter:  return 50033;  // UIA_CustomControlTypeId
        case View::AccessRole::image:  return 50006;  // UIA_ImageControlTypeId
        default: return 50033;                         // UIA_CustomControlTypeId
    }
}

// ── Minimal IRawElementProviderSimple on the root HWND ────────────────────
//
// Implements the bare minimum surface a UIA client needs to discover the
// process: ProviderOptions, HostRawElementProvider (chain to the
// standard HWND provider for default properties), and a fallback
// GetPropertyValue that reports the root View's label + role. Per-widget
// providers land in Phase 3.

struct UiaSession;

class PulpHostProvider : public IRawElementProviderSimple {
public:
    explicit PulpHostProvider(UiaSession* session) : session_(session) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(refs_.fetch_add(1) + 1);
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        auto prev = refs_.fetch_sub(1);
        if (prev == 1) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(prev - 1);
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        // Server-side provider hosted by the process whose HWND owns it.
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID /*patternId*/,
                                       IUnknown** pRetVal) override {
        // Root host provider exposes no patterns directly — per-widget
        // providers will (Phase 3). Returning nullptr here is the UIA
        // contract when a pattern isn't supported.
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID propertyId,
                                     VARIANT* pRetVal) override;
    IFACEMETHODIMP get_HostRawElementProvider(
        IRawElementProviderSimple** pRetVal) override;

private:
    std::atomic<LONG> refs_{1};
    UiaSession* session_;
};

struct UiaSession {
    HWND hwnd = nullptr;
    View* root = nullptr;
    bool clients_listening = false;
    PulpHostProvider* host_provider = nullptr;  // refcounted; released in shutdown
};

// Small utilities. BSTR ownership follows COM rules (caller frees via
// SysFreeString). Returning BSTR through VARIANT hands ownership to UIA.

static BSTR make_bstr(const std::string& s) {
    // Convert UTF-8 to UTF-16 for COM. MultiByteToWideChar returns
    // length including terminator; SysAllocStringLen expects length
    // without terminator (matching the UTF-16 character count minus
    // the NUL it allocates itself).
    if (s.empty()) return SysAllocString(L"");
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()),
                                    nullptr, 0);
    if (wlen <= 0) return SysAllocString(L"");
    BSTR out = SysAllocStringLen(nullptr, static_cast<UINT>(wlen));
    if (!out) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out, wlen);
    return out;
}

// ── PulpHostProvider method bodies (defined after UiaSession complete) ───

IFACEMETHODIMP PulpHostProvider::GetPropertyValue(PROPERTYID propertyId,
                                                    VARIANT* pRetVal) {
    if (!pRetVal) return E_POINTER;
    VariantInit(pRetVal);
    if (!session_ || !session_->root) return S_OK;

    switch (propertyId) {
        case UIA_NamePropertyId: {
            const auto& label = session_->root->access_label();
            if (!label.empty()) {
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = make_bstr(label);
            }
            break;
        }
        case UIA_ControlTypePropertyId: {
            pRetVal->vt = VT_I4;
            pRetVal->lVal = access_role_to_uia_type(
                session_->root->access_role());
            break;
        }
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = VARIANT_TRUE;
            break;
        }
        case UIA_FrameworkIdPropertyId: {
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(L"Pulp");
            break;
        }
        default:
            // Leave pRetVal empty (VT_EMPTY) — UIA falls back to the
            // HostRawElementProvider for unhandled properties.
            break;
    }
    return S_OK;
}

IFACEMETHODIMP PulpHostProvider::get_HostRawElementProvider(
    IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    if (!session_ || !session_->hwnd) return S_OK;
    // Chain to the default HWND provider for geometry, focus, etc.
    return UiaHostProviderFromHwnd(session_->hwnd, pRetVal);
}

// ── Lifecycle ────────────────────────────────────────────────────────────

void* init_accessibility(View& root, void* hwnd) {
    auto* session = new UiaSession{};
    session->hwnd = static_cast<HWND>(hwnd);
    session->root = &root;
    session->clients_listening = UiaClientsAreListening() ? true : false;
    session->host_provider = new PulpHostProvider(session);
    runtime::log_info(
        "Windows UIA: session ready (clients_listening={})",
        session->clients_listening);
    return session;
}

void shutdown_accessibility(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    // Detach the provider from any cached UIA client references.
    if (session->hwnd) {
        UiaReturnRawElementProvider(session->hwnd, 0, 0, nullptr);
    }
    if (session->host_provider) {
        // #500 / #485: PulpHostProvider holds a raw UiaSession*, but
        // UIA clients cache provider pointers and can make method
        // calls on them asynchronously. If we simply Release() our ref
        // and delete session, a cached client could call any provider
        // method (e.g. get_HostRawElementProvider, GetPropertyValue)
        // and dereference the now-freed session_ → UAF.
        //
        // UiaDisconnectProvider is the officially-sanctioned shutdown
        // barrier: it waits for in-flight UIA calls to drain AND
        // rejects all subsequent calls (returning UIA_E_ELEMENTNOTAVAILABLE).
        // After it returns, no UIA-originated call can reach the
        // provider, so our session deletion below is safe.
        UiaDisconnectProvider(session->host_provider);
        session->host_provider->Release();
        session->host_provider = nullptr;
    }
    runtime::log_info("Windows UIA: session shutdown");
    delete session;
}

void accessibility_tree_changed(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session || !session->host_provider) return;
    if (!UiaClientsAreListening()) return;
    UiaRaiseStructureChangedEvent(
        session->host_provider,
        StructureChangeType_ChildrenBulkAdded, nullptr, 0);
}

// ── Phase 2: WM_GETOBJECT handler ────────────────────────────────────────
// The WindowHost's WndProc needs to forward WM_GETOBJECT to this. We
// expose a C entry point to keep the WndProc free of UIA headers.
extern "C" LRESULT pulp_uia_handle_wm_getobject(void* handle,
                                                 HWND hwnd,
                                                 WPARAM wParam,
                                                 LPARAM lParam) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session || !session->host_provider) return 0;
    // UIA_RootObjectId == UiaRootObjectId (negative magic from UIA)
    if (static_cast<long>(lParam) != UiaRootObjectId) return 0;
    return UiaReturnRawElementProvider(hwnd, wParam, lParam,
                                       session->host_provider);
}

// ── Phase 2 event-raising helpers ────────────────────────────────────────

void notify_accessibility_value_changed(void* handle, View& /*target*/) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session || !session->host_provider) return;
    if (!UiaClientsAreListening()) return;
    // Phase 3 will resolve the target View to its per-widget provider and
    // raise property-change on THAT provider. Until per-widget providers
    // exist, raise at root granularity so clients see at least the fact
    // that SOMETHING changed. Better than nothing.
    VARIANT old_v, new_v;
    VariantInit(&old_v);
    VariantInit(&new_v);
    UiaRaiseAutomationPropertyChangedEvent(
        session->host_provider, UIA_ValueValuePropertyId, old_v, new_v);
}

void notify_accessibility_focus_changed(void* handle, View& /*target*/) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session || !session->host_provider) return;
    if (!UiaClientsAreListening()) return;
    UiaRaiseAutomationEvent(session->host_provider,
                            UIA_AutomationFocusChangedEventId);
}

void notify_accessibility_name_changed(void* handle, View& /*target*/) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session || !session->host_provider) return;
    if (!UiaClientsAreListening()) return;
    VARIANT old_v, new_v;
    VariantInit(&old_v);
    VariantInit(&new_v);
    UiaRaiseAutomationPropertyChangedEvent(
        session->host_provider, UIA_NamePropertyId, old_v, new_v);
}

} // namespace pulp::view

#endif // _WIN32
