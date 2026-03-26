// pulp — CLI tool for the Pulp audio plugin framework
// Wraps common build/test/status operations

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── Helpers ──────────────────────────────────────────────────────────────────

static int run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

static std::string exec_output(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static fs::path find_project_root() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "core")) {
            return dir;
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// ── Commands ─────────────────────────────────────────────────────────────────

static int cmd_build(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    bool needs_configure = !fs::exists(build_dir / "CMakeCache.txt");

    // Check if CMakeLists.txt is newer than CMakeCache
    if (!needs_configure && fs::exists(build_dir / "CMakeCache.txt")) {
        auto cmake_time = fs::last_write_time(root / "CMakeLists.txt");
        auto cache_time = fs::last_write_time(build_dir / "CMakeCache.txt");
        if (cmake_time > cache_time) needs_configure = true;
    }

    if (needs_configure) {
        std::cout << "Configuring...\n";
        int rc = run("cmake -B " + build_dir.string() + " -S " + root.string());
        if (rc != 0) return rc;
    }

    std::cout << "Building...\n";

    std::string build_cmd = "cmake --build " + build_dir.string();

    // Pass through extra args (e.g., --target, -j)
    for (auto& arg : args) {
        build_cmd += " " + arg;
    }

    return run(build_cmd);
}

static int cmd_test(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Build directory not found, building first...\n";
        int rc = cmd_build({});
        if (rc != 0) return rc;
    }

    std::string test_cmd = "ctest --test-dir " + build_dir.string() + " --output-on-failure";

    for (auto& arg : args) {
        test_cmd += " " + arg;
    }

    return run(test_cmd);
}

static int cmd_status([[maybe_unused]] const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    // Read project info from CMakeLists.txt
    std::cout << "Pulp Project Status\n";
    std::cout << "====================\n";
    std::cout << "Root: " << root.string() << "\n";

    // Git info
    auto branch = exec_output("git -C " + root.string() + " branch --show-current");
    auto commit = exec_output("git -C " + root.string() + " log --oneline -1");
    if (!branch.empty()) std::cout << "Branch: " << branch << "\n";
    if (!commit.empty()) std::cout << "Commit: " << commit << "\n";

    // Build state
    auto build_dir = root / "build";
    if (fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Build: configured\n";
    } else {
        std::cout << "Build: not configured (run `pulp build`)\n";
    }

    // Count source files
    int cpp_count = 0, hpp_count = 0, test_count = 0;
    for (auto& entry : fs::recursive_directory_iterator(root / "core")) {
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".mm") ++cpp_count;
        if (ext == ".hpp" || ext == ".h") ++hpp_count;
    }
    for (auto& entry : fs::directory_iterator(root / "test")) {
        if (entry.path().extension() == ".cpp") ++test_count;
    }
    std::cout << "Source files: " << cpp_count << " impl, " << hpp_count << " headers\n";
    std::cout << "Test files: " << test_count << "\n";

    // Count examples
    int example_count = 0;
    if (fs::exists(root / "examples")) {
        for (auto& entry : fs::directory_iterator(root / "examples")) {
            if (entry.is_directory()) ++example_count;
        }
    }
    std::cout << "Examples: " << example_count << "\n";

    // Check for plugin formats
    std::cout << "\nPlugin Formats:\n";
    if (fs::exists(root / "external" / "vst3sdk"))
        std::cout << "  VST3: available\n";
    else
        std::cout << "  VST3: SDK not found\n";

    if (fs::exists(root / "external" / "AudioUnitSDK"))
        std::cout << "  AU:   available\n";
    else
        std::cout << "  AU:   SDK not found\n";

    std::cout << "  CLAP: available (fetched via CMake)\n";

    return 0;
}

static int cmd_clean([[maybe_unused]] const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (fs::exists(build_dir)) {
        std::cout << "Removing build directory...\n";
        fs::remove_all(build_dir);
        std::cout << "Clean.\n";
    } else {
        std::cout << "Nothing to clean.\n";
    }
    return 0;
}

