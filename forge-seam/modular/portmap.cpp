#include "forge/portmap.hpp"

#include <choc/text/choc_JSON.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace forge_modular {

namespace {

std::vector<MappedWidget> read_group(const choc::value::ValueView& mod,
                                     const char* group) {
    std::vector<MappedWidget> out;
    if (!mod.hasObjectMember(group)) return out;
    const auto arr = mod[group];
    for (uint32_t i = 0; i < arr.size(); ++i) {
        const auto w = arr[i];
        MappedWidget one;
        one.index = w["index"].getWithDefault<int>(0);
        one.name = w["name"].getWithDefault<std::string>("");
        one.x = static_cast<float>(w["x"].getWithDefault<double>(0.0));
        one.y = static_cast<float>(w["y"].getWithDefault<double>(0.0));
        // Jacks carry no size: Rack draws them all alike, and a width of zero
        // says "not measured" rather than "zero wide".
        one.w = static_cast<float>(w["w"].getWithDefault<double>(0.0));
        one.h = static_cast<float>(w["h"].getWithDefault<double>(0.0));
        out.push_back(one);
    }
    return out;
}

}  // namespace

PortMap PortMap::parse(const std::string& json) {
    PortMap out;
    if (json.empty()) return out;
    try {
        const auto doc = choc::json::parse(json);
        if (!doc.hasObjectMember("modules")) return out;
        const auto mods = doc["modules"];
        for (uint32_t i = 0; i < mods.size(); ++i) {
            const auto m = mods[i];
            const auto plugin = m["plugin"].getWithDefault<std::string>("");
            const auto model = m["model"].getWithDefault<std::string>("");
            if (plugin.empty() || model.empty()) continue;
            MappedModule one;
            one.plugin_version = m["pluginVersion"].getWithDefault<std::string>("");
            // Absent means version 1: the scanner that wrote those entries
            // predates the field, and recorded jacks only.
            one.scan_version =
                static_cast<int>(m["scan"].getWithDefault<int64_t>(1));
            if (m.hasObjectMember("size") && m["size"].size() >= 2) {
                one.width = static_cast<float>(m["size"][0].getWithDefault<double>(0.0));
                one.height = static_cast<float>(m["size"][1].getWithDefault<double>(0.0));
            }
            one.params = read_group(m, "params");
            one.inputs = read_group(m, "inputs");
            one.outputs = read_group(m, "outputs");
            out.by_key_[plugin + "/" + model] = std::move(one);
        }
    } catch (...) {
        // A map we cannot read means nothing is measured, which the callers
        // already handle -- they draw a plain face. It must not be a crash on
        // the paint path.
        return PortMap{};
    }
    return out;
}

const PortMap& PortMap::shared() {
    static const PortMap map = [] {
        const char* home = std::getenv("HOME");
        const std::string path = std::string(home ? home : ".") +
                                 "/Library/Application Support/Rack2/forge-portmap.json";
        std::ifstream f(path);
        if (!f) return PortMap{};
        std::stringstream ss;
        ss << f.rdbuf();
        return parse(ss.str());
    }();
    return map;
}

const MappedModule* PortMap::find(const std::string& plugin,
                                  const std::string& model) const {
    const auto it = by_key_.find(plugin + "/" + model);
    return it == by_key_.end() ? nullptr : &it->second;
}

PortMap::Gap PortMap::gap_for(const std::string& plugin, const std::string& model,
                              const std::string& installed_version) const {
    const auto* m = find(plugin, model);
    if (!m) return Gap::unmeasured;
    // An older scanner measured less, so the entry is incomplete however well
    // the plugin version matches -- and the version matching is exactly what
    // made these look trustworthy.
    if (m->scan_version < kScanVersion) return Gap::stale;
    if (installed_version.empty() || m->plugin_version.empty()) return Gap::none;
    return m->plugin_version == installed_version ? Gap::none : Gap::stale;
}

}  // namespace forge_modular
