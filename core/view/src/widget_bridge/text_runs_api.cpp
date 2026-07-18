// widget_bridge/text_runs_api.cpp - text-run typography registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <pulp/canvas/attributed_string.hpp>

#include <algorithm>
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
    // offsets) so single-line mixed text renders each run with its own style
    // (the native equivalent of the web nested-<span> path). The dominant style
    // is read from the Label (codegen emits the single-style setters first).
    register_bridge_function(api, "setTextRuns", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* l = dynamic_cast<Label*>(self.widget(id));
        if (!l || args.numArgs < 2 || !args[1] || !args[1]->isArray())
            return choc::value::Value();
        const std::string& text = l->text();
        const int n = static_cast<int>(text.size());

        canvas::TextSpan base;
        if (!l->font_family().empty()) base.font_family = l->font_family();
        if (l->font_size() > 0.0f)     base.font_size = l->font_size();
        base.font_weight = l->font_weight();
        base.italic = (l->font_style() == 1);
        if (l->has_own_text_color()) base.color = l->text_color();

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
            if (r.hasObjectMember("fontWeight"))
                span.font_weight = static_cast<int>(r["fontWeight"].getWithDefault<int64_t>(span.font_weight));
            if (r.hasObjectMember("fontSize"))
                span.font_size = static_cast<float>(r["fontSize"].getWithDefault<double>(span.font_size));
            if (r.hasObjectMember("color"))
                span.color = parse_bridge_css_color(std::string(r["color"].toString()));
            if (r.hasObjectMember("fontStyle"))
                span.italic = (std::string(r["fontStyle"].toString()) == "italic");
            if (r.hasObjectMember("letterSpacing"))
                span.letter_spacing = static_cast<float>(r["letterSpacing"].getWithDefault<double>(span.letter_spacing));
            runs.push_back({std::max(0, s), std::min(n, e), span});
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
