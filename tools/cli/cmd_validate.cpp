// cmd_validate.cpp — pulp validate command

#include "cli_common.hpp"
#include "validator_discovery.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include <pulp/runtime/system.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/format/editor_ui.hpp>

int cmd_validate(const std::vector<std::string>& args) {
    // Parse flags first so invalid flags fail loud before we do any
    // project-root / build-dir work. Codex P2 on PR #381 flagged that
    // the shellout test couldn't actually distinguish `--strict` from
    // any other unknown flag because flag parsing ran after the
    // project-root check bail-out.
    bool run_all = false;
    bool json_output = false;
    bool screenshot = false;
    bool strict = false;   // #51: skipped-because-missing-tool ⇒ fail
    std::string report_path;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--all") run_all = true;
        else if (args[i] == "--json") json_output = true;
        else if (args[i] == "--screenshot") screenshot = true;
        else if (args[i] == "--strict") strict = true;
        else if (args[i] == "--report") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                std::cerr << "pulp validate: --report requires a path\n";
                return 2;
            }
            report_path = args[++i];
        }
        else if (args[i].size() >= 2 && args[i].substr(0, 2) == "--") {
            std::cerr << "pulp validate: unknown flag: " << args[i] << "\n";
            std::cerr << "Known flags: --all --json --screenshot --strict "
                         "--report <path>\n";
            return 2;
        }
    }

    auto root = resolve_active_project_root(nullptr);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir)) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    // Validator-discovery preflight (#743). Catches the ripped-out-of-
    // bundle failure mode (`/usr/local/bin/pluginval` with a detached
    // signature → amfid SIGKILLs it mid-run, exit 137, zero stderr) by
    // running `spctl --assess` on each validator before we shell out.
    // Missing validators are left to the existing `--strict` advisory
    // below; only *broken* copies abort here, because broken copies
    // silently kill the run in a way the user can't diagnose without
    // already knowing about this class of failure.
    {
        namespace vd = pulp::cli::validator_discovery;
        auto env = vd::make_default_env();
        auto reports = vd::discover_validators(env);
        if (vd::has_broken_validator(reports)) {
            std::cerr << "\n" << color::red()
                      << "pulp validate: aborting — one or more validators "
                         "have a broken code signature." << color::reset()
                      << "\n"
                      << "This is almost always a copy of the validator "
                         "ripped out of its .app\nbundle; running it would "
                         "be killed by amfid (exit 137) with no useful\n"
                         "diagnostic. Details:\n\n";
            std::cerr << vd::render_report(reports, g_color_enabled);
            std::cerr << "\nRun " << color::cyan()
                      << "`pulp doctor --validators --fix`" << color::reset()
                      << " to clean up user-owned copies, or follow the\n"
                         "`fix:` line above for root-owned copies.\n";
            return 1;
        }
    }

    int total = 0, passed = 0, failed = 0, skipped = 0;
    int skipped_missing_tool = 0;  // #51: subset of `skipped` we care about

    // Missing-tool tracking (#51 / #356). A "skipped because the
    // validator isn't installed" outcome is otherwise invisible — under
    // --strict we upgrade it to a hard failure, and under default mode
    // we print a loud warning at the end listing which tools are gone
    // and how to install them.
    struct MissingTool {
        std::string tool;
        std::string format;
        std::string install_hint;
    };
    std::vector<MissingTool> missing_tools;
    auto note_missing = [&](const std::string& tool,
                            const std::string& format,
                            const std::string& hint) {
        for (const auto& m : missing_tools) {
            if (m.tool == tool) return;  // only report each tool once
        }
        missing_tools.push_back({tool, format, hint});
    };
    auto disable_plugin_editor = [](std::string cmd) {
#ifdef _WIN32
        return std::string("set \"PULP_DISABLE_PLUGIN_EDITOR=1\" && "
                           "set \"PULP_HEADLESS=1\" && "
                           "set \"PULP_TEST_MODE=1\" && ") + cmd;
#else
        return std::string("PULP_DISABLE_PLUGIN_EDITOR=1 "
                           "PULP_HEADLESS=1 "
                           "PULP_TEST_MODE=1 ") + cmd;
#endif
    };

    // JSON report accumulator
    std::vector<std::string> report_entries;

    auto record = [&](const std::string& tool, const std::string& plugin_path,
                      const std::string& format, const std::string& status,
                      int exit_code, const std::string& error_msg) {
        if (json_output || !report_path.empty()) {
            std::ostringstream e;
            e << "    {\"type\": \"validator\", \"status\": \"" << status << "\", "
              << "\"target\": \"" << fs::path(plugin_path).filename().string() << "\", "
              << "\"validator\": {"
              << "\"tool\": \"" << tool << "\", "
              << "\"plugin_path\": \"" << plugin_path << "\", "
              << "\"plugin_format\": \"" << format << "\", "
              << "\"exit_code\": " << exit_code;
            if (!error_msg.empty()) e << ", \"stderr\": \"" << error_msg << "\"";
            e << "}}";
            report_entries.push_back(e.str());
        }
    };

    // ── CLAP validation ──���──────────────────────────────────────────────

    auto clap_dir = build_dir / "CLAP";
    if (fs::exists(clap_dir)) {
        bool has_clap_validator = !find_executable_in_path("clap-validator").empty();

        for (auto& entry : fs::directory_iterator(clap_dir)) {
            if (entry.path().extension() == ".clap") {
                auto name = entry.path().stem().string();

                if (!bundle_contains_payload(entry.path())) {
                    std::cout << "CLAP: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                    ++skipped;
                    record("clap-validator", entry.path().string(), "clap", "skip", -1,
                           "bundle directory exists but plugin binary is missing");
                    continue;
                }

                ++total;

                if (has_clap_validator) {
                    std::cout << "CLAP: validating " << name << "... ";
                    auto clap_path = entry.path().string();
                    int rc = run(disable_plugin_editor(
                        "clap-validator validate \"" + clap_path + "\" 2>/dev/null"));
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("clap-validator", clap_path, "clap", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("clap-validator", clap_path, "clap", "fail", rc, "");
                    }
                } else {
                    // Fallback: dlopen test. Still count the missing
                    // validator against the strict gate so CI knows.
                    note_missing("clap-validator", "clap",
                                 "cargo install clap-validator");
                    ++skipped_missing_tool;
                    std::cout << "CLAP: " << name << " (dlopen check only, clap-validator not installed)... ";
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R clap-dlopen-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("clap-validator", entry.path().string(), "clap", "pass", 0, "dlopen only");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("clap-validator", entry.path().string(), "clap", "fail", rc, "dlopen only");
                    }
                }
            }
        }
    }

    // ── VST3 validation (pluginval) ─────────────────────────────────────

    auto vst3_dir = build_dir / "VST3";
    if (fs::exists(vst3_dir)) {
        bool has_pluginval = !find_executable_in_path("pluginval").empty();

        for (auto& entry : fs::directory_iterator(vst3_dir)) {
            if (entry.path().extension() == ".vst3") {
                auto name = entry.path().stem().string();
                ++total;

                if (has_pluginval) {
                    std::cout << "VST3: validating " << name << " (pluginval)... ";
                    auto vst3_path = entry.path().string();
                    int rc = run(disable_plugin_editor(
                        "pluginval --strictness-level 5 --timeout-ms 30000 --validate \""
                        + vst3_path + "\" 2>/dev/null"));
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("pluginval", vst3_path, "vst3", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("pluginval", vst3_path, "vst3", "fail", rc, "");
                    }
                } else {
                    std::cout << "VST3: " << name << " SKIPPED (pluginval not installed)\n";
                    ++skipped;
                    ++skipped_missing_tool;
                    note_missing("pluginval", "vst3",
                                 "brew install pluginval (macOS) "
                                 "| https://github.com/Tracktion/pluginval/releases");
                    record("pluginval", entry.path().string(), "vst3", "skip", -1,
                           "pluginval not found in PATH");
                }
            }
        }
    }

    // ── vstvalidator (evaluation: run if --all and tool is available) ────

    if (run_all && fs::exists(vst3_dir)) {
        bool has_vstvalidator = !find_executable_in_path("vstvalidator").empty();

        if (has_vstvalidator) {
            for (auto& entry : fs::directory_iterator(vst3_dir)) {
                if (entry.path().extension() == ".vst3") {
                    auto name = entry.path().stem().string();

                    if (!bundle_contains_payload(entry.path())) {
                        std::cout << "VST3: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                        ++skipped;
                        record("pluginval", entry.path().string(), "vst3", "skip", -1,
                               "bundle directory exists but plugin binary is missing");
                        continue;
                    }

                    ++total;
                    std::cout << "VST3: validating " << name << " (vstvalidator)... ";
                    auto vst3_path = entry.path().string();
                    int rc = run(disable_plugin_editor(
                        "vstvalidator \"" + vst3_path + "\" 2>/dev/null"));
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("vstvalidator", vst3_path, "vst3", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("vstvalidator", vst3_path, "vst3", "fail", rc, "");
                    }
                }
            }
        } else {
            std::cout << "VST3: vstvalidator not found — skipping vstvalidator checks.\n";
            std::cout << "      vstvalidator is the Steinberg VST3 SDK validator.\n";
            std::cout << "      It is not widely distributed; build it from the VST3 SDK if needed.\n";
            std::cout << "      Go/no-go: optional — pluginval covers most VST3 validation needs.\n";
        }
    }

    // ── AU validation (macOS only) ──────────────────────────────────────

