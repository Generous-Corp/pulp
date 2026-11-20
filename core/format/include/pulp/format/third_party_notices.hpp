#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format {

struct ThirdPartyNotice {
    std::string name;
    std::string version;
    std::string license;
    std::string copyright;
    std::string source;
    std::string license_text;
    std::string sha256;
    std::vector<std::string> required_by;
    std::string feature;
};

struct ThirdPartyNotices {
    std::string schema;
    std::string product;
    std::string target;
    std::string format;
    std::vector<ThirdPartyNotice> notices;
    std::vector<std::string> sdks_without_manifest;
    std::vector<std::string> unattributed_targets;
    std::string reason;

    bool available() const noexcept { return reason.empty(); }
};

// Loads and parses one Contents/Resources/ThirdPartyNotices.json file.
ThirdPartyNotices load_third_party_notices(const std::filesystem::path& bundle);

// Parses the generated pulp.third-party-notices.v1 document.
ThirdPartyNotices parse_third_party_notices(std::string_view json);

// Reads the notices belonging to the loaded plug-in image. This performs file
// I/O and parsing on first use, so callers must keep it off the audio thread.
const ThirdPartyNotices& third_party_notices();

}  // namespace pulp::format
