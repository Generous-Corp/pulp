// cmd_validate.cpp — pulp validate command

#include "cli_common.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include <pulp/runtime/system.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/format/editor_ui.hpp>

int cmd_validate(const std::vector<std::string>& args) {
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

    // Parse flags
    bool run_all = false;
    bool json_output = false;
    bool screenshot = false;
    std::string report_path;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--all") run_all = true;
        else if (args[i] == "--json") json_output = true;
        else if (args[i] == "--screenshot") screenshot = true;
        else if (args[i] == "--report" && i + 1 < args.size())
            report_path = args[++i];
    }

    int total = 0, passed = 0, failed = 0, skipped = 0;

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
                    int rc = run("clap-validator validate \"" + clap_path + "\" 2>/dev/null");
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
                    // Fallback: dlopen test
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
                    int rc = run("pluginval --strictness-level 5 --timeout-ms 30000 --validate \""
                                 + vst3_path + "\" 2>/dev/null");
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
                    int rc = run("vstvalidator \"" + vst3_path + "\" 2>/dev/null");
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
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R auval-" + name + " --output-on-failure 2>/dev/null";
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

    return failed > 0 ? 1 : 0;
}
