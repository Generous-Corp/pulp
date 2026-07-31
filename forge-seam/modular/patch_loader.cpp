#include "forge/patch_loader.hpp"
#include <map>

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace forge_modular {

namespace {

/// The role a cable's colour encodes.
///
/// Read from the file rather than re-derived, so the preview never disagrees
/// with what Rack shows -- but that only works because the GENERATOR now writes
/// this field from the patch's structure (patch.py, ROLE_COLORS) rather than
/// letting the model choose. While the model chose, most colours fell outside
/// the convention and landed in the fallback below, which taught "audio" for
/// cables that were nothing of the kind.
///
/// A patch a person wired themselves still lands in the fallback, and honestly
/// so: their colours mean whatever they meant to that person. Deriving role
/// structurally for imported patches needs the module inventory here, which the
/// app does not yet load.
SignalRole role_from_color(const std::string& hex) {
    if (hex.size() < 7) return SignalRole::audio;
    auto eq = [&](const char* other) {
        for (std::size_t i = 1; i < 7; ++i)
            if (std::tolower(hex[i]) != std::tolower(other[i])) return false;
        return true;
    };
    if (eq("#3695ef")) return SignalRole::pitch;
    if (eq("#ffb437")) return SignalRole::clock;
    if (eq("#8b4ade")) return SignalRole::mod;
    if (eq("#00b56e")) return SignalRole::audio;
    // Anything else came from a hand-wired patch. Audio is the honest default:
    // it is the role every patch has at least one of.
    return SignalRole::audio;
}

}  // namespace

LoadedPatch load_patch(const std::string& path) {
    LoadedPatch out;
    std::ifstream f(path);
    if (!f) {
        out.error = "could not open that patch";
        return out;
    }
    std::stringstream ss;
    ss << f.rdbuf();

    choc::value::Value root;
    try {
        root = choc::json::parse(ss.str());
    } catch (const std::exception& e) {
        out.error = std::string("that patch is not readable: ") + e.what();
        return out;
    }
    if (!root.isObject() || !root.hasObjectMember("modules")) {
        out.error = "that file is not a Rack patch";
        return out;
    }

    // Rack's ids are arbitrary integers; the preview keys on strings. Mapping
    // once here keeps every id in one form downstream.
    std::unordered_map<int64_t, std::string> id_to_key;

    const auto modules = root["modules"];
    for (uint32_t i = 0; i < modules.size(); ++i) {
        const auto m = modules[i];
        RackModule rm;
        const auto id = m.hasObjectMember("id") ? m["id"].getWithDefault<int64_t>(0)
                                                : static_cast<int64_t>(i);
        rm.id = std::to_string(id);
        id_to_key[id] = rm.id;
        rm.brand = m.hasObjectMember("plugin")
                       ? m["plugin"].getWithDefault<std::string>("") : "";
        rm.name = m.hasObjectMember("model")
                      ? m["model"].getWithDefault<std::string>("") : rm.id;
        // A patch carries no panel width, so a module the local inventory does
        // not know gets a plain default rather than a guess dressed up as fact.
        rm.hp = 8;
        // Nor does it carry jack coordinates. Ports are discovered from the
        // cables below, and every module is treated as unplaced so its cables
        // dock at the panel edge instead of landing on invented positions.
        rm.placed = false;
        out.modules.push_back(std::move(rm));
    }

    if (!root.hasObjectMember("cables")) return out;

    // The reason each cable exists travels beside the patch, because Rack owns
    // the .vcv format and will not carry our prose. Absent for a patch a person
    // wired themselves, and for anything built before the sidecar existed --
    // the explanation degrades to the wiring, which is still true, just terser.
    std::map<std::string, std::string> why;
    if (path.size() > 4 && path.substr(path.size() - 4) == ".vcv") {
        std::ifstream wf(path.substr(0, path.size() - 4) + ".why.json");
        if (wf) {
            std::stringstream ws;
            ws << wf.rdbuf();
            try {
                const auto wroot = choc::json::parse(ws.str());
                if (wroot.isObject()) {
                    for (uint32_t i = 0; i < wroot.size(); ++i) {
                        const auto m = wroot.getObjectMemberAt(i);
                        why[m.name] = m.value.getWithDefault<std::string>("");
                    }
                }
            } catch (...) {
                // A malformed sidecar must not cost the patch. The wiring is
                // the artifact; the prose is the commentary on it.
            }
        }
    }

    const auto cables = root["cables"];
    for (uint32_t i = 0; i < cables.size(); ++i) {
        const auto c = cables[i];
        const auto from_id = c["outputModuleId"].getWithDefault<int64_t>(-1);
        const auto to_id = c["inputModuleId"].getWithDefault<int64_t>(-1);
        if (!id_to_key.count(from_id) || !id_to_key.count(to_id)) continue;

        Connection conn;
        conn.from_module = id_to_key[from_id];
        conn.to_module = id_to_key[to_id];
        // A .vcv carries port INDICES, not names -- Rack resolves names from
        // the installed module. So these read "out1" rather than "MIX" until
        // the local inventory is consulted, which is what patch.py explain
        // does and what Ask therefore answers with. Showing the index is
        // honest; inventing a plausible name would not be.
        conn.from_port = "out" + std::to_string(c["outputId"].getWithDefault<int64_t>(0));
        conn.to_port = "in" + std::to_string(c["inputId"].getWithDefault<int64_t>(0));
        conn.role = role_from_color(
            c.hasObjectMember("color") ? c["color"].getWithDefault<std::string>("")
                                       : "");
        const auto key = std::to_string(from_id) + ":" +
                         std::to_string(c["outputId"].getWithDefault<int64_t>(0)) + ">" +
                         std::to_string(to_id) + ":" +
                         std::to_string(c["inputId"].getWithDefault<int64_t>(0));
        if (const auto it = why.find(key); it != why.end()) conn.why = it->second;
        out.connections.push_back(conn);

        // Register the ports the cables actually use, so the explanation can
        // name them and the docking fan has something to order by.
        auto add_port = [&](const std::string& module_key, const std::string& port_id) {
            for (auto& m : out.modules) {
                if (m.id != module_key) continue;
                for (const auto& p : m.ports)
                    if (p.id == port_id) return;
                Port p;
                p.id = port_id;
                p.name = port_id;
                m.ports.push_back(p);
                return;
            }
        };
        add_port(conn.from_module, conn.from_port);
        add_port(conn.to_module, conn.to_port);
    }
    return out;
}

}  // namespace forge_modular
