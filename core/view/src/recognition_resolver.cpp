/// @file recognition_resolver.cpp
/// Implementation of the single key-based recognition merge layer.
/// See recognition_resolver.hpp for the module rationale and the #4677 hook.

#include <pulp/view/recognition_resolver.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace pulp::view {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Whether `name` (case-insensitively) starts with `prefix`. Empty prefix never
// matches (a blank prefix must not match every node).
bool name_has_prefix(const std::string& name, const std::string& prefix) {
    if (prefix.empty() || name.size() < prefix.size()) return false;
    return to_lower(name).compare(0, prefix.size(), to_lower(prefix)) == 0;
}

} // namespace

AudioWidgetType audio_widget_kind_from_manifest_id(const std::string& id) {
    const auto lower = to_lower(id);
    if (lower == "knob") return AudioWidgetType::knob;
    if (lower == "fader" || lower == "slider") return AudioWidgetType::fader;
    if (lower == "meter") return AudioWidgetType::meter;
    if (lower == "xy_pad" || lower == "xypad" || lower == "xy-pad")
        return AudioWidgetType::xy_pad;
    if (lower == "waveform") return AudioWidgetType::waveform;
    if (lower == "spectrum") return AudioWidgetType::spectrum;
    return AudioWidgetType::none;
}

const char* audio_widget_kind_to_manifest_id(AudioWidgetType kind) {
    switch (kind) {
        case AudioWidgetType::knob:     return "knob";
        case AudioWidgetType::fader:    return "fader";
        case AudioWidgetType::meter:    return "meter";
        case AudioWidgetType::xy_pad:   return "xy_pad";
        case AudioWidgetType::waveform: return "waveform";
        case AudioWidgetType::spectrum: return "spectrum";
        case AudioWidgetType::none:     return "none";
    }
    return "none";
}

RecognitionResolver RecognitionResolver::with_builtin_library() {
    // The built-in Pulp Figma Library, mirrored from
    // tools/figma-plugin/library-manifest.json. Kept in code so the C++ importer
    // can re-resolve a designer's keys without reading the JSON at runtime; a
    // unit test pins this table against the JSON so the two never drift.
    // component_set_key values are the published Figma keys; "TBD-" placeholders
    // are intentionally excluded (no real key collision possible) — there are
    // none today.
    RecognitionSource builtin;
    builtin.name = "builtin-library";
    builtin.entries = {
        {AudioWidgetType::knob,
         "f74264ffa9108521fb0d3398dc8f5ea88e23a84e", "Pulp / Knob", ""},
        {AudioWidgetType::fader,
         "1c2b727f0c0e11026512725aeb546997f16042bd", "Pulp / Fader", ""},
        {AudioWidgetType::meter,
         "52e1636086b855cb2d20d341d4cfa15e94151eef", "Pulp / Meter", ""},
        {AudioWidgetType::xy_pad,
         "9dc09d4cbf65341f12c21ece408ad653886059b9", "Pulp / XYPad", ""},
        {AudioWidgetType::waveform,
         "2c0797af5c939638ec6a89d893ba310a088ce46c", "Pulp / Waveform", ""},
        {AudioWidgetType::spectrum,
         "f6730821fc7557e93f904d171a45339207abf9e3", "Pulp / Spectrum", ""},
    };
    RecognitionResolver r;
    r.add_source(std::move(builtin));
    return r;
}

RecognitionResolver& RecognitionResolver::add_source(RecognitionSource source) {
    sources_.push_back(std::move(source));
    return *this;
}