static int cmd_validate(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir)) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    int total = 0, passed = 0, failed = 0, skipped = 0;

    // CLAP validation
    auto clap_dir = build_dir / "CLAP";
    if (fs::exists(clap_dir)) {
        bool has_clap_validator = !exec_output("which clap-validator 2>/dev/null").empty();

        for (auto& entry : fs::directory_iterator(clap_dir)) {
            if (entry.path().extension() == ".clap") {
                auto name = entry.path().stem().string();
                ++total;

                if (has_clap_validator) {
                    std::cout << "CLAP: validating " << name << "... ";
                    auto clap_path = entry.path().string();
                    int rc = run("clap-validator validate \"" + clap_path + "\" 2>/dev/null");
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                    }
                } else {
                    // Fallback: dlopen test
                    std::cout << "CLAP: " << name << " (dlopen check only, clap-validator not installed)... ";
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R clap-dlopen-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                    }
                }
            }
        }
    }

    // AU validation (macOS only)
#ifdef __APPLE__
    auto au_dir = build_dir / "AU";
    if (fs::exists(au_dir)) {
        for (auto& entry : fs::directory_iterator(au_dir)) {
            if (entry.path().extension() == ".component") {
                auto name = entry.path().stem().string();
                ++total;
                std::cout << "AU: " << name << " (auval check)... ";

                // Check if auval exists
                if (!exec_output("which auval 2>/dev/null").empty()) {
                    // Run auval test from ctest
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R auval-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                    }
                } else {
                    std::cout << "SKIPPED (auval not found)\n";
                    ++skipped;
                }
            }
        }
    }
#endif

    std::cout << "\nValidation Summary: " << total << " total, "
              << passed << " passed, " << failed << " failed, "
              << skipped << " skipped\n";

    return failed > 0 ? 1 : 0;
}

static int cmd_ship(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    // Parse subcommand
    std::string sub = args.empty() ? "help" : args[0];

    if (sub == "sign") {
        // Sign all plugin bundles
        std::string identity;
        std::string entitlements = (root / "ship" / "templates" / "entitlements.plist").string();
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--identity" && i + 1 < args.size())
                identity = args[++i];
            else if (args[i] == "--entitlements" && i + 1 < args.size())
                entitlements = args[++i];
        }
        if (identity.empty()) {
            std::cerr << "Usage: pulp ship sign --identity \"Developer ID Application: ...\"\n";
            return 1;
        }

        int signed_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << "Signing " << entry.path().filename().string() << "...\n";
                    std::string cmd = "codesign --force --sign \"" + identity
                        + "\" --timestamp --options runtime"
                        + " --entitlements \"" + entitlements + "\""
                        + " \"" + entry.path().string() + "\"";
                    if (run(cmd) == 0) ++signed_count;
                    else std::cerr << "  FAILED\n";
                }
            }
        }
        std::cout << "Signed " << signed_count << " bundles.\n";
        return 0;
    }

    if (sub == "package") {
        auto artifacts = root / "artifacts";
        fs::create_directories(artifacts);

        std::string version = "0.1.0";
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version" && i + 1 < args.size())
                version = args[++i];
        }

        int pkg_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            std::string format_lower = dir_name;
            for (auto& c : format_lower) c = static_cast<char>(std::tolower(c));

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    auto name = entry.path().stem().string();
                    auto pkg_name = name + "-" + dir_name + "-" + version + ".pkg";
                    auto pkg_path = artifacts / pkg_name;

                    std::string install_loc = "/Library/Audio/Plug-Ins/";
                    if (ext == ".vst3") install_loc += "VST3/";
                    else if (ext == ".clap") install_loc += "CLAP/";
                    else install_loc = std::string(getenv("HOME") ? getenv("HOME") : "~")
                                     + "/Library/Audio/Plug-Ins/Components/";

                    std::cout << "Packaging " << name << " (" << dir_name << ")...\n";
                    std::string cmd = "pkgbuild --component \"" + entry.path().string() + "\""
                        + " --identifier \"com.pulp." + name + "." + format_lower + "\""
                        + " --version \"" + version + "\""
                        + " --install-location \"" + install_loc + "\""
                        + " \"" + pkg_path.string() + "\" 2>/dev/null";
                    if (run(cmd) == 0) ++pkg_count;
                    else std::cerr << "  FAILED\n";
                }
            }
        }
        std::cout << "Created " << pkg_count << " packages in " << artifacts.string() << "\n";
        return 0;
    }

    if (sub == "check") {
        // Check signing status of all built plugins
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << entry.path().filename().string() << ": ";
                    int rc = run("codesign --verify --deep --strict \""
                                 + entry.path().string() + "\" 2>/dev/null");
                    std::cout << (rc == 0 ? "signed" : "unsigned") << "\n";
                }
            }
        }
        return 0;
    }

    // Help
    std::cout << "pulp ship — signing and packaging commands\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  sign     Sign all plugin bundles\n";
    std::cout << "           --identity \"Developer ID Application: ...\"\n";
    std::cout << "           --entitlements path/to/entitlements.plist\n";
    std::cout << "  package  Create .pkg installers for all built plugins\n";
    std::cout << "           --version 1.0.0\n";
    std::cout << "  check    Check signing status of built plugins\n";
    return 0;
}

