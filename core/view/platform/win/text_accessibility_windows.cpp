// Windows UI Automation backend for TextAccessibilityNode (font v2 Slice 2.6).
//
// Replaces the default "none" backend defined in
// core/view/src/text_accessibility.cpp on Windows builds. Painted text
// that registers a TextAccessibilityNode through this backend gets
// surfaced to Narrator (and other UIA clients) as a real
// IRawElementProviderSimple keyed by the caller-stable node id.
//
// Scope: SCAFFOLD-LEVEL provider. Each registered node vends a minimal
// IRawElementProviderSimple wrapper that reports the configured Name,
// ControlType, and an "IsControlElement"/"IsContentElement" pair so UIA
// clients see the element as participating in both trees. The provider
// instances are owned by an std::unordered_map<id, ComPtr<...>>; the COM
// refcount mirrors that ownership (registry holds the +1, UIA can AddRef
// transiently when handing the provider to a client).
//
// Out of scope (deliberately) on this slice:
// - ITextProvider2 / ITextRangeProvider implementations. The header
//   contract mentions them as the eventual target, but the scaffold ships
//   only the discovery surface (ControlType + Name) so screen readers see
//   *something* through the same registry the cross-platform shadow
//   exposes. ITextProvider2 is the obvious follow-up once the painted-
//   text sites start registering selection ranges in earnest.
// - IRawElementProviderFragment tree walk. The existing
//   accessibility_win.cpp wires the root host provider into WM_GETOBJECT;
//   text providers don't appear in the fragment tree yet. The follow-up
//   slice that adds per-widget fragment providers will plug these in.
//
// Memory model: each PulpTextAccessibilityProvider is reference-counted
// via std::atomic<LONG>. The registry holds a single +1 reference per
// id (vended through a small ComPtr-style holder) and releases it on
// unregister or replace. UIA clients can transiently AddRef the provider
// when they hand it back across the COM boundary; that ref is dropped
// when the client releases its copy.
//
// Linux AccessKit is deferred — see platform/linux/text_accessibility_linux.cpp
// for that backend's scaffold state.

#ifdef _WIN32

#include <pulp/view/text_accessibility.hpp>

#include <pulp/platform/win32_sane.hpp>  // brings in ObjBase.h + oleidl.h
#include <UIAutomation.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

namespace {

// Map an enum TextAccessibilityRole → UIA control-type ID. The numeric
// constants come from the UIA control-type enumeration documented at:
// https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-controltype-ids
long control_type_for(TextAccessibilityRole role) {
    switch (role) {
        case TextAccessibilityRole::Label:      return UIA_TextControlTypeId;       // 50020
        case TextAccessibilityRole::Button:     return UIA_ButtonControlTypeId;     // 50000
        case TextAccessibilityRole::TextEditor: return UIA_EditControlTypeId;       // 50004
        case TextAccessibilityRole::Heading:    return UIA_HeaderControlTypeId;     // 50034
        case TextAccessibilityRole::Other:
        default:                                return UIA_CustomControlTypeId;     // 50025
    }
}

// Convert UTF-8 → UTF-16 BSTR. Caller owns the BSTR (SysFreeString or
// hand it through a VARIANT). Empty input returns SysAllocString(L"")
// so VT_BSTR semantics stay consistent (never nullptr).
BSTR make_bstr(const std::string& s) {
    if (s.empty()) return SysAllocString(L"");
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                          static_cast<int>(s.size()),
                                          nullptr, 0);
    if (wlen <= 0) return SysAllocString(L"");
    BSTR out = SysAllocStringLen(nullptr, static_cast<UINT>(wlen));
    if (!out) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out, wlen);
    return out;
}

}  // namespace

// ── PulpTextAccessibilityProvider ─────────────────────────────────────────
//
// One instance per registered TextAccessibilityNode. Implements the
// minimal IRawElementProviderSimple surface UIA needs to expose the node
// as a discoverable control: a control-type, a name (the text content),
// and IsControlElement / IsContentElement booleans so the element is
// reachable through both the control tree and the content tree.
//
// The class is final so the inheritance chain doesn't accidentally grow;
// the IRawElementProviderSimple vtable is the only COM surface this
// scaffold needs.
class PulpTextAccessibilityProvider final : public IRawElementProviderSimple {
public:
    explicit PulpTextAccessibilityProvider(const TextAccessibilityNode& node)
        : node_(node) {}

