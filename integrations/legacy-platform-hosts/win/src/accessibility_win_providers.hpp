#pragma once

#ifdef _WIN32

#include "accessibility_win_fragment_lifecycle.hpp"
#include "accessibility_win_util.hpp"
#include <pulp/platform/win32_sane.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/platform/uia_mapping.hpp>
#include <pulp/view/view.hpp>
#include <UIAutomation.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pulp::view {

// Internal UIA provider/session ownership used only by the Windows provider
// implementation translation units.

struct UiaSession;
class PulpHostProvider;
class PulpFragmentProvider;
void shutdown_accessibility(void* handle);
using uia_detail::make_bstr;

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

    // Owns publication, reader draining, and reentrant deferred retirement for
    // the depth-first provider vector. The protocol lives in one independently
    // tested RAII component rather than being repeated across this backend.
    UiaFragmentLifecycle fragment_lifecycle;

    // UIA disconnect/event calls can pump the owning STA and re-enter full
    // shutdown. Operations keep the session allocation alive until their outer
    // stack frames unwind; shutdown unpublishes immediately, then finalizes at
    // depth zero.
    std::uint32_t operation_depth = 0;
    bool shutdown_requested = false;
    bool shutdown_in_progress = false;
    PulpHostProvider* retired_host_provider = nullptr;
    // A realm owner attached during reentrant shutdown covers every provider
    // operation, including the host provider, until operation_depth reaches
    // zero and final session deletion runs.
    std::shared_ptr<void> retained_owner;
};

class UiaSessionOperation {
public:
    UiaSessionOperation() = default;
    explicit UiaSessionOperation(UiaSession* session) { begin(session); }
    void begin(UiaSession* session) {
        session_ = session;
        if (!session_ || session_->shutdown_requested) {
            session_ = nullptr;
            return;
        }
        ++session_->operation_depth;
    }
    ~UiaSessionOperation() {
        if (!session_) return;
        auto* session = session_;
        --session->operation_depth;
        if (session->operation_depth != 0) return;
        if (session->shutdown_requested && !session->shutdown_in_progress) {
            shutdown_accessibility(session);
            return;
        }
        // Provider Call objects destroy their call-gate/host lock before this
        // operation member. Release an owner retained by ordinary reentrant
        // realm teardown only after the last provider lock has unwound.
        auto retained_owner = std::move(session->retained_owner);
    }
    UiaSessionOperation(const UiaSessionOperation&) = delete;
    UiaSessionOperation& operator=(const UiaSessionOperation&) = delete;
    explicit operator bool() const noexcept { return session_ != nullptr; }

private:
    UiaSession* session_ = nullptr;
};

// ── Per-widget fragment provider ──────────────────────────────────────────

