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

// Cross-platform entry — see accessibility_provider.hpp. Today these
// route to the existing UIA stub; a future commit will spin up a
// fragment-root provider, retain it per window, and tear it down in
// shutdown_accessibility.
void* init_accessibility(View& root, void* /*hwnd*/) {
    init_win_accessibility(root);
    // Returning a non-null sentinel lets callers pair init/shutdown
    // without pretending we have a real COM provider yet.
    return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
}

void shutdown_accessibility(void* /*handle*/) {
    // TODO: release the UIA fragment-root and its child providers.
    runtime::log_info("Windows UI Automation: shutdown (stub)");
}

void accessibility_tree_changed(void* /*handle*/) {
    // TODO: UiaRaiseStructureChangedEvent(root_provider, StructureChangeType_ChildrenReordered, ...)
}

} // namespace pulp::view

#endif // _WIN32
