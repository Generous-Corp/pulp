#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT "."
#endif

namespace {

struct RegistrationSite {
    std::string name;
    int line = 0;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<RegistrationSite> widget_bridge_registrations(const std::string& source) {
    std::vector<RegistrationSite> out;
    const std::regex pattern("engine_\\.register_function\\(\"([^\"]+)\"");
    std::istringstream lines(source);
    std::string line_text;
    int line = 1;
    while (std::getline(lines, line_text)) {
        for (std::sregex_iterator it(line_text.begin(), line_text.end(), pattern), end; it != end; ++it)
            out.push_back({(*it)[1].str(), line});
        ++line;
    }
    return out;
}

} // namespace

TEST_CASE("WidgetBridge JS native API registrations are unique",
          "[view][widget-bridge][api-contract]") {
    const auto source_path = std::filesystem::path(PULP_REPO_ROOT) /
                             "core/view/src/widget_bridge.cpp";
    const auto source = read_text(source_path);
    const auto registrations = widget_bridge_registrations(source);

    std::map<std::string, std::vector<int>> lines_by_name;
    for (const auto& site : registrations)
        lines_by_name[site.name].push_back(site.line);

    std::ostringstream duplicates;
    for (const auto& [name, lines] : lines_by_name) {
        if (lines.size() <= 1) continue;
        duplicates << name << " at lines";
        for (const auto line : lines)
            duplicates << ' ' << line;
        duplicates << '\n';
    }

    INFO("Duplicate WidgetBridge native JS registrations:\n" << duplicates.str());
    REQUIRE(duplicates.str().empty());
}