// ── Docs helpers ────────────────────────────────────────────────────────────

static std::string read_file_contents(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Trim leading/trailing whitespace from a string
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Case-insensitive substring search
static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

// Simple YAML line parser: returns value after "key: " on a line like "  key: value"
static std::string yaml_value(const std::string& line, const std::string& key) {
    auto pos = line.find(key + ":");
    if (pos == std::string::npos) return {};
    auto val_start = pos + key.size() + 1;
    if (val_start >= line.size()) return {};
    return trim(line.substr(val_start));
}

// ── pulp docs ───────────────────────────────────────────────────────────────

static int docs_index(const fs::path& docs_dir) {
    auto index_path = docs_dir / "status" / "docs-index.yaml";
    std::string content = read_file_contents(index_path);
    if (content.empty()) {
        std::cerr << "Error: docs index not found at " << index_path.string() << "\n";
        std::cerr << "Hint: the docs/ tree may not be set up yet.\n";
        return 1;
    }

    std::cout << "Available Documentation\n";
    std::cout << "=======================\n\n";

    std::istringstream stream(content);
    std::string line;
    std::string slug, path, kind;

    auto flush_entry = [&]() {
        if (!slug.empty()) {
            std::cout << "  " << slug;
            if (!kind.empty()) std::cout << " (" << kind << ")";
            std::cout << "\n";
            if (!path.empty()) std::cout << "    -> docs/" << path << "\n";
        }
        slug.clear(); path.clear(); kind.clear();
    };

    while (std::getline(stream, line)) {
        auto s = yaml_value(line, "slug");
        if (!s.empty()) {
            flush_entry();
            slug = s;
        }
        auto p = yaml_value(line, "path");
        if (!p.empty()) path = p;
        auto k = yaml_value(line, "kind");
        if (!k.empty()) kind = k;
    }
    flush_entry();

    std::cout << "\nUse `pulp docs open <slug>` to read a doc.\n";
    return 0;
}

static int docs_search(const fs::path& docs_dir, const std::string& query) {
    if (query.empty()) {
        std::cerr << "Usage: pulp docs search <query>\n";
        return 1;
    }
    if (!fs::exists(docs_dir)) {
        std::cerr << "Error: docs/ directory not found.\n";
        return 1;
    }

    int match_count = 0;
    for (auto& entry : fs::recursive_directory_iterator(docs_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        std::string line;
        int line_num = 0;
        bool file_printed = false;
        int matches_in_file = 0;

        while (std::getline(f, line)) {
            ++line_num;
            if (icontains(line, query)) {
                if (!file_printed) {
                    auto rel = fs::relative(entry.path(), docs_dir);
                    std::cout << "\ndocs/" << rel.string() << ":\n";
                    file_printed = true;
                }
                // Print up to 5 matches per file
                if (matches_in_file < 5) {
                    // Truncate long lines for display
                    std::string display = line;
                    if (display.size() > 120) display = display.substr(0, 117) + "...";
                    std::cout << "  " << line_num << ": " << trim(display) << "\n";
                }
                ++matches_in_file;
                ++match_count;
            }
        }
        if (matches_in_file > 5) {
            std::cout << "  ... and " << (matches_in_file - 5) << " more matches\n";
        }
    }

    if (match_count == 0) {
        std::cout << "No matches for \"" << query << "\" in docs/\n";
    } else {
        std::cout << "\n" << match_count << " match(es) found.\n";
    }
    return 0;
}

static int docs_open(const fs::path& docs_dir, const std::string& slug) {
    if (slug.empty()) {
        std::cerr << "Usage: pulp docs open <slug>\n";
        return 1;
    }

    auto index_path = docs_dir / "status" / "docs-index.yaml";
    std::string index_content = read_file_contents(index_path);
    if (index_content.empty()) {
        std::cerr << "Error: docs index not found at " << index_path.string() << "\n";
        return 1;
    }

    // Find the path for the given slug
    std::istringstream stream(index_content);
    std::string line;
    std::string current_slug, current_path;
    bool found = false;

    while (std::getline(stream, line)) {
        auto s = yaml_value(line, "slug");
        if (!s.empty()) {
            // Check if previous entry matched
            if (current_slug == slug && !current_path.empty()) {
                found = true;
                break;
            }
            current_slug = s;
            current_path.clear();
        }
        auto p = yaml_value(line, "path");
        if (!p.empty()) current_path = p;
    }
    // Check last entry
    if (!found && current_slug == slug && !current_path.empty()) {
        found = true;
    }

    if (!found) {
        std::cerr << "Error: no doc found for slug \"" << slug << "\"\n";
        std::cerr << "Run `pulp docs index` to see available docs.\n";
        return 1;
    }

    auto file_path = docs_dir / current_path;
    std::string content = read_file_contents(file_path);
    if (content.empty()) {
        std::cerr << "Error: file not found at " << file_path.string() << "\n";
        return 1;
    }

    std::cout << content;
    return 0;
}

static int docs_show_support(const fs::path& docs_dir, const std::string& thing) {
    if (thing.empty()) {
        std::cerr << "Usage: pulp docs show support <thing>\n";
        return 1;
    }

    auto matrix_path = docs_dir / "status" / "support-matrix.yaml";
    std::string content = read_file_contents(matrix_path);
    if (content.empty()) {
        std::cerr << "Error: support matrix not found at " << matrix_path.string() << "\n";
        return 1;
    }

    // Structured YAML lookup: parse section > entry > fields
    // The YAML has this structure:
    //   section_name:        (indent 0)
    //     entry_name:        (indent 2) — or "  - name: X" style not used here
    //       field: value     (indent 4)
    //
    // We match `thing` as either a section name or an entry name (exact, case-insensitive).

    std::istringstream stream(content);
    std::string line;
    bool found = false;

    std::string section_name;
    std::string entry_name;

    // Lowercase version of query for matching
    std::string query_lower = thing;
    for (auto& c : query_lower) c = static_cast<char>(std::tolower(c));

    // First pass: try exact match on section or entry names
    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) continue;

        // Top-level section (indent 0, ends with ':')
        if (indent == 0 && !trimmed.empty() && trimmed.back() == ':') {
            section_name = trimmed.substr(0, trimmed.size() - 1);
            continue;
        }

        // Entry-level key (indent 2, ends with ':')
        if (indent == 2 && !trimmed.empty() && trimmed.back() == ':') {
            entry_name = trimmed.substr(0, trimmed.size() - 1);

            std::string entry_lower = entry_name;
            for (auto& c : entry_lower) c = static_cast<char>(std::tolower(c));

            if (entry_lower == query_lower) {
                if (!found) {
                    std::cout << "Support info for \"" << thing << "\":\n\n";
                    found = true;
                }
                std::cout << "  Section:  " << section_name << "\n";
                std::cout << "  Entry:    " << entry_name << "\n";

                // Read child fields (indent > 2)
                while (std::getline(stream, line)) {
                    auto ni = line.find_first_not_of(' ');
                    if (ni == std::string::npos || ni <= 2) {
                        // Push back by re-processing this line next iteration
                        // We can't seek reliably on stringstream, so just break
                        break;
                    }
                    std::string field = trim(line);
                    if (field.empty() || field[0] == '#') continue;
                    auto colon = field.find(':');
                    if (colon != std::string::npos) {
                        auto key = trim(field.substr(0, colon));
                        auto val = trim(field.substr(colon + 1));
                        // Capitalize first letter of key for display
                        if (!key.empty()) key[0] = static_cast<char>(std::toupper(key[0]));
                        std::cout << "  " << key << ": " << val << "\n";
                    }
                }
                std::cout << "\n";
            }
            continue;
        }

        // Check if this is a "key: value" line at indent 2 inside subsystems section
        // (subsystems uses "module_name: status" format at indent 2)
        if (indent == 2 && !trimmed.empty() && trimmed.back() != ':') {
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                auto key = trim(trimmed.substr(0, colon));
                auto val = trim(trimmed.substr(colon + 1));
                std::string key_lower = key;
                for (auto& c : key_lower) c = static_cast<char>(std::tolower(c));

                if (key_lower == query_lower) {
                    if (!found) {
                        std::cout << "Support info for \"" << thing << "\":\n\n";
                        found = true;
                    }
                    std::cout << "  Section: " << section_name << "\n";
                    std::cout << "  " << key << ": " << val << "\n\n";
                }
            }
        }
    }

    // If exact match failed, try section-level match
    if (!found) {
        stream.clear();
        stream.str(content);
        section_name.clear();

        std::string section_lower;
        bool in_matching_section = false;

        while (std::getline(stream, line)) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            auto indent = line.find_first_not_of(' ');
            if (indent == std::string::npos) continue;

            if (indent == 0 && !trimmed.empty() && trimmed.back() == ':') {
                section_name = trimmed.substr(0, trimmed.size() - 1);
                section_lower = section_name;
                for (auto& c : section_lower) c = static_cast<char>(std::tolower(c));
                in_matching_section = (section_lower == query_lower);
                if (in_matching_section && !found) {
                    std::cout << "Support info for \"" << thing << "\":\n\n";
                    std::cout << "[" << section_name << "]\n";
                    found = true;
                }
                continue;
            }

            if (in_matching_section && indent >= 2) {
                auto colon = trimmed.find(':');
                if (colon != std::string::npos && trimmed[0] != '-') {
                    auto key = trim(trimmed.substr(0, colon));
                    auto val = trim(trimmed.substr(colon + 1));
                    if (!val.empty()) {
                        std::cout << "  " << key << ": " << val << "\n";
                    } else {
                        std::cout << "  " << key << ":\n";
                    }
                }
            }
        }
    }

    if (!found) {
        std::cerr << "No support info found for \"" << thing << "\"\n";
        std::cerr << "Available sections: platforms, formats, audio_io, midi_io, rendering, subsystems\n";
        std::cerr << "Available entries: macos, windows, linux, vst3, au_v2, clap, standalone, etc.\n";
        return 1;
    }
    return 0;
}

