// Windows UI Automation accessibility provider
// Stub implementation — provides the interface surface for future implementation.
//
// When fully implemented, this will:
// - Implement IRawElementProviderSimple for each View with an AccessRole
// - Map AccessRole to UIA control types (Slider, Button, Text, Group, etc.)
// - Expose access_label as Name property, access_value as Value pattern
// - Handle WM_GETOBJECT to return providers to screen readers (Narrator, JAWS, NVDA)

#ifdef _WIN32

#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::view {

// Map Pulp AccessRole to Windows UIA Control Type IDs
// UIA_SliderControlTypeId = 50015, UIA_ButtonControlTypeId = 50000, etc.
[[maybe_unused]] static int access_role_to_uia_type(View::AccessRole role) {
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

} // namespace pulp::view

#endif // _WIN32