#ifdef __APPLE__
    auto au_dir = build_dir / "AU";
    if (fs::exists(au_dir)) {
        for (auto& entry : fs::directory_iterator(au_dir)) {
            if (entry.path().extension() == ".component") {
                auto name = entry.path().stem().string();

                if (!bundle_contains_payload(entry.path())) {
                    std::cout << "AU: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                    ++skipped;
                    record("auval", entry.path().string(), "au", "skip", -1,
                           "bundle directory exists but plugin binary is missing");
                    continue;
                }

                ++total;
                std::cout << "AU: " << name << " (auval check)... ";

                if (!find_executable_in_path("auval").empty()) {
                    auto test_cmd = disable_plugin_editor(
                        "ctest --test-dir " + build_dir.string() + " -R auval-" + name
                        + " --output-on-failure 2>/dev/null");
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("auval", entry.path().string(), "au", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("auval", entry.path().string(), "au", "fail", rc, "");
                    }
                } else {
                    std::cout << "SKIPPED (auval not found)\n";
                    ++skipped;
                    ++skipped_missing_tool;
                    note_missing("auval", "au",
                                 "ships with Xcode Command Line Tools "
                                 "(`xcode-select --install`)");
                    record("auval", entry.path().string(), "au", "skip", -1,
                           "auval not found");
                }
            }
        }
    }
