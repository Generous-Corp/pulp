// cmd_config.cpp — `pulp config get/set` thin wrappers
//
// Release-discovery Slice 2 (#547 / parent #499). Surfaces the
// `[update]` section of ~/.pulp/config.toml so users can toggle modes
// without hand-editing TOML:
//
//   pulp config get update.mode
//   pulp config set update.mode manual
//   pulp config set update.check_interval_hours 24
//   pulp config list
//
// Full auto/prompt/manual/off enforcement lands in Slice 5 (#499).
// This slice only owns the config surface + banner emission; pushing
// the mode value anywhere other than the banner check happens later.

#include "cli_common.hpp"
#include "update_check.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace uc = pulp::cli::update_check;

namespace {

fs::path config_path_or_empty() {
    auto home = pulp_home();
    if (home.empty()) return {};
    return home / "config.toml";
}

// Split "section.key" into (section, key). Returns false if not
// dotted — the CLI currently supports only dotted lookups.
bool split_dotted_key(const std::string& in,
                      std::string& section,
                      std::string& key) {
    auto dot = in.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= in.size()) return false;
    section = in.substr(0, dot);
    key = in.substr(dot + 1);
    return true;
}

// Allowed sections/keys. Kept narrow so `pulp config set foo.bar baz`
// doesn't silently introduce a typo'd key. Add entries here when new
// slices need new knobs.
bool is_allowed_key(const std::string& section, const std::string& key) {
    if (section == "update") {
        return key == "mode" ||
               key == "check_interval_hours" ||
               key == "channel";
    }
    return false;
}

// Validates a value for (section, key). Returns empty on success,
// human-readable error on failure.
std::string validate_value(const std::string& section,
                           const std::string& key,
                           const std::string& value) {
    if (section == "update" && key == "mode") {
        if (value == "auto" || value == "prompt" ||
            value == "manual" || value == "off") return {};
        return "update.mode must be one of: auto, prompt, manual, off";
    }
    if (section == "update" && key == "check_interval_hours") {
        for (char c : value) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return "update.check_interval_hours must be a non-negative integer";
        }
        return {};
    }
    if (section == "update" && key == "channel") {
        if (value == "stable" || value == "beta") return {};
        return "update.channel must be one of: stable, beta";
    }
    return {};
}

int usage() {
    std::cout << "pulp config — read/write ~/.pulp/config.toml\n\n";
    std::cout << "Usage: pulp config <command>\n\n";
    std::cout << "Commands:\n";
    std::cout << "  get <section.key>           Print the value (empty if unset)\n";
    std::cout << "  set <section.key> <value>   Write the value atomically\n";
    std::cout << "  list                        Dump current update.* settings\n";
    std::cout << "\nSupported keys (update section):\n";
    std::cout << "  update.mode                   auto | prompt | manual | off  (default: prompt)\n";
    std::cout << "  update.check_interval_hours   default: 24\n";
    std::cout << "  update.channel                stable | beta                 (default: stable)\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp config set update.mode manual\n";
    std::cout << "  pulp config get update.mode\n";
    return 0;
}

}  // namespace

int cmd_config(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        return usage();
    }

    const std::string& sub = args[0];

    auto path = config_path_or_empty();
    if (path.empty()) {
        std::cerr << "Error: could not determine pulp home (HOME / USERPROFILE unset).\n";
        return 1;
    }

    if (sub == "get") {
        if (args.size() < 2) {
            std::cerr << "Error: `pulp config get` requires <section.key>\n";
            return 1;
        }
        std::string section, key;
        if (!split_dotted_key(args[1], section, key)) {
            std::cerr << "Error: key must be dotted, e.g. update.mode\n";
            return 1;
        }
        std::string contents;
        if (fs::exists(path)) {
            std::ifstream f(path);
            std::ostringstream buf;
            buf << f.rdbuf();
            contents = buf.str();
        }
        auto value = uc::read_toml_key_in_section(contents, section, key);
        std::cout << value << "\n";
        return 0;
    }

    if (sub == "set") {
        if (args.size() < 3) {
            std::cerr << "Error: `pulp config set` requires <section.key> <value>\n";
            return 1;
        }
        std::string section, key;
        if (!split_dotted_key(args[1], section, key)) {
            std::cerr << "Error: key must be dotted, e.g. update.mode\n";
            return 1;
        }
        if (!is_allowed_key(section, key)) {
            std::cerr << "Error: unknown config key: " << section << "." << key << "\n";
            std::cerr << "       Run `pulp config --help` for supported keys.\n";
            return 1;
        }
        auto err = validate_value(section, key, args[2]);
        if (!err.empty()) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }

        std::string contents;
        if (fs::exists(path)) {
            std::ifstream f(path);
            std::ostringstream buf;
            buf << f.rdbuf();
            contents = buf.str();
        }
        auto rewritten = uc::write_toml_key_in_section(contents, section, key, args[2]);

        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            std::cerr << "Error: could not write " << path.string() << "\n";
            return 1;
        }
        f << rewritten;
        std::cout << "Set " << section << "." << key << " = " << args[2] << "\n";
        std::cout << "    " << path.string() << "\n";
        return 0;
    }

    if (sub == "list") {
        std::string contents;
        if (fs::exists(path)) {
            std::ifstream f(path);
            std::ostringstream buf;
            buf << f.rdbuf();
            contents = buf.str();
        }
        auto show = [&](const char* key, const char* default_value) {
            auto v = uc::read_toml_key_in_section(contents, "update", key);
            if (v.empty()) v = default_value;
            std::cout << "  update." << key << " = " << v << "\n";
        };
        std::cout << "Pulp config (" << path.string() << "):\n";
        show("mode", "prompt");
        show("check_interval_hours", "24");
        show("channel", "stable");
        return 0;
    }

    std::cerr << "Unknown config subcommand: " << sub << "\n";
    return usage();
}
