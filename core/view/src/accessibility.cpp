// Out-of-line definitions for the accessibility interfaces.
//
// The interfaces in pulp/view/accessibility.hpp are otherwise
// header-only. Without an out-of-line virtual definition the compiler
// has nowhere to emit the typeinfo / vtable, which makes
// dynamic_cast<...InterfaceType*>(view) fail to link on builds with
// -Wl,--no-undefined (e.g., the Android NDK toolchain). Anchoring
// the vtables here is the standard fix.

#include <pulp/view/accessibility.hpp>
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

// ── Live-region announcements (workstream 04 slice 4.3) ─────────────────

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

} // namespace pulp::view
