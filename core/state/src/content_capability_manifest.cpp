#include <pulp/state/content_capability_manifest.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace pulp::state {

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string string_member(const choc::value::ValueView& object, std::string_view name) {
    auto value = object[name];
    return value.isString() ? std::string(value.getString()) : std::string{};
}

std::vector<std::string> string_array_member(const choc::value::ValueView& object,
                                             std::string_view name) {
    std::vector<std::string> out;
    auto value = object[name];
    if (!value.isArray())
        return out;
    for (uint32_t i = 0; i < value.size(); ++i) {
        auto item = value[i];
        if (item.isString())
            out.emplace_back(item.getString());
    }
    return out;
}

bool all_in(const std::vector<std::string>& values, const std::vector<std::string>& allowed,
            std::string* offending) {
    for (const auto& value : values) {
        if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
            if (offending)
                *offending = value;
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<ContentCapabilityManifest> parse_content_capability_manifest(std::string_view text,
                                                                           std::string* error) {
    auto fail = [&](std::string message) -> std::optional<ContentCapabilityManifest> {
        if (error)
            *error = std::move(message);
        return std::nullopt;
    };

    try {
        auto json = choc::json::parse(std::string(text));
        if (!json.isObject())
            return fail("manifest root must be a JSON object");
        const auto schema = string_member(json, "schema");
        if (schema != "pulp.plugin-runtime.v1")
            return fail("schema must be pulp.plugin-runtime.v1");

        ContentCapabilityManifest manifest;
        manifest.schema = schema;
        manifest.plugin_id = string_member(json, "pluginId");
        if (manifest.plugin_id.empty())
            return fail("pluginId is required");

        auto content = json["content"];
        if (!content.isObject())
            return fail("content object is required");

        manifest.capabilities = string_array_member(content, "capabilities");
        manifest.content_kinds = string_array_member(content, "kinds");
        if (manifest.capabilities.empty())
            return fail("content.capabilities must contain at least one capability");
        if (manifest.content_kinds.empty())
            return fail("content.kinds must contain at least one content kind");

        auto reload = content["reload"];
        if (reload.isObject()) {
            manifest.hot_reload_kinds = string_array_member(reload, "hotReloadKinds");
            manifest.manual_rescan_kinds = string_array_member(reload, "manualRescanKinds");

            std::string unsupported;
            if (!all_in(manifest.hot_reload_kinds, manifest.content_kinds, &unsupported))
                return fail("content.reload.hotReloadKinds contains unsupported kind: " +
                            unsupported);
            if (!all_in(manifest.manual_rescan_kinds, manifest.content_kinds, &unsupported))
                return fail("content.reload.manualRescanKinds contains unsupported kind: " +
                            unsupported);
        }

        if (error)
            error->clear();
        return manifest;
    } catch (const std::exception& e) {
        return fail(std::string("invalid JSON: ") + e.what());
    } catch (...) {
        return fail("invalid JSON");
    }
}

std::optional<ContentCapabilityManifest> load_content_capability_manifest(const fs::path& path,
                                                                          std::string* error) {
    auto text = read_text(path);
    if (text.empty()) {
        if (error)
            *error = "manifest file is missing or empty";
        return std::nullopt;
    }
    return parse_content_capability_manifest(text, error);
}

std::string content_capability_manifest_to_json(const ContentCapabilityManifest& manifest) {
    auto obj = choc::value::createObject("PulpPluginRuntime");
    obj.addMember("schema", manifest.schema.empty() ? "pulp.plugin-runtime.v1" : manifest.schema);
    obj.addMember("pluginId", manifest.plugin_id);

    auto content = choc::value::createObject("ContentCapabilities");
    auto caps = choc::value::createEmptyArray();
    for (const auto& capability : manifest.capabilities)
        caps.addArrayElement(choc::value::Value(capability));
    auto kinds = choc::value::createEmptyArray();
    for (const auto& kind : manifest.content_kinds)
        kinds.addArrayElement(choc::value::Value(kind));
    content.addMember("capabilities", std::move(caps));
    content.addMember("kinds", std::move(kinds));
    if (!manifest.hot_reload_kinds.empty() || !manifest.manual_rescan_kinds.empty()) {
        auto reload = choc::value::createObject("ContentReloadPolicy");
        auto hot = choc::value::createEmptyArray();
        for (const auto& kind : manifest.hot_reload_kinds)
            hot.addArrayElement(choc::value::Value(kind));
        auto manual = choc::value::createEmptyArray();
        for (const auto& kind : manifest.manual_rescan_kinds)
            manual.addArrayElement(choc::value::Value(kind));
        reload.addMember("hotReloadKinds", std::move(hot));
        reload.addMember("manualRescanKinds", std::move(manual));
        content.addMember("reload", std::move(reload));
    }
    obj.addMember("content", std::move(content));
    return choc::json::toString(obj, true);
}

} // namespace pulp::state