std::optional<RecognitionSource> RecognitionResolver::parse_manifest_json(
    const std::string& json,
    const std::string& source_name,
    std::string* error_out) {
    auto set_err = [&](const std::string& msg) {
        if (error_out) *error_out = msg;
        return std::nullopt;
    };

    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(json);
    } catch (const std::exception& e) {
        return set_err(std::string("manifest is not valid JSON: ") + e.what());
    } catch (...) {
        // choc may throw a non-std::exception on some malformed inputs.
        return set_err("manifest is not valid JSON");
    }

    if (!parsed.isObject() || !parsed.hasObjectMember("widgets") ||
        !parsed["widgets"].isObject()) {
        return set_err("manifest must be an object with a \"widgets\" object "
                       "(flat library-manifest shape)");
    }

    RecognitionSource source;
    source.name = source_name;

    auto widgets = parsed["widgets"];
    const auto count = widgets.size();
    for (uint32_t i = 0; i < count; ++i) {
        const auto member = widgets.getObjectMemberAt(i);
        const std::string widget_name = member.name != nullptr ? member.name : "";
        const auto w = member.value;
        if (!w.isObject()) continue;

        RecognitionEntry entry;

        // `kind` defaults to the widget map key (so the built-in shape, which
        // keys by kind, parses; a user manifest may key by anything and set an
        // explicit `kind`).
        std::string kind_id = widget_name;
        if (w.hasObjectMember("kind") && w["kind"].isString())
            kind_id = std::string(w["kind"].toString());
        entry.kind = audio_widget_kind_from_manifest_id(kind_id);

        if (w.hasObjectMember("component_set_key") &&
            w["component_set_key"].isString())
            entry.component_set_key =
                std::string(w["component_set_key"].toString());
        if (w.hasObjectMember("name_prefix") && w["name_prefix"].isString())
            entry.name_prefix = std::string(w["name_prefix"].toString());
        // #4677 forward-compat: a manifest may carry a custom-control factory_id
        // instead of (or in addition to) a built-in kind.
        if (w.hasObjectMember("factory_id") && w["factory_id"].isString())
            entry.factory_id = std::string(w["factory_id"].toString());

        // Skip placeholder keys (mirrors the TS registry's TBD- exclusion) so a
        // half-authored manifest cannot collide with a real key.
        if (entry.component_set_key.rfind("TBD-", 0) == 0)
            entry.component_set_key.clear();

        // An entry must resolve to SOMETHING (a built-in kind or a factory) AND
        // have at least one identity signal (key or prefix), else it is inert.
        const bool has_target =
            entry.kind != AudioWidgetType::none || !entry.factory_id.empty();
        const bool has_identity =
            !entry.component_set_key.empty() || !entry.name_prefix.empty();
        if (!has_target || !has_identity) continue;

        source.entries.push_back(std::move(entry));
    }

    if (source.entries.empty())
        return set_err("manifest has no usable widget entries (each needs a kind "
                       "or factory_id and a component_set_key or name_prefix)");

    return source;
}

ResolvedControl RecognitionResolver::resolve(const std::string& component_key,
                                             const std::string& name) const {
    ResolvedControl result;

    // 1) Authoritative key match. Walk sources in REVERSE so the
    //    highest-precedence (last-added) source wins on collision.
    if (!component_key.empty()) {
        for (auto it = sources_.rbegin(); it != sources_.rend(); ++it) {
            for (const auto& e : it->entries) {
                if (!e.component_set_key.empty() &&
                    e.component_set_key == component_key) {
                    result.matched = true;
                    result.kind = e.kind;
                    result.factory_id = e.factory_id;
                    result.source_name = it->name;
                    result.via = "key";
                    return result;
                }
            }
        }
    }

    // 2) Name-prefix fallback. Same reverse precedence.
    if (!name.empty()) {
        for (auto it = sources_.rbegin(); it != sources_.rend(); ++it) {
            for (const auto& e : it->entries) {
                if (name_has_prefix(name, e.name_prefix)) {
                    result.matched = true;
                    result.kind = e.kind;
                    result.factory_id = e.factory_id;
                    result.source_name = it->name;
                    result.via = "name_prefix";
                    return result;
                }
            }
        }
    }

    return result;  // matched == false
}

