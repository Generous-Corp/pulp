#include "au_info_plist.hpp"

#include <fstream>
#include <iterator>
#include <optional>
#include <regex>

namespace pulp::cli::au_info_plist {
namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::optional<std::string> extract_xml_string_value(std::string_view dict,
                                                    std::string_view key) {
    const std::string pattern =
        "<key>\\s*" + std::string(key) + "\\s*</key>\\s*<string>\\s*([^<]+?)\\s*</string>";
    std::cmatch match;
    const auto begin = dict.data();
    const auto end = begin + dict.size();
    if (!std::regex_search(begin, end, match, std::regex(pattern))) return std::nullopt;
    return match[1].str();
}

}  // namespace

std::string parse_unique_id_from_text(std::string_view plist) {
    if (plist.empty()) return {};

    const auto audio_components = plist.find("<key>AudioComponents</key>");
    if (audio_components == std::string_view::npos) return {};
    const auto dict_begin = plist.find("<dict>", audio_components);
    if (dict_begin == std::string_view::npos) return {};
    const auto dict_end = plist.find("</dict>", dict_begin);
    if (dict_end == std::string_view::npos) return {};
    const auto dict = plist.substr(dict_begin, dict_end - dict_begin);

    const auto type = extract_xml_string_value(dict, "type");
    const auto subtype = extract_xml_string_value(dict, "subtype");
    const auto manufacturer = extract_xml_string_value(dict, "manufacturer");
    if (!type || !subtype || !manufacturer) return {};
    return *type + ":" + *subtype + ":" + *manufacturer;
}

std::string unique_id_from_bundle(const std::filesystem::path& bundle) {
    return parse_unique_id_from_text(read_text_file(bundle / "Contents" / "Info.plist"));
}

}  // namespace pulp::cli::au_info_plist
