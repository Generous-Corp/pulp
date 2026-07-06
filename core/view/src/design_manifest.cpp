// design_manifest.cpp — compile the design contract (token allowlist + component
// contracts) into a deterministic manifest, JSON, and an LLM binding prompt.

#include <pulp/design/design_manifest.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>

namespace pulp::design {

namespace {

// Lowercase "#rrggbb", or "#rrggbbaa" when the token carries a non-opaque alpha.
std::string color_to_hex(const pulp::view::Color& c) {
    char buf[10];
    if (c.a8() == 255) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r8(), c.g8(), c.b8());
    } else {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.r8(), c.g8(), c.b8(), c.a8());
    }
    return buf;
}

// A float dimension rendered as its shortest round-tripping decimal ("8",
// "1.5", "0"), so the manifest diffs on real value changes rather than
// formatting noise. std::to_chars is locale-independent (unlike snprintf's
// LC_NUMERIC-sensitive "%f") and never truncates, which keeps the manifest
// byte-deterministic even inside an embedding app that has called setlocale.
std::string dimension_to_string(float v) {
    char buf[32];  // shortest float form fits comfortably (worst case ~15 chars)
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec != std::errc{}) return "0";  // unreachable for finite floats in 32 bytes
    return std::string(buf, ptr);
}

void sort_tokens(std::vector<ManifestToken>& tokens) {
    std::sort(tokens.begin(), tokens.end(), [](const ManifestToken& a, const ManifestToken& b) {
        if (a.kind != b.kind) return a.kind < b.kind;  // color, dimension, string
        return a.name < b.name;
    });
}

}  // namespace

DesignManifest compile_design_manifest(const pulp::view::Theme& theme,
                                       const std::vector<ComponentInfo>& components,
                                       std::string source,
                                       std::string appearance) {
    DesignManifest manifest;
    manifest.manifest_version = std::string(kManifestVersion);
    manifest.source = std::move(source);
    manifest.appearance = std::move(appearance);
    manifest.theme_schema_version = pulp::view::Theme::kSchemaVersion;

    manifest.tokens.reserve(theme.colors.size() + theme.dimensions.size() + theme.strings.size());
    for (const auto& [name, color] : theme.colors)
        manifest.tokens.push_back({name, "color", color_to_hex(color)});
    for (const auto& [name, dim] : theme.dimensions)
        manifest.tokens.push_back({name, "dimension", dimension_to_string(dim)});
    for (const auto& [name, str] : theme.strings)
        manifest.tokens.push_back({name, "string", str});
    sort_tokens(manifest.tokens);

    manifest.components.reserve(components.size());
    for (const auto& info : components) {
        ManifestComponent c;
        c.name = info.name;
        c.category = std::string(category_name(info.category));
        c.native_class = info.native_class;
        c.header = info.header;
        c.figma_component = info.figma_component;
        c.usage = info.usage;
        c.reskin_tokens = info.reskin_tokens;
        std::sort(c.reskin_tokens.begin(), c.reskin_tokens.end());
        manifest.components.push_back(std::move(c));
    }
    std::sort(manifest.components.begin(), manifest.components.end(),
              [](const ManifestComponent& a, const ManifestComponent& b) { return a.name < b.name; });

    return manifest;
}

DesignManifest compile_ink_signal_manifest(bool dark) {
    return compile_design_manifest(ink_signal_theme(dark), catalog(), "ink-signal",
                                   dark ? "dark" : "light");
}

std::string manifest_to_json(const DesignManifest& manifest) {
    auto root = choc::value::createObject("");
    root.addMember("manifest_version", manifest.manifest_version);
    root.addMember("source", manifest.source);
    root.addMember("appearance", manifest.appearance);
    root.addMember("theme_schema_version", manifest.theme_schema_version);

    auto tokens = choc::value::createEmptyArray();
    for (const auto& t : manifest.tokens) {
        auto obj = choc::value::createObject("");
        obj.addMember("name", t.name);
        obj.addMember("kind", t.kind);
        obj.addMember("value", t.value);
        tokens.addArrayElement(obj);
    }
    root.addMember("tokens", tokens);

    auto components = choc::value::createEmptyArray();
    for (const auto& c : manifest.components) {
        auto obj = choc::value::createObject("");
        obj.addMember("name", c.name);
        obj.addMember("category", c.category);
        obj.addMember("native_class", c.native_class);
        obj.addMember("header", c.header);
        obj.addMember("figma_component", c.figma_component);
        obj.addMember("usage", c.usage);
        auto reskin = choc::value::createEmptyArray();
        for (const auto& tok : c.reskin_tokens) reskin.addArrayElement(tok);
        obj.addMember("reskin_tokens", reskin);
        components.addArrayElement(obj);
    }
    root.addMember("components", components);

    return choc::json::toString(root, /*pretty=*/true);
}