static int docs_show_command(const fs::path& docs_dir, const std::string& name) {
    if (name.empty()) {
        std::cerr << "Usage: pulp docs show command <name>\n";
        return 1;
    }

    auto cmd_path = docs_dir / "status" / "cli-commands.yaml";
    std::string content = read_file_contents(cmd_path);
    if (content.empty()) {
        std::cerr << "Error: CLI commands manifest not found at " << cmd_path.string() << "\n";
        return 1;
    }

    // Parse the YAML to find the matching command entry and collect its fields
    std::istringstream stream(content);
    std::string line;
    bool found = false;
    bool in_entry = false;
    bool in_subcommands = false;
    bool in_args = false;
    bool in_sub_args = false;

    std::string cmd_status, cmd_summary, cmd_docs;
    struct SubCmd { std::string name; std::string summary; };
    struct Arg { std::string name; std::string required; std::string description; std::string kind; };
    std::vector<SubCmd> subcommands;
    std::vector<Arg> top_args;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        auto indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) indent = 0;

        auto n = yaml_value(line, "name");

        // Top-level command entry (indent <= 4, typically "  - name: X")
        if (!n.empty() && indent <= 4) {
            if (in_entry) break;  // We already found our match, stop at next top-level entry
            if (n == name) {
                found = true;
                in_entry = true;
            }
            continue;
        }

        if (!in_entry) continue;

        // Detect section transitions within the entry
        if (trimmed.find("subcommands:") == 0) {
            in_subcommands = true;
            in_args = false;
            in_sub_args = false;
            continue;
        }
        if (trimmed.find("args:") == 0 && !in_subcommands) {
            in_args = true;
            in_subcommands = false;
            in_sub_args = false;
            continue;
        }

        // Inside subcommands list
        if (in_subcommands) {
            // Sub-args within a subcommand -- skip arg entries for display
            if (trimmed.find("args:") == 0) {
                in_sub_args = true;
                continue;
            }
            if (in_sub_args) {
                // A new subcommand entry (- name: at subcommand indent) ends sub-args
                if (!n.empty() && indent <= 6) {
                    in_sub_args = false;
                } else {
                    continue;  // Skip arg detail lines
                }
            }
            if (!n.empty()) {
                subcommands.push_back({n, {}});
                continue;
            }
            auto s = yaml_value(line, "summary");
            if (!s.empty() && !subcommands.empty()) {
                subcommands.back().summary = s;
                continue;
            }
        }

        // Inside top-level args list
        if (in_args) {
            if (!n.empty()) {
                top_args.push_back({n, {}, {}, {}});
                continue;
            }
            auto r = yaml_value(line, "required");
            if (!r.empty() && !top_args.empty()) { top_args.back().required = r; continue; }
            auto d = yaml_value(line, "description");
            if (!d.empty() && !top_args.empty()) { top_args.back().description = d; continue; }
            auto k = yaml_value(line, "kind");
            if (!k.empty() && !top_args.empty()) { top_args.back().kind = k; continue; }
        }

        // Top-level scalar fields
        if (!in_subcommands && !in_args) {
            auto st = yaml_value(line, "status");
            if (!st.empty()) { cmd_status = st; continue; }
            auto su = yaml_value(line, "summary");
            if (!su.empty()) { cmd_summary = su; continue; }
            auto dc = yaml_value(line, "docs");
            if (!dc.empty()) { cmd_docs = dc; continue; }
        }
    }

    if (!found) {
        std::cerr << "No command found for \"" << name << "\"\n";
        std::cerr << "Check docs/status/cli-commands.yaml for available commands.\n";
        return 1;
    }

    // Render the output
    std::cout << "Command: " << name << "\n";
    if (!cmd_status.empty())  std::cout << "  Status:  " << cmd_status << "\n";
    if (!cmd_summary.empty()) std::cout << "  Summary: " << cmd_summary << "\n";

    if (!top_args.empty()) {
        std::cout << "\n  Arguments:\n";
        for (auto& a : top_args) {
            std::cout << "    " << a.name;
            if (!a.kind.empty()) std::cout << " (" << a.kind << ")";
            if (!a.description.empty()) std::cout << " — " << a.description;
            std::cout << "\n";
        }
    }

    if (!subcommands.empty()) {
        std::cout << "\n  Subcommands:\n";
        for (auto& sc : subcommands) {
            std::cout << "    " << sc.name;
            if (!sc.summary.empty()) std::cout << " — " << sc.summary;
            std::cout << "\n";
        }
    }

    std::cout << "\nSee also: docs/reference/cli.md\n";
    return 0;
}

