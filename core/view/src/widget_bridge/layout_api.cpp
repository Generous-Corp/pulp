// widget_bridge/layout_api.cpp - layout-related registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include "../import_validation_bridge.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void BridgeRegistrars::register_layout_grid_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // createGrid(id, parentId) — creates a grid container (CSS display: grid)
    register_bridge_function(api, "createGrid", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>();
        v->set_id(id);
        v->set_layout_mode(LayoutMode::grid);
        self.widgets_[id] = v.get();
        self.resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });


    // setGrid(id, property, value) — set grid container properties
    register_bridge_function(api, "setGrid", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (!v) return choc::value::Value();

        if (key == "template_columns") {
            auto tmpl = args.get<std::string>(2, "");
            v->grid().template_columns = GridStyle::parse_template(tmpl);
        } else if (key == "template_rows") {
            auto tmpl = args.get<std::string>(2, "");
            v->grid().template_rows = GridStyle::parse_template(tmpl);
        } else if (key == "column_gap") {
            v->grid().column_gap = static_cast<float>(args.get<double>(2, 0));
        } else if (key == "row_gap") {
            v->grid().row_gap = static_cast<float>(args.get<double>(2, 0));
        } else if (key == "gap") {
            float g = static_cast<float>(args.get<double>(2, 0));
            v->grid().column_gap = g;
            v->grid().row_gap = g;
        } else if (key == "column_start") {
            v->grid().grid_column_start = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "column_end") {
            v->grid().grid_column_end = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "row_start") {
            v->grid().grid_row_start = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "row_end") {
            v->grid().grid_row_end = static_cast<int>(args.get<double>(2, 0));
        }
        // Extended CSS grid surface: implicit tracks, auto-flow, template
        // areas, and named/numeric grid-area assignment.
        else if (key == "auto_columns") {
            auto tracks = GridStyle::parse_template(args.get<std::string>(2, "auto"));
            if (!tracks.empty()) v->grid().auto_columns = tracks[0];
        } else if (key == "auto_rows") {
            auto tracks = GridStyle::parse_template(args.get<std::string>(2, "auto"));
            if (!tracks.empty()) v->grid().auto_rows = tracks[0];
        } else if (key == "auto_flow") {
            v->grid().auto_flow = GridStyle::parse_auto_flow(args.get<std::string>(2, "row"));
        } else if (key == "template_areas") {
            v->grid().template_areas = GridStyle::parse_template_areas(args.get<std::string>(2, ""));
        } else if (key == "grid_area") {
            // CSS: `grid-area: header` references a named area on the
            // parent. CSS also accepts `grid-area: 1 / 2 / 3 / 4`
            // (row-start / col-start / row-end / col-end). Distinguish
            // by checking for digits + slashes.
            auto val = args.get<std::string>(2, "");
            if (val.find('/') != std::string::npos) {
                std::vector<int> nums;
                std::string acc;
                for (char c : val) {
                    if (c == '/') {
                        try { nums.push_back(std::stoi(acc)); } catch (...) {}
                        acc.clear();
                    } else if (!std::isspace(static_cast<unsigned char>(c))) acc += c;
                }
                if (!acc.empty()) {
                    try { nums.push_back(std::stoi(acc)); } catch (...) {}
                }
                if (nums.size() >= 4) {
                    v->grid().grid_row_start = nums[0];
                    v->grid().grid_column_start = nums[1];
                    v->grid().grid_row_end = nums[2];
                    v->grid().grid_column_end = nums[3];
                }
            } else {
                v->grid().grid_area_name = val;
            }
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_layout_flex_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setFlex", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        View* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        auto& f = v->flex();
        auto val = args.get<double>(2, 0);
        // Slider/drag jitter diagnostic. When PULP_DEBUG_FLEX_THRASH is set to
        // any value, log every setFlex call so per-frame style churn during a
        // drag is visible.
        // The getenv result is cached on first call and only emits when enabled.
        static const bool flex_thrash_log = std::getenv("PULP_DEBUG_FLEX_THRASH") != nullptr;
        if (flex_thrash_log) {
            std::string sval;
            try { sval = args.get<std::string>(2, ""); } catch (...) {}
            std::fprintf(stderr, "[flex-thrash] id=%s key=%s num=%.3f str='%s'\n",
                         id.c_str(), key.c_str(), val, sval.c_str());
        }
        // Accept the flexDirection spellings used by CSS/RN: `row`,
        // `column`, and the reverse modes. `column`, the bridge shorthand
        // `col`, and unknown values all resolve to column.
        if (key == "direction") {
            auto dir = args.get<std::string>(2, "col");
            if (dir == "row")                 f.direction = FlexDirection::row;
            else if (dir == "row-reverse")    f.direction = FlexDirection::row_reverse;
            else if (dir == "column-reverse") f.direction = FlexDirection::column_reverse;
            else                              f.direction = FlexDirection::column;
        }
        else if (key == "gap") f.gap = (float)val;
        else if (key == "padding") f.padding = (float)val;
        // Per-edge padding accepts either a number ("10" -> px) or a
        // percentage string ("5%" -> percent of the parent's width / inline
        // size). Yoga padding does not support "auto", so unrecognized strings
        // fall through to the px path with the parsed numeric value, or 0 on
        // total parse failure. Mirrors width/height/min/max patterns.
        else if (key == "padding_top") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_top.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_top.unit = pulp::view::DimensionUnit::percent;
                    f.padding_top = -1; // sentinel
                } catch (...) { /* keep current */ }
            } else {
                f.padding_top = (float)val;
                f.dim_padding_top.value = (float)val;
                f.dim_padding_top.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_right") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_right.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_right.unit = pulp::view::DimensionUnit::percent;
                    f.padding_right = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.padding_right = (float)val;
                f.dim_padding_right.value = (float)val;
                f.dim_padding_right.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_bottom") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_bottom.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_bottom.unit = pulp::view::DimensionUnit::percent;
                    f.padding_bottom = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.padding_bottom = (float)val;
                f.dim_padding_bottom.value = (float)val;
                f.dim_padding_bottom.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_left") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_left.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_left.unit = pulp::view::DimensionUnit::percent;
                    f.padding_left = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.padding_left = (float)val;
                f.dim_padding_left.value = (float)val;
                f.dim_padding_left.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "flex_grow") f.flex_grow = (float)val;
        else if (key == "flex_shrink") f.flex_shrink = (float)val;
        // flex_basis accepts a number ("100" -> px), a percentage string
        // ("50%" -> percent of parent), or "auto" for Yoga's preferred-size
        // path. The unit lives on FlexStyle::dim_flex_basis; yoga_layout.cpp
        // dispatches to YGNodeStyleSetFlexBasis{Percent,Auto}.
        else if (key == "flex_basis") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_flex_basis.unit = pulp::view::DimensionUnit::auto_;
                f.flex_basis = -1; // sentinel: use preferred
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_flex_basis.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_flex_basis.unit = pulp::view::DimensionUnit::percent;
                    f.flex_basis = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.flex_basis = (float)val;
                f.dim_flex_basis.value = (float)val;
                f.dim_flex_basis.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "flex_wrap") {
            // Accept numeric bridge values and CSS keyword strings, including
            // `wrap-reverse`.
            auto sval = args.get<std::string>(2, "");
            if (sval == "wrap-reverse")        f.flex_wrap = FlexWrap::wrap_reverse;
            else if (sval == "wrap")           f.flex_wrap = FlexWrap::wrap;
            else if (sval == "nowrap" || sval == "no-wrap") f.flex_wrap = FlexWrap::no_wrap;
            else                                f.flex_wrap = (val > 0) ? FlexWrap::wrap : FlexWrap::no_wrap;
        }
        else if (key == "order") f.order = (int)val;
        // width/height accept either a number ("100" -> px) or a percentage
        // string ("100%" -> percent of parent). The CSS translator passes the
        // raw resolved string so the unit survives the bridge boundary;
        // yoga_layout.cpp routes percent values through FlexStyle::dim_*.
        else if (key == "width") {
            auto sval = args.get<std::string>(2, "");
            // Accept `'auto'` for hug-contents sizing. Yoga supports this
            // natively via YGNodeStyleSetWidthAuto. Figma auto-layout,
            // intrinsic-sizing cards, and responsive containers all emit this.
            // yoga_layout.cpp keys on `dim_width.unit == auto_`.
            if (sval == "auto") {
                f.dim_width.unit = pulp::view::DimensionUnit::auto_;
                f.dim_width.value = 0;
                f.preferred_width = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_width.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_width.unit = pulp::view::DimensionUnit::percent;
                    f.preferred_width = 0;  // disambiguate: percent path
                } catch (...) { /* keep current state on parse fail */ }
            } else {
                f.preferred_width = (float)val;
                f.dim_width.value = (float)val;
                f.dim_width.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "height") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_height.unit = pulp::view::DimensionUnit::auto_;
                f.dim_height.value = 0;
                f.preferred_height = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_height.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_height.unit = pulp::view::DimensionUnit::percent;
                    f.preferred_height = 0;
                } catch (...) { /* keep current state on parse fail */ }
            } else {
                f.preferred_height = (float)val;
                f.dim_height.value = (float)val;
                f.dim_height.unit = pulp::view::DimensionUnit::px;
            }
        }
        // min/max width/height accept either a number ("100" -> px) or a
        // percentage string ("50%" -> percent of parent). Units are stored on
        // FlexStyle::dim_*; yoga_layout.cpp dispatches to Yoga's percent path
        // or the existing px API.
        else if (key == "min_width") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_min_width.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_min_width.unit = pulp::view::DimensionUnit::percent;
                    f.min_width = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.min_width = (float)val;
                f.dim_min_width.value = (float)val;
                f.dim_min_width.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "min_height") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_min_height.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_min_height.unit = pulp::view::DimensionUnit::percent;
                    f.min_height = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.min_height = (float)val;
                f.dim_min_height.value = (float)val;
                f.dim_min_height.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "max_width") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_max_width.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_max_width.unit = pulp::view::DimensionUnit::percent;
                    f.max_width = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.max_width = (float)val;
                f.dim_max_width.value = (float)val;
                f.dim_max_width.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "max_height") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_max_height.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_max_height.unit = pulp::view::DimensionUnit::percent;
                    f.max_height = 0;
                } catch (...) { /* keep current */ }
            } else {
                f.max_height = (float)val;
                f.dim_max_height.value = (float)val;
                f.dim_max_height.unit = pulp::view::DimensionUnit::px;
            }
        }
        // Aspect ratio is the width/height ratio. Any non-finite or
        // non-positive value clears the slot, matching CSS `aspect-ratio: auto`;
        // a finite positive value pins it. The CSS shim parses both `1.5` and
        // `16/9` before reaching here, and @pulp/react accepts `aspectRatio`
        // directly as a number.
        else if (key == "aspect_ratio" || key == "aspectRatio") {
            if (val > 0.0 && std::isfinite(val)) f.aspect_ratio = (float)val;
            else f.aspect_ratio.reset();
        }
        // Margin
        else if (key == "margin") f.margin = (float)val;
        // Per-edge margin accepts a number ("10" -> px), a percentage string
        // ("5%" -> percent of the parent's width / inline size), or "auto" for
        // Yoga's YGNodeStyleSetMarginAuto path. Yoga supports `auto` on margin
        // only, not padding. yoga_layout.cpp dispatches on dim_margin_*.unit;
        // the legacy float field uses -1 as a sentinel so uniform `margin`
        // consumers keep working.
        else if (key == "margin_top") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_top.unit = pulp::view::DimensionUnit::auto_;
                f.margin_top = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_top.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_top.unit = pulp::view::DimensionUnit::percent;
                    f.margin_top = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_top = (float)val;
                f.dim_margin_top.value = (float)val;
                f.dim_margin_top.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_right") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_right.unit = pulp::view::DimensionUnit::auto_;
                f.margin_right = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_right.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_right.unit = pulp::view::DimensionUnit::percent;
                    f.margin_right = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_right = (float)val;
                f.dim_margin_right.value = (float)val;
                f.dim_margin_right.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_bottom") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_bottom.unit = pulp::view::DimensionUnit::auto_;
                f.margin_bottom = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_bottom.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_bottom.unit = pulp::view::DimensionUnit::percent;
                    f.margin_bottom = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_bottom = (float)val;
                f.dim_margin_bottom.value = (float)val;
                f.dim_margin_bottom.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_left") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_left.unit = pulp::view::DimensionUnit::auto_;
                f.margin_left = -1;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_left.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_left.unit = pulp::view::DimensionUnit::percent;
                    f.margin_left = -1;
                } catch (...) { /* keep current */ }
            } else {
                f.margin_left = (float)val;
                f.dim_margin_left.value = (float)val;
                f.dim_margin_left.unit = pulp::view::DimensionUnit::px;
            }
        }
        // Logical-edge fan-out. `margin_start`, `margin_end`,
        // `padding_start`, `padding_end`, `start`, and `end` route to Yoga's
        // YGEdgeStart / YGEdgeEnd, which flip with `direction_writing` (the
        // existing `direction` key is flex-direction). Same value coverage as
        // the _left/_right siblings: px, percent, plus `auto` for margin.
        else if (key == "margin_start") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_start.unit = pulp::view::DimensionUnit::auto_;
                f.dim_margin_start.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_start.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_start.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_margin_start.value = (float)val;
                f.dim_margin_start.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "margin_end") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_margin_end.unit = pulp::view::DimensionUnit::auto_;
                f.dim_margin_end.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_margin_end.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_margin_end.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_margin_end.value = (float)val;
                f.dim_margin_end.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_start") {
            // Padding has no `auto` API (Yoga); reject the keyword and
            // fall through to the px path with a parsed value (or 0
            // on parse failure) — same behavior as the per-side
            // padding_top/right/bottom/left handlers above.
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_start.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_start.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_padding_start.value = (float)val;
                f.dim_padding_start.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "padding_end") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_padding_end.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_padding_end.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_padding_end.value = (float)val;
                f.dim_padding_end.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "start") {
            // Accept `'auto'` so `inset-inline-start: auto` and the equivalent
            // RN style key route through Yoga's `YGNodeStyleSetPositionAuto`.
            // Otherwise the keyword would parse as 0 and pin the edge to 0px.
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_start.unit = pulp::view::DimensionUnit::auto_;
                f.dim_start.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_start.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_start.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_start.value = (float)val;
                f.dim_start.unit = pulp::view::DimensionUnit::px;
            }
        }
        else if (key == "end") {
            auto sval = args.get<std::string>(2, "");
            if (sval == "auto") {
                f.dim_end.unit = pulp::view::DimensionUnit::auto_;
                f.dim_end.value = 0;
            } else if (!sval.empty() && sval.back() == '%') {
                try {
                    f.dim_end.value = std::stof(sval.substr(0, sval.size() - 1));
                    f.dim_end.unit = pulp::view::DimensionUnit::percent;
                } catch (...) { /* keep current */ }
            } else {
                f.dim_end.value = (float)val;
                f.dim_end.unit = pulp::view::DimensionUnit::px;
            }
        }
        // Node writing direction, distinct from `direction` (flex-direction).
        // Accepts `'ltr'`, `'rtl'`, and `'inherit'`; anything else reverts to
        // `inherit`.
        else if (key == "direction_writing") {
            auto a = args.get<std::string>(2, "inherit");
            if (a == "ltr")      f.writing_direction = pulp::view::FlexStyle::WritingDirection::ltr;
            else if (a == "rtl") f.writing_direction = pulp::view::FlexStyle::WritingDirection::rtl;
            else                 f.writing_direction = pulp::view::FlexStyle::WritingDirection::inherit;
        }
        // Directional gap.
        // Accept a `'NN%'` string for parity with padding/margin edges. Yoga
        // does not have a YGNodeStyleSetGapPercent API today, so the percent
        // value is stored on the scalar gap slot and treated as px until Yoga
        // grows percent-gap support. The catalog entry remains `partial`.
        else if (key == "row_gap") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try { f.row_gap = std::stof(sval.substr(0, sval.size() - 1)); }
                catch (...) { /* keep current */ }
            } else {
                f.row_gap = (float)val;
            }
        }
        else if (key == "column_gap") {
            auto sval = args.get<std::string>(2, "");
            if (!sval.empty() && sval.back() == '%') {
                try { f.column_gap = std::stof(sval.substr(0, sval.size() - 1)); }
                catch (...) { /* keep current */ }
            } else {
                f.column_gap = (float)val;
            }
        }
        // Alignment.
        // Accept both bare `start`/`end` bridge forms and the `flex-start` /
        // `flex-end` CSS/RN spellings. The CSS shim maps prefixed forms to the
        // bare ones, but @pulp/react's prop-applier passes RN values through
        // verbatim, so the bridge has to accept both.
        else if (key == "align_items") {
            auto a = args.get<std::string>(2,"stretch");
            if (a=="start" || a=="flex-start") f.align_items=FlexAlign::start;
            else if (a=="center")              f.align_items=FlexAlign::center;
            else if (a=="end" || a=="flex-end") f.align_items=FlexAlign::end;
            // CSS `first baseline` aliases to plain `baseline`. The baseline-set
            // "first" selector is the default behavior and what Yoga's
            // YGAlignBaseline computes, so collapsing them preserves behavior.
            // `last baseline` is intentionally not aliased: it requires
            // baseline-set tracking that Yoga does not implement, so compat
            // keeps it in unsupportedValues.
            else if (a=="baseline" || a=="first baseline")
                                                f.align_items=FlexAlign::baseline;
            else                               f.align_items=FlexAlign::stretch;
        }
        else if (key == "align_self") {
            auto a = args.get<std::string>(2,"auto");
            if (a=="start" || a=="flex-start") f.align_self=FlexAlign::start;
            else if (a=="center")              f.align_self=FlexAlign::center;
            else if (a=="end" || a=="flex-end") f.align_self=FlexAlign::end;
            else if (a=="stretch")             f.align_self=FlexAlign::stretch;
            else if (a=="baseline")            f.align_self=FlexAlign::baseline;
            else                               f.align_self=FlexAlign::auto_;
        }
        // align_content controls multi-line flex cross-axis distribution.
        // Yoga supports this natively via YGNodeStyleSetAlignContent. Accepts
        // both bare `start`/`end` bridge forms and `flex-start` / `flex-end`
        // CSS/RN spellings. Space-* values live on FlexStyle's sibling
        // align_content_space enum because FlexAlign has no space variants.
        else if (key == "align_content") {
            auto a = args.get<std::string>(2,"start");
            using AcSpace = pulp::view::FlexStyle::AlignContentSpace;
            // Reset space slot first; only the space-* branches set it.
            f.align_content_space = AcSpace::none;
            if (a=="start" || a=="flex-start")     f.align_content=FlexAlign::start;
            else if (a=="center")                  f.align_content=FlexAlign::center;
            else if (a=="end" || a=="flex-end")    f.align_content=FlexAlign::end;
            else if (a=="stretch")                 f.align_content=FlexAlign::stretch;
            else if (a=="space-between"||a=="space_between") {
                f.align_content_space = AcSpace::space_between;
            }
            else if (a=="space-around"||a=="space_around") {
                f.align_content_space = AcSpace::space_around;
            }
            else if (a=="space-evenly"||a=="space_evenly") {
                f.align_content_space = AcSpace::space_evenly;
            }
            else                                   f.align_content=FlexAlign::start;
        }
        else if (key == "justify_content") {
            auto j = args.get<std::string>(2,"start");
            // Intentionally not aliased:
            //
            //   `left` / `right` — direction-context-dependent. CSS spec:
            //       on a row container, `right` ≡ flex-end (LTR) /
            //       flex-start (RTL).
            //       on a column container, BOTH `left` and `right`
            //       behave as `start` (per CSS Box Alignment §4.3 —
            //       "If the property's axis is parallel to the inline
            //       axis, behaves as flex-end; otherwise behaves as
            //       start"). A direction-agnostic alias would silently
            //       misrender vertical flex containers. The fully-correct
            //       fix would peek at the widget's flex-direction here
            //       and dispatch accordingly; that's more state coupling
            //       than this dispatcher carries today. Keep both values
            //       in compat.json/unsupportedValues until we either
            //       add direction-aware aliasing or RTL writing-mode
            //       support arrives.
            //
            //   `stretch` — grows AUTO-sized items equally; FlexJustify
            //       has no equivalent. Aliasing to flex-start would
            //       silently misrender stretching layouts. Documented
            //       unsupported.
            //
            //   `normal`  — per CSS spec, "behaves as `stretch`" on flex
            //       containers, so it inherits the same problem.
            //       Documented unsupported.
            if (j=="start" || j=="flex-start")                f.justify_content=FlexJustify::start;
            else if (j=="center")                             f.justify_content=FlexJustify::center;
            else if (j=="end" || j=="flex-end")               f.justify_content=FlexJustify::end_;
            else if (j=="space-between"||j=="space_between")  f.justify_content=FlexJustify::space_between;
            else if (j=="space-around"||j=="space_around")    f.justify_content=FlexJustify::space_around;
            else if (j=="space-evenly"||j=="space_evenly")    f.justify_content=FlexJustify::space_evenly;
            else                                              f.justify_content=FlexJustify::start;
        }
        v->invalidate_layout();  // auto-invalidation on flex property change
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_layout_query_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "layout", [&self](choc::javascript::ArgumentList) {
        self.root_.clear_layout_dirty();
        self.root_.layout_children();
        self.request_repaint();
        return choc::value::Value();
    });


    // getLayoutRect(id) -> {x, y, width, height, top, right, bottom, left}
    // Returns layout-resolved bounds in root-relative coordinates.
    //
    // Force a fresh layout pass before reading bounds. Spectr's editor and any
    // React-imported tree call this via
    // Element.getBoundingClientRect() in mount-time effects to size
    // a canvas / SVG / drawing surface. If the JS commit that mounted
    // the React tree hasn't yet been followed by a yoga_layout pass,
    // the bounds read back as the View's stale default (0×0) — which
    // gates the entire canvas paint pipeline (drawSpectrum/drawRulers
    // bail at getBoundingClientRect == 0). Forcing layout here closes
    // that timing gap. Layout is internally idempotent if nothing has
    // changed, so the cost is bounded to one tree walk per call.
    register_bridge_function(api, "getLayoutRect", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        self.root_.layout_children();
        View* v = id.empty() ? &self.root_ : self.widget(id);
        return make_layout_rect_value(v);
    });


    // getLayoutAncestorRects(id) -> [{id, bounds}, ...]
    // Same coordinate space as getLayoutRect(), but with the native View
    // parent chain included so validation can localize coordinate drift.
    register_bridge_function(api, "getLayoutAncestorRects", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        self.root_.layout_children();
        View* v = id.empty() ? &self.root_ : self.widget(id);
        return make_layout_ancestor_chain_value(v);
    });


    // getRootSize() -> {width, height} — actual root view dimensions for vw/vh/matchMedia
    register_bridge_function(api, "getRootSize", [&self](choc::javascript::ArgumentList) {
        auto b = self.root_.bounds();
        auto r = choc::value::createObject("");
        r.addMember("width", choc::value::createFloat64(b.width));
        r.addMember("height", choc::value::createFloat64(b.height));
        return r;
    });
}

