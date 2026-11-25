// Windows UI Automation accessibility provider.
//
// Root provider:
// - IRawElementProviderSimple on the root (host provider) so UIA clients
//   can discover the process. WM_GETOBJECT handler returns this via
//   UiaReturnRawElementProvider.
// - Event-raising helpers for value / focus / structure / name changes.
//   These short-circuit when no assistive tech is attached
//   (UiaClientsAreListening() == FALSE) — the cheap path matters because
//   widgets call them on every parameter nudge.
//
// Per-widget fragments: each accessible View is exposed as a
// PulpFragmentProvider implementing IRawElementProviderFragment
// (Navigate / GetRuntimeId / get_BoundingRectangle / get_FragmentRoot)
// and IRawElementProviderSimple (GetPatternProvider / GetPropertyValue).
// The root host provider becomes the IRawElementProviderFragmentRoot, so
// a screen reader can walk the entire widget tree, not just see the
// process. Range widgets (slider / meter) additionally expose
// IRangeValueProvider, and sliders expose IValueProvider, driven by the
// View's AccessibilityValueInterface.
//
// Threading & lifetime: fragments are built once at init (and rebuilt on
// structural change). UIA clients hold refcounts and can call provider methods
// asynchronously. Providers advertise UseComThreading so UI Automation
// marshals an STA provider back to its owning STA, where the View tree and its
// script callbacks live. Each provider also owns a call gate: retirement closes
// and drains that gate before borrowed View* / UiaSession* state is released;
// UiaDisconnectProvider additionally tells UIA that cached elements are gone.

#ifdef _WIN32

