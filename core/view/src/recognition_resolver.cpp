/// @file recognition_resolver.cpp
/// Implementation of the single key-based recognition merge layer.
/// See recognition_resolver.hpp for the module rationale and the #4677 hook.

#include <pulp/view/recognition_resolver.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
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
            } else if (unmatched_out && !component_key.empty()) {
                // Never-silent-knob (P7): a present-but-unmatched component is
                // surfaced, never guessed. Deduplicate by key.
                if (unmatched_seen.insert(component_key).second)
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

} // namespace pulp::view
