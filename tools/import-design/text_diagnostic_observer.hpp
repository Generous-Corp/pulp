// SPDX-License-Identifier: MIT
#pragma once

#include <pulp/view/widgets.hpp>
#include <choc/text/choc_JSON.h>

namespace pulp::import_design {

// Opt-in on a disposable observer tree, before its sole render. No highlight
// is installed: selection geometry is populated by the normal Label painter.
inline void enable_text_observation(view::View& root) {
    if (auto* label = dynamic_cast<view::Label*>(&root))
        label->set_selection_policy(view::Label::SelectionPolicy::always);
    for (std::size_t i = 0; i < root.child_count(); ++i)
        enable_text_observation(*root.child_at(i));
}

inline void observe_text_tree(const view::View& node,
                              float parent_x, float parent_y,
                              bool supported, choc::value::Value& rows) {
    const auto bounds = node.bounds();
    const float x = parent_x + bounds.x, y = parent_y + bounds.y;
    supported = supported && node.visible() && !node.has_render_transform();
    if (const auto* label = dynamic_cast<const view::Label*>(&node)) {
        auto row = choc::value::createObject("");
        row.addMember("anchor", std::string(node.anchor_id()));
        row.addMember("text", label->text());
        // This is the paint request, not a resolved SkTypeface census.
        row.addMember("paint_font_request", label->effective_font_family());
        row.addMember("paint_font_weight", label->effective_font_weight());
        const bool text_supported = label->text_transform() == view::Label::TextTransform::none &&
            label->text_direction() == canvas::TextDirection::left_to_right &&
            !label->has_attributed_string();
        row.addMember("coordinate_supported", supported && text_supported);
        const auto layout = label->selectable_layout();
        row.addMember("measured", layout.measured);
        auto lines = choc::value::createEmptyArray();
        if (supported && text_supported && layout.measured) {
            for (const auto& line : layout.lines) {
                auto value = choc::value::createObject("");
                value.addMember("start_utf8", line.start_utf8);
                value.addMember("end_utf8", line.end_utf8);
                value.addMember("selection_top", y + line.top);
                value.addMember("selection_height", line.height);
                auto positions = choc::value::createEmptyArray();
                auto offsets = choc::value::createEmptyArray();
                for (float position : line.x_offsets) positions.addArrayElement(x + position);
                for (int offset : line.byte_offsets) offsets.addArrayElement(offset);
                value.addMember("caret_x", std::move(positions));
                value.addMember("byte_offsets", std::move(offsets));
                lines.addArrayElement(std::move(value));
            }
        }
        row.addMember("lines", std::move(lines));
        rows.addArrayElement(std::move(row));
    }
    for (std::size_t i = 0; i < node.child_count(); ++i)
        observe_text_tree(*node.child_at(i), x, y,
                          supported && !node.applies_child_paint_offset(), rows);
}

inline std::string observe_text(const view::View& root, const std::string& input) {
    auto report = choc::value::createObject("");
    report.addMember("schema", "pulp-native-selection-diagnostics-v1");
    report.addMember("input", input);
    report.addMember("coordinate_space", "observer-root logical pixels; untransformed only");
    report.addMember("viewport_width", root.bounds().width);
    report.addMember("viewport_height", root.bounds().height);
    report.addMember("source", "Label::selectable_layout after Skia render");
    report.addMember("limitations", "Selection bands and caret positions are not glyph ink, glyph advances, baselines or a resolved-face census");
    auto rows = choc::value::createEmptyArray();
    observe_text_tree(root, 0, 0, true, rows);
    report.addMember("runs", std::move(rows));
    return choc::json::toString(report, true);
}

} // namespace pulp::import_design
