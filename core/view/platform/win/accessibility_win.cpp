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
// structural change) and live as long as the session. UIA clients hold
// refcounts and can call provider methods asynchronously, so the
// shutdown path (see shutdown_accessibility) disconnects every provider
// via UiaDisconnectProvider before the session — which the fragments
// borrow raw View* / UiaSession* from — is destroyed.

#ifdef _WIN32

#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/platform/uia_mapping.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/platform/win32_sane.hpp>  // brings in ObjBase.h + oleidl.h
#include <UIAutomation.h>
#include <atomic>
#include <vector>

namespace pulp::view {

// Map Pulp AccessRole to Windows UIA Control Type IDs. Delegates to the
// shared, offline-tested mapping table (uia_mapping.hpp) so the COM TU
// and the cross-platform unit tests can never drift.
static long access_role_to_uia_type(View::AccessRole role) {
    return static_cast<long>(uia::role_to_control_type(role));
}

struct UiaSession;
class PulpHostProvider;
class PulpFragmentProvider;

// ── Small utilities ──────────────────────────────────────────────────────
// BSTR ownership follows COM rules (caller frees via SysFreeString).
// Returning BSTR through VARIANT hands ownership to UIA.

static BSTR make_bstr(const std::string& s) {
    // Convert UTF-8 to UTF-16 for COM. Passing an explicit byte count
    // makes MultiByteToWideChar return the UTF-16 length without a
    // terminator, which is exactly what SysAllocStringLen expects.
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

// ── Session ──────────────────────────────────────────────────────────────

struct UiaSession {
    HWND hwnd = nullptr;
    View* root = nullptr;
    bool clients_listening = false;

    // Atomic so shutdown can null it BEFORE we tell UIA there is no
    // provider for this HWND. The WM_GETOBJECT handler and every
    // event-raising helper load this with acquire ordering; shutdown
    // uses exchange(nullptr, acq_rel) as the publication barrier. See
    // #514 for the race this closes.
    std::atomic<PulpHostProvider*> host_provider{nullptr};  // refcounted; released in shutdown

    // Per-widget fragments, depth-first order (root excluded — the root
    // is the host provider / fragment root). Each entry is AddRef'd once
    // for the session's ownership and Release'd in shutdown. UIA clients
    // take their own refs as they walk. Built at init and rebuilt on
    // structural change. The ordering here defines each fragment's
    // runtime-id index and its parent/sibling navigation.
    std::vector<PulpFragmentProvider*> fragments;

    // Root-level (parent == fragment root) first/last child indices into
    // `fragments`, so PulpHostProvider::Navigate(FirstChild/LastChild) is
    // O(1). -1 when there are no top-level accessible fragments.
    int root_first_child = -1;
    int root_last_child = -1;
};

// A fragment's static place in the tree, captured when the fragment set
// is built. Navigation is pure index arithmetic over UiaSession::fragments
// so the hot Navigate path makes no View-tree walk.
struct FragmentNode {
    View* view = nullptr;
    int index = 0;          // depth-first index into session->fragments
    int parent_index = -1;  // -1 ⇒ parent is the fragment root (host)
    int first_child = -1;   // -1 ⇒ leaf
    int last_child = -1;
    int prev_sibling = -1;
    int next_sibling = -1;
};

// ── Per-widget fragment provider ──────────────────────────────────────────

class PulpFragmentProvider final : public IRawElementProviderSimple,
                                    public IRawElementProviderFragment,
                                    public IRangeValueProvider,
                                    public IValueProvider {
public:
    PulpFragmentProvider(UiaSession* session, FragmentNode node)
        : session_(session), node_(node) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragment)) {
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == __uuidof(IRangeValueProvider) &&
                   supports_range_value()) {
            *ppv = static_cast<IRangeValueProvider*>(this);
        } else if (riid == __uuidof(IValueProvider) && supports_value()) {
            *ppv = static_cast<IValueProvider*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
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
        *pRetVal = static_cast<ProviderOptions>(
            ProviderOptions_ServerSideProvider |
            ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID patternId,
                                       IUnknown** pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        if (patternId == UIA_RangeValuePatternId && supports_range_value()) {
            *pRetVal = static_cast<IRangeValueProvider*>(this);
            AddRef();
        } else if (patternId == UIA_ValuePatternId && supports_value()) {
            *pRetVal = static_cast<IValueProvider*>(this);
            AddRef();
        }
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID propertyId,
                                     VARIANT* pRetVal) override;
    IFACEMETHODIMP get_HostRawElementProvider(
        IRawElementProviderSimple** pRetVal) override {
        // Per UIA docs: only the fragment root returns a host provider;
        // child fragments return nullptr (they inherit the root's HWND
        // host). Returning S_OK + null is the documented contract.
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection direction,
                             IRawElementProviderFragment** pRetVal) override;
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** pRetVal) override;
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* pRetVal) override;
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;  // No nested fragment roots.
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        if (View* v = node_.view) v->set_focus(true);
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(
        IRawElementProviderFragmentRoot** pRetVal) override;

    // IRangeValueProvider
    IFACEMETHODIMP SetValue(double val) override {
        // Only claim the write when the pattern is actually advertised —
        // is_read_only() below must agree with what this returns.
        if (auto* vif = value_iface();
            vif && uia::role_supports_value(role_or_none())) {
            vif->set_current_value(val);
            return S_OK;
        }
        return UIA_E_NOTSUPPORTED;
    }
    IFACEMETHODIMP get_Value(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        if (auto* vif = value_iface()) *pRetVal = vif->get_current_value();
        return S_OK;
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        // IsReadOnly means exactly "SetValue will fail". uia::is_read_only()
        // is the shared predicate both SetValue overloads are written against;
        // the old proxy (supports_value()) reported "editable" for a
        // TextEditor whose SetValue then returned UIA_E_NOTSUPPORTED, and for
        // a Knob with no value interface at all.
        *pRetVal = uia::is_read_only(role_or_none(), value_iface() != nullptr,
                                     editable_text_iface() != nullptr)
                       ? VARIANT_TRUE
                       : VARIANT_FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_Maximum(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        if (auto* vif = value_iface()) *pRetVal = vif->get_maximum_value();
        return S_OK;
    }
    IFACEMETHODIMP get_Minimum(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        if (auto* vif = value_iface()) *pRetVal = vif->get_minimum_value();
        return S_OK;
    }
    IFACEMETHODIMP get_LargeChange(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        if (auto* vif = value_iface()) *pRetVal = vif->get_step_size() * 10.0;
        return S_OK;
    }
    IFACEMETHODIMP get_SmallChange(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        if (auto* vif = value_iface()) *pRetVal = vif->get_step_size();
        return S_OK;
    }

    // IValueProvider
    // (SetValue collides by name with IRangeValueProvider::SetValue but
    // has a BSTR signature; declare it explicitly to disambiguate.)
    IFACEMETHODIMP SetValue(LPCWSTR val) override {
        // Route the edit into the View's text interface (TextEditor). Without
        // this every Narrator edit was rejected while get_IsReadOnly claimed
        // the field was editable.
        auto* tif = editable_text_iface();
        if (!tif || !val) return UIA_E_NOTSUPPORTED;
        const int len = WideCharToMultiByte(CP_UTF8, 0, val, -1, nullptr, 0,
                                            nullptr, nullptr);
        if (len <= 0) return UIA_E_NOTSUPPORTED;
        std::string utf8(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, val, -1, utf8.data(), len, nullptr,
                            nullptr);
        tif->set_text(utf8);
        return S_OK;
    }
    IFACEMETHODIMP get_Value(BSTR* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        if (View* v = node_.view) {
            // Shared resolver: value interface → text interface → access_value.
            // Returning a NULL BSTR (what the text-interface-less path used to
            // do for every TextEditor) makes Narrator read nothing at all.
            const std::string value = accessibility_value_string(*v);
            if (!value.empty()) *pRetVal = make_bstr(value);
        }
        return S_OK;
    }
    // IValueProvider::get_IsReadOnly shares the same name/signature as
    // the IRangeValueProvider one above; a single override satisfies
    // both vtables.

    View* view() const { return node_.view; }

private:
    View::AccessRole role_or_none() const {
        return node_.view ? node_.view->access_role() : View::AccessRole::none;
    }
    // Pattern availability = role allows it AND the View can serve it. A
    // pattern advertised without a source resolves to a null interface (or,
    // worse, to zeros in a degenerate 0..0 range).
    bool supports_range_value() const {
        return uia::exposes_range_value(role_or_none(),
                                        value_iface() != nullptr);
    }
    bool supports_value() const {
        return uia::exposes_value(
            role_or_none(), value_iface() != nullptr,
            text_iface() != nullptr,
            node_.view && !node_.view->access_value().empty());
    }
    AccessibilityValueInterface* value_iface() const {
        return node_.view
            ? dynamic_cast<AccessibilityValueInterface*>(node_.view)
            : nullptr;
    }
    AccessibilityTextInterface* text_iface() const {
        return node_.view
            ? dynamic_cast<AccessibilityTextInterface*>(node_.view)
            : nullptr;
    }
    AccessibilityTextInterface* editable_text_iface() const {
        auto* tif = text_iface();
        return (tif && tif->is_editable()) ? tif : nullptr;
    }

    std::atomic<LONG> refs_{1};
    UiaSession* session_;
    FragmentNode node_;
};