#endif

#if defined(__APPLE__) || defined(_WIN32)
    // ── AAX validation (optional validator) ─────────────────────────────

    auto aax_dir = build_dir / "AAX";
    if (fs::exists(aax_dir)) {
        auto validator_root = find_aax_validator_root();
        bool has_aax_validator = !validator_root.empty();
        bool printed_guidance = false;

        for (auto& entry : fs::directory_iterator(aax_dir)) {
            if (entry.path().extension() != ".aaxplugin") continue;

            auto name = entry.path().stem().string();

            if (!bundle_contains_payload(entry.path())) {
                std::cout << "AAX: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                ++skipped;
                record("aax-validator", entry.path().string(), "aax", "skip", -1,
                       "bundle directory exists but plugin binary is missing");
                continue;
            }

            ++total;

            if (!has_aax_validator) {
                std::cout << "AAX: " << name << " SKIPPED (AAX validator not installed)\n";
                ++skipped;
                ++skipped_missing_tool;
                note_missing("aax-validator", "aax",
                             "ships with the Avid AAX SDK — see "
                             "`.agents/skills/aax/SKILL.md`");
                record("aax-validator", entry.path().string(), "aax", "skip", -1,
                       "AAX validator not found");
                if (!printed_guidance) {
                    print_aax_setup_guidance(false, true);
                    printed_guidance = true;
                }
                continue;
            }

            std::cout << "AAX: validating " << name
                      << (run_all ? " (aaxval full)... " : " (aaxval describe)... ");
            auto output = run_aax_validator_command(validator_root, entry.path(), run_all);
            auto summary = truncate_message(output, 400);

            if (aax_validator_passed(output)) {
                std::cout << "PASSED\n";
                ++passed;
                record("aax-validator", entry.path().string(), "aax", "pass", 0, "");
            } else {
                std::cout << "FAILED\n";
                ++failed;
                record("aax-validator", entry.path().string(), "aax", "fail", 1, summary);
            }
        }
    }
