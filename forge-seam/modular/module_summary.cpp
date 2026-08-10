#include "forge/module_summary.hpp"

#include <choc/text/choc_JSON.h>
#include <forge/design_tokens.hpp>
#include <pulp/view/widgets.hpp>

#include <fstream>
#include <map>
#include <sstream>

namespace forge_modular {
namespace {

using pulp::view::FlexDirection;
using pulp::view::Label;
namespace color = forge::design::color;

/// "4 knobs · 1 slider", counted by kind and left out when the count is zero.
std::string tally(const std::map<std::string, int>& counts) {
    std::string out;
    for (const auto& [kind, n] : counts) {
        if (n <= 0) continue;
        if (!out.empty()) out += " \xC2\xB7 ";
        out += std::to_string(n) + " " + kind + (n == 1 ? "" : "s");
    }
    return out;
}

}  // namespace

bool ModuleSummary::set_manifest(const std::string& path) {
    rows_.clear();
    description_.clear();

    std::ifstream f(path);
    if (!f) { rebuild(); return false; }
    std::stringstream ss;
    ss << f.rdbuf();

    choc::value::Value root;
    try {
        root = choc::json::parse(ss.str());
    } catch (...) {
        rebuild();
        return false;
    }
    if (!root.isObject() || !root.hasObjectMember("modules")) {
        rebuild();
        return false;
    }
    const auto modules = root["modules"];
    if (modules.size() == 0) { rebuild(); return false; }
    const auto m = modules[0];

    description_ = m.hasObjectMember("description")
                       ? m["description"].getWithDefault<std::string>("") : "";

    // WIDTH. A Eurorack HP is 5.08mm; the millimetres are worth showing because
    // that is what a rack is measured in when something has to fit.
    if (m.hasObjectMember("hp")) {
        const auto hp = m["hp"].getWithDefault<int64_t>(0);
        if (hp > 0) {
            std::ostringstream mm;
            // Two places: 12 HP is 60.96mm, and rounding it to 61.0 loses the
            // figure someone measuring a rack space actually needs.
            mm.precision(2);
            mm << std::fixed << (static_cast<double>(hp) * 5.08);
            rows_.emplace_back("WIDTH", std::to_string(hp) + " HP \xC2\xB7 " +
                                            mm.str() + " mm");
        }
    }

    // CONTROLS, counted by the kind the manifest gives each one.
    if (m.hasObjectMember("params")) {
        const auto params = m["params"];
        std::map<std::string, int> kinds;
        for (uint32_t i = 0; i < params.size(); ++i) {
            const auto p = params[i];
            auto kind = p.hasObjectMember("type")
                            ? p["type"].getWithDefault<std::string>("knob")
                            : std::string("knob");
            if (kind.empty()) kind = "knob";
            ++kinds[kind];
        }
        const auto text = tally(kinds);
        if (!text.empty()) rows_.emplace_back("CONTROLS", text);
    }

    // I/O, split by the role each jack carries rather than a bare count: "4 CV
    // in" says what the module expects, "4 in" does not.
    const auto io_side = [&](const char* member, const char* suffix,
                             std::map<std::string, int>& into) {
        if (!m.hasObjectMember(member)) return;
        const auto arr = m[member];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            auto role = arr[i].hasObjectMember("role")
                            ? arr[i]["role"].getWithDefault<std::string>("")
                            : std::string();
            if (role.empty()) role = "signal";
            ++into[role + " " + suffix];
        }
    };
    std::map<std::string, int> io;
    io_side("inputs", "in", io);
    io_side("outputs", "out", io);
    if (!io.empty()) {
        // Counted individually, so the plural in tally() would read "2 Cv ins".
        std::string text;
        for (const auto& [what, n] : io) {
            if (!text.empty()) text += " \xC2\xB7 ";
            text += std::to_string(n) + " " + what;
        }
        rows_.emplace_back("I/O", text);
    }

    if (m.hasObjectMember("lights")) {
        const auto n = static_cast<int>(m["lights"].size());
        if (n > 0)
            rows_.emplace_back("LIGHTS", std::to_string(n) +
                                             (n == 1 ? " light" : " lights"));
    }

    rebuild();
    return !rows_.empty();
}

void ModuleSummary::rebuild() {
    while (child_count() > 0) remove_child(child_at(0));
    flex().direction = FlexDirection::column;
    flex().gap = 4;

    if (!description_.empty()) {
        auto note = std::make_unique<Label>(description_);
        note->set_font_family(forge::design::type::display);
        note->set_font_size(12.5f);
        note->set_text_color(color::text_muted);
        note->set_multi_line(true);
        note->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        note->flex().flex_shrink = 0;
        note->flex().padding_bottom = 8;
        add_child(std::move(note));
    }

    for (const auto& [key, value] : rows_) {
        auto row = std::make_unique<View>();
        row->flex().direction = FlexDirection::row;
        row->flex().dim_width = {100, pulp::view::DimensionUnit::percent};
        row->flex().flex_shrink = 0;
        row->flex().gap = 10;

        auto label = std::make_unique<Label>(key);
        label->set_font_family(forge::design::type::mono);
        label->set_font_size(10.5f);
        label->set_text_color(color::text_faint);
        label->flex().preferred_width = 78;
        label->flex().flex_shrink = 0;
        row->add_child(std::move(label));

        auto val = std::make_unique<Label>(value);
        val->set_font_family(forge::design::type::mono);
        val->set_font_size(10.5f);
        val->set_text_color(color::text_muted);
        val->flex().flex_grow = 1;
        row->add_child(std::move(val));

        add_child(std::move(row));
    }
    request_repaint();
}

}  // namespace forge_modular
