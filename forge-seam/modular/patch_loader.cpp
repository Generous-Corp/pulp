#include "forge/patch_loader.hpp"

#include "forge/portmap.hpp"
#include <map>

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace forge_modular {

namespace {

/// Can RACK actually create this module on this machine?
///
/// The preview draws from the patch and from our own manifests. Rack can only
/// create what its installed plugin BINARY contains, and on a machine running
/// an older build those differ -- so a patch renders perfectly here and opens
/// over there as a different rack, with modules silently missing. Reported as
/// "the VCV Rack patch/models are DIFFERENT than what I see in Forge Modular",
/// and nothing compared them.
///
/// Read straight from the installed plugin.json, cached per plugin: it is the
/// same file Rack reads, so this cannot drift from what Rack will do.
/// What an installed plugin.json says: which models it has, and its version.
///
/// Both come from one read. The version is what a port-map entry is checked
/// against, so reading it from a different place than the model list would let
/// the two disagree about which plugin is even installed.
struct InstalledPlugin {
    std::set<std::string> models;
    std::string version;
};

const InstalledPlugin& installed_plugin(const std::string& plugin) {
    static std::map<std::string, InstalledPlugin> cache;
    static std::mutex lock;
    std::lock_guard<std::mutex> held(lock);
    auto it = cache.find(plugin);
    if (it == cache.end()) {
        InstalledPlugin found;
        std::set<std::string>& models = found.models;
        const char* home = std::getenv("HOME");
        const std::filesystem::path base =
            std::string(home ? home : ".") + "/Library/Application Support/Rack2";
        std::error_code ec;
        for (const auto& dir : std::filesystem::directory_iterator(base, ec)) {
            if (!dir.is_directory() ||
                dir.path().filename().string().rfind("plugins-", 0) != 0)
                continue;
            const auto manifest = dir.path() / plugin / "plugin.json";
            if (!std::filesystem::exists(manifest, ec)) continue;
            std::ifstream f(manifest);
            std::stringstream ss;
            ss << f.rdbuf();
            try {
                const auto doc = choc::json::parse(ss.str());
                if (doc.hasObjectMember("version"))
                    found.version =
                        doc["version"].getWithDefault<std::string>("");
                if (doc.hasObjectMember("modules")) {
                    const auto ms = doc["modules"];
                    for (uint32_t i = 0; i < ms.size(); ++i)
                        if (ms[i].hasObjectMember("slug"))
                            models.insert(
                                ms[i]["slug"].getWithDefault<std::string>(""));
                }
            } catch (...) {
                // A manifest we cannot read is not evidence of absence.
                models.clear();
                found.version.clear();
                break;
            }
        }
        it = cache.emplace(plugin, std::move(found)).first;
    }
    return it->second;
}

/// The version of the plugin installed here, or empty when it is not.
std::string installed_version(const std::string& plugin) {
    if (plugin.empty() || plugin == "Core") return {};
    return installed_plugin(plugin).version;
}

bool rack_can_create(const std::string& plugin, const std::string& model) {
    if (plugin.empty() || model.empty()) return true;
    // Core is compiled into Rack rather than installed, so it is always there.
    if (plugin == "Core") return true;
    const auto& found = installed_plugin(plugin);
    // No manifest found at all means Rack is not installed here, or this is
    // somebody else's machine's patch. Not knowing is not the same as knowing
    // it is missing, so nothing is claimed.
    if (found.models.empty()) return true;
    return found.models.count(model) > 0;
}


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
        // A patch carries neither panel width nor jack coordinates, so both
        // used to be a default: every module 8 HP, every module "unplaced" so
        // its cables docked at the panel edge.
        //
        // That default is visibly wrong once you look at it. Fundamental's
        // Scope is 13 HP drawn at 8 -- squashed by a third -- and its VCA is
        // 5 HP stretched to 8. A rack of them reads as a bad drawing rather
        // than as a rack, and every cable ends at a panel border instead of a
        // jack.
        //
        // CARTOG measured both from inside Rack, so use what it measured.
        // A module nobody has scanned still gets the honest default: the
        // fallback is for the unmeasured, not for everyone.
        // Where the module sits, if the patch says. Rack writes `pos` as
        // [HP across, row down]; ignoring it laid a two-row rack out as one
        // very long strip that then shrank to fit, losing the arrangement the
        // patch was built to be read in.
        if (m.hasObjectMember("pos") && m["pos"].size() >= 2) {
            rm.grid_x = static_cast<int>(m["pos"][0].getWithDefault<int64_t>(0));
            rm.grid_y = static_cast<int>(m["pos"][1].getWithDefault<int64_t>(0));
            rm.has_grid_pos = true;
        }
        rm.hp = 8;
        rm.placed = false;
        if (const auto* mapped = PortMap::shared().find(rm.brand, rm.name)) {
            if (mapped->width > 0.0f) {
                // 15 points to the HP, which is Rack's own RACK_GRID_WIDTH.
                rm.hp = std::max(1, static_cast<int>(
                                        std::lround(mapped->width / 15.0f)));
            }
            const float w = mapped->width > 0.0f ? mapped->width : 1.0f;
            auto add = [&](const std::vector<MappedWidget>& group,
                           const char* prefix, bool input) {
                for (const auto& jack : group) {
                    Port port;
                    port.id = prefix + std::to_string(jack.index);
                    // The author's own label -- "Frequency modulation" rather
                    // than "in1", which is what makes a wiring explanation
                    // readable for a module we did not write.
                    port.name = jack.name.empty() ? port.id : jack.name;
                    port.x = jack.x / w;      // across the panel, 0..1
                    port.y = jack.y;          // down it, already in points
                    port.input = input;
                    rm.ports.push_back(port);
                }
            };
            add(mapped->inputs, "in", true);
            add(mapped->outputs, "out", false);
            // Placed means "we know where its jacks are", which is now true.
            rm.placed = !rm.ports.empty();
            // Whether its CONTROLS are known is a separate question, and it
            // is answered by what the entry HOLDS rather than by which
            // scanner wrote it.
            //
            // Keying the badge off gap_for() marked modules whose controls we
            // do know. Core's AudioInterface carries a measured param and
            // draws its knob from vendor artwork, and it wore the badge
            // anyway, because its entry predates the scan field and so reads
            // as old. A badge on a panel with visible knobs invites exactly
            // the question it is meant to answer.
            //
            // gap_for still knows the difference -- it is what a "rescan for
            // lights" prompt would ask -- but the badge means the narrower
            // thing a person can see: we cannot show you this module's
            // controls.
            rm.controls_measured = !mapped->params.empty();
        } else {
            // Our own modules are not scanned and never need to be: their
            // controls come from the manifest their panel was emitted from,
            // which is always present. Marking them unmapped because the port
            // map has no entry would put the badge on every module we drew
            // ourselves, fully, with all its knobs -- the exact opposite of
            // what it means.
            rm.controls_measured = (rm.brand == "ForgeModular");
        }
        rm.available = rack_can_create(rm.brand, rm.name);
        out.modules.push_back(std::move(rm));
    }

    if (!root.hasObjectMember("cables")) return out;

    // The reason each cable exists travels beside the patch, because Rack owns
    // the .vcv format and will not carry our prose. Absent for a patch a person
    // wired themselves, and for anything built before the sidecar existed --
    // the explanation degrades to the wiring, which is still true, just terser.
    struct CableNote {
        std::string why, from, to;
    };
    std::map<std::string, CableNote> notes;
    struct ModuleNote {
        std::string name;
        int hp = 0;
        std::map<std::string, std::pair<float, float>> ports;
    };
    std::map<std::string, ModuleNote> module_notes;
    if (path.size() > 4 && path.substr(path.size() - 4) == ".vcv") {
        std::ifstream wf(path.substr(0, path.size() - 4) + ".why.json");
        if (wf) {
            std::stringstream ws;
            ws << wf.rdbuf();
            try {
                const auto wroot = choc::json::parse(ws.str());
                if (wroot.isObject() && wroot.hasObjectMember("cables")) {
                    const auto cs = wroot["cables"];
                    for (uint32_t i = 0; i < cs.size(); ++i) {
                        const auto m = cs.getObjectMemberAt(i);
                        CableNote n;
                        n.why = m.value["why"].getWithDefault<std::string>("");
                        n.from = m.value["from_port"].getWithDefault<std::string>("");
                        n.to = m.value["to_port"].getWithDefault<std::string>("");
                        notes[m.name] = std::move(n);
                    }
                    if (wroot.hasObjectMember("modules")) {
                        const auto ms = wroot["modules"];
                        for (uint32_t i = 0; i < ms.size(); ++i) {
                            const auto m = ms.getObjectMemberAt(i);
                            ModuleNote note;
                            if (m.value.isObject()) {
                                note.name = m.value["name"].getWithDefault<std::string>("");
                                note.hp = static_cast<int>(
                                    m.value["hp"].getWithDefault<int64_t>(0));
                                if (m.value.hasObjectMember("ports")) {
                                    const auto ps = m.value["ports"];
                                    for (uint32_t j = 0; j < ps.size(); ++j) {
                                        const auto pm = ps.getObjectMemberAt(j);
                                        if (pm.value.size() < 2) continue;
                                        note.ports[pm.name] = {
                                            static_cast<float>(pm.value[0].getWithDefault<double>(0.5)),
                                            static_cast<float>(pm.value[1].getWithDefault<double>(0.0))};
                                    }
                                }
                            } else {
                                note.name = m.value.getWithDefault<std::string>("");
                            }
                            module_notes[m.name] = std::move(note);
                        }
                    }
                } else if (wroot.isObject()) {
                    // The first sidecars were a flat key->reason map. They
                    // still load; they just cannot name a port.
                    for (uint32_t i = 0; i < wroot.size(); ++i) {
                        const auto m = wroot.getObjectMemberAt(i);
                        notes[m.name].why = m.value.getWithDefault<std::string>("");
                    }
                }
            } catch (...) {
                // A malformed sidecar must not cost the patch. The wiring is
                // the artifact; the prose is the commentary on it.
            }
        }
    }

    for (auto& m : out.modules) {
        const auto it = module_notes.find(m.id);
        if (it == module_notes.end()) continue;
        if (!it->second.name.empty()) m.display = it->second.name;
        // A .vcv records no width, so every panel used to be drawn in the same
        // default slot and its artwork letterboxed inside it -- which is why
        // the modules in a rack did not line up. The generator knows the real
        // width; it writes it down.
        if (it->second.hp > 0) m.hp = it->second.hp;
        for (const auto& [id, xy] : it->second.ports) {
            Port p;
            p.id = id;
            p.name = id;
            p.x = xy.first;
            p.y = xy.second;
            m.ports.push_back(p);
        }
        // Jack coordinates are what "placed" means: without them a cable has
        // nowhere to land and is drawn hanging off the panel edge.
        if (!it->second.ports.empty()) m.placed = true;
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
        // The port IDS stay on the connection, because that is what the jack
        // geometry is keyed on. The silkscreen NAME belongs to the port, and
        // is written onto it below -- putting the name here instead broke the
        // lookup, and every cable landed at the top of the panel rather than
        // on the jack it belongs to.
        if (const auto it = notes.find(key); it != notes.end())
            conn.why = it->second.why;
        // Name the two jacks this cable uses. "out0 → in1" is honest but
        // teaches nothing; inventing a plausible name would teach something
        // false. The generator resolved these against the installed modules,
        // so they are neither guessed nor made up.
        if (const auto it = notes.find(key); it != notes.end()) {
            const auto name_port = [&](const std::string& module_key,
                                       const std::string& port_id,
                                       const std::string& shown) {
                if (shown.empty()) return;
                for (auto& m : out.modules) {
                    if (m.id != module_key) continue;
                    for (auto& p : m.ports) {
                        if (p.id != port_id) continue;
                        p.name = shown;
                        return;
                    }
                    // Named but not placed: a module whose jack positions we
                    // do not have still gets to say what its ports are called.
                    // The cable then hangs at the default spot, which the
                    // preview already treats as unplaced.
                    Port p;
                    p.id = port_id;
                    p.name = shown;
                    m.ports.push_back(p);
                    return;
                }
            };
            name_port(conn.from_module, conn.from_port, it->second.from);
            name_port(conn.to_module, conn.to_port, it->second.to);
        }
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