void BridgeRegistrars::register_layout_box_model_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // CSS box-sizing keyword. Yoga's `YGNodeStyleSetBoxSizing` honors the spec,
    // so record the enum on FlexStyle and let `build_yoga_subtree` route it
    // through.
    // The unset FlexStyle default is `border-box` to match Yoga/imported web
    // layout convention; this setter maps absent or unknown keywords to
    // `content-box`, the CSS-spec keyword default.
    register_bridge_function(api, "setBoxSizing", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "content-box");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        auto& f = v->flex();
        if (kw == "border-box") {
            f.box_sizing = BoxSizing::border_box;
        } else if (kw == "inherit") {
            f.box_sizing = v->parent() ? v->parent()->flex().box_sizing
                                       : BoxSizing::content_box;
        } else {
            f.box_sizing = BoxSizing::content_box;
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_layout_position_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setPosition(id, "static"/"relative"/"absolute"/"fixed"/"sticky") — CSS position
    // Instrument under PULP_DEBUG_FLEX_THRASH so we can see whether JSX
    // `position: "absolute"` reaches the bridge. When runtime React imports
    // miss this path, inline absolute children render at the wrong place.
    register_bridge_function(api, "setPosition", [&self](choc::javascript::ArgumentList args) {
        static const bool flex_thrash_log = std::getenv("PULP_DEBUG_FLEX_THRASH") != nullptr;
        if (flex_thrash_log) {
            std::fprintf(stderr, "[flex-thrash] setPosition id=%s pos=%s\n",
                         args.get<std::string>(0, "").c_str(),
                         args.get<std::string>(1, "").c_str());
        }
        auto id = args.get<std::string>(0, "");
        auto pos = args.get<std::string>(1, "static");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (pos == "relative") v->set_position(View::Position::relative);
        else if (pos == "absolute") v->set_position(View::Position::absolute);
        else if (pos == "fixed") v->set_position(View::Position::fixed);
        else if (pos == "sticky") v->set_position(View::Position::sticky);
        else v->set_position(View::Position::static_);
        return choc::value::Value();
    });


    // setSubpixelLayout(id, enabled) — opt the layout pass containing this
    // view out of Yoga's whole-pixel grid rounding. Design-import codegen
    // emits it on the imported root: the script replays geometry Figma
    // already solved at fractional coordinates, and per-node rounding
    // visibly de-centers concentric siblings (a knob's value-ring arc vs
    // its body ellipse). Pass-wide by design — one Yoga pass has one
    // config — so hand-authored flowed UIs keep the pixel-grid default
    // unless a design subtree is present.
    register_bridge_function(api, "setSubpixelLayout", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_subpixel_layout(args.get<bool>(1, true));
        return choc::value::Value();
    });

    // setTop/setRight/setBottom/setLeft(id, px-or-percent) — CSS positioning
    // offsets. Accept either a number ("50" -> px) or a percentage string
    // ("50%" -> percent of parent). The CSS translator and @pulp/react
    // prop-applier pass raw percent strings for these keys, so the unit
    // survives the bridge boundary; yoga_layout.cpp routes them through
    // View::top_unit_ / right_unit_ / bottom_unit_ / left_unit_.
    register_bridge_function(api, "setTop", [&self](choc::javascript::ArgumentList args) {
        auto* v = self.widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_top(std::stof(sval.substr(0, sval.size() - 1)),
                               pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_top(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_top(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setRight", [&self](choc::javascript::ArgumentList args) {
        auto* v = self.widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_right(std::stof(sval.substr(0, sval.size() - 1)),
                                 pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_right(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_right(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setBottom", [&self](choc::javascript::ArgumentList args) {
        auto* v = self.widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_bottom(std::stof(sval.substr(0, sval.size() - 1)),
                                  pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_bottom(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_bottom(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setLeft", [&self](choc::javascript::ArgumentList args) {
        auto* v = self.widget(args.get<std::string>(0, ""));
        if (v) {
            auto sval = args.get<std::string>(1, "");
            if (!sval.empty() && sval.back() == '%') {
                try {
                    v->set_left(std::stof(sval.substr(0, sval.size() - 1)),
                                pulp::view::DimensionUnit::percent);
                } catch (...) { v->set_left(static_cast<float>(args.get<double>(1, 0))); }
            } else {
                v->set_left(static_cast<float>(args.get<double>(1, 0)));
            }
        }
        return choc::value::Value();
    });


    // setZIndex(id, n) — CSS z-index
    register_bridge_function(api, "setZIndex", [&self](choc::javascript::ArgumentList args) {
        auto* v = self.widget(args.get<std::string>(0, ""));
        if (v) v->set_z_index(static_cast<int>(args.get<double>(1, 0)));
        return choc::value::Value();
    });
}

} // namespace pulp::view