static int docs_show_cmake(const fs::path& docs_dir, const std::string& name) {
    if (name.empty()) {
        std::cerr << "Usage: pulp docs show cmake <name>\n";
        return 1;
    }

    auto cmake_path = docs_dir / "status" / "cmake-functions.yaml";
    std::string content = read_file_contents(cmake_path);
    if (content.empty()) {
        std::cerr << "Error: CMake functions manifest not found at " << cmake_path.string() << "\n";
        return 1;
    }

    std::istringstream stream(content);
    std::string line;
    bool found = false;
    bool in_entry = false;

    while (std::getline(stream, line)) {
        auto n = yaml_value(line, "name");
        if (!n.empty()) {
            if (in_entry) break;
            if (n == name) {
                found = true;
                in_entry = true;
                std::cout << "CMake function: " << n << "\n";
            }
            continue;
        }
        if (in_entry) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed == "-") continue;
            std::cout << "  " << trimmed << "\n";
        }
    }

    if (!found) {
        std::cerr << "No CMake function found for \"" << name << "\"\n";
        std::cerr << "Check docs/status/cmake-functions.yaml for available functions.\n";
        return 1;
    }

    std::cout << "\nSee also: docs/reference/cmake.md\n";
    return 0;
}

static int docs_show_style(const fs::path& docs_dir) {
    auto style_path = docs_dir / "status" / "style-rules.yaml";
    std::string content = read_file_contents(style_path);
    if (content.empty()) {
        std::cerr << "Error: style rules not found at " << style_path.string() << "\n";
        return 1;
    }

    std::cout << "Style Rules\n";
    std::cout << "===========\n\n";

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        // Print rule entries readably
        auto id = yaml_value(line, "id");
        auto rule = yaml_value(line, "rule");
        auto severity = yaml_value(line, "severity");
        if (!id.empty()) {
            std::cout << "\n[" << id << "]\n";
        } else if (!rule.empty()) {
            std::cout << "  Rule: " << rule << "\n";
        } else if (!severity.empty()) {
            std::cout << "  Severity: " << severity << "\n";
        } else {
            // Print other key-value pairs
            auto colon = trimmed.find(':');
            if (colon != std::string::npos && trimmed.front() != '-' && trimmed.front() != '#') {
                auto key = trim(trimmed.substr(0, colon));
                auto val = trim(trimmed.substr(colon + 1));
                if (!key.empty() && !val.empty()) {
                    std::cout << "  " << key << ": " << val << "\n";
                }
            }
        }
    }

    std::cout << "\nFull details:\n";
    std::cout << "  docs/policies/code-style.md\n";
    std::cout << "  docs/policies/agent-contribution-rules.md\n";
    return 0;
}

