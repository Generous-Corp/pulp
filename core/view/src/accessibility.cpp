// Out-of-line definitions for the accessibility interfaces.
//
// The interfaces in pulp/view/accessibility.hpp are otherwise
// header-only. Without an out-of-line virtual definition the compiler
// has nowhere to emit the typeinfo / vtable, which makes
// dynamic_cast<...InterfaceType*>(view) fail to link on builds with
// -Wl,--no-undefined (e.g., the Android NDK toolchain). Anchoring
// the vtables here is the standard fix.

#include <pulp/view/accessibility.hpp>
#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

#include <sstream>

namespace pulp::view {

// Anchor the vtables. The out-of-line virtual destructor is enough —
// = default in the .cpp ensures the typeinfo lands in this TU.
AccessibilityValueInterface::~AccessibilityValueInterface() = default;
AccessibilityTextInterface::~AccessibilityTextInterface() = default;
AccessibilityTableInterface::~AccessibilityTableInterface() = default;
AccessibilityCellInterface::~AccessibilityCellInterface() = default;

// Default implementation: format the current value as a percentage of
// the [min, max] range, falling back to a raw decimal if the range is
// degenerate. Subclasses can override for unit-specific formatting
// (e.g., "-6 dB", "440 Hz", "120 BPM").
std::string AccessibilityValueInterface::get_value_string() const {
    double v   = get_current_value();
    double lo  = get_minimum_value();
    double hi  = get_maximum_value();
    std::ostringstream out;
    if (hi > lo) {
        double pct = ((v - lo) / (hi - lo)) * 100.0;
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        out << static_cast<int>(pct + 0.5) << "%";
    } else {
        out << v;
    }
    return out.str();
}

// ── Value resolution ─────────────────────────────────────────────────────

AccessToggleState accessibility_toggle_state(const View& v) {
    auto parse = [](const std::string& s) {
        if (s == "true")  return AccessToggleState::on;
        if (s == "false") return AccessToggleState::off;
        if (s == "mixed") return AccessToggleState::mixed;
        return AccessToggleState::unset;
    };
    // aria-checked wins over aria-pressed: a checkbox/switch state is more
    // semantically load-bearing than a toggle button's pressed state. Same
    // priority the macOS bridge applies.
    if (const auto st = parse(v.access_checked()); st != AccessToggleState::unset)
        return st;
    return parse(v.access_pressed());
}

std::string accessibility_toggle_state_string(const View& v) {
    const bool from_checked = !v.access_checked().empty();
    switch (accessibility_toggle_state(v)) {
        case AccessToggleState::on:
            return from_checked ? "checked" : "pressed";
        case AccessToggleState::off:
            return from_checked ? "unchecked" : "not pressed";
        case AccessToggleState::mixed:
            return "mixed";
        case AccessToggleState::unset:
            break;
    }
    return "";
}

std::string accessibility_value_string(const View& v) {
    if (const auto* vif = dynamic_cast<const AccessibilityValueInterface*>(&v))
        return vif->get_value_string();
    if (const auto* tif = dynamic_cast<const AccessibilityTextInterface*>(&v))
        return tif->get_text();
    if (!v.access_value().empty()) return v.access_value();
    // A Checkbox / Toggle / ToggleButton has no other value source: its state
    // IS its value. macOS turns the state into a native @YES/@NO before it gets
    // here; iOS and Android read this string, and without it they announced the
    // role and nothing else.
    return accessibility_toggle_state_string(v);
}

bool has_accessibility_value(const View& v) {
    if (dynamic_cast<const AccessibilityValueInterface*>(&v)) return true;
    if (dynamic_cast<const AccessibilityTextInterface*>(&v)) return true;
    if (!v.access_value().empty()) return true;
    return accessibility_toggle_state(v) != AccessToggleState::unset;
}

AccessibilityInterfaceSet accessibility_interfaces(const View& v) {
    AccessibilityInterfaceSet set;
    set.value = dynamic_cast<const AccessibilityValueInterface*>(&v) != nullptr;
    // A numeric value is served through the Value interface, not as text: a
    // Knob is not a text field. Text covers the two STRING sources — a text
    // interface (exported even when EMPTY: an empty editable field still has a
    // caret, a character count of 0, and an EDITABLE state) and a non-empty
    // access_value slot (a ComboBox's selected item).
    const bool has_text_iface =
        dynamic_cast<const AccessibilityTextInterface*>(&v) != nullptr;
    set.text = !set.value && (has_text_iface || !v.access_value().empty());
    return set;
}

std::string accessibility_text_content(const View& v) {
    if (const auto* tif = dynamic_cast<const AccessibilityTextInterface*>(&v))
        return tif->get_text();
    return v.access_value();
}

bool accessibility_text_editable(const View& v) {
    const auto* tif = dynamic_cast<const AccessibilityTextInterface*>(&v);
    return tif != nullptr && tif->is_editable();
}

bool is_accessibility_element(const View& v) {
    const View::AccessRole role = v.access_role();
    if (role == View::AccessRole::none) return false;
    if (is_structural_access_role(role)) return true;
    if (!v.access_label().empty()) return true;
    if (has_accessibility_value(v)) return true;
    // A checkbox / switch announces its state even unnamed.
    if (!v.access_checked().empty() || !v.access_pressed().empty()) return true;
    return false;
}

// ── Live-region announcements ────────────────────────────────────────────

namespace {
    AnnouncementSink& current_sink() {
        static AnnouncementSink sink;
        return sink;
    }
}

void set_announcement_sink(AnnouncementSink sink) {
    current_sink() = std::move(sink);
}

void announce_accessibility(std::string_view text,
                            AnnouncementPriority priority) {
    const auto& sink = current_sink();
    if (sink) {
        sink(text, priority);
        return;
    }
    const char* pol =
        priority == AnnouncementPriority::Assertive ? "assertive" : "polite";
    pulp::runtime::log_info("a11y announce ({}): {}", pol,
                            std::string(text));
}

// Default accessibility_pump(): a no-op for every platform whose provider runs
// callback-driven on the OS side (macOS / iOS / Windows / Android). The Linux
// AT-SPI provider needs to service inbound D-Bus calls, so it supplies its own
// definition in platform/linux/accessibility_linux.cpp — excluded here on the
// Linux *desktop* build to avoid a duplicate symbol. Android is __linux__ but
// uses the TalkBack provider (no D-Bus pump), so it keeps this no-op.
#if !defined(__linux__) || defined(__ANDROID__)
void accessibility_pump(void* /*handle*/) {}
#endif

} // namespace pulp::view
