// widget_bridge/layout_api.cpp - layout-related registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

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

void WidgetBridge::register_layout_grid_api() {

    // createGrid(id, parentId) — creates a grid container (CSS display: grid)
    engine_.register_function("createGrid", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>();
        v->set_id(id);
        v->set_layout_mode(LayoutMode::grid);
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });


    // setGrid(id, property, value) — set grid container properties
    engine_.register_function("setGrid", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        auto* v = widget(id);
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
        // pulp #1434 Phase A2-2 — extended grid surface.
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

void WidgetBridge::register_layout_flex_api() {

    engine_.register_function("setFlex", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        View* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto& f = v->flex();
        auto val = args.get<double>(2, 0);
        // pulp-internal #70 — slider/drag jitter diagnostic. When
        // PULP_DEBUG_FLEX_THRASH=1, log every setFlex call so the
        // per-frame style update rate during a slider drag becomes
        // visible. Reading getenv every call is cheap and only fires
        // when the env var is set, so production paths stay quiet.
        static const bool flex_thrash_log = std::getenv("PULP_DEBUG_FLEX_THRASH") != nullptr;
        if (flex_thrash_log) {
            std::string sval;
            try { sval = args.get<std::string>(2, ""); } catch (...) {}
            std::fprintf(stderr, "[flex-thrash] id=%s key=%s num=%.3f str='%s'\n",
                         id.c_str(), key.c_str(), val, sval.c_str());
        }
        // pulp #1434 (rn batch B) — accept all five flexDirection
        // spellings: 'row' / 'column' (CSS / RN canonical), 'col' (legacy
        // pulp shorthand emitted by web-compat-style-decl.js's
        // setFlex(direction)), plus the reverse modes 'row-reverse' /
        // 'column-reverse'. Anything else falls through to column
        // (matches the prior default behavior for unknown values).
        if (key == "direction") {
            auto dir = args.get<std::string>(2, "col");
            if (dir == "row")                 f.direction = FlexDirection::row;
            else if (dir == "row-reverse")    f.direction = FlexDirection::row_reverse;
            else if (dir == "column-reverse") f.direction = FlexDirection::column_reverse;
            else                              f.direction = FlexDirection::column;
        }
        else if (key == "gap") f.gap = (float)val;
        else if (key == "padding") f.padding = (float)val;
        // pulp #1434 (cross-surface mega-batch) — per-edge padding accepts
        // either a number ("10" → px) or a percentage string ("5%" →
        // percent of parent main-axis size). Yoga's padding does NOT
        // support "auto" (only margin does), so unrecognized strings fall
        // through to the px path with the parsed numeric value (or 0 on
        // total parse failure). Mirrors width/height/min/max patterns.
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
        // pulp #1434 (rn batch C) — flex_basis accepts a number ("100"
        // → px), a percentage string ("50%" → percent of parent), or
        // the keyword "auto" (Yoga's YGAuto: "use the preferred size").
        // The unit lives on FlexStyle::dim_flex_basis; yoga_layout.cpp
        // dispatches to YGNodeStyleSetFlexBasis{Percent,Auto} for the
        // non-px paths.
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
            // pulp #1434 Triage #14 — accept numeric (legacy 0/1) and
            // the CSS keyword strings, including the previously-
            // inexpressible `wrap-reverse`.
            auto sval = args.get<std::string>(2, "");
            if (sval == "wrap-reverse")        f.flex_wrap = FlexWrap::wrap_reverse;
            else if (sval == "wrap")           f.flex_wrap = FlexWrap::wrap;
            else if (sval == "nowrap" || sval == "no-wrap") f.flex_wrap = FlexWrap::no_wrap;
            else                                f.flex_wrap = (val > 0) ? FlexWrap::wrap : FlexWrap::no_wrap;
        }
        else if (key == "order") f.order = (int)val;
        // pulp #1423 — width/height accept either a number ("100" → px)
        // or a percentage string ("100%" → percent of parent). The CSS
        // translator passes the raw resolved string for these two keys
        // so the unit survives the bridge boundary; Yoga's native
        // YGNodeStyleSet{Width,Height}Percent path is reached via
        // FlexStyle::dim_width / dim_height in yoga_layout.cpp.
        else if (key == "width") {
            auto sval = args.get<std::string>(2, "");
            // pulp #1434 (sub-agent #12 follow-up) — accept the keyword
            // `'auto'` for "hug contents" sizing. Yoga supports this
            // natively via YGNodeStyleSetWidthAuto. Figma auto-layout,
            // v0 intrinsic-sizing cards, and Claude Design responsive
            // containers all emit this. The dispatch path in
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
        // pulp #1434 (rn batch C) — min/max width/height accept either a
        // number ("100" → px) or a percentage string ("50%" → percent of
        // parent). Same shape as #1426's width/height: store unit on
        // FlexStyle::dim_*; yoga_layout.cpp dispatches to
        // YGNodeStyleSet{Min,Max}{Width,Height}Percent for the percent
        // path and the existing px API otherwise.
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
        // Aspect ratio (pulp #1434) — width/height ratio. A value <= 0 or
        // NaN clears the slot (matches CSS `aspect-ratio: auto`); a finite
        // positive value pins the slot. The CSS shim in
        // web-compat-style-decl.js parses both `1.5` and `16/9` forms
        // before reaching here, and the @pulp/react prop-applier accepts
        // `aspectRatio` directly as a number.
        else if (key == "aspect_ratio" || key == "aspectRatio") {
            if (val > 0.0 && std::isfinite(val)) f.aspect_ratio = (float)val;
            else f.aspect_ratio.reset();
        }
        // Margin
        else if (key == "margin") f.margin = (float)val;
        // pulp #1434 (cross-surface mega-batch) — per-edge margin accepts
        // a number ("10" → px), a percentage string ("5%" → percent of
        // parent main-axis size), or the keyword "auto" (Yoga
        // YGNodeStyleSetMarginAuto — used for centering with
        // `marginLeft: 'auto'; marginRight: 'auto'` etc.). Yoga supports
        // `auto` on margin only, not padding. yoga_layout.cpp dispatches
        // on dim_margin_*.unit; the legacy float field is kept (-1
        // sentinel) so consumers still using uniform `margin` work
        // unchanged.
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
        // pulp #1542 — yoga logical-edge fan-out. `margin_start` /
        // `margin_end` / `padding_start` / `padding_end` / `start` /
        // `end` route to Yoga's YGEdgeStart / YGEdgeEnd which flip
        // with the writing direction (set via `direction_writing` —
        // the existing `direction` key is taken by flex-direction).
        // Same value coverage as the per-side _left/_right siblings:
        // px (number), percent string, plus `auto` for margin.
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
            // pulp DIVERGE→PASS sweep — accept `'auto'` so `inset-inline-
            // start: auto` (CSS) and the equivalent RN style key route
            // through Yoga's `YGNodeStyleSetPositionAuto`. Without this
            // path the keyword fell through to (float)val == 0 and
            // pinned the start edge to 0px.
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
        // pulp #1542 — node writing direction. Distinct from
        // `direction` (which is flex-direction). Accepts `'ltr'`,
        // `'rtl'`, `'inherit'`. Anything else reverts to `inherit`.
        else if (key == "direction_writing") {
            auto a = args.get<std::string>(2, "inherit");
            if (a == "ltr")      f.writing_direction = pulp::view::FlexStyle::WritingDirection::ltr;
            else if (a == "rtl") f.writing_direction = pulp::view::FlexStyle::WritingDirection::rtl;
            else                 f.writing_direction = pulp::view::FlexStyle::WritingDirection::inherit;
        }
        // Directional gap.
        // pulp Wave 2 css.2 — accept a `'NN%'` string for parity with
        // padding/margin edges. Yoga itself does not have a
        // YGNodeStyleSetGapPercent API today, so the percent value is
        // stored on the scalar `row_gap` / `column_gap` slot verbatim
        // (treated as px until the Yoga update lands). This is the same
        // best-effort treatment we apply to other percent-but-Yoga-
        // doesn't-honor cases — the catalog entry stays `partial` and
        // documents the gap.
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
        // pulp #1434 (rn batch B) — accept both bare `start`/`end`
        // (Yoga / pulp short forms) and the `flex-start`/`flex-end`
        // CSS / RN canonical spellings. The CSS shim's _cssToFlex
        // already maps the prefixed forms to the bare ones for the
        // CSS path, but @pulp/react's prop-applier passes RN values
        // through verbatim — so the bridge has to accept both.
        // FlexAlign has no `baseline` variant yet (separate gap;
        // would need YGAlignBaseline plumbing); falls through to
        // stretch / auto_ as before.
        else if (key == "align_items") {
            auto a = args.get<std::string>(2,"stretch");
            if (a=="start" || a=="flex-start") f.align_items=FlexAlign::start;
            else if (a=="center")              f.align_items=FlexAlign::center;
            else if (a=="end" || a=="flex-end") f.align_items=FlexAlign::end;
            // pulp #1434 Tier 1 (css/alignItems) — CSS-spec `first baseline`
            // aliases to plain `baseline`. The baseline-set "first" selector
            // is the default behaviour (and what Yoga's YGAlignBaseline
            // computes), so collapsing them is observable-behaviour-preserving.
            // NOTE: `last baseline` is intentionally NOT aliased — it requires
            // baseline-set tracking (align children against the LAST baseline
            // line in a multi-line flex container) that YGAlignBaseline does
            // not implement. Codex P1 on PR #1853 — keeping `last baseline`
            // in compat.json/unsupportedValues is the honest answer.
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
        // pulp #1434 (sub-agent #12 follow-up) — align_content controls
        // multi-line flex cross-axis distribution. Yoga supports this
        // natively via YGNodeStyleSetAlignContent. Accepts both bare
        // `start`/`end` (Yoga / pulp short forms) and `flex-start` /
        // `flex-end` (CSS / RN canonical). Space-* values (space-between
        // / space-around / space-evenly) live on a sibling enum field
        // (FlexStyle::align_content_space) because FlexAlign has no
        // space variants — those don't make sense for align_items /
        // align_self, only for align_content.
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
            // INTENTIONALLY NOT aliased (Codex P1 on PR #1853, two
            // separate findings, both kept honest):
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

void WidgetBridge::register_layout_query_api() {

    engine_.register_function("layout", [this](choc::javascript::ArgumentList) {
        root_.clear_layout_dirty();
        root_.layout_children();
        request_repaint();
        return choc::value::Value();
    });


    // getLayoutRect(id) -> {x, y, width, height, top, right, bottom, left}
    // Returns layout-resolved bounds in root-relative coordinates.
    //
    // pulp #1899 — force a fresh layout pass before reading bounds.
    // Spectr's editor (and any React-imported tree) calls this via
    // Element.getBoundingClientRect() in mount-time effects to size
    // a canvas / SVG / drawing surface. If the JS commit that mounted
    // the React tree hasn't yet been followed by a yoga_layout pass,
    // the bounds read back as the View's stale default (0×0) — which
    // gates the entire canvas paint pipeline (drawSpectrum/drawRulers
    // bail at getBoundingClientRect == 0). Forcing layout here closes
    // that timing gap. Layout is internally idempotent if nothing has
    // changed, so the cost is bounded to one tree walk per call.
    engine_.register_function("getLayoutRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        root_.layout_children();
        View* v = id.empty() ? &root_ : widget(id);
        return make_layout_rect_value(v);
    });


    // getLayoutAncestorRects(id) -> [{id, bounds}, ...]
    // Same coordinate space as getLayoutRect(), but with the native View
    // parent chain included so validation can localize coordinate drift.
    engine_.register_function("getLayoutAncestorRects", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        root_.layout_children();
        View* v = id.empty() ? &root_ : widget(id);
        return make_layout_ancestor_chain_value(v);
    });


    // getRootSize() -> {width, height} — actual root view dimensions for vw/vh/matchMedia
    engine_.register_function("getRootSize", [this](choc::javascript::ArgumentList) {
        auto b = root_.bounds();
        auto r = choc::value::createObject("");
        r.addMember("width", choc::value::createFloat64(b.width));
        r.addMember("height", choc::value::createFloat64(b.height));
        return r;
    });
}

void WidgetBridge::register_layout_box_model_api() {

    // pulp #1516 — CSS box-sizing keyword. Yoga 3.x's
    // `YGNodeStyleSetBoxSizing` honors the spec, so we just record the
    // enum on FlexStyle and let `build_yoga_subtree` route it through.
    // Default `content-box` matches the CSS spec; web designs typically
    // reset to `border-box` via `* { box-sizing: border-box }`.
    engine_.register_function("setBoxSizing", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "content-box");
        auto* v = id.empty() ? &root_ : widget(id);
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

void WidgetBridge::register_layout_position_api() {

    // setPosition(id, "static"/"relative"/"absolute"/"fixed") — CSS position
    // pulp-internal #70 — instrument under PULP_DEBUG_FLEX_THRASH so we
    // can see whether the JSX's `position: "absolute"` actually reaches
    // the bridge (it has been observed missing for runtime-React
    // imports, which makes inline absolute children render at the
    // wrong place).
    engine_.register_function("setPosition", [this](choc::javascript::ArgumentList args) {
        static const bool flex_thrash_log = std::getenv("PULP_DEBUG_FLEX_THRASH") != nullptr;
        if (flex_thrash_log) {
            std::fprintf(stderr, "[flex-thrash] setPosition id=%s pos=%s\n",
                         args.get<std::string>(0, "").c_str(),
                         args.get<std::string>(1, "").c_str());
        }
        auto id = args.get<std::string>(0, "");
        auto pos = args.get<std::string>(1, "static");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pos == "relative") v->set_position(View::Position::relative);
        else if (pos == "absolute") v->set_position(View::Position::absolute);
        else if (pos == "fixed") v->set_position(View::Position::fixed);
        else if (pos == "sticky") v->set_position(View::Position::sticky);
        else v->set_position(View::Position::static_);
        return choc::value::Value();
    });


    // setTop/setRight/setBottom/setLeft(id, px-or-percent) — CSS positioning
    // offsets. pulp #1434 batch 6 — accept either a number ("50" → px) or
    // a percentage string ("50%" → percent of parent). The CSS translator
    // and @pulp/react prop-applier pass the raw resolved string for these
    // four keys when the value is a percent, so the unit survives the
    // bridge boundary; Yoga's YGNodeStyleSetPositionPercent path is reached
    // via View::top_unit_ / etc. in yoga_layout.cpp. Mirrors the
    // FlexStyle::dim_width pattern from pulp #1423 (PR #1426) for width/height.
    engine_.register_function("setTop", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
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

    engine_.register_function("setRight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
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

    engine_.register_function("setBottom", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
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

    engine_.register_function("setLeft", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
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
    engine_.register_function("setZIndex", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) v->set_z_index(static_cast<int>(args.get<double>(1, 0)));
        return choc::value::Value();
    });
}

} // namespace pulp::view
