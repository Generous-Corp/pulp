#pragma once

// Pure decision logic for org.freedesktop.DBus.Properties on the AT-SPI
// objects: which (interface, property) pairs exist on a node, which are
// writable, and which D-Bus error a rejected Get or Set maps to. It has no
// D-Bus or View dependency, so it is testable on every platform.

#include <string_view>

namespace pulp::view::atspi_props {

// D-Bus basic-type codes carried by a variant's content.
inline constexpr int kTypeInt32 = 'i';
inline constexpr int kTypeDouble = 'd';
inline constexpr int kTypeVariant = 'v';

// What a node exposes. `is_root` is the application object; `has_text` and
// `has_value` say whether the node implements the Text / Value interfaces.
struct NodeShape {
    bool is_root = false;
    bool has_text = false;
    bool has_value = false;
};

enum class Writable { no, int32, double_ };

// True when Properties.Get can answer for this pair.
inline bool is_readable(const NodeShape& n, std::string_view iface, std::string_view prop) {
    if (iface == "org.a11y.atspi.Accessible")
        return prop == "Name" || prop == "Description" || prop == "Parent" ||
               prop == "ChildCount";
    if (iface == "org.a11y.atspi.Application")
        return n.is_root &&
               (prop == "ToolkitName" || prop == "Version" || prop == "AtspiVersion" ||
                prop == "Id");
    if (iface == "org.a11y.atspi.Text")
        return n.has_text && (prop == "CharacterCount" || prop == "CaretOffset");
    if (iface == "org.a11y.atspi.Value")
        return n.has_value && (prop == "CurrentValue" || prop == "MinimumValue" ||
                               prop == "MaximumValue" || prop == "MinimumIncrement");
    return false;
}

// Writable properties: Application.Id (i, root only) and Value.CurrentValue (d).
inline Writable writable_type(const NodeShape& n, std::string_view iface, std::string_view prop) {
    if (!is_readable(n, iface, prop)) return Writable::no;
    if (iface == "org.a11y.atspi.Application" && prop == "Id") return Writable::int32;
    if (iface == "org.a11y.atspi.Value" && prop == "CurrentValue") return Writable::double_;
    return Writable::no;
}

enum class SetVerdict { apply, unknown_property, read_only, invalid_args };

// `variant_type` is the D-Bus type code inside the variant argument, or 0 when
// the argument is missing or is not a variant.
inline SetVerdict decide_set(const NodeShape& n, std::string_view iface, std::string_view prop,
                             int variant_type) {
    if (!is_readable(n, iface, prop)) return SetVerdict::unknown_property;
    const Writable w = writable_type(n, iface, prop);
    if (w == Writable::no) return SetVerdict::read_only;
    const int want = (w == Writable::int32) ? kTypeInt32 : kTypeDouble;
    return variant_type == want ? SetVerdict::apply : SetVerdict::invalid_args;
}

inline const char* error_name(SetVerdict v) {
    switch (v) {
    case SetVerdict::unknown_property:
        return "org.freedesktop.DBus.Error.UnknownProperty";
    case SetVerdict::read_only:
        return "org.freedesktop.DBus.Error.PropertyReadOnly";
    case SetVerdict::invalid_args:
        return "org.freedesktop.DBus.Error.InvalidArgs";
    case SetVerdict::apply:
        break;
    }
    return "";
}

} // namespace pulp::view::atspi_props