static int cmd_docs(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto docs_dir = root / "docs";

    if (args.empty()) {
        std::cout << "pulp docs — local documentation reader\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  index                    List available docs\n";
        std::cout << "  search <query>           Search docs for a string\n";
        std::cout << "  open <slug>              Print a doc by slug\n";
        std::cout << "  show support <thing>     Look up support status\n";
        std::cout << "  show command <name>      Look up a CLI command\n";
        std::cout << "  show cmake <name>        Look up a CMake function\n";
        std::cout << "  show style               Show code style rules\n";
        std::cout << "  check                    Validate docs consistency\n";
        std::cout << "  build-site               Generate static docs site\n";
        std::cout << "  build-api                Generate API reference (Doxygen)\n";
        return 0;
    }

    std::string sub = args[0];

    if (sub == "build-site") {
        auto script = root / "tools" / "build-docs.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: build script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        // Pass through extra args
        for (size_t i = 1; i < args.size(); ++i) {
            cmd += " " + args[i];
        }
        return run(cmd);
    }

    if (sub == "build-api") {
        auto script = root / "tools" / "build-api-docs.sh";
        if (!fs::exists(script)) {
            std::cerr << "Error: build script not found at " << script.string() << "\n";
            return 1;
        }
        return run("bash \"" + script.string() + "\"");
    }

    if (sub == "check") {
        // Run the docs consistency check script
        auto script = root / "tools" / "check-docs.sh";
        if (!fs::exists(script)) {
            std::cerr << "Error: check script not found at " << script.string() << "\n";
            return 1;
        }
        return run("bash \"" + script.string() + "\"");
    }

    if (sub == "index") {
        return docs_index(docs_dir);
    }

    if (sub == "search") {
        std::string query;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!query.empty()) query += " ";
            query += args[i];
        }
        return docs_search(docs_dir, query);
    }

    if (sub == "open") {
        if (args.size() < 2) {
            std::cerr << "Usage: pulp docs open <slug>\n";
            return 1;
        }
        return docs_open(docs_dir, args[1]);
    }

    if (sub == "show") {
        if (args.size() < 2) {
            std::cerr << "Usage: pulp docs show <support|command|cmake|style> [name]\n";
            return 1;
        }
        std::string show_sub = args[1];

        if (show_sub == "support") {
            if (args.size() < 3) {
                std::cerr << "Usage: pulp docs show support <thing>\n";
                return 1;
            }
            return docs_show_support(docs_dir, args[2]);
        }
        if (show_sub == "command") {
            if (args.size() < 3) {
                std::cerr << "Usage: pulp docs show command <name>\n";
                return 1;
            }
            return docs_show_command(docs_dir, args[2]);
        }
        if (show_sub == "cmake") {
            if (args.size() < 3) {
                std::cerr << "Usage: pulp docs show cmake <name>\n";
                return 1;
            }
            return docs_show_cmake(docs_dir, args[2]);
        }
        if (show_sub == "style") {
            return docs_show_style(docs_dir);
        }

        std::cerr << "Unknown show topic: " << show_sub << "\n";
        std::cerr << "Available: support, command, cmake, style\n";
        return 1;
    }

    std::cerr << "Unknown docs subcommand: " << sub << "\n";
    std::cerr << "Run `pulp docs` for usage.\n";
    return 1;
}