// ── Root host provider (also the fragment root) ───────────────────────────

class PulpHostProvider final : public IRawElementProviderSimple,
                               public IRawElementProviderFragment,
                               public IRawElementProviderFragmentRoot {
public:
    explicit PulpHostProvider(UiaSession* session) : session_(session) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragment)) {
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragmentRoot)) {
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
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
        *pRetVal = static_cast<ProviderOptions>(
            ProviderOptions_ServerSideProvider |
            ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID /*patternId*/,
                                       IUnknown** pRetVal) override {
        // The fragment root exposes no patterns directly — patterns live
        // on the per-widget fragments. Returning nullptr is the UIA
        // contract when a pattern isn't supported.
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID propertyId,
                                     VARIANT* pRetVal) override;
    IFACEMETHODIMP get_HostRawElementProvider(
        IRawElementProviderSimple** pRetVal) override;

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection direction,
                             IRawElementProviderFragment** pRetVal) override;
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** pRetVal) override {
        // The fragment root has no runtime id of its own — UIA derives it
        // from the host HWND provider. Returning null is the contract.
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* pRetVal) override;
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override { return S_OK; }
    IFACEMETHODIMP get_FragmentRoot(
        IRawElementProviderFragmentRoot** pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = this;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderFragmentRoot
    IFACEMETHODIMP ElementProviderFromPoint(
        double x, double y, IRawElementProviderFragment** pRetVal) override;
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** pRetVal) override;

