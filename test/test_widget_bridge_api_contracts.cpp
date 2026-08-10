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
        // A public JS global defined in the bridge preamble rather than
        // registered natively. Required whenever the API takes a callback:
        // CHOC's NativeFunction cannot carry a JSValue, so those must be a JS
        // shim over a native primitive. Tracked here so a preamble API cannot
        // ship invisible to the generated docs and @pulp/react typings.
        "preamble_function",
    };
    return kinds;
}

/// A parsed manifest, plus whether its header was where it belongs.
///
/// `header_ok` is REPORTED rather than asserted inside the parser, and that is
/// the whole point. This check used to be a fatal `REQUIRE` here, so a manifest
/// whose header had moved ended the entire TEST_CASE at its first assertion:
/// the category allowlist, the duplicate-name scan and the registrar
/// cross-check below never ran, and stayed silent rather than red. That is not
/// hypothetical — sorting the manifest alphabetically swept the header row out
/// of line 4, and five rows carrying a category that is not in the allowlist
/// shipped behind the mask.
///
/// So a displaced header is now one loud, local failure, and every row is still
/// parsed and still checked.
struct Manifest {
    std::vector<ManifestEntry> entries;
    bool header_ok = false;
    int first_row_line = 0;
};

Manifest read_manifest(const std::filesystem::path& path) {
    INFO("Reading manifest: " << path);
    std::ifstream in(path);
    REQUIRE(in.good());

    Manifest result;
    std::string line;
    int line_number = 0;
    bool examined_first_row = false;
    while (std::getline(in, line)) {
        ++line_number;
        const auto cleaned = trim(line);
        if (cleaned.empty() || starts_with(cleaned, "#"))
            continue;

        auto fields = split_fields(cleaned);
        if (!examined_first_row) {
            // The `jsx` column (5th) is optional per-row but declared in the
            // header so the schema is self-documenting.
            static const std::vector<std::string> expected_header =
                {"name", "category", "kind", "source", "jsx"};
            examined_first_row = true;
            result.first_row_line = line_number;
            if (fields == expected_header) {
                result.header_ok = true;
                continue;               // consume the header row
            }
            // Not the header. Fall through and treat it as data, so the rest of
            // the file is still checked; the caller reports the missing header.
        }

        // 4 fields = no jsx tag; 5 = with the optional @pulp/react tag. Checked
        // per row, and non-fatally for the same reason as the header: one bad
        // row should not cancel the checks covering every other row.
        INFO("Invalid WidgetBridge API manifest row at line " << line_number << ": " << line);
        CHECK((fields.size() == 4 || fields.size() == 5));
        if (fields.size() != 4 && fields.size() != 5) {
            result.entries.push_back({cleaned, "", "", "", "", line_number});
            continue;
        }
        result.entries.push_back({fields[0], fields[1], fields[2], fields[3],
                                  fields.size() == 5 ? fields[4] : std::string{}, line_number});
    }

    REQUIRE_FALSE(result.entries.empty());
    return result;
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

    // Public preamble globals: a `function name(` definition inside the JS the
    // bridge evaluates. Names starting with `__` are preamble-internal helpers
    // (__dispatch__, __invokeTimer__, ...), not API, so they are excluded.
    const std::regex preamble_pattern("function\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s*\\(");

    for (std::sregex_iterator it(source.begin(), source.end(), preamble_pattern), end; it != end; ++it) {
        const auto name = (*it)[1].str();
        if (name.rfind("__", 0) == 0) continue;
        out.push_back({
            name,
            "preamble_function",
            source_path,
            line_number_at(source, static_cast<size_t>((*it).position())),
            true,
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

    const auto parsed = read_manifest(manifest_path);
    // Non-fatal on purpose: a displaced header must not cancel the checks below.
    INFO("The manifest's `name category kind source jsx` header must be the first "
         "non-comment line; found something else at line " << parsed.first_row_line);
    CHECK(parsed.header_ok);
    const auto& manifest = parsed.entries;

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

// A displaced header must fail LOUDLY and LOCALLY, without cancelling the
// checks behind it.
//
// The regression this pins is not "the header moved" — it is that the header
// check used to be a fatal assertion inside the parser, so moving the header
// ended the whole test case at its first check. Every later check, including
// the category allowlist, then passed by never running, and a category that is
// not in the allowlist reached main behind that mask.
//
// So the property under test is the one that was actually missing: when the
// header is wrong, the parser still returns EVERY row, so the allowlist still
// has something to judge.
TEST_CASE("WidgetBridge manifest parsing survives a displaced header",
          "[view][widget-bridge][api-contract]") {
    const auto dir = std::filesystem::temp_directory_path()
                   / "pulp-widget-bridge-manifest-contract";
    std::filesystem::create_directories(dir);
    const auto path = dir / "displaced_header.tsv";

    // The exact shape of the real defect: sorted alphabetically, so the header
    // is no longer the first non-comment line but is still present, and a row
    // carrying a bogus category sits behind it.
    {
        std::ofstream out(path, std::ios::trunc);
        out << "# WidgetBridge native JS API manifest.\n"
            << "__cancelFrame__\truntime\tfunction\tcore/view/src/widget_bridge/runtime_api.cpp\n"
            << "name\tcategory\tkind\tsource\tjsx\n"
            << "setToggleOn\tnot_a_real_category\tfunction\tcore/view/src/widget_bridge/x.cpp\n";
    }

    const auto parsed = read_manifest(path);

    CHECK_FALSE(parsed.header_ok);
    CHECK(parsed.first_row_line == 2);

    // The point of the fix: parsing did not stop, so every row still reaches
    // the allowlist. Three rows, because the displaced header is now read as
    // data too — which is itself detectable, rather than silently fatal.
    REQUIRE(parsed.entries.size() == 3);
    const auto has = [&parsed](std::string_view name) {
        return std::any_of(parsed.entries.begin(), parsed.entries.end(),
                           [name](const ManifestEntry& e) { return e.name == name; });
    };
    CHECK(has("__cancelFrame__"));
    CHECK(has("setToggleOn"));
    CHECK(has("name"));

    // And the bogus category is visible to a caller rather than masked.
    const auto bad = std::find_if(parsed.entries.begin(), parsed.entries.end(),
                                  [](const ManifestEntry& e) { return e.name == "setToggleOn"; });
    REQUIRE(bad != parsed.entries.end());
    CHECK(bad->category == "not_a_real_category");
    CHECK_FALSE(allowed_categories().count(bad->category) > 0);

    std::filesystem::remove_all(dir);
}

// A well-formed header still reads as one, and is still consumed rather than
// returned as a row.
TEST_CASE("WidgetBridge manifest parsing accepts a well-formed header",
          "[view][widget-bridge][api-contract]") {
    const auto dir = std::filesystem::temp_directory_path()
                   / "pulp-widget-bridge-manifest-contract-ok";
    std::filesystem::create_directories(dir);
    const auto path = dir / "good_header.tsv";
    {
        std::ofstream out(path, std::ios::trunc);
        out << "# comment\n"
            << "name\tcategory\tkind\tsource\tjsx\n"
            << "setValue\twidget_value\tfunction\tcore/view/src/widget_bridge/y.cpp\n";
    }

    const auto parsed = read_manifest(path);

    CHECK(parsed.header_ok);
    REQUIRE(parsed.entries.size() == 1);
    CHECK(parsed.entries.front().name == "setValue");

    std::filesystem::remove_all(dir);
}
