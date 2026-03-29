#pragma once

/// @file design_import.hpp
/// Import design files from external tools (Figma, Stitch, v0, Pencil) into
/// Pulp's web-compat JS format. Supports a normalized intermediate
/// representation (IR) and W3C Design Tokens.

#include <pulp/view/theme.hpp>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <variant>

namespace pulp::view {

// ── Design source types ─────────────────────────────────────────────────

enum class DesignSource {
    figma,
    stitch,
    v0,
    pencil
};

/// Convert string to DesignSource, returns nullopt for unknown sources.
std::optional<DesignSource> parse_design_source(const std::string& name);

/// Convert DesignSource to display name.
const char* design_source_name(DesignSource source);

// ── Normalized Intermediate Representation ──────────────────────────────

/// Layout direction for flex containers.
enum class LayoutDirection { row, column };

/// Alignment values for flex containers.
enum class LayoutAlign {
    flex_start, flex_end, center, stretch, space_between, space_around
};

/// Sizing mode for an IR node dimension.
enum class SizingMode { fixed, hug, fill };

/// Style properties for an IR node.
struct IRStyle {
    std::optional<std::string> background_color;
    std::optional<std::string> background_gradient;   // linear-gradient(...)
    std::optional<std::string> color;                  // text color
    std::optional<float> opacity;
    std::optional<float> border_radius;
    std::optional<std::string> border;                 // e.g. "1px solid #333"
    std::optional<std::string> box_shadow;
    std::optional<std::string> filter;                 // e.g. "blur(4px)"
    std::optional<std::string> font_family;
    std::optional<float> font_size;
    std::optional<int> font_weight;
    std::optional<std::string> font_style;             // normal, italic
    std::optional<std::string> text_align;
    std::optional<float> letter_spacing;
    std::optional<float> line_height;
    std::optional<std::string> text_transform;
    std::optional<std::string> overflow;               // hidden, scroll, auto
    std::optional<std::string> cursor;
    std::optional<std::string> position;               // absolute, relative
    std::optional<float> top, left, right, bottom;
    std::optional<int> z_index;
    std::optional<std::string> transform;              // rotate, scale, etc.
    std::optional<float> width, height;
    std::optional<float> min_width, min_height;
    std::optional<float> max_width, max_height;
};

/// Layout properties for an IR container node.
struct IRLayout {
    LayoutDirection direction = LayoutDirection::column;
    float gap = 0.0f;
    float padding_top = 0.0f;
    float padding_right = 0.0f;
    float padding_bottom = 0.0f;
    float padding_left = 0.0f;
    LayoutAlign justify = LayoutAlign::flex_start;
    LayoutAlign align = LayoutAlign::stretch;
    bool wrap = false;
    SizingMode width_mode = SizingMode::fixed;
    SizingMode height_mode = SizingMode::fixed;
};

/// Audio widget type detected from naming conventions or annotations.
enum class AudioWidgetType {
    none,       // Not an audio widget
    knob,
    fader,
    meter,
    xy_pad,
    waveform,
    spectrum
};

/// A single node in the normalized design IR.
struct IRNode {
    std::string type;   // "frame", "text", "image", "button", "input", "slider"
    std::string name;
    std::string text_content;            // For text nodes
    IRStyle style;
    IRLayout layout;
    AudioWidgetType audio_widget = AudioWidgetType::none;
    std::string audio_label;             // Label for audio widgets (e.g. "Gain")
    float audio_min = 0.0f;
    float audio_max = 1.0f;
    float audio_default = 0.5f;
    std::vector<IRNode> children;
    std::unordered_map<std::string, std::string> attributes;  // Extra metadata
};

/// Design token collection (W3C-compatible).
struct IRTokens {
    std::unordered_map<std::string, std::string> colors;       // name → "#hex"
    std::unordered_map<std::string, float> dimensions;         // name → px value
    std::unordered_map<std::string, std::string> strings;      // name → string
};

/// Complete design IR document.
struct DesignIR {
    IRNode root;
    IRTokens tokens;
    DesignSource source = DesignSource::figma;
    std::string source_file;
};

// ── Source adapters ─────────────────────────────────────────────────────

/// Parse a Figma export JSON into a DesignIR.
DesignIR parse_figma_json(const std::string& json);

/// Parse Stitch HTML export into a DesignIR.
DesignIR parse_stitch_html(const std::string& html);

/// Parse v0-generated TSX/Tailwind into a DesignIR.
DesignIR parse_v0_tsx(const std::string& tsx);

/// Parse Pencil node tree JSON into a DesignIR.
DesignIR parse_pencil_json(const std::string& json);

/// Detect audio widget type from a node name.
AudioWidgetType detect_audio_widget(const std::string& name);

// ── Code generator ──────────────────────────────────────────────────────

/// Options for code generation.
struct CodeGenOptions {
    bool include_tokens = true;       // Generate token assignments
    bool include_comments = true;     // Generate inline comments
    std::string root_variable = "root";
    int indent_spaces = 2;
};

/// Generate Pulp web-compat JS code from a DesignIR.
std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts = {});

// ── W3C Design Tokens ───────────────────────────────────────────────────

/// Parse W3C Design Tokens JSON into a Pulp Theme.
///
/// W3C format:
/// @code
/// {
///   "color": {
///     "primary": { "$value": "#89B4FA", "$type": "color" },
///     "bg": { "$value": "#1E1E2E", "$type": "color" }
///   },
///   "dimension": {
///     "spacing-md": { "$value": "8px", "$type": "dimension" }
///   }
/// }
/// @endcode
Theme parse_w3c_tokens(const std::string& json);

/// Export a Pulp Theme to W3C Design Tokens JSON format.
std::string export_w3c_tokens(const Theme& theme);

/// Convert IR tokens to a Pulp Theme.
Theme ir_tokens_to_theme(const IRTokens& tokens);

/// Convert a Pulp Theme to W3C-compatible IR tokens.
IRTokens theme_to_ir_tokens(const Theme& theme);

} // namespace pulp::view