class PulpFragmentProvider final : public IRawElementProviderSimple,
                                    public IRawElementProviderFragment,
                                    public IRangeValueProvider,
                                    public IValueProvider {
public:
    PulpFragmentProvider(UiaSession* session, UiaFragmentNode node)
        : session_(session), node_(node) {}

    class Call {
    public:
        explicit Call(PulpFragmentProvider& provider)
            : provider_(provider), lock_(provider.retirement_mutex_),
              active_(provider.available_) {
            if (active_) {
                ++provider_.active_calls_;
                operation_.begin(provider.session_);
            }
            valid_ = static_cast<bool>(operation_) && provider.session_;
        }
        ~Call() {
            if (!active_) return;
            assert(provider_.active_calls_ != 0);
            --provider_.active_calls_;
            std::shared_ptr<void> retired_owner;
            if (provider_.active_calls_ == 0)
                retired_owner = std::move(provider_.retired_owner_);
            lock_.unlock();
            retired_owner.reset();
        }
        explicit operator bool() const noexcept { return valid_; }
        UiaSession* session() const noexcept { return provider_.session_; }
        View* view() const noexcept { return provider_.node_.view; }
    private:
        PulpFragmentProvider& provider_;
        UiaSessionOperation operation_;
        std::unique_lock<std::recursive_mutex> lock_;
        bool active_ = false;
        bool valid_ = false;
    };

    bool retire() {
        std::lock_guard lock(retirement_mutex_);
        available_ = false;
        session_ = nullptr;
        node_.view = nullptr;
        return active_calls_ == 0;
    }
    bool can_disconnect() {
        std::lock_guard lock(retirement_mutex_);
        return active_calls_ == 0;
    }
    void retain_until_idle(std::shared_ptr<void> owner) {
        if (!owner) return;
        std::lock_guard lock(retirement_mutex_);
        if (active_calls_ == 0) return;
        assert(!retired_owner_ || retired_owner_ == owner);
        retired_owner_ = std::move(owner);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragment)) {
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == __uuidof(IRangeValueProvider) ||
                   riid == __uuidof(IValueProvider)) {
            Call call(*this);
            if (!call) {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            if (riid == __uuidof(IRangeValueProvider) &&
                supports_range_value(call.view())) {
                *ppv = static_cast<IRangeValueProvider*>(this);
            } else if (riid == __uuidof(IValueProvider) &&
                       supports_value(call.view())) {
                *ppv = static_cast<IValueProvider*>(this);
            } else {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
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
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (patternId == UIA_RangeValuePatternId &&
            supports_range_value(call.view())) {
            *pRetVal = static_cast<IRangeValueProvider*>(this);
            AddRef();
        } else if (patternId == UIA_ValuePatternId &&
                   supports_value(call.view())) {
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
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (View* v = call.view()) v->set_focus(true);
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(
        IRawElementProviderFragmentRoot** pRetVal) override;

    // IRangeValueProvider
    IFACEMETHODIMP SetValue(double val) override {
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        // Only claim the write when the pattern is actually advertised —
        // is_read_only() below must agree with what this returns.
        if (auto* vif = value_iface(call.view());
            vif && uia::role_supports_value(role_or_none(call.view()))) {
            vif->set_current_value(val);
            return S_OK;
        }
        return UIA_E_NOTSUPPORTED;
    }
    IFACEMETHODIMP get_Value(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (auto* vif = value_iface(call.view())) *pRetVal = vif->get_current_value();
        return S_OK;
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        // IsReadOnly means exactly "SetValue will fail". uia::is_read_only()
        // is the shared predicate both SetValue overloads are written against;
        // the old proxy (supports_value()) reported "editable" for a
        // TextEditor whose SetValue then returned UIA_E_NOTSUPPORTED, and for
        // a Knob with no value interface at all.
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        *pRetVal = uia::is_read_only(role_or_none(call.view()),
                                     value_iface(call.view()) != nullptr,
                                     editable_text_iface(call.view()) != nullptr)
                       ? VARIANT_TRUE
                       : VARIANT_FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_Maximum(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (auto* vif = value_iface(call.view())) *pRetVal = vif->get_maximum_value();
        return S_OK;
    }
    IFACEMETHODIMP get_Minimum(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (auto* vif = value_iface(call.view())) *pRetVal = vif->get_minimum_value();
        return S_OK;
    }
    IFACEMETHODIMP get_LargeChange(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (auto* vif = value_iface(call.view())) *pRetVal = vif->get_step_size() * 10.0;
        return S_OK;
    }
    IFACEMETHODIMP get_SmallChange(double* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = 0.0;
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (auto* vif = value_iface(call.view())) *pRetVal = vif->get_step_size();
        return S_OK;
    }

    // IValueProvider
    // (SetValue collides by name with IRangeValueProvider::SetValue but
    // has a BSTR signature; declare it explicitly to disambiguate.)
    IFACEMETHODIMP SetValue(LPCWSTR val) override {
        // Route the edit into the View's text interface (TextEditor). Without
        // this every Narrator edit was rejected while get_IsReadOnly claimed
        // the field was editable.
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        auto* tif = editable_text_iface(call.view());
        if (!tif || !val) return UIA_E_NOTSUPPORTED;
        const int len = WideCharToMultiByte(CP_UTF8, 0, val, -1, nullptr, 0,
                                            nullptr, nullptr);
        if (len <= 0) return UIA_E_NOTSUPPORTED;
        std::string utf8(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, val, -1, utf8.data(), len, nullptr,
                            nullptr);
        utf8.pop_back();  // discard the terminator written by the -1 input form
        tif->set_text(utf8);
        return S_OK;
    }
    IFACEMETHODIMP get_Value(BSTR* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        Call call(*this);
        if (!call) return UIA_E_ELEMENTNOTAVAILABLE;
        if (View* v = call.view()) {
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
    static View::AccessRole role_or_none(View* view) {
        return view ? view->access_role() : View::AccessRole::none;
    }
    // Pattern availability = role allows it AND the View can serve it. A
    // pattern advertised without a source resolves to a null interface (or,
    // worse, to zeros in a degenerate 0..0 range).
    static bool supports_range_value(View* view) {
        return uia::exposes_range_value(role_or_none(view),
                                        value_iface(view) != nullptr);
    }
    static bool supports_value(View* view) {
        return uia::exposes_value(
            role_or_none(view), value_iface(view) != nullptr,
            text_iface(view) != nullptr,
            view && !view->access_value().empty());
    }
    static AccessibilityValueInterface* value_iface(View* view) {
        return view
            ? dynamic_cast<AccessibilityValueInterface*>(view)
            : nullptr;
    }
    static AccessibilityTextInterface* text_iface(View* view) {
        return view
            ? dynamic_cast<AccessibilityTextInterface*>(view)
            : nullptr;
    }
    static AccessibilityTextInterface* editable_text_iface(View* view) {
        auto* tif = text_iface(view);
        return (tif && tif->is_editable()) ? tif : nullptr;
    }

    std::atomic<LONG> refs_{1};
    std::recursive_mutex retirement_mutex_;
    bool available_ = true;
    std::uint32_t active_calls_ = 0;
    std::shared_ptr<void> retired_owner_;
    UiaSession* session_;
    UiaFragmentNode node_;
};

// ── Root host provider (also the fragment root) ───────────────────────────

class PulpHostProvider final : public IRawElementProviderSimple,
                               public IRawElementProviderFragment,
                               public IRawElementProviderFragmentRoot {
public:
    explicit PulpHostProvider(UiaSession* session) : session_(session) {}

    class Call {
    public:
        explicit Call(PulpHostProvider& provider)
            : provider_(provider), lock_(provider.call_mutex_) {
            if (provider.active_ && provider.session_)
                operation_.begin(provider.session_);
        }
        explicit operator bool() const noexcept {
            return static_cast<bool>(operation_)
                && provider_.active_ && provider_.session_;
        }
        UiaSession* session() const noexcept { return provider_.session_; }
    private:
        PulpHostProvider& provider_;
        UiaSessionOperation operation_;
        std::unique_lock<std::recursive_mutex> lock_;
    };

    void retire() {
        std::lock_guard lock(call_mutex_);
        active_ = false;
        session_ = nullptr;
    }

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
    std::recursive_mutex call_mutex_;
    bool active_ = true;
    UiaSession* session_;
};

} // namespace pulp::view

#endif // _WIN32
