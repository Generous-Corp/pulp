// cmd_project.cpp — dispatcher for the `pulp project` command family.
//
// Roadmap item P11-1: this file was historically a single ~1,047-line
// translation unit holding the pin/bump, undo, and unpin handlers plus
// all the shared pin-rewrite helpers. It is now split into focused
// sibling TUs (see cmd_project_internal.hpp for the map):
//
//   cmd_project.cpp          — this file: `cmd_project` dispatch + `do_unpin`
//   cmd_project_common.cpp   — shared pin-file / project-root helpers
//   cmd_project_bump.cpp     — `do_bump` (pin / bump)
//   cmd_project_undo.cpp     — `do_undo`
//
// The user-facing command surface is unchanged:
//
//   pulp project pin / bump   Pin the project's SDK version
//   pulp project unpin        Switch back to floating-SDK mode
//   pulp project undo         Revert a previous bump batch

#include "cmd_project_internal.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace pulp_cli::project_detail;

// Pulp #2087: `pulp project unpin` removes the SDK pin from pulp.toml
// so the project goes back to floating mode and tracks the latest
// installed SDK on every rebuild. Sibling of `pin` (= `bump`).
//
// Implementation: rewrite the project's pulp.toml so its sdk_version
// becomes the floating marker `"latest"`. We DON'T delete the field
// entirely because (a) downstream tooling that greps for it loses a
// clear signal, and (b) keeping the line preserves user comments and
// surrounding TOML structure intact.
int do_unpin(const std::vector<std::string>& args) {
    bool dry_run = false;
    for (const auto& a : args) {
        if (a == "--help" || a == "-h" || a == "help") {
            std::cout <<
                "pulp project unpin — remove the SDK pin so the project tracks latest\n\n"
                "Usage:\n"
                "  pulp project unpin             Switch CWD project to floating SDK mode\n"
                "  pulp project unpin --dry-run   Show plan without rewriting\n"
                "\n"
                "After unpin, the project's resolved SDK is the newest installed\n"
                "version under ~/.pulp/sdk/<x.y.z>/, picked up on every rebuild.\n"
                "Re-pin with `pulp project pin <version>` (or `pulp project bump`).\n";
            return 0;
        }
        if (a == "--dry-run") { dry_run = true; continue; }
        std::cerr << "pulp project unpin: unknown argument '" << a << "'\n";
        return 2;
    }

    bool found_pulp_source = false;
    auto target = find_bumpable_project_root_from(fs::current_path(), &found_pulp_source);
    if (found_pulp_source) {
        std::cerr << "pulp project unpin: refusing to run inside the Pulp source checkout.\n";
        return 1;
    }
    if (target.empty()) {
        std::cerr << "pulp project unpin: not inside a Pulp project (expected pulp.toml)\n";
        return 1;
    }

    auto toml = target / "pulp.toml";
    if (!fs::exists(toml)) {
        std::cerr << "pulp project unpin: no pulp.toml at " << toml.string() << "\n";
        return 1;
    }

    auto body = read_text(toml);
    if (body.empty()) {
        std::cerr << "pulp project unpin: could not read " << toml.string() << "\n";
        return 1;
    }

    // Find the sdk_version line and capture the current value (so we
    // can print "was X.Y.Z, now floating"). Keep it lightweight — same
    // single-line semantics read_pulp_toml_value uses.
    std::string current;
    {
        auto pos = body.find("sdk_version");
        if (pos != std::string::npos) {
            auto q1 = body.find('"', pos);
            auto q2 = (q1 == std::string::npos) ? std::string::npos
                                                : body.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                current = body.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }

    if (current.empty() || current == "latest") {
        std::cout << "pulp project unpin: " << target.filename().string()
                  << " is already floating (sdk_version = \"latest\").\n";
        return 0;
    }

    if (dry_run) {
        std::cout << "[dry-run] would set sdk_version = \"latest\" in "
                  << toml.string() << " (was \"" << current << "\")\n";
        return 0;
    }

    // Replace "<old>" with "latest" on the sdk_version line.
    auto pos = body.find("sdk_version");
    auto q1 = body.find('"', pos);
    auto q2 = body.find('"', q1 + 1);
    std::string rewritten = body.substr(0, q1 + 1) + "latest"
                          + body.substr(q2);

    if (!write_text_atomic(toml, rewritten)) {
        std::cerr << "pulp project unpin: write failed at " << toml.string() << "\n";
        return 1;
    }

    std::cout << color::green() << "unpinned" << color::reset()
              << " " << target.filename().string()
              << "  was " << current << " -> now floating (tracks latest)\n";
    std::cout << "\nThe project will resolve its SDK at command time to the newest\n"
                 "installed version under ~/.pulp/sdk/. Re-pin any time with\n"
                 "`pulp project pin <version>`.\n";
    return 0;
}

}  // namespace

int cmd_project(const std::vector<std::string>& args) {
    using namespace pulp_cli::project_detail;

    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_project_help();
        return args.empty() ? 1 : 0;
    }

    const auto& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    // Pulp #2087: `pin` is the primary, intuitive name for what this
    // command does. `bump` predates the rename and stays as a
    // deprecated alias so existing scripts and skill docs keep working
    // through one minor release. New docs and skill examples should
    // use `pin`.
    if (sub == "pin" || sub == "bump") return do_bump(rest);
    if (sub == "unpin") return do_unpin(rest);
    if (sub == "undo") return do_undo(rest);

    std::cerr << "pulp project: unknown subcommand '" << sub << "'\n";
    print_project_help();
    return 2;
}