#endif

    // ── Summary ─────────────────────────────────────────────────────────

    std::cout << "\nValidation Summary: " << total << " total, "
              << passed << " passed, " << failed << " failed, "
              << skipped << " skipped\n";

    // ── Missing-validator advisory (#51 / #356) ────────────────────────
    //
    // Without this, a green `pulp validate` on a machine that lacks
    // pluginval/clap-validator/auval looks identical to one that ran
    // every validator and all passed. Print a loud warning listing
    // which tools are absent. Under --strict, turn the warning into
    // a hard failure so CI can actually gate on it.
    if (!missing_tools.empty()) {
        std::cerr << "\n";
        std::cerr << (strict ? "ERROR" : "WARNING")
                  << ": " << missing_tools.size()
                  << " validator(s) not installed — coverage is incomplete.\n";
        for (const auto& m : missing_tools) {
            std::cerr << "  - " << m.tool << " (" << m.format << "): "
                      << m.install_hint << "\n";
        }
        std::cerr << "\n";
        std::cerr << "Skipped-because-missing-tool count: "
                  << skipped_missing_tool << "\n";
        if (!strict) {
            std::cerr << "Re-run with --strict to treat missing validators "
                         "as failures (e.g. in CI).\n";
        }
    }

    const bool strict_fail = strict && skipped_missing_tool > 0;

    // ── JSON report output ───────��────────────────────────────��─────────

    if (json_output || !report_path.empty()) {
        auto git_ref = exec_output("git -C \"" + root.string() + "\" rev-parse --short HEAD 2>/dev/null");

        auto now = std::time(nullptr);
        char ts[64];
        auto utc = pulp::runtime::gmtime_utc(now);
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

        std::ostringstream report;
        report << "{\n";
        report << "  \"version\": 1,\n";
        report << "  \"timestamp\": \"" << ts << "\",\n";
        if (!git_ref.empty())
            report << "  \"git_ref\": \"" << git_ref << "\",\n";
        report << "  \"reports\": [\n";
        for (size_t i = 0; i < report_entries.size(); ++i) {
            report << report_entries[i];
            if (i + 1 < report_entries.size()) report << ",";
            report << "\n";
        }
        report << "  ]\n";
        report << "}\n";

        if (json_output) {
            std::cout << "\n" << report.str();
        }
        if (!report_path.empty()) {
            std::ofstream f(report_path);
            if (f.good()) {
                f << report.str();
                std::cout << "Report written to " << report_path << "\n";
            } else {
                std::cerr << "Failed to write report to " << report_path << "\n";
            }
        }
    }

    // ── Screenshot capture (--screenshot) ───────────────────────────────────
    if (screenshot) {
        auto screenshots_dir = root / "artifacts" / "screenshots";
        fs::create_directories(screenshots_dir);
        int captured = 0;

        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext != ".vst3" && ext != ".clap" && ext != ".component") continue;

                auto name = entry.path().stem().string();
                auto png_name = name + "-" + dir_name + ".png";
                auto png_path = screenshots_dir / png_name;

                pulp::state::StateStore store;
                auto editor_ui = pulp::format::build_editor_ui(store, false);

                if (!editor_ui.root) {
                    std::cout << "  Screenshot: " << name << " (" << dir_name << ") — no editor, skipping\n";
                    continue;
                }

                uint32_t w = 400, h = 300;
                bool ok = pulp::view::render_to_file(*editor_ui.root, w, h, png_path.string());
                if (ok) {
                    std::cout << "  Screenshot: " << png_path.filename().string() << "\n";
                    ++captured;
                } else {
                    std::cerr << "  Screenshot FAILED: " << name << " (" << dir_name << ")\n";
                }
            }
        }

        if (captured > 0)
            std::cout << captured << " screenshot(s) saved to " << screenshots_dir.string() << "\n";
        else
            std::cout << "No plugin screenshots captured.\n";
    }

    return (failed > 0 || strict_fail) ? 1 : 0;
}
