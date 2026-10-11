#pragma once

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pulp::import_design {

// Why an archive member name could write outside the extraction directory, or
// nullopt when it cannot.
//   - `..` anywhere catches `a/../../etc/x` and a trailing `..`.
//   - is_absolute() catches POSIX `/foo`, Windows `C:\foo` and UNC `\\srv\sh\x`
//     on the host that would honour them.
//   - A drive-relative `C:foo` is not absolute to std::filesystem on Linux or
//     macOS, where this CLI also reads archives authored on Windows.
//   - A leading `\` is an ordinary character to fs::path on a Unix host.
inline std::optional<std::string_view> unsafe_zip_entry_reason(const std::string& name) {
    if (name.find("..") != std::string::npos) return "contains '..'";
    if (std::filesystem::path(name).is_absolute()) return "absolute path";
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
        name[1] == ':')
        return "drive-relative Windows path";
    if (!name.empty() && (name[0] == '/' || name[0] == '\\')) return "leading slash";
    return std::nullopt;
}

// The member names already extracted from one archive, keyed by the file they
// land on. Two members that land on one file make the import ambiguous: the
// second silently overwrites the first, so which document (or asset) was
// imported depends on archive order. The key folds case and normalizes the
// path, because extraction targets a case-insensitive filesystem on macOS and
// Windows by default, where `Scene.pulp.json` and `./scene.pulp.json` are both
// the same file as `scene.pulp.json`.
class ZipMemberSet {
public:
    // The earlier member that lands on the same file as @p name, or nullopt
    // after recording @p name as the owner of that file.
    std::optional<std::string> claim(const std::string& name) {
        std::string key = std::filesystem::path(name).lexically_normal().generic_string();
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto [it, inserted] = claimed_.emplace(std::move(key), name);
        if (inserted) return std::nullopt;
        return it->second;
    }

private:
    std::unordered_map<std::string, std::string> claimed_;
};

}  // namespace pulp::import_design
