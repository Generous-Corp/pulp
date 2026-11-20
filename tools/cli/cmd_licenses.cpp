#include "cli_common.hpp"

#include <pulp/format/third_party_notices.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

namespace {

void usage() {
    std::cout << "Usage: pulp licenses <bundle-path> [--json] [--check]\n";
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

} // namespace

int cmd_licenses(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 2 : 0;
    }
    bool json = false;
    bool check = false;
    std::filesystem::path bundle;
    for (const auto& arg : args) {
        if (arg == "--json")
            json = true;
        else if (arg == "--check")
            check = true;
        else if (bundle.empty())
            bundle = arg;
        else {
            std::cerr << "pulp licenses: unexpected argument '" << arg << "'\n";
            return 2;
        }
    }
    if (bundle.empty()) {
        usage();
        return 2;
    }
    auto json_path = bundle;
    if (std::filesystem::is_directory(json_path)) {
        if (std::filesystem::exists(json_path / "Contents" / "Resources" /
                                    "ThirdPartyNotices.json"))
            json_path /= "Contents/Resources/ThirdPartyNotices.json";
        else
            json_path /= "ThirdPartyNotices.json";
    }
    if (!std::filesystem::is_regular_file(json_path)) {
        std::cerr << "pulp licenses: ThirdPartyNotices.json is missing\n";
        return 1;
    }
    const auto raw = read_text(json_path);
    const auto notices = pulp::format::parse_third_party_notices(raw);
    if (!notices.available()) {
        std::cerr << "pulp licenses: "
                  << (notices.reason.empty() ? "missing notice document" : notices.reason) << "\n";
        return 1;
    }
    if (json) {
        std::cout << raw;
        if (raw.empty() || raw.back() != '\n')
            std::cout << '\n';
    } else {
        auto text_path = json_path.parent_path() / "ThirdPartyNotices.txt";
        if (std::filesystem::exists(text_path))
            std::cout << read_text(text_path);
        else {
            std::cout << "Third-party notices for " << notices.product << " (" << notices.format
                      << ")\n\n";
            for (const auto& notice : notices.notices)
                std::cout << notice.name << " " << notice.version << " — " << notice.license
                          << "\n";
        }
    }
    if (check && (!notices.sdks_without_manifest.empty() || !notices.unattributed_targets.empty()))
        return 1;
    return 0;
}