#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/platform/uia_mapping.hpp>
#include <pulp/view/view.hpp>
#include "accessibility_win_fragment_lifecycle.hpp"
#include "accessibility_win_internal.hpp"
#include "accessibility_win_providers.hpp"
#include "accessibility_win_util.hpp"
#include <pulp/runtime/log.hpp>
#include <pulp/platform/win32_sane.hpp>  // brings in ObjBase.h + oleidl.h
#include <UIAutomation.h>
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pulp::view {

// Delegate role mapping to the shared offline-tested table.
static long access_role_to_uia_type(View::AccessRole role) {
    return static_cast<long>(uia::role_to_control_type(role));
}

// ── Geometry: View → screen-space UiaRect ─────────────────────────────────
// Mirror accessibility_mac.mm's root-relative walk, then offset by the HWND's
// screen position. Windows UIA wants device-pixel screen coordinates.
static UiaRect view_to_screen_rect(UiaSession* session, View* view) {
    UiaRect r{0, 0, 0, 0};
    if (!session || !session->hwnd || !view) return r;

    float rx = 0, ry = 0;
    View* v = view;
    while (v) {
        rx += v->bounds().x;
        ry += v->bounds().y;
        v = v->parent();
    }
    auto b = view->bounds();

    POINT origin{0, 0};
    ClientToScreen(session->hwnd, &origin);  // client (0,0) → screen px
    r.left   = static_cast<double>(origin.x) + rx;
    r.top    = static_cast<double>(origin.y) + ry;
    r.width  = static_cast<double>(b.width);
    r.height = static_cast<double>(b.height);
    return r;
}

// ── PulpFragmentProvider method bodies ────────────────────────────────────

IFACEMETHODIMP PulpFragmentProvider::GetPropertyValue(PROPERTYID propertyId,
                                                       VARIANT* pRetVal) {
    if (!pRetVal) return E_POINTER;
    VariantInit(pRetVal);
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    View* v = call.view();
    if (!v) return S_OK;

    switch (propertyId) {
        case UIA_NamePropertyId: {
            const auto& label = v->access_label();
            if (!label.empty()) {
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = make_bstr(label);
            }
            break;
        }
        case UIA_ControlTypePropertyId: {
            pRetVal->vt = VT_I4;
            pRetVal->lVal = access_role_to_uia_type(v->access_role());
            break;
        }
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: {
            pRetVal->vt = VT_BOOL;
            // aria-hidden="true" demotes the element to off-screen / not
            // content, matching the mac isAccessibilityElement gate.
            pRetVal->boolVal =
                (v->access_hidden() == "true") ? VARIANT_FALSE : VARIANT_TRUE;
            break;
        }
        case UIA_IsEnabledPropertyId: {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal =
                (v->access_disabled() == "true") ? VARIANT_FALSE : VARIANT_TRUE;
            break;
        }
        case UIA_HasKeyboardFocusPropertyId: {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = v->has_focus() ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case UIA_IsKeyboardFocusablePropertyId: {
            pRetVal->vt = VT_BOOL;
            // Report the view's ACTUAL focusability. This used to proxy off
            // pattern availability (range-value or toggle), which reported
            // VARIANT_FALSE for every focusable button / combo box / text field
            // once those stopped masquerading as sliders and toggles.
            pRetVal->boolVal = v->focusable() ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case UIA_IsValuePatternAvailablePropertyId: {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = supports_value(v) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case UIA_IsRangeValuePatternAvailablePropertyId: {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal =
                supports_range_value(v) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case UIA_ValueValuePropertyId: {
            // The "current value" property surfaced by the Value pattern —
            // same shared resolver as IValueProvider::get_Value.
            const std::string value = accessibility_value_string(*v);
            if (!value.empty()) {
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = make_bstr(value);
            }
            break;
        }
        // NO UIA_ToggleToggleStatePropertyId. A toggle-state property with no
        // ITogglePattern behind it is unreachable: a client asks
        // IsTogglePatternAvailable (which is FALSE — patterns_for_role()
        // advertises no Toggle pattern because PulpFragmentProvider implements
        // no IToggleProvider) and never queries the property. Answering it
        // anyway was dead code that made the Windows lane LOOK like it
        // announced checkbox state; it does not. Narrator announces a Pulp
        // checkbox's role and name and NO state until IToggleProvider lands.
        // See docs/guides/modules/view.md ("what is and is not wired").
        case UIA_FrameworkIdPropertyId: {
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(L"Pulp");
            break;
        }
        default:
            break;  // VT_EMPTY — UIA falls back to the fragment root.
    }
    return S_OK;
}

IFACEMETHODIMP PulpFragmentProvider::Navigate(
    NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    UiaSession* session = call.session();
    auto fragments = session->fragment_lifecycle.read();
    if (!fragments) return S_OK;

    auto resolve = [&](int idx) -> IRawElementProviderFragment* {
        if (idx < 0 ||
            idx >= static_cast<int>(fragments.providers().size())) return nullptr;
        PulpFragmentProvider* f =
            fragments.providers()[static_cast<size_t>(idx)];
        if (!f) return nullptr;
        f->AddRef();
        return static_cast<IRawElementProviderFragment*>(f);
    };

    switch (direction) {
        case NavigateDirection_Parent: {
            if (node_.parent_index == -1) {
                // Parent is the fragment root (host provider).
                PulpHostProvider* hp =
                    session->host_provider.load(std::memory_order_acquire);
                if (hp) {
                    hp->AddRef();
                    *pRetVal = static_cast<IRawElementProviderFragment*>(hp);
                }
            } else {
                *pRetVal = resolve(node_.parent_index);
            }
            break;
        }
        case NavigateDirection_FirstChild:
            *pRetVal = resolve(node_.first_child);
            break;
        case NavigateDirection_LastChild:
            *pRetVal = resolve(node_.last_child);
            break;
        case NavigateDirection_NextSibling:
            *pRetVal = resolve(node_.next_sibling);
            break;
        case NavigateDirection_PreviousSibling:
            *pRetVal = resolve(node_.prev_sibling);
            break;
        default:
            break;
    }
    return S_OK;
}

IFACEMETHODIMP PulpFragmentProvider::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto rid = uia::runtime_id_for_index(node_.index);
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, uia::RuntimeId::count);
    if (!sa) return E_OUTOFMEMORY;
    for (LONG i = 0; i < uia::RuntimeId::count; ++i) {
        int32_t val = rid.ids[static_cast<size_t>(i)];
        HRESULT hr = SafeArrayPutElement(sa, &i, &val);
        if (FAILED(hr)) {
            SafeArrayDestroy(sa);
            return hr;
        }
    }
    *pRetVal = sa;
    return S_OK;
}

IFACEMETHODIMP PulpFragmentProvider::get_BoundingRectangle(UiaRect* pRetVal) {
    if (!pRetVal) return E_POINTER;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    *pRetVal = view_to_screen_rect(call.session(), call.view());
    return S_OK;
}

IFACEMETHODIMP PulpFragmentProvider::get_FragmentRoot(
    IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    PulpHostProvider* hp =
        call.session()->host_provider.load(std::memory_order_acquire);
    if (hp) {
        // Host provider implements IRawElementProviderFragmentRoot.
        return hp->QueryInterface(__uuidof(IRawElementProviderFragmentRoot),
                                   reinterpret_cast<void**>(pRetVal));
    }
    return S_OK;
}

// ── PulpHostProvider method bodies ────────────────────────────────────────

IFACEMETHODIMP PulpHostProvider::GetPropertyValue(PROPERTYID propertyId,
                                                    VARIANT* pRetVal) {
    if (!pRetVal) return E_POINTER;
    VariantInit(pRetVal);
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    UiaSession* session = call.session();
    if (!session->root) return S_OK;

    switch (propertyId) {
        case UIA_NamePropertyId: {
            const auto& label = session->root->access_label();
            if (!label.empty()) {
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = make_bstr(label);
            }
            break;
        }
        case UIA_ControlTypePropertyId: {
            pRetVal->vt = VT_I4;
            pRetVal->lVal = access_role_to_uia_type(
                session->root->access_role());
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
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!call.session()->hwnd) return S_OK;
    // Chain to the default HWND provider for geometry, focus, etc.
    return UiaHostProviderFromHwnd(call.session()->hwnd, pRetVal);
}

IFACEMETHODIMP PulpHostProvider::Navigate(
    NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    UiaSession* session = call.session();
    auto fragments = session->fragment_lifecycle.read();
    if (!fragments) return S_OK;

    auto resolve = [&](int idx) -> IRawElementProviderFragment* {
        if (idx < 0 ||
            idx >= static_cast<int>(fragments.providers().size())) return nullptr;
        PulpFragmentProvider* f =
            fragments.providers()[static_cast<size_t>(idx)];
        if (!f) return nullptr;
        f->AddRef();
        return static_cast<IRawElementProviderFragment*>(f);
    };

    switch (direction) {
        case NavigateDirection_FirstChild:
            *pRetVal = resolve(fragments.first_child());
            break;
        case NavigateDirection_LastChild:
            *pRetVal = resolve(fragments.last_child());
            break;
        case NavigateDirection_Parent:
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling:
            // The fragment root has no parent or siblings within this
            // provider — UIA links it to the HWND host above it.
            break;
        default:
            break;
    }
    return S_OK;
}

IFACEMETHODIMP PulpHostProvider::get_BoundingRectangle(UiaRect* pRetVal) {
    if (!pRetVal) return E_POINTER;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    // Fragment root bounds = the root View (the whole client area).
    *pRetVal = view_to_screen_rect(call.session(), call.session()->root);
    return S_OK;
}

IFACEMETHODIMP PulpHostProvider::ElementProviderFromPoint(
    double x, double y, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    UiaSession* session = call.session();
    auto fragments = session->fragment_lifecycle.read();
    if (!fragments) return S_OK;
    // Linear hit-test over fragments; deepest (last in DFS) match wins so
    // a child reports over its container. Fragment count is small (UI
    // widgets), so a scan is cheaper than maintaining a spatial index.
    PulpFragmentProvider* hit = nullptr;
    for (auto* f : fragments.providers()) {
        if (!f || !f->view()) continue;
        UiaRect r = view_to_screen_rect(session, f->view());
        if (x >= r.left && x < r.left + r.width &&
            y >= r.top && y < r.top + r.height) {
            hit = f;  // keep scanning; later (deeper) wins
        }
    }
    if (hit) {
        hit->AddRef();
        *pRetVal = static_cast<IRawElementProviderFragment*>(hit);
    }
    return S_OK;
}

IFACEMETHODIMP PulpHostProvider::GetFocus(
    IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    Call call(*this);
    if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
    UiaSession* session = call.session();
    auto fragments = session->fragment_lifecycle.read();
    if (!fragments) return S_OK;
    for (auto* f : fragments.providers()) {
        if (f && f->view() && f->view()->has_focus()) {
            f->AddRef();
            *pRetVal = static_cast<IRawElementProviderFragment*>(f);
            return S_OK;
        }
    }
    return S_OK;
}

namespace {

class HostProviderReference {
public:
    explicit HostProviderReference(PulpHostProvider* provider)
        : provider_(provider) {
        if (provider_) provider_->AddRef();
    }
    ~HostProviderReference() {
        if (provider_) provider_->Release();
    }
    HostProviderReference(const HostProviderReference&) = delete;
    HostProviderReference& operator=(const HostProviderReference&) = delete;
    PulpHostProvider* get() const noexcept { return provider_; }

private:
    PulpHostProvider* provider_ = nullptr;
};

PulpFragmentProvider* fragment_for(UiaSession& session, View& target) {
    auto fragments = session.fragment_lifecycle.read();
    if (!fragments) return nullptr;
    for (auto* fragment : fragments.providers()) {
        if (fragment && fragment->view() == &target) {
            fragment->AddRef();
            return fragment;
        }
    }
    return nullptr;
}

void raise_event(IRawElementProviderSimple& provider, View* live_view,
                 UiaAccessibilityEvent event) {
    switch (event) {
        case UiaAccessibilityEvent::value_changed: {
            VARIANT old_value, new_value;
            VariantInit(&old_value);
            VariantInit(&new_value);
            if (live_view) {
                const std::string value =
                    accessibility_value_string(*live_view);
                if (!value.empty()) {
                    new_value.vt = VT_BSTR;
                    new_value.bstrVal = make_bstr(value);
                }
            }
            UiaRaiseAutomationPropertyChangedEvent(
                &provider, UIA_ValueValuePropertyId, old_value, new_value);
            VariantClear(&new_value);
            return;
        }
        case UiaAccessibilityEvent::focus_changed:
            UiaRaiseAutomationEvent(
                &provider, UIA_AutomationFocusChangedEventId);
            return;
        case UiaAccessibilityEvent::name_changed: {
            VARIANT old_value, new_value;
            VariantInit(&old_value);
            VariantInit(&new_value);
            UiaRaiseAutomationPropertyChangedEvent(
                &provider, UIA_NamePropertyId, old_value, new_value);
            return;
        }
    }
}

}  // namespace

void raise_uia_accessibility_event(void* handle, View& target,
                                   UiaAccessibilityEvent event) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    UiaSessionOperation operation(session);
    if (!operation) return;
    HostProviderReference host_ref(
        session->host_provider.load(std::memory_order_acquire));
    auto* host = host_ref.get();
    if (!host || !UiaClientsAreListening()) return;

    if (auto* fragment = fragment_for(*session, target)) {
        bool delivered = false;
        {
            PulpFragmentProvider::Call call(*fragment);
            if (call) {
                raise_event(*fragment, call.view(), event);
                delivered = true;
            }
        }
        fragment->Release();
        if (delivered) return;
    }

    PulpHostProvider::Call host_call(*host);
    if (host_call) raise_event(*host, nullptr, event);
}

// ── Lifecycle ────────────────────────────────────────────────────────────

void* init_accessibility(View& root, void* hwnd) {
    auto* session = new UiaSession{};
    session->hwnd = static_cast<HWND>(hwnd);
    session->root = &root;
    session->clients_listening = UiaClientsAreListening() ? true : false;
    // Build the fragment tree before publishing the host provider so a
    // concurrent WM_GETOBJECT reader that loads the host provider also
    // sees a complete fragment set when it navigates.
    session->fragment_lifecycle.rebuild(session, session->root);
    // Release store so a concurrent WM_GETOBJECT reader sees a fully
    // constructed PulpHostProvider through its acquire load.
    session->host_provider.store(new PulpHostProvider(session),
                                 std::memory_order_release);
    detail::register_accessibility_provider(root, session);
    runtime::log_info(
        "Windows UIA: session ready (clients_listening={}, fragments={})",
        session->clients_listening,
        session->fragment_lifecycle.active_size());
    return session;
}

void shutdown_accessibility(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    if (session->shutdown_in_progress) return;
    session->shutdown_in_progress = true;

    if (!session->shutdown_requested) {
        session->shutdown_requested = true;

        // #514: Null the atomic FIRST, BEFORE we tell UIA there is no
        // provider for this HWND. See the host-provider race note below.
        session->retired_host_provider = session->host_provider.exchange(
            nullptr, std::memory_order_acq_rel);

        // Close fragment call gates immediately. An in-flight fragment moves
        // into the preallocated retired set, where ScriptedUiSession teardown
        // can attach the realm owner before this operation unwinds.
        session->fragment_lifecycle.release();

        if (session->hwnd)
            UiaReturnRawElementProvider(session->hwnd, 0, 0, nullptr);
    }

    // A disconnect above or in fragment lifecycle code may have re-entered
    // shutdown from an active provider/session operation. Publication is
    // already closed; defer deletion until the outermost lease unwinds.
    if (session->operation_depth != 0) {
        session->shutdown_in_progress = false;
        return;
    }

    // Keep the session discoverable while an active provider call is still
    // borrowing the View graph. ScriptedUiSession teardown can then attach its
    // retained realm owner before the outermost operation reaches this
    // depth-zero finalization point. No new UIA calls can begin because the
    // host publication and fragment gates are already closed.
    detail::unregister_accessibility_provider(handle);
    auto* hp = std::exchange(session->retired_host_provider, nullptr);

    // Disconnect + release the per-widget fragments before the host
    // provider. They borrow session_ and a raw View*; UiaDisconnectProvider
    // drains any in-flight client call and rejects new ones so the
    // session deletion below cannot UAF a cached fragment pointer.
    session->fragment_lifecycle.release();
    session->fragment_lifecycle.drain();
    // Teardown can itself be reentrant from a provider callback. Any provider
    // still active here was made inert and detached from the session by
    // retire(); drop our ownership without a self-blocking disconnect. UIA's
    // in-flight/client refs keep the COM object alive until those calls return.
    session->fragment_lifecycle.abandon_retired();

    if (hp) {
        // Close and drain the local call gate first. UiaDisconnectProvider is
        // still required to release UIA resources, but session lifetime does
        // not depend on that outbound COM call succeeding.
        hp->retire();
        UiaDisconnectProvider(hp);
        hp->Release();
    }
    runtime::log_info("Windows UIA: session shutdown");
    delete session;
}

void accessibility_tree_changed(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    UiaSessionOperation operation(session);
    if (!operation) return;
    auto* hp = session->host_provider.load(std::memory_order_acquire);
    if (!hp) return;
    // Rebuild the fragment set to reflect the new tree even when no
    // client is listening (so a client that attaches later walks the
    // current tree). Cheap relative to a structural UI change.
    session->fragment_lifecycle.rebuild(session, session->root);
    if (session->shutdown_requested) return;
    if (!UiaClientsAreListening()) return;
    UiaRaiseStructureChangedEvent(
        hp, StructureChangeType_ChildrenBulkAdded, nullptr, 0);
}

void accessibility_tree_will_change(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    UiaSessionOperation operation(session);
    if (!operation) return;
    session->fragment_lifecycle.release();
}

bool accessibility_tree_retirement_ready(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return true;
    UiaSessionOperation operation(session);
    if (!operation) return true;
    session->fragment_lifecycle.drain();
    return session->fragment_lifecycle.retired_size() == 0;
}

void accessibility_retain_until_retired(void* handle,
                                        std::shared_ptr<void> owner) noexcept {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session || !owner) return;
    // This hook is intentionally valid after shutdown_requested while an outer
    // provider operation is unwinding. The registry keeps the session alive
    // and serialized on its owner thread until depth-zero finalization.
    if (session->operation_depth != 0) {
        assert(!session->retained_owner || session->retained_owner == owner);
        session->retained_owner = owner;
    }
    session->fragment_lifecycle.retain_retired(std::move(owner));
}

void accessibility_pump(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    UiaSessionOperation operation(session);
    if (!operation) return;
    session->fragment_lifecycle.drain();
}

// ── WM_GETOBJECT handler ──────────────────────────────────────────────────
// The WindowHost's WndProc forwards WM_GETOBJECT here. We expose a C
// entry point to keep the WndProc free of UIA headers.
extern "C" LRESULT pulp_uia_handle_wm_getobject(void* handle,
                                                 HWND hwnd,
                                                 WPARAM wParam,
                                                 LPARAM lParam) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return 0;
    // Acquire-load to pair with shutdown's exchange-release: once
    // shutdown has nulled host_provider we must NOT republish a stale
    // pointer to UIA, or UIA will AddRef a provider we are about to
    // Release → UAF (#514).
    auto* hp = session->host_provider.load(std::memory_order_acquire);
    if (!hp) return 0;
    if (static_cast<long>(lParam) != UiaRootObjectId) return 0;
    return UiaReturnRawElementProvider(hwnd, wParam, lParam, hp);
}


} // namespace pulp::view

#endif // _WIN32