namespace {

int apply_recursive(IRNode& node,
                    const RecognitionResolver& resolver,
                    std::vector<UnmatchedComponent>* unmatched_out,
                    std::unordered_set<std::string>& unmatched_seen) {
    int wired = 0;

    const auto key_it = node.attributes.find("figmaComponentKey");
    const auto name_it = node.attributes.find("figmaMainComponentName");
    const bool has_component_identity =
        key_it != node.attributes.end() || name_it != node.attributes.end();

    if (has_component_identity) {
        const std::string component_key =
            key_it != node.attributes.end() ? key_it->second : std::string{};
        // Prefer the component's own name for the prefix fallback; fall back to
        // the node name.
        const std::string ident_name =
            name_it != node.attributes.end() ? name_it->second : node.name;

        // Strictly additive: never override a kind the TS lane already stamped.
        if (node.audio_widget == AudioWidgetType::none) {
            const auto resolved = resolver.resolve(component_key, ident_name);
            if (resolved.matched) {
                if (resolved.kind != AudioWidgetType::none) {
                    node.audio_widget = resolved.kind;
                    node.attributes["recognitionSource"] = resolved.source_name;
                    node.attributes["recognitionVia"] = resolved.via;
                    ++wired;
                } else if (!resolved.factory_id.empty()) {
                    // #4677 custom-control path: record the factory for the
                    // materializer. (No built-in widget kind to stamp.)
                    node.attributes["recognitionFactoryId"] = resolved.factory_id;
                    node.attributes["recognitionSource"] = resolved.source_name;
                    node.attributes["recognitionVia"] = resolved.via;
                    ++wired;
                }
            } else if (unmatched_out) {
                // Never-silent-knob (P7): a present-but-unmatched component is
                // surfaced, never guessed. Dedup by component_key when present,
                // else by the identity name — a detached/local component carries
                // only figmaMainComponentName, not a component_key, and must
                // still be surfaced (not silently dropped).
                const std::string& dedup_key =
                    !component_key.empty() ? component_key : ident_name;
                if (!dedup_key.empty() && unmatched_seen.insert(dedup_key).second)
                    unmatched_out->push_back({component_key, ident_name});
            }
        }
    }

    for (auto& child : node.children)
        wired += apply_recursive(child, resolver, unmatched_out, unmatched_seen);

    return wired;
}

} // namespace

int apply_recognition_resolver(IRNode& root,
                               const RecognitionResolver& resolver,
                               std::vector<UnmatchedComponent>* unmatched_out) {
    if (resolver.empty()) return 0;
    std::unordered_set<std::string> unmatched_seen;
    return apply_recursive(root, resolver, unmatched_out, unmatched_seen);
}

// ── Installed-package custom-control source (the P8 merge half) ───────────────

namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Parse one package's `design_controls` array (a JSON array of flat
// {factory_id, component_set_key?, name_prefix?} objects) into recognition
// entries. Skips entries with no factory_id or no identity field — those can
// never resolve a node, mirroring the schema's `anyOf` identity requirement.
std::vector<RecognitionEntry> parse_design_controls_array(
    const choc::value::ValueView& arr) {
    std::vector<RecognitionEntry> entries;
    if (!arr.isArray()) return entries;
    for (uint32_t i = 0; i < arr.size(); ++i) {
        const auto e = arr[static_cast<int>(i)];
        if (!e.isObject()) continue;

        RecognitionEntry entry;
        // design_controls entries always target a custom factory (kind stays
        // none); the merged resolver routes a factory_id match to the
        // custom-control materialize path.
        if (e.hasObjectMember("factory_id") && e["factory_id"].isString())
            entry.factory_id = std::string(e["factory_id"].toString());
        if (e.hasObjectMember("component_set_key") &&
            e["component_set_key"].isString())
            entry.component_set_key =
                std::string(e["component_set_key"].toString());
        if (e.hasObjectMember("name_prefix") && e["name_prefix"].isString())
            entry.name_prefix = std::string(e["name_prefix"].toString());

        const bool has_identity =
            !entry.component_set_key.empty() || !entry.name_prefix.empty();
        if (entry.factory_id.empty() || !has_identity) continue;

        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

PackageDesignControlSources gather_package_design_controls(
    const std::filesystem::path& lockfile_path,
    const std::filesystem::path& registry_path) {
    PackageDesignControlSources result;

    const std::string lock_text = read_text_file(lockfile_path);
    if (lock_text.empty()) return result;  // no installed packages → no sources

    choc::value::Value lock;
    try {
        lock = choc::json::parse(lock_text);
    } catch (...) {
        result.warnings.push_back(
            "packages.lock.json is not valid JSON; no custom controls merged");
        return result;
    }
    if (!lock.isObject() || !lock.hasObjectMember("packages") ||
        !lock["packages"].isObject())
        return result;  // empty/odd lockfile → nothing installed

    const std::string registry_text = read_text_file(registry_path);
    if (registry_text.empty()) {
        // Installed packages but no registry to look up their controls. Only a
        // warning if any package was actually locked.
        if (lock["packages"].size() > 0)
            result.warnings.push_back(
                "registry.json not found; installed packages' custom controls "
                "could not be merged");
        return result;
    }

    choc::value::Value registry;
    try {
        registry = choc::json::parse(registry_text);
    } catch (...) {
        result.warnings.push_back(
            "registry.json is not valid JSON; no custom controls merged");
        return result;
    }
    if (!registry.isObject() || !registry.hasObjectMember("packages") ||
        !registry["packages"].isObject())
        return result;

    const auto reg_packages = registry["packages"];
    const auto locked = lock["packages"];

    // Walk installed packages in lockfile order so the produced source order is
    // deterministic (the resolver merges later sources OVER earlier ones; a
    // stable order makes the dedup/collision behavior reproducible and pinnable).
    // NOTE: "last package wins a shared key" relies on (a) choc::json preserving
    // JSON object-member insertion order and (b) the CLI writing the lockfile in
    // a stable order. Both hold today; if a future lockfile writer reorders,
    // sort `locked`'s members by package id here for a layout-independent tiebreak.
    for (uint32_t i = 0; i < locked.size(); ++i) {
        const auto member = locked.getObjectMemberAt(i);
        const std::string pkg_id = member.name != nullptr ? member.name : "";
        if (pkg_id.empty()) continue;

        if (!reg_packages.hasObjectMember(pkg_id)) {
            result.warnings.push_back("installed package '" + pkg_id +
                                      "' is not in the registry; its custom "
                                      "controls (if any) were not merged");
            continue;
        }
        const auto pkg = reg_packages[pkg_id];
        if (!pkg.isObject() || !pkg.hasObjectMember("design_controls"))
            continue;  // package declares no custom controls — contributes nothing

        auto entries = parse_design_controls_array(pkg["design_controls"]);
        if (entries.empty()) continue;

        RecognitionSource source;
        source.name = pkg_id;
        source.entries = std::move(entries);
        result.sources.push_back(std::move(source));
    }

    return result;
}

PackageDesignControlSources discover_package_design_controls(
    const std::filesystem::path& start_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path cur = fs::absolute(start_dir, ec);
    if (ec) return {};
    for (int i = 0; i < 32; ++i) {
        const auto lock = cur / "packages.lock.json";
        if (fs::exists(lock, ec) && !ec) {
            // The registry lives at <root>/tools/packages/registry.json — the
            // SAME path the CLI's find_registry_path resolves (package_commands_util.cpp),
            // NOT a sibling of the lockfile. If absent, gather still runs and
            // reports the missing-registry warning for any locked package.
            return gather_package_design_controls(
                lock, cur / "tools" / "packages" / "registry.json");
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    return {};
}

// ── Materialize half: stamp resolved custom controls onto the IR ─────────────

namespace {

int materialize_recursive(IRNode& node) {
    int materialized = 0;

    const auto factory_it = node.attributes.find("recognitionFactoryId");
    if (factory_it != node.attributes.end() && !factory_it->second.empty()) {
        const std::string& factory_id = factory_it->second;

        // Idempotent: skip if a custom element for this factory already exists.
        const bool already = std::any_of(
            node.interactive_elements.begin(), node.interactive_elements.end(),
            [&](const IRInteractiveElement& e) {
                return e.kind == InteractiveElementKind::custom &&
                       e.factory_id == factory_id;
            });
        if (!already) {
            IRInteractiveElement el;
            el.kind = InteractiveElementKind::custom;
            el.factory_id = factory_id;
            // Geometry from the node's own box (SVG/overlay coords). The overlay
            // sits over the baked render, so a zero box still materializes a
            // (zero-size) overlay rather than guessing — placement verification
            // surfaces a degenerate extent downstream.
            el.x = node.style.left.value_or(0.0f);
            el.y = node.style.top.value_or(0.0f);
            el.w = node.style.width.value_or(0.0f);
            el.h = node.style.height.value_or(0.0f);
            // A registered custom factory is ladder rung 4 (vs rung 5 = inert);
            // the materializer downgrades to inert + diagnostic when the factory
            // isn't registered, so carrying rung 4 here is the resolved intent.
            el.resolution_rung = 4;
            if (node.source_node_id) el.source_node_id = *node.source_node_id;
            node.interactive_elements.push_back(std::move(el));
            ++materialized;
        }
    }

    for (auto& child : node.children)
        materialized += materialize_recursive(child);
    return materialized;
}

} // namespace

int materialize_recognized_custom_controls(IRNode& root) {
    return materialize_recursive(root);
}

} // namespace pulp::view
