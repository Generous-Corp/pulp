// design_binding_metadata.hpp — typed model of the `pulp*` native-binding
// attribute contract.
//
// Created in the 2026-05-29 frontend-IR refactor (PR-3). Before this, the
// stringly-typed `pulp*` attributes that carry the native-C++ binding
// contract were read by hand in three places — the v0/TSX writer
// (design_import_v0_tsx.cpp), the widget-semantics reader
// (design_import_native_common.cpp), and the C++ codegen / binding-manifest
// emitter (design_cpp_codegen.cpp) — each re-enumerating the ~45 `pulp*`
// keys with ad-hoc `attr(node, "pulp...")` calls. NativeBindingMetadata is
// the single typed model: one parse() reads the attributes, one serialize()
// writes them back. Field presence/absence and exact string values round-trip
// byte-identically, so the emitted attribute strings and the generated
// binding-manifest JSON are unchanged.
//
// PRIVATE: lives under core/view/src/, not the installed include tree. Do not
// reference from headers outside core/view/src/.

#pragma once

#include <pulp/view/design_ir.hpp>

#include <optional>
#include <string>

namespace pulp::view {

// Typed model of the `pulp*` binding-contract attributes on an IRNode.
//
// Every field uses std::optional<std::string> for free / open-domain string
// values so that "absent" vs "present-but-empty" vs "present-with-value"
// round-trips exactly. Numeric thumb/corner/font fields parse to
// std::optional<float>; the two boolean flags carry their semantic defaults
// (both true) for the reader, and remember whether the attribute was actually
// present so serialize() can reproduce write-time presence.
struct NativeBindingMetadata {
    // Routing / family.
    std::optional<std::string> route_id;             // pulpRouteId
    std::optional<std::string> route_type;           // pulpRouteType ("native_cpp")
    std::optional<std::string> source_family;        // pulpSourceFamily
    std::optional<std::string> source_path;          // pulpSourcePath

    // Single-parameter binding.
    std::optional<std::string> param_key;            // pulpParamKey
    std::optional<std::string> binding_module;       // pulpBindingModule
    std::optional<std::string> binding_param;        // pulpBindingParam

    // Choice (toggle) binding.
    std::optional<std::string> choice_value;         // pulpChoiceValue
    std::optional<std::string> choice_label;         // pulpChoiceLabel

    // XY-pad binding.
    std::optional<std::string> x_param_key;          // pulpParamKeyX
    std::optional<std::string> y_param_key;          // pulpParamKeyY
    std::optional<std::string> x_binding_module;     // pulpBindingModuleX
    std::optional<std::string> x_binding_param;      // pulpBindingParamX
    std::optional<std::string> y_binding_module;     // pulpBindingModuleY
    std::optional<std::string> y_binding_param;      // pulpBindingParamY

    // Meter / waveform.
    std::optional<std::string> meter_source;         // pulpMeterSource
    std::optional<std::string> meter_channel;        // pulpMeterChannel
    std::optional<std::string> meter_value_key;      // pulpMeterValueKey
    std::optional<std::string> waveform_shape;       // pulpWaveformShape

    // Value / text.
    std::optional<std::string> value_key;            // pulpValueKey
    std::optional<std::string> initial_value;        // pulpInitialValue
    std::optional<std::string> placeholder;          // pulpPlaceholder
    std::optional<std::string> default_value_source; // pulpDefaultValueSource

    // Contracts / host actions / docs.
    std::optional<std::string> focus_contract;       // pulpFocusContract
    std::optional<std::string> payload_contract;     // pulpPayloadContract
    std::optional<std::string> host_action;          // pulpHostAction
    std::optional<std::string> host_action_label;    // pulpHostActionLabel
    std::optional<std::string> type_label;           // pulpTypeLabel
    std::optional<std::string> description;          // pulpDescription
    std::optional<std::string> event_contract;       // pulpEventContract
    std::optional<std::string> gesture_contract;     // pulpGestureContract
    std::optional<std::string> style_tokens;         // pulpStyleTokens
    std::optional<std::string> fallback_reason;      // pulpFallbackReason
    std::optional<std::string> widget_schema;        // pulpWidgetSchema

    // Grid templates.
    std::optional<std::string> grid_template_columns; // pulpGridTemplateColumns
    std::optional<std::string> grid_template_rows;    // pulpGridTemplateRows

    // Numeric visuals. Stored as the *raw* attribute text so the
    // binding-manifest emission (which writes the raw string) stays
    // byte-identical; the semantics reader parses on demand via the helpers
    // below, matching the original attr_float() behavior exactly.
    std::optional<std::string> thumb_width;          // pulpThumbWidth
    std::optional<std::string> thumb_height;         // pulpThumbHeight
    std::optional<std::string> thumb_corner_radius;  // pulpThumbCornerRadius
    std::optional<std::string> corner_radius;        // pulpCornerRadius
    std::optional<std::string> font_size;            // pulpFontSize

    // Toggle visuals (free strings — colors are raw attribute text).
    std::optional<std::string> thumb_shape;          // pulpThumbShape (raw, un-normalized)
    std::optional<std::string> on_background_color;  // pulpOnBackgroundColor
    std::optional<std::string> off_background_color; // pulpOffBackgroundColor
    std::optional<std::string> on_text_color;        // pulpOnTextColor
    std::optional<std::string> off_text_color;       // pulpOffTextColor
    std::optional<std::string> on_border_color;      // pulpOnBorderColor
    std::optional<std::string> off_border_color;     // pulpOffBorderColor

    // Boolean flags, stored as the raw attribute text so presence and exact
    // spelling round-trip. show_internal_label() and hit_testable() apply the
    // reader's original semantics (default true; only an explicit "false"
    // disables show_internal_label; hit_testable uses the general bool parse).
    std::optional<std::string> show_internal_label;  // pulpShowInternalLabel
    std::optional<std::string> hit_testable;         // pulpHitTestable

    // Parsed-float accessors — match attr_float(): strtof with trailing
    // whitespace tolerance, std::nullopt on non-numeric / empty.
    std::optional<float> thumb_width_value() const;
    std::optional<float> thumb_height_value() const;
    std::optional<float> thumb_corner_radius_value() const;
    std::optional<float> corner_radius_value() const;
    std::optional<float> font_size_value() const;

    // show_internal_label is disabled iff the raw attribute equals "false"
    // (case-insensitive); default true otherwise.
    bool show_internal_label_value() const;

    // Read the `pulp*` contract attributes off `node` into a typed model.
    static NativeBindingMetadata parse(const IRNode& node);

    // Write this model back onto `node.attributes` using the writer's
    // no-overwrite / skip-empty semantics (set_contract_attr): a key is only
    // written when the value is present and non-empty AND the node does not
    // already carry a non-empty value for that key.
    void serialize(IRNode& node) const;
};

}  // namespace pulp::view
