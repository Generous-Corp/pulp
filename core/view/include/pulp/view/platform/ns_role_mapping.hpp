#pragma once

// Apple NSAccessibility mapping for Pulp AccessRole.
//
// ObjC++-only header (NSAccessibilityRole is an NSString*), shared by BOTH
// macOS accessibility bridges so they cannot drift:
//
//   * core/view/platform/mac/accessibility_mac.mm  — standalone window host
//   * core/view/platform/mac/plugin_view_host_mac.mm — plug-in editor host
//
// Before this header the two carried independent switch/if-chains that had
// already diverged (meter was NSAccessibilityProgressIndicatorRole in one and
// NSAccessibilityLevelIndicatorRole in the other), so a DAW user and a
// standalone user heard different things for the same widget.
//
// Note on AccessRole::none: views with `none` are EXCLUDED from the
// accessibility tree by the collectors in both hosts — they never reach this
// function. The `none` case below exists only to keep the switch exhaustive
// (so adding an AccessRole value is a compile error here, not a silent
// fallthrough); NSAccessibilityUnknownRole is a defensive value, not the
// mechanism by which unroled views are hidden.

#include <TargetConditionals.h>

#if defined(__OBJC__) && TARGET_OS_OSX

#include <pulp/view/view.hpp>

#import <AppKit/NSAccessibilityConstants.h>

namespace pulp::view {

/// Map a Pulp AccessRole to the best-fit NSAccessibilityRole.
///
/// Roles with no first-class AppKit equivalent are documented inline rather
/// than silently mapped to Unknown:
///   * combo_box  → PopUpButton: Pulp's ComboBox is a non-editable dropdown,
///                  which is exactly AppKit's pop-up button.
///   * tab        → RadioButton: AppKit models tab items as radio buttons in a
///                  tab group (AXRadioButton + AXTabButton subrole); there is
///                  no NSAccessibilityTabRole.
///   * dialog     → Group: AXDialog is a *subrole* of AXWindow, so an in-view
///                  dialog cannot claim it. Group is the neutral container.
///   * heading    → StaticText: AppKit has no heading role (AXHeading is a
///                  WebKit-only extension).
inline NSAccessibilityRole ns_role_for_access_role(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider:       return NSAccessibilitySliderRole;
        case View::AccessRole::toggle:       return NSAccessibilityCheckBoxRole;
        case View::AccessRole::label:        return NSAccessibilityStaticTextRole;
        case View::AccessRole::group:        return NSAccessibilityGroupRole;
        case View::AccessRole::meter:        return NSAccessibilityLevelIndicatorRole;
        case View::AccessRole::image:        return NSAccessibilityImageRole;
        case View::AccessRole::button:       return NSAccessibilityButtonRole;
        case View::AccessRole::link:         return NSAccessibilityLinkRole;
        case View::AccessRole::checkbox:     return NSAccessibilityCheckBoxRole;
        case View::AccessRole::radio:        return NSAccessibilityRadioButtonRole;
        case View::AccessRole::text_field:   return NSAccessibilityTextFieldRole;
        case View::AccessRole::combo_box:    return NSAccessibilityPopUpButtonRole;
        case View::AccessRole::list:         return NSAccessibilityListRole;
        case View::AccessRole::list_item:    return NSAccessibilityRowRole;
        case View::AccessRole::table:        return NSAccessibilityTableRole;
        case View::AccessRole::row:          return NSAccessibilityRowRole;
        case View::AccessRole::cell:         return NSAccessibilityCellRole;
        case View::AccessRole::tab:          return NSAccessibilityRadioButtonRole;
        case View::AccessRole::tab_list:     return NSAccessibilityTabGroupRole;
        case View::AccessRole::menu:         return NSAccessibilityMenuRole;
        case View::AccessRole::menu_item:    return NSAccessibilityMenuItemRole;
        case View::AccessRole::progress_bar: return NSAccessibilityProgressIndicatorRole;
        case View::AccessRole::dialog:       return NSAccessibilityGroupRole;
        case View::AccessRole::heading:      return NSAccessibilityStaticTextRole;
        case View::AccessRole::scroll_bar:   return NSAccessibilityScrollBarRole;
        case View::AccessRole::none:         break;
    }
    return NSAccessibilityUnknownRole;
}

}  // namespace pulp::view

#endif  // __OBJC__ && TARGET_OS_OSX
