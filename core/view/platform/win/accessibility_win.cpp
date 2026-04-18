// Windows UI Automation accessibility provider
// Stub implementation — provides the interface surface for future implementation.
//
// When fully implemented, this will:
// - Implement IRawElementProviderSimple for each View with an AccessRole
// - Map AccessRole to UIA control types (Slider, Button, Text, Group, etc.)
// - Expose access_label as Name property, access_value as Value pattern
// - Handle WM_GETOBJECT to return providers to screen readers (Narrator, JAWS, NVDA)

#ifdef _WIN32

#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/platform/win32_sane.hpp>  // now brings in ObjBase.h + oleidl.h
#include <UIAutomation.h>

namespace pulp::view {

// Map Pulp AccessRole to Windows UIA Control Type IDs
// UIA_SliderControlTypeId = 50015, UIA_ButtonControlTypeId = 50000, etc.
static int access_role_to_uia_type(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return 50015;  // UIA_SliderControlTypeId
        case View::AccessRole::toggle: return 50000;  // UIA_ButtonControlTypeId
        case View::AccessRole::label:  return 50020;  // UIA_TextControlTypeId
        case View::AccessRole::group:  return 50026;  // UIA_GroupControlTypeId
        case View::AccessRole::meter:  return 50033;  // UIA_CustomControlTypeId
        case View::AccessRole::image:  return 50006;  // UIA_ImageControlTypeId
        default: return 50033; // UIA_CustomControlTypeId
    }
}

// Stub: Initialize Windows UI Automation for a view tree
void init_win_accessibility(View& /*root*/) {
    runtime::log_info("Windows UI Automation: stub initialized");
    // TODO: implement IRawElementProviderSimple + IAccessible
    // TODO: handle WM_GETOBJECT in HWND message proc
    // TODO: fire UIA events on value/focus changes
}

// ── Phase 1 bootstrap (workstream 04 #247) ──────────────────────────────
// Record the HWND + root so the WindowHost can forward WM_GETOBJECT, and
// probe UiaClientsAreListening so cheap paths can bail early when no
// assistive tech is attached. Full IRawElementProviderSimple + Fragment(Root)
// impls land in phase 2.
struct UiaSession {
    HWND hwnd = nullptr;
    View* root = nullptr;
    bool clients_listening = false;
};

void* init_accessibility(View& root, void* hwnd) {
    init_win_accessibility(root);
    auto* session = new UiaSession{};
    session->hwnd = static_cast<HWND>(hwnd);
    session->root = &root;
    session->clients_listening = UiaClientsAreListening();
    runtime::log_info(
        "Windows UIA: session ready (clients_listening={})",
        session->clients_listening);
    return session;
}

void shutdown_accessibility(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    runtime::log_info("Windows UIA: session shutdown");
    delete session;
    // TODO phase 2: UiaReturnRawElementProvider(hwnd, 0, 0, nullptr)
    // + release the fragment-root and any per-widget providers.
}

void accessibility_tree_changed(void* handle) {
    auto* session = static_cast<UiaSession*>(handle);
    if (!session) return;
    // TODO phase 2: UiaRaiseStructureChangedEvent(provider,
    //                StructureChangeType_ChildrenReordered, nullptr, 0);
    (void)session;
}

} // namespace pulp::view

#endif // _WIN32
