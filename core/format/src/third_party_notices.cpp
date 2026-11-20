#include <pulp/format/third_party_notices.hpp>

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <cstdint>
#include <sstream>
#include <system_error>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <dlfcn.h>
#endif

namespace pulp::format {
namespace {

std::string string_member(const choc::value::ValueView& object, const char* name) {
    return object.hasObjectMember(name) && object[name].isString()
               ? std::string(object[name].getString())
               : std::string{};
}

std::vector<std::string> string_array_member(const choc::value::ValueView& object,
                                             const char* name) {
    std::vector<std::string> result;
    if (!object.hasObjectMember(name) || !object[name].isArray()) return result;
    const auto values = object[name];
    for (uint32_t i = 0; i < values.size(); ++i) {
        auto item = values[i];
        if (item.isString()) result.emplace_back(item.getString());
    }
    return result;
}

std::filesystem::path resource_path_for_image(const std::filesystem::path& image) {
    if (image.empty()) return {};
    auto current = image;
    if (current.has_filename()) current = current.parent_path();
    for (;;) {
        const auto candidate = current / "Contents" / "Resources" / "ThirdPartyNotices.json";
        if (std::filesystem::is_regular_file(candidate)) return candidate;
        const auto direct = current / "ThirdPartyNotices.json";
        if (std::filesystem::is_regular_file(direct)) return direct;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

std::filesystem::path loaded_image_path() {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&third_party_notices), &info) != 0 && info.dli_fname)
        return info.dli_fname;
#endif
    return {};
}

}  // namespace

ThirdPartyNotices parse_third_party_notices(std::string_view json) {
    ThirdPartyNotices result;
    try {
        const auto root = choc::json::parse(std::string(json));
        if (!root.isObject()) {
            result.reason = "notice document is not an object";
            return result;
        }
        result.schema = string_member(root, "schema");
        result.product = string_member(root, "product");
        result.target = string_member(root, "target");
        result.format = string_member(root, "format");
        result.sdks_without_manifest = string_array_member(root, "sdks_without_manifest");
        result.unattributed_targets = string_array_member(root, "unattributed_targets");
        if (root.hasObjectMember("notices") && root["notices"].isArray()) {
            const auto values = root["notices"];
            for (uint32_t i = 0; i < values.size(); ++i) {
                auto item = values[i];
                if (!item.isObject()) continue;
                ThirdPartyNotice notice;
                notice.name = string_member(item, "name");
                notice.version = string_member(item, "version");
                notice.license = string_member(item, "license");
                notice.copyright = string_member(item, "copyright");
                notice.source = string_member(item, "source");
                notice.license_text = string_member(item, "license_text");
                notice.sha256 = string_member(item, "sha256");
                notice.required_by = string_array_member(item, "required_by");
                notice.feature = string_member(item, "feature");
                result.notices.push_back(std::move(notice));
            }
        }
        if (result.schema != "pulp.third-party-notices.v1")
            result.reason = "unsupported notice schema";
    } catch (const std::exception& error) {
        result.reason = std::string("failed to parse notice document: ") + error.what();
    } catch (...) {
        result.reason = "failed to parse notice document";
    }
    return result;
}

ThirdPartyNotices load_third_party_notices(const std::filesystem::path& bundle) {
    auto path = bundle;
    if (std::filesystem::is_directory(path))
        path = resource_path_for_image(path / "Contents" / "MacOS" / "plugin");
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
        ThirdPartyNotices result;
        result.reason = "ThirdPartyNotices.json is missing";
        return result;
    }
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    if (!input.good() && !input.eof()) {
        ThirdPartyNotices result;
        result.reason = "failed to read ThirdPartyNotices.json";
        return result;
    }
    return parse_third_party_notices(text.str());
}

const ThirdPartyNotices& third_party_notices() {
    static const ThirdPartyNotices cached = [] {
        const auto path = resource_path_for_image(loaded_image_path());
        return path.empty() ? ThirdPartyNotices{.reason = "ThirdPartyNotices.json is missing"}
                            : load_third_party_notices(path);
    }();
    return cached;
}

}  // namespace pulp::format