namespace {

std::string member_string(const choc::value::ValueView& obj, const char* key) {
    if (!obj.isObject() || !obj.hasObjectMember(key)) return {};
    auto v = obj[key];
    return v.isString() ? std::string(v.getString()) : std::string{};
}

}  // namespace

DesignManifest manifest_from_json(const std::string& json) {
    DesignManifest m;
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (...) {
        return m;  // malformed input → empty contract, never throw
    }
    if (!root.isObject()) return m;

    m.manifest_version = member_string(root, "manifest_version");
    m.source = member_string(root, "source");
    m.appearance = member_string(root, "appearance");
    if (root.hasObjectMember("theme_schema_version"))
        m.theme_schema_version = root["theme_schema_version"].getWithDefault<int>(0);

    if (root.hasObjectMember("tokens") && root["tokens"].isArray()) {
        auto tokens = root["tokens"];
        for (uint32_t i = 0; i < tokens.size(); ++i) {
            auto t = tokens[i];
            ManifestToken tok{member_string(t, "name"), member_string(t, "kind"),
                              member_string(t, "value")};
            if (!tok.name.empty()) m.tokens.push_back(std::move(tok));
        }
    }
    if (root.hasObjectMember("components") && root["components"].isArray()) {
        auto components = root["components"];
        for (uint32_t i = 0; i < components.size(); ++i) {
            auto c = components[i];
            ManifestComponent comp;
            comp.name = member_string(c, "name");
            comp.category = member_string(c, "category");
            comp.native_class = member_string(c, "native_class");
            comp.header = member_string(c, "header");
            comp.figma_component = member_string(c, "figma_component");
            comp.usage = member_string(c, "usage");
            if (c.isObject() && c.hasObjectMember("reskin_tokens") && c["reskin_tokens"].isArray()) {
                auto rt = c["reskin_tokens"];
                for (uint32_t j = 0; j < rt.size(); ++j)
                    if (rt[j].isString()) comp.reskin_tokens.push_back(std::string(rt[j].getString()));
            }
            if (!comp.name.empty()) m.components.push_back(std::move(comp));
        }
    }
    return m;
}

std::string emit_binding_prompt(const DesignManifest& manifest) {
    std::string out;
    out.reserve(4096);

    out += "# Design binding contract\n\n";
    out += "Source: `" + manifest.source + "` (" + manifest.appearance + ")  \n";
    out += "Manifest: `" + manifest.manifest_version + "`\n\n";
    out += "When generating or editing this UI, bind **only** to the tokens and "
           "components listed below. Do not invent token names, hard-code raw "
           "colors or sizes where a token exists, or target a widget that is not "
           "in the component list. Reference a token by its name (for example "
           "`var(--color-bg)` in web-compat output, or the matching theme key in "
           "native output). A value or widget outside this contract is a "
           "fidelity break and is flagged by the adherence check.\n\n";

    // Tokens grouped by kind, preserving the manifest's sorted order.
    out += "## Allowed tokens\n\n";
    const std::array<std::pair<const char*, const char*>, 3> kinds = {{
        {"color", "Colors"}, {"dimension", "Dimensions"}, {"string", "Strings"}}};
    for (const auto& [kind, heading] : kinds) {
        bool any = false;
        for (const auto& t : manifest.tokens) {
            if (t.kind != kind) continue;
            if (!any) {
                out += std::string("### ") + heading + "\n\n";
                any = true;
            }
            out += "- `" + t.name + "` = " + t.value + "\n";
        }
        if (any) out += "\n";
    }

    out += "## Allowed components\n\n";
    for (const auto& c : manifest.components) {
        out += "- **" + c.name + "** (`" + c.native_class + "`, " + c.category + ")";
        if (!c.usage.empty()) out += " — " + c.usage;
        out += "\n";
        if (!c.reskin_tokens.empty()) {
            out += "  - themed by: ";
            for (size_t i = 0; i < c.reskin_tokens.size(); ++i) {
                if (i) out += ", ";
                out += "`" + c.reskin_tokens[i] + "`";
            }
            out += "\n";
        }
    }
    out += "\n";

    return out;
}

}  // namespace pulp::design
