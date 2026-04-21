// cmd_docs.cpp — pulp docs command

#include "cli_common.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

// ── Docs helpers ────────────────────────────────────────────────────────────

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
                if (matches_in_file < 5) {
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
        // Fuzzy fallback: search file names and headings
        struct FuzzyHit { int score; std::string display; fs::path path; };
        std::vector<FuzzyHit> hits;

        for (auto& entry : fs::recursive_directory_iterator(docs_dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".md") continue;

            auto rel = fs::relative(entry.path(), docs_dir).string();
            int name_score = fuzzy_score(rel, query);
            if (name_score > 0) {
                hits.push_back({name_score, "docs/" + rel, entry.path()});
            }

            // Also check first heading
            std::ifstream f(entry.path());
            std::string line;
            while (std::getline(f, line)) {
                if (line.size() > 2 && line[0] == '#') {
                    int heading_score = fuzzy_score(line, query);
                    if (heading_score > name_score) {
                        hits.push_back({heading_score, "docs/" + rel + " — " + trim(line), entry.path()});
                    }
                    break;
                }
            }
        }

        if (!hits.empty()) {
            std::sort(hits.begin(), hits.end(), [](const FuzzyHit& a, const FuzzyHit& b) {
                return a.score > b.score;
            });

            std::cout << "No exact matches for \"" << query << "\". Did you mean:\n";
            int shown = 0;
            for (auto& hit : hits) {
                if (shown >= 5) break;
                std::cout << "  " << hit.display << "\n";
                ++shown;
            }
        } else {
            std::cout << "No matches for \"" << query << "\" in docs/\n";
        }
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

    std::istringstream stream(index_content);
    std::string line;
    std::string current_slug, current_path;
    bool found = false;

    while (std::getline(stream, line)) {
        auto s = yaml_value(line, "slug");
        if (!s.empty()) {
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

    std::istringstream stream(content);
    std::string line;
    bool found = false;

    std::string section_name;
    std::string entry_name;

    std::string query_lower = thing;
    for (auto& c : query_lower) c = static_cast<char>(std::tolower(c));

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) continue;

        if (indent == 0 && !trimmed.empty() && trimmed.back() == ':') {
            section_name = trimmed.substr(0, trimmed.size() - 1);
            continue;
        }

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

                while (std::getline(stream, line)) {
                    auto ni = line.find_first_not_of(' ');
                    if (ni == std::string::npos || ni <= 2) break;
                    std::string field = trim(line);
                    if (field.empty() || field[0] == '#') continue;
                    auto colon = field.find(':');
                    if (colon != std::string::npos) {
                        auto key = trim(field.substr(0, colon));
                        auto val = trim(field.substr(colon + 1));
                        if (!key.empty()) key[0] = static_cast<char>(std::toupper(key[0]));
                        std::cout << "  " << key << ": " << val << "\n";
                    }
                }
                std::cout << "\n";
            }
            continue;
        }

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

        if (!n.empty() && indent <= 4) {
            if (in_entry) break;
            if (n == name) {
                found = true;
                in_entry = true;
            }
            continue;
        }

        if (!in_entry) continue;

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

        if (in_subcommands) {
            if (trimmed.find("args:") == 0) {
                in_sub_args = true;
                continue;
            }
            if (in_sub_args) {
                if (!n.empty() && indent <= 6) {
                    in_sub_args = false;
                } else {
                    continue;
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

// ── Entry point ─────────────────────────────────────────────────────────────

int cmd_docs(const std::vector<std::string>& args) {
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
        // #577 PR 4 retired tools/build-docs.py in favour of MkDocs Material.
        // Delegate to `mkdocs build`. Extra args are forwarded so callers can
        // pass `--site-dir`, `--strict`, etc.
        std::string cmd = "mkdocs build";
        for (size_t i = 1; i < args.size(); ++i) {
            cmd += " " + args[i];
        }
        int rc = run(cmd);
        if (rc != 0) {
            std::cerr << "Hint: install docs deps with "
                         "`pip install -r requirements-docs.txt`\n";
        }
        return rc;
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