static void print_usage() {
    std::cout << "pulp — Pulp audio plugin framework CLI\n\n";
    std::cout << "Usage: pulp <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  create   Scaffold a new plugin project from templates\n";
    std::cout << "  inspect  Launch the component inspector\n";
    std::cout << "  audit    License and clean-room audit\n";
    std::cout << "  build    Build the project (configure + compile)\n";
    std::cout << "  test     Run the test suite\n";
    std::cout << "  status   Show project status and info\n";
    std::cout << "  validate Run plugin format validators (clap-validator, auval)\n";
    std::cout << "  ship     Sign, package, and check plugins\n";
    std::cout << "  docs     Browse local documentation\n";
    std::cout << "  clean    Remove build directory\n";
    std::cout << "  help     Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp build              # Build all targets\n";
    std::cout << "  pulp build --target X   # Build specific target\n";
    std::cout << "  pulp test               # Run all tests\n";
    std::cout << "  pulp test -R Knob       # Run tests matching 'Knob'\n";
    std::cout << "  pulp ship sign --identity \"Developer ID Application: Foo\"\n";
    std::cout << "  pulp ship package --version 1.0.0\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp docs open cli      # Read a doc by slug\n";
    std::cout << "  pulp status             # Show project info\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i)
        args.push_back(argv[i]);

    if (command == "build")    return cmd_build(args);
    if (command == "test")     return cmd_test(args);
    if (command == "status")   return cmd_status(args);
    if (command == "validate") return cmd_validate(args);
    if (command == "ship")     return cmd_ship(args);
    if (command == "docs")     return cmd_docs(args);
    if (command == "clean")    return cmd_clean(args);
    if (command == "add") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "add-component.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: add-component script not found\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "audit") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "audit.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: audit script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "inspect") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto screenshot_bin = root / "build" / "tools" / "screenshot" / "pulp-screenshot";
        if (!fs::exists(screenshot_bin)) {
            std::cerr << "Error: pulp-screenshot not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = screenshot_bin.string() + " --demo";
        for (auto& arg : args) cmd += " " + arg;
        return run(cmd);
    }
    if (command == "create") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "create-project.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: create script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    std::cerr << "Run `pulp help` for usage\n";
    return 1;
}