    // ── IUnknown ──────────────────────────────────────────────────────
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
        return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const auto prev = refs_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(prev - 1);
    }

    // ── IRawElementProviderSimple ────────────────────────────────────
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        // Server-side provider: the process owning this object is the
        // one that originated it. UIA uses this hint to decide whether
        // to marshal calls across a process boundary.
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID /*patternId*/,
                                       IUnknown** pRetVal) override {
        // Scaffold-only: no patterns advertised yet. ITextProvider2 lands
        // in the follow-up slice; returning nullptr here is the documented
        // way to tell UIA the pattern is not supported.
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID propertyId,
                                     VARIANT* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        VariantInit(pRetVal);

        switch (propertyId) {
            case UIA_NamePropertyId: {
                BSTR bs = make_bstr(node_.text);
                if (bs) {
                    pRetVal->vt = VT_BSTR;
                    pRetVal->bstrVal = bs;
                }
                break;
            }
            case UIA_AutomationIdPropertyId: {
                BSTR bs = make_bstr(node_.id);
                if (bs) {
                    pRetVal->vt = VT_BSTR;
                    pRetVal->bstrVal = bs;
                }
                break;
            }
            case UIA_ControlTypePropertyId: {
                pRetVal->vt = VT_I4;
                pRetVal->lVal = control_type_for(node_.role);
                break;
            }
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId: {
                pRetVal->vt = VT_BOOL;
                pRetVal->boolVal = VARIANT_TRUE;
                break;
            }
            case UIA_FrameworkIdPropertyId: {
                // Match the host provider in accessibility_win.cpp so
                // UIA clients can filter on "Pulp" as a framework.
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = SysAllocString(L"Pulp");
                break;
            }
            default:
                // VT_EMPTY → UIA falls back to the host provider chain
                // (or treats the property as unset).
                break;
        }
        return S_OK;
    }

    IFACEMETHODIMP get_HostRawElementProvider(
        IRawElementProviderSimple** pRetVal) override {
        // The TextAccessibilityNode registry is HWND-agnostic — nodes
        // are owned by the painted-text surface, not by any particular
        // window. The host provider chain belongs to the per-window
        // root in accessibility_win.cpp; returning nullptr here is the
        // documented "no host" answer and keeps this provider standalone.
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }

    // Test hook: expose the long control-type without going through a
    // VARIANT round-trip. Lets unit tests assert the role mapping
    // directly. Not part of the COM surface.
    long control_type() const { return control_type_for(node_.role); }

    const TextAccessibilityNode& node() const { return node_; }

    // Replace the backing node in-place (same id) so a re-register with
    // the same key doesn't churn the COM object identity. Assistive
    // tech caches provider pointers; preserving identity across updates
    // keeps client-side state coherent.
    void update_node(const TextAccessibilityNode& node) { node_ = node; }

private:
    ~PulpTextAccessibilityProvider() = default;

    TextAccessibilityNode node_;
    std::atomic<LONG> refs_{1};
};

namespace {

struct Registry {
    std::mutex mu;
    // Shadow map for cross-platform snapshot semantics. Same shape as
    // the default backend so snapshot_accessibility_nodes() doesn't need
    // to round-trip through COM.
    std::unordered_map<std::string, TextAccessibilityNode> nodes;
    // Per-id COM provider. The map holds exactly one strong reference
    // per entry; replacing or erasing the entry Release()s that ref.
    std::unordered_map<std::string, PulpTextAccessibilityProvider*> providers;
};

Registry& registry() {
    static Registry r;
    return r;
}

}  // namespace

std::string_view accessibility_backend_name() noexcept {
    return "windows-uia";
}

void register_text_accessibility_node(const TextAccessibilityNode& node) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);

    // Shadow-map update — identical semantics to the default backend.
    r.nodes[node.id] = node;

    // Same-id update: keep the existing provider pointer so AT clients
    // that have cached it continue to resolve through us. The node
    // value the provider mirrors gets replaced in-place.
    auto it = r.providers.find(node.id);
    if (it != r.providers.end()) {
        it->second->update_node(node);
        return;
    }

    // First-time registration: construct the provider with +1 refcount
    // and stash it. The registry now owns that single ref; subsequent
    // QueryInterface / GetPatternProvider calls from UIA AddRef and
    // Release on their own copies.
    auto* prov = new PulpTextAccessibilityProvider(node);
    r.providers.emplace(node.id, prov);
}

void unregister_text_accessibility_node(std::string_view id) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    const std::string key(id);
    r.nodes.erase(key);
    auto it = r.providers.find(key);
    if (it != r.providers.end()) {
        // Drop the registry's +1 ref. Any UIA-cached client refs keep
        // the object alive until they're released; the object becomes
        // orphaned (no longer findable through the registry) but no
        // UAF can happen because the COM refcount is the sole truth.
        it->second->Release();
        r.providers.erase(it);
    }
}

std::vector<TextAccessibilityNode> snapshot_accessibility_nodes() {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    std::vector<TextAccessibilityNode> out;
    out.reserve(r.nodes.size());
    for (const auto& [_, node] : r.nodes) {
        out.push_back(node);
    }
    return out;
}

// ── Test hook ─────────────────────────────────────────────────────────────
//
// Returns a non-owning pointer to the live provider for the given id, or
// nullptr if no such node is registered. Defined in this TU so the
// Windows-only unit test can verify the COM bridge without pulling
// UIAutomation.h into the public header set. Callers must not Release
// this pointer; the registry continues to hold the +1 ref.
extern "C" PulpTextAccessibilityProvider*
pulp_text_accessibility_lookup_windows(const char* id_utf8) {
    if (!id_utf8) return nullptr;
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    auto it = r.providers.find(std::string(id_utf8));
    if (it == r.providers.end()) return nullptr;
    return it->second;
}

}  // namespace pulp::view

#endif  // _WIN32
