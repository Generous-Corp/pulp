#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT "."
#endif

namespace {

struct ManifestEntry {
    std::string name;
    std::string category;
    std::string kind;
    std::string source;
    std::string jsx;  // optional 5th column — @pulp/react reachability tag
    int line = 0;
};

struct RegistrationSite {
    std::string name;
    std::string kind;
    std::string source;
    int line = 0;
    bool registry_backed = false;
};

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string read_text(const std::filesystem::path& path) {
    INFO("Reading source file: " << path);
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::vector<std::string> split_fields(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> fields;
    std::string field;
    while (input >> field)
        fields.push_back(field);
    return fields;
}

const std::set<std::string>& allowed_categories() {
    static const std::set<std::string> categories = {
        "accessibility",
        "animation",
        "audio_meter",
        "canvas2d",
        "css_style",
        "dom",
        "events",
        "gpu",
        "layout",
        "metadata",
        "platform_services",
        "query",
        "runtime",
        "runtime_import",
        "shader",
        "state_binding",
        "storage_assets",
        "svg",
        "theme",
        "tokens",
        "typography",
        "widget_assets",
        "widget_factory",
        "widget_schema",
        "widget_value",
    };
    return categories;
}

const std::set<std::string>& allowed_kinds() {
    static const std::set<std::string> kinds = {
        "function",
        "host_object",
        "promise_function",
    };
    return kinds;
}

std::vector<ManifestEntry> read_manifest(const std::filesystem::path& path) {
    INFO("Reading manifest: " << path);
    std::ifstream in(path);
    REQUIRE(in.good());

    std::vector<ManifestEntry> out;
    std::string line;
    int line_number = 0;
    bool saw_header = false;
    while (std::getline(in, line)) {
        ++line_number;
        const auto cleaned = trim(line);
        if (cleaned.empty() || starts_with(cleaned, "#"))
            continue;

        const auto fields = split_fields(cleaned);
        if (!saw_header) {
            // The `jsx` column (5th) is optional per-row but declared in the
            // header so the schema is self-documenting (pulp #3656 follow-up).
            const std::vector<std::string> expected_header =
                {"name", "category", "kind", "source", "jsx"};
            INFO("Invalid WidgetBridge API manifest header at line " << line_number);
            REQUIRE(fields == expected_header);
            saw_header = true;
            continue;
        }

        INFO("Invalid WidgetBridge API manifest row at line " << line_number << ": " << line);
        // 4 fields = no jsx tag; 5 = with the optional @pulp/react tag.
        REQUIRE((fields.size() == 4 || fields.size() == 5));
        out.push_back({fields[0], fields[1], fields[2], fields[3],
                       fields.size() == 5 ? fields[4] : std::string{}, line_number});
    }

    REQUIRE(saw_header);
    REQUIRE_FALSE(out.empty());
    return out;
}

std::string relative_source_path(const std::filesystem::path& repo_root,
                                 const std::filesystem::path& path) {
    return path.lexically_relative(repo_root).generic_string();
}

std::vector<std::filesystem::path> bridge_registrar_sources(
    const std::filesystem::path& repo_root) {
    const auto view_src = repo_root / "core/view/src";
    std::vector<std::filesystem::path> sources;

    const auto primary = view_src / "widget_bridge.cpp";
    if (std::filesystem::is_regular_file(primary))
        sources.push_back(primary);

    for (const auto& entry : std::filesystem::directory_iterator(view_src)) {
        if (!entry.is_regular_file())
            continue;

        const auto path = entry.path();
        const auto filename = path.filename().generic_string();
        if (filename == "widget_bridge.cpp")
            continue;
        if (starts_with(filename, "widget_bridge_") && path.extension() == ".cpp")
            sources.push_back(path);
    }

    const auto split_registrar_dir = view_src / "widget_bridge";
    if (std::filesystem::is_directory(split_registrar_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(split_registrar_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cpp")
                sources.push_back(entry.path());
        }
    }

    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    REQUIRE_FALSE(sources.empty());
    return sources;
}

int line_number_at(std::string_view text, size_t offset) {
    const auto end = text.begin() + static_cast<std::ptrdiff_t>(offset);
    return static_cast<int>(std::count(text.begin(), end, '\n')) + 1;
}

std::vector<RegistrationSite> widget_bridge_registrations(const std::string& source,
                                                          const std::string& source_path) {
    std::vector<RegistrationSite> out;
    const std::regex direct_pattern(
        "engine_\\s*\\.\\s*register_(function|host_object|promise_function)\\s*\\(\\s*\"([^\"]+)\"");

    for (std::sregex_iterator it(source.begin(), source.end(), direct_pattern), end; it != end; ++it) {
        out.push_back({
            (*it)[2].str(),
            (*it)[1].str(),
            source_path,
            line_number_at(source, static_cast<size_t>((*it).position())),
            false,
        });
    }

    const std::regex registry_pattern(
        "register_bridge_(function|host_object|promise_function)\\s*\\(\\s*[^,]+,\\s*\"([^\"]+)\"");

    for (std::sregex_iterator it(source.begin(), source.end(), registry_pattern), end; it != end; ++it) {
        out.push_back({
            (*it)[2].str(),
            (*it)[1].str(),
            source_path,
            line_number_at(source, static_cast<size_t>((*it).position())),
            true,
        });
    }
    return out;
}

} // namespace

TEST_CASE("WidgetBridge JS native API manifest matches registrar sources",
          "[view][widget-bridge][api-contract]") {
    const auto repo_root = std::filesystem::path(PULP_REPO_ROOT);
    const auto manifest_path = repo_root / "core/view/src/widget_bridge_api_manifest.tsv";

    const auto manifest = read_manifest(manifest_path);
    const auto source_paths = bridge_registrar_sources(repo_root);

    std::set<std::string> scanned_sources;
    std::vector<RegistrationSite> registrations;
    for (const auto& path : source_paths) {
        const auto source = relative_source_path(repo_root, path);
        scanned_sources.insert(source);

        auto source_registrations = widget_bridge_registrations(read_text(path), source);
        registrations.insert(registrations.end(),
                             source_registrations.begin(),
                             source_registrations.end());
    }

    INFO("WidgetBridge native JS registrations found: " << registrations.size());
    REQUIRE_FALSE(registrations.empty());
    const auto registry_backed_count = std::count_if(
        registrations.begin(), registrations.end(),
        [](const RegistrationSite& site) { return site.registry_backed; });
    INFO("WidgetBridge registry-backed native JS registrations found: " << registry_backed_count);
    REQUIRE(registry_backed_count > 0);

    std::map<std::string, std::vector<RegistrationSite>> registrations_by_name;
    for (const auto& site : registrations)
        registrations_by_name[site.name].push_back(site);

    std::ostringstream duplicate_registrations;
    for (const auto& [name, sites] : registrations_by_name) {
        if (sites.size() <= 1)
            continue;
        duplicate_registrations << name;
        for (const auto& site : sites)
            duplicate_registrations << " at " << site.source << ':' << site.line;
        duplicate_registrations << '\n';
    }
    INFO("Duplicate WidgetBridge native JS registrations:\n" << duplicate_registrations.str());
    REQUIRE(duplicate_registrations.str().empty());

    std::map<std::string, ManifestEntry> manifest_by_name;
    std::map<std::string, std::vector<int>> manifest_lines_by_name;
    std::ostringstream invalid_manifest_entries;
    for (const auto& entry : manifest) {
        manifest_lines_by_name[entry.name].push_back(entry.line);

        if (allowed_categories().find(entry.category) == allowed_categories().end()) {
            invalid_manifest_entries << entry.name << " line " << entry.line
                                     << " has unknown category '" << entry.category << "'\n";
        }
        if (allowed_kinds().find(entry.kind) == allowed_kinds().end()) {
            invalid_manifest_entries << entry.name << " line " << entry.line
                                     << " has unknown kind '" << entry.kind << "'\n";
        }
        if (scanned_sources.find(entry.source) == scanned_sources.end()) {
            invalid_manifest_entries << entry.name << " line " << entry.line
                                     << " owns unscanned source '" << entry.source << "'\n";
        }
        // pulp #3656 follow-up — when the optional `jsx` tag is present it
        // must start with a known @pulp/react-reachability prefix so the
        // vitest jsx-parity contract can classify it. The full prop/factory/
        // geometry ↔ @pulp/react wiring is checked TS-side in
        // packages/pulp-react/test/widget-bridge-jsx-contract.test.ts.
        if (!entry.jsx.empty()) {
            static const std::vector<std::string> kJsxPrefixes = {
                "prop:", "factory:", "geometry:", "event:", "internal"};
            const bool ok = std::any_of(
                kJsxPrefixes.begin(), kJsxPrefixes.end(),
                [&](const std::string& p) { return starts_with(entry.jsx, p); });
            if (!ok) {
                invalid_manifest_entries << entry.name << " line " << entry.line
                    << " has unknown jsx tag '" << entry.jsx << "'\n";
            }
        }

        manifest_by_name.emplace(entry.name, entry);
    }

    for (const auto& [name, lines] : manifest_lines_by_name) {
        if (lines.size() <= 1)
            continue;
        invalid_manifest_entries << name << " appears in manifest lines";
        for (const auto line : lines)
            invalid_manifest_entries << ' ' << line;
        invalid_manifest_entries << '\n';
    }
    INFO("Invalid WidgetBridge API manifest entries:\n" << invalid_manifest_entries.str());
    REQUIRE(invalid_manifest_entries.str().empty());

    std::ostringstream manifest_mismatches;
    for (const auto& site : registrations) {
        const auto manifest_it = manifest_by_name.find(site.name);
        if (manifest_it == manifest_by_name.end()) {
            manifest_mismatches << site.name << " registered at " << site.source << ':' << site.line
                                << " is missing from core/view/src/widget_bridge_api_manifest.tsv\n";
            continue;
        }

        const auto& entry = manifest_it->second;
        if (entry.kind != site.kind) {
            manifest_mismatches << site.name << " registered at " << site.source << ':' << site.line
                                << " has kind '" << site.kind << "' but manifest line " << entry.line
                                << " says '" << entry.kind << "'\n";
        }
        if (entry.source != site.source) {
            manifest_mismatches << site.name << " registered at " << site.source << ':' << site.line
                                << " but manifest line " << entry.line
                                << " owns '" << entry.source << "'\n";
        }
    }

    for (const auto& [name, entry] : manifest_by_name) {
        if (registrations_by_name.find(name) == registrations_by_name.end()) {
            manifest_mismatches << name << " at manifest line " << entry.line
                                << " is not registered by any WidgetBridge registrar source\n";
        }
    }

    INFO("WidgetBridge API manifest mismatches:\n" << manifest_mismatches.str());
    REQUIRE(manifest_mismatches.str().empty());
}
