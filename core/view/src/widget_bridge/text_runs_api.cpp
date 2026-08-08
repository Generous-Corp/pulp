// widget_bridge/text_runs_api.cpp - text-run typography registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <pulp/canvas/attributed_string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void BridgeRegistrars::register_widget_text_runs_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setTextRuns(id, [{start,end,fontWeight?,fontSize?,color?,fontStyle?,
    // letterSpacing?}, ...]) — per-range styled text. Builds a
    // canvas::AttributedString over the Label's text (offsets are UTF-8 BYTE
    // offsets) so mixed text renders each run with its own style through both
    // captured and responsively reflowed lines (the native equivalent of the
    // web nested-<span> path). The dominant style
    // is read from the Label (codegen emits the single-style setters first).
    register_bridge_function(api, "setTextRuns", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* l = dynamic_cast<Label*>(self.widget(id));
        if (!l || args.numArgs < 2 || !args[1] || !args[1]->isArray())
            return choc::value::Value();
        const std::string& text = l->text();
        const int n = static_cast<int>(text.size());
        const auto snap_utf8 = [&](int offset) {
            offset = std::clamp(offset, 0, n);
            while (offset > 0 && offset < n &&
                   (static_cast<unsigned char>(text[
                        static_cast<std::size_t>(offset)]) & 0xC0) == 0x80) {
                ++offset;
            }
            return offset;
        };

        canvas::TextSpan base;
        base.inherit_font_family = l->font_family().empty();
        base.inherit_font_size = !l->has_own_font_size();
        base.inherit_font_weight = !l->has_own_font_weight();
        base.inherit_color = !l->has_own_text_color();
        base.inherit_letter_spacing = !l->has_own_letter_spacing();
        if (!base.inherit_font_family) base.font_family = l->font_family();
        if (!base.inherit_font_size)   base.font_size = l->font_size();
        base.font_weight = l->font_weight();
        base.italic = (l->font_style() != 0);
        base.font_slant = l->font_style();
        base.letter_spacing = l->letter_spacing();
        if (!base.inherit_color) base.color = l->text_color();

        struct Run { int s, e; canvas::TextSpan span; };
        std::vector<Run> runs;
        auto& arr = *args[1];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            auto r = arr[i];
            if (!r.isObject()) continue;
            int s = static_cast<int>(r["start"].getWithDefault<int64_t>(0));
            int e = static_cast<int>(r["end"].getWithDefault<int64_t>(0));
            if (e <= s || s >= n) continue;
            canvas::TextSpan span = base;  // inherit dominant, override below
            if (r.hasObjectMember("fontWeight")) {
                span.font_weight = static_cast<int>(r["fontWeight"].getWithDefault<int64_t>(span.font_weight));
                span.inherit_font_weight = false;
            }
            if (r.hasObjectMember("fontSize")) {
                span.font_size = static_cast<float>(r["fontSize"].getWithDefault<double>(span.font_size));
                span.inherit_font_size = false;
            }
            if (r.hasObjectMember("fontFamily")) {
                span.font_family = std::string(r["fontFamily"].toString());
                span.inherit_font_family = false;
            }
            if (r.hasObjectMember("color")) {
                span.color = parse_bridge_css_color(std::string(r["color"].toString()));
                span.inherit_color = false;
            }
            if (r.hasObjectMember("fontStyle"))
            {
                auto slant = std::string(r["fontStyle"].toString());
                std::transform(slant.begin(), slant.end(), slant.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                span.font_slant = slant.rfind("oblique", 0) == 0 ? 2
                                : slant == "italic" ? 1 : 0;
                span.italic = span.font_slant != 0;
                span.inherit_font_slant = false;
            }
            if (r.hasObjectMember("letterSpacing")) {
                span.letter_spacing = static_cast<float>(r["letterSpacing"].getWithDefault<double>(span.letter_spacing));
                span.inherit_letter_spacing = false;
            }
            if (r.hasObjectMember("textDecoration")) {
                span.decoration_override = true;
                const auto decoration = std::string(r["textDecoration"].toString());
                if (decoration.find("underline") != std::string::npos)
                    span.decoration = canvas::TextDecoration::underline;
                else if (decoration.find("line-through") != std::string::npos)
                    span.decoration = canvas::TextDecoration::strikethrough;
                else if (decoration.find("overline") != std::string::npos)
                    span.decoration = canvas::TextDecoration::overline;
            }
            runs.push_back({snap_utf8(s), snap_utf8(e), span});
        }
        std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.s < b.s; });

        canvas::AttributedString attr;
        auto add = [&](int a, int b, const canvas::TextSpan& proto) {
            if (b <= a) return;
            canvas::TextSpan sp = proto; sp.text = text.substr(a, b - a); attr.append(sp);
        };
        int cursor = 0;
        for (auto& rn : runs) {
            int s = rn.s, e = rn.e;
            if (e <= cursor) continue;
            if (s < cursor) s = cursor;
            add(cursor, s, base);     // gap inherits the dominant style
            add(s, e, rn.span);       // styled run
            cursor = e;
        }
        add(cursor, n, base);         // trailing
        l->set_attributed_string(std::move(attr));
        return choc::value::Value();
    });
}

} // namespace pulp::view