private:
    std::atomic<LONG> refs_{1};
    UiaSession* session_;
};

// ── Fragment-set construction ─────────────────────────────────────────────
//
// Mirror the mac collect_accessible() walk: depth-first, every View with
// a non-none AccessRole becomes a fragment. We capture parent/child/
// sibling indices up front so Navigate is pure arithmetic. The recursion
// threads the parent's fragment index so children link to it; views with
// AccessRole::none are skipped as fragments but still recursed into so a
// deeply-nested accessible descendant of a plain container is reachable
// (the descendant re-parents onto the nearest accessible ancestor, or
// onto the fragment root if there is none).

namespace {

void build_fragment_nodes(View& v, int parent_index,
                          std::vector<FragmentNode>& nodes) {
    int my_index = parent_index;
    if (is_accessibility_element(v)) {
        FragmentNode node;
        node.view = &v;
        node.index = static_cast<int>(nodes.size());
        node.parent_index = parent_index;
        nodes.push_back(node);
        my_index = node.index;
        // Sibling / child chains are derived in a second pass
        // (link_fragment_nodes); here we only record parentage.
    }
    for (size_t i = 0; i < v.child_count(); ++i) {
        if (View* child = v.child_at(i)) {
            build_fragment_nodes(*child, my_index, nodes);
        }
    }
}

// Second pass: derive first/last child + sibling chains from parentage.
void link_fragment_nodes(std::vector<FragmentNode>& nodes) {
    for (auto& n : nodes) {
        int prev = -1;
        for (auto& cand : nodes) {
            if (cand.parent_index != n.index) continue;
            if (n.first_child == -1) n.first_child = cand.index;
            n.last_child = cand.index;
            cand.prev_sibling = prev;
            if (prev != -1) nodes[static_cast<size_t>(prev)].next_sibling = cand.index;
            prev = cand.index;
        }
    }
}

// Root-level (parent == -1) first/last/sibling links, computed the same
// way the per-node pass does for children, so Navigate on the root and
// on root-level fragments agree.
struct RootLinks { int first = -1; int last = -1; };

RootLinks link_root_level(std::vector<FragmentNode>& nodes) {
    RootLinks rl;
    int prev = -1;
    for (auto& cand : nodes) {
        if (cand.parent_index != -1) continue;
        if (rl.first == -1) rl.first = cand.index;
        rl.last = cand.index;
        cand.prev_sibling = prev;
        if (prev != -1) nodes[static_cast<size_t>(prev)].next_sibling = cand.index;
        prev = cand.index;
    }
    return rl;
}

}  // namespace

// ── Geometry: View → screen-space UiaRect ─────────────────────────────────
//
// Mirror accessibility_mac.mm's root-relative walk, then offset by the
// HWND's screen position. Windows UIA wants device-pixel screen
// coordinates (top-left origin), which matches Pulp's top-down view
// space — no Y flip needed (unlike Cocoa).

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
    View* v = node_.view;
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
            pRetVal->boolVal = supports_value() ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case UIA_IsRangeValuePatternAvailablePropertyId: {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal =
                supports_range_value() ? VARIANT_TRUE : VARIANT_FALSE;
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
    if (!session_) return S_OK;

    auto resolve = [&](int idx) -> IRawElementProviderFragment* {
        if (idx < 0 ||
            idx >= static_cast<int>(session_->fragments.size())) return nullptr;
        PulpFragmentProvider* f = session_->fragments[static_cast<size_t>(idx)];
        if (!f) return nullptr;
        f->AddRef();
        return static_cast<IRawElementProviderFragment*>(f);
    };

    switch (direction) {
        case NavigateDirection_Parent: {
            if (node_.parent_index == -1) {
                // Parent is the fragment root (host provider).
                PulpHostProvider* hp =
                    session_->host_provider.load(std::memory_order_acquire);
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
    *pRetVal = view_to_screen_rect(session_, node_.view);
    return S_OK;
}

IFACEMETHODIMP PulpFragmentProvider::get_FragmentRoot(
    IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    if (!session_) return S_OK;
    PulpHostProvider* hp =
        session_->host_provider.load(std::memory_order_acquire);
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

IFACEMETHODIMP PulpHostProvider::Navigate(
    NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    if (!session_) return S_OK;

    auto resolve = [&](int idx) -> IRawElementProviderFragment* {
        if (idx < 0 ||
            idx >= static_cast<int>(session_->fragments.size())) return nullptr;
        PulpFragmentProvider* f = session_->fragments[static_cast<size_t>(idx)];
        if (!f) return nullptr;
        f->AddRef();
        return static_cast<IRawElementProviderFragment*>(f);
    };

    switch (direction) {
        case NavigateDirection_FirstChild:
            *pRetVal = resolve(session_->root_first_child);
            break;
        case NavigateDirection_LastChild:
            *pRetVal = resolve(session_->root_last_child);
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
    // Fragment root bounds = the root View (the whole client area).
    *pRetVal = view_to_screen_rect(session_,
                                   session_ ? session_->root : nullptr);
    return S_OK;
}

IFACEMETHODIMP PulpHostProvider::ElementProviderFromPoint(
    double x, double y, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    if (!session_) return S_OK;
    // Linear hit-test over fragments; deepest (last in DFS) match wins so
    // a child reports over its container. Fragment count is small (UI
    // widgets), so a scan is cheaper than maintaining a spatial index.
    PulpFragmentProvider* hit = nullptr;
    for (auto* f : session_->fragments) {
        if (!f || !f->view()) continue;
        UiaRect r = view_to_screen_rect(session_, f->view());
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
    if (!session_) return S_OK;
    for (auto* f : session_->fragments) {
        if (f && f->view() && f->view()->has_focus()) {
            f->AddRef();
            *pRetVal = static_cast<IRawElementProviderFragment*>(f);
            return S_OK;
        }
    }
    return S_OK;
}

// ── Fragment lifecycle helpers ────────────────────────────────────────────

namespace {

// Build (or rebuild) the session's fragment set from the current View
// tree. Releases any previously-held fragments first. Must run on the UI
// thread; UIA navigation is serialized through the COM apartment.
void rebuild_fragments(UiaSession* session) {
    if (!session) return;
    // Disconnect old fragments before dropping our ref so a cached UIA
    // client holding a stale fragment cannot call into it (and through it
    // into the about-to-be-replaced FragmentNode) after the swap. New
    // fragments replace them; UIA re-queries after the StructureChanged
    // event the caller raises.
    for (auto* f : session->fragments) {
        if (f) {
            UiaDisconnectProvider(f);
            f->Release();
        }
    }
    session->fragments.clear();
    session->root_first_child = -1;
    session->root_last_child = -1;

    if (!session->root) return;

    std::vector<FragmentNode> nodes;
    // Walk the root's children (the root itself is the fragment root, not
    // a fragment), matching snapshot_accessibility_tree / mac behavior.
    for (size_t i = 0; i < session->root->child_count(); ++i) {
        if (View* child = session->root->child_at(i)) {
            build_fragment_nodes(*child, -1, nodes);
        }
    }
    link_fragment_nodes(nodes);
    RootLinks rl = link_root_level(nodes);
    session->root_first_child = rl.first;
    session->root_last_child = rl.last;

    session->fragments.reserve(nodes.size());
    for (auto& n : nodes) {
        session->fragments.push_back(new PulpFragmentProvider(session, n));
    }
}

void release_fragments(UiaSession* session) {
    if (!session) return;
    for (auto* f : session->fragments) {
        if (f) {
            // Drain in-flight UIA calls before dropping our ref, same as
            // the host provider. Cached clients may still call a fragment
            // method that touches session_ / the borrowed View*.
            UiaDisconnectProvider(f);
            f->Release();
        }
    }
    session->fragments.clear();
    session->root_first_child = -1;
    session->root_last_child = -1;
}

}  // namespace

// ── Lifecycle ────────────────────────────────────────────────────────────

void* init_accessibility(View& root, void* hwnd) {
    auto* session = new UiaSession{};
    session->hwnd = static_cast<HWND>(hwnd);
    session->root = &root;
    session->clients_listening = UiaClientsAreListening() ? true : false;
    // Build the fragment tree before publishing the host provider so a
    // concurrent WM_GETOBJECT reader that loads the host provider also
    // sees a complete fragment set when it navigates.
    rebuild_fragments(session);
    // Release store so a concurrent WM_GETOBJECT reader sees a fully
    // constructed PulpHostProvider through its acquire load.
    session->host_provider.store(new PulpHostProvider(session),
                                 std::memory_order_release);
    runtime::log_info(
        "Windows UIA: session ready (clients_listening={}, fragments={})",
        session->clients_listening, session->fragments.size());
    return session;
}

void shutdown_accessibility(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;

    // #514: Null the atomic FIRST, BEFORE we tell UIA there is no
    // provider for this HWND. See the host-provider race note below.
    auto* hp = session->host_provider.exchange(nullptr,
                                               std::memory_order_acq_rel);

    if (session->hwnd) {
        UiaReturnRawElementProvider(session->hwnd, 0, 0, nullptr);
    }

    // Disconnect + release the per-widget fragments before the host
    // provider. They borrow session_ and a raw View*; UiaDisconnectProvider
    // drains any in-flight client call and rejects new ones so the
    // session deletion below cannot UAF a cached fragment pointer.
    release_fragments(session);

    if (hp) {
        // #500 / #485 / #514: UiaDisconnectProvider is the sanctioned
        // shutdown barrier — it waits for in-flight UIA calls to drain
        // AND rejects subsequent calls, so the cached-client UAF window
        // is closed. After it returns no UIA-originated call can reach
        // the provider, making the session deletion safe.
        UiaDisconnectProvider(hp);
        hp->Release();
    }
    runtime::log_info("Windows UIA: session shutdown");
    delete session;
}

void accessibility_tree_changed(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    auto* hp = session->host_provider.load(std::memory_order_acquire);
    if (!hp) return;
    // Rebuild the fragment set to reflect the new tree even when no
    // client is listening (so a client that attaches later walks the
    // current tree). Cheap relative to a structural UI change.
    rebuild_fragments(session);
    if (!UiaClientsAreListening()) return;
    UiaRaiseStructureChangedEvent(
        hp, StructureChangeType_ChildrenBulkAdded, nullptr, 0);
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

// ── Event-raising helpers ─────────────────────────────────────────────────
//
// Resolve the target View to its per-widget fragment so the OS event
// names the precise element. Falls back to the fragment root when the
// target has no fragment (e.g. an AccessRole::none container, or an
// event raised before the fragment set was built).

namespace {

PulpFragmentProvider* fragment_for(UiaSession* session, View& target) {
    if (!session) return nullptr;
    for (auto* f : session->fragments) {
        if (f && f->view() == &target) return f;
    }
    return nullptr;
}

}  // namespace

void notify_accessibility_value_changed(void* handle, View& target) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    auto* hp = session->host_provider.load(std::memory_order_acquire);
    if (!hp) return;
    if (!UiaClientsAreListening()) return;

    VARIANT old_v, new_v;
    VariantInit(&old_v);
    VariantInit(&new_v);

    if (PulpFragmentProvider* frag = fragment_for(session, target)) {
        // Populate the new value so the reader can announce it directly.
        if (View* v = frag->view()) {
            if (auto* vif = dynamic_cast<AccessibilityValueInterface*>(v)) {
                new_v.vt = VT_BSTR;
                new_v.bstrVal = make_bstr(vif->get_value_string());
            } else if (!v->access_value().empty()) {
                new_v.vt = VT_BSTR;
                new_v.bstrVal = make_bstr(v->access_value());
            }
        }
        UiaRaiseAutomationPropertyChangedEvent(
            static_cast<IRawElementProviderSimple*>(frag),
            UIA_ValueValuePropertyId, old_v, new_v);
        VariantClear(&new_v);
        return;
    }
    // No fragment for the target — raise at root granularity so clients
    // at least learn something changed.
    UiaRaiseAutomationPropertyChangedEvent(
        hp, UIA_ValueValuePropertyId, old_v, new_v);
}

void notify_accessibility_focus_changed(void* handle, View& target) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    auto* hp = session->host_provider.load(std::memory_order_acquire);
    if (!hp) return;
    if (!UiaClientsAreListening()) return;
    if (PulpFragmentProvider* frag = fragment_for(session, target)) {
        UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(frag),
                                UIA_AutomationFocusChangedEventId);
        return;
    }
    UiaRaiseAutomationEvent(hp, UIA_AutomationFocusChangedEventId);
}

void notify_accessibility_name_changed(void* handle, View& target) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    auto* hp = session->host_provider.load(std::memory_order_acquire);
    if (!hp) return;
    if (!UiaClientsAreListening()) return;
    VARIANT old_v, new_v;
    VariantInit(&old_v);
    VariantInit(&new_v);
    if (PulpFragmentProvider* frag = fragment_for(session, target)) {
        UiaRaiseAutomationPropertyChangedEvent(
            static_cast<IRawElementProviderSimple*>(frag),
            UIA_NamePropertyId, old_v, new_v);
        return;
    }
    UiaRaiseAutomationPropertyChangedEvent(
        hp, UIA_NamePropertyId, old_v, new_v);
}

} // namespace pulp::view

#endif // _WIN32
