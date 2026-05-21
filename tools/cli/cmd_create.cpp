// cmd_create.cpp — pulp create command

#include "cli_common.hpp"
#include "create_targets.hpp"
#include "projects_registry.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

// ── Template helpers (local to this file) ───────────────────────────────────

static std::string to_class_name(const std::string& name) {
    std::string result;
    bool capitalize = true;
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_') { capitalize = true; continue; }
        if (capitalize) { result += static_cast<char>(std::toupper(c)); capitalize = false; }
        else result += c;
    }
    return result;
}

static std::string to_lower_name(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == ' ' || c == '_') result += '-';
        else if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
    }
    return result;
}

static std::string to_namespace_name(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == ' ' || c == '-') result += '_';
        else if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
    }
    return result;
}

static std::string make_plugin_code(const std::string& class_name) {
    std::string clean;
    for (char c : class_name) if (std::isalpha(c)) clean += c;
    if (clean.size() >= 4) return clean.substr(0, 4);
    return (clean + "xxxx").substr(0, 4);
}

static std::string make_aax_product_code(const std::string& class_name) {
    auto clean = make_plugin_code(class_name);
    if (clean.size() < 4) clean = (clean + "xxxx").substr(0, 4);
    clean[3] = 'P';
    return clean;
}

static std::string make_mfr_code(const std::string& mfr) {
    std::string clean;
    for (char c : mfr) if (std::isalpha(c)) clean += c;
    if (clean.size() >= 4) return clean.substr(0, 4);
    return (clean + "xxxx").substr(0, 4);
}

static std::string make_vst3_uid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;

    std::array<uint32_t, 4> words = {
        dist(gen), dist(gen), dist(gen), dist(gen)
    };

    std::ostringstream out;
    out << "Steinberg::FUID(";
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out << ", ";
        out << "0x"
            << std::uppercase
            << std::hex
            << std::setw(8)
            << std::setfill('0')
            << words[i];
    }
    out << ")";
    return out.str();
}

static std::string expand_template_str(const std::string& tmpl,
                                        const std::vector<std::pair<std::string,std::string>>& vars) {
    std::string result = tmpl;
    for (auto& [key, val] : vars) {
        result = replace_all_str(result, "{{" + key + "}}", val);
    }
    return result;
}

// ── cmd_create ──────────────────────────────────────────────────────────────

int cmd_create(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        // Try to find root from current binary's build dir
        auto build_dir = build_dir_from_current_binary();
        if (!build_dir.empty()) {
            root = cmake_home_directory(build_dir);
            if (!root.empty() && (!fs::exists(root / "CMakeLists.txt") || !fs::exists(root / "core")))
                root.clear();
        }
    }

    // Parse args
    std::string name, type = "effect", manufacturer = "Pulp", output_path, tmpl, targets_arg;
    bool no_build = false;
    bool ci_mode = false;
    bool in_tree_mode = false;
    bool mpe_mode = false;
    // Pulp #2087: default new projects to floating-SDK mode (track latest)
    // instead of exact-pin. `--pin` opts into exact-version pinning at
    // create-time for users who want reproducibility from day one.
    bool pin_at_create = false;
    // 2026-05 build-default flip: scaffolded projects default to Release
    // builds. Debug is opt-in via `--debug` for developers who need
    // assertions / faster iteration. Was Debug-by-default; that
    // silently shipped 5–10× slower binaries for users running the
    // resulting app and didn't match the "build the app" mental model.
    bool debug_build = false;
    for (size_t i = 0; i < args.size(); ++i) {
        auto take_value = [&](std::string& out, const char* flag) -> bool {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp create: " << flag << " requires a value\n";
                return false;
            }
            out = args[++i];
            return true;
        };
        if (args[i] == "--type") { if (!take_value(type, "--type")) return 2; continue; }
        if (args[i] == "--mpe") { mpe_mode = true; continue; }
        if (args[i] == "--template") { if (!take_value(tmpl, "--template")) return 2; continue; }
        if (args[i] == "--manufacturer") { if (!take_value(manufacturer, "--manufacturer")) return 2; continue; }
        if (args[i] == "--output") { if (!take_value(output_path, "--output")) return 2; continue; }
        if (args[i] == "--targets" || args[i] == "--target") {
            if (!take_value(targets_arg, args[i].c_str())) return 2;
            continue;
        }
        if (args[i] == "--in-tree" || args[i] == "--example") { in_tree_mode = true; continue; }
        if (args[i] == "--no-build") { no_build = true; continue; }
        if (args[i] == "--no-interactive" || args[i] == "--ci") { ci_mode = true; continue; }
        if (args[i] == "--pin") { pin_at_create = true; continue; }
        if (args[i] == "--debug") { debug_build = true; continue; }
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp create — create a new plugin project\n\n";
            std::cout << "Usage: pulp create <name> [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --type <effect|instrument|app|bare>  Plugin type (default: effect)\n";
            std::cout << "  --mpe                                Opt into MPE (per-note expression) — only with --type instrument\n";
            std::cout << "  --template <name>                    Use named template (e.g. gain)\n";
            std::cout << "  --manufacturer <name>                Manufacturer (default: Pulp)\n";
            std::cout << "  --output <dir>                       Override output directory\n";
            std::cout << "  --targets <list>                     Comma-separated extra targets (e.g. android)\n";
            std::cout << "  --in-tree, --example                 Add the project to examples/ using the local Pulp checkout\n";
            std::cout << "  --no-build                           Skip build after scaffolding\n";
            std::cout << "  --no-interactive, --ci               Non-interactive mode (use defaults)\n";
            std::cout << "  --pin                                Write the current SDK version into pulp.toml (default: floating, tracks latest)\n";
            std::cout << "  --debug                              Configure CMAKE_BUILD_TYPE=Debug (default: Release)\n";
            std::cout << "\nDefault behavior: create a standalone product project, even inside the Pulp repo.\n";
            std::cout << "Inside the repo, the default location is next to the repo root unless\n";
            std::cout << "PULP_PROJECTS_DIR or ~/.pulp/config.toml overrides it.\n";
            std::cout << "\nSDK pinning: new projects default to floating mode (sdk_version = \"latest\")\n";
            std::cout << "  so they track the latest installed SDK and pick up framework fixes\n";
            std::cout << "  on every rebuild. Pass --pin to write the exact current SDK version\n";
            std::cout << "  instead, or run `pulp project pin <version>` from inside the project\n";
            std::cout << "  at any time.\n";
            return 0;
        }
        if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "pulp create: unknown flag: " << args[i] << "\n";
            std::cerr << "Usage: pulp create <name> [--type effect|instrument|app|bare] [options]\n";
            return 2;
        }
        if (name.empty() && !args[i].empty() && args[i][0] != '-') { name = args[i]; continue; }
    }

    // Parse --targets list (comma-separated, case-insensitive)
    bool want_android = false;
    if (!targets_arg.empty()) {
        std::string buf;
        auto flush = [&]() {
            std::string lower;
            for (char c : buf) lower += static_cast<char>(std::tolower(c));
            lower = trim(lower);
            if (lower == "android") want_android = true;
            // Desktop targets (vst3/au/clap/standalone) are already handled via
            // default_create_formats() — accepting them here is a no-op so the
            // flag can be used uniformly.
            buf.clear();
        };
        for (char c : targets_arg) {
            if (c == ',' || c == ' ') flush();
            else buf += c;
        }
        flush();
    }

    if (name.empty()) {
        std::cerr << "Usage: pulp create <name> [--type effect|instrument|app|bare] [options]\n";
        return 1;
    }

    std::string template_key = tmpl.empty() ? type : tmpl;

    if (tmpl.empty() && type != "effect" && type != "instrument" && type != "app" && type != "bare") {
        std::cerr << "Error: --type must be 'effect', 'instrument', 'app', or 'bare'\n";
        return 1;
    }

    if (in_tree_mode && root.empty()) {
        std::cerr << "Error: --in-tree/--example can only be used from inside the Pulp repo\n";
        return 1;
    }

    bool standalone_mode = !in_tree_mode;

    auto log = [&](const std::string& msg) {
        if (!ci_mode) std::cout << msg;
    };

    // Compute names
    std::string class_name = to_class_name(name);
    std::string lower_name = to_lower_name(name);
    std::string ns = to_namespace_name(name);
    std::string factory = ns;
    std::string plugin_code = make_plugin_code(class_name);
    std::string aax_product_code = make_aax_product_code(class_name);
    std::string mfr_code = make_mfr_code(manufacturer);
    std::string bundle_id = "com." + to_namespace_name(manufacturer) + "." + ns;
    std::string header_name = replace_all_str(lower_name, "-", "_") + ".hpp";

    // Output directory
    fs::path out_dir;
    if (!output_path.empty()) {
        out_dir = fs::path(output_path);
        if (!out_dir.is_absolute()) out_dir = fs::current_path() / out_dir;
    } else if (in_tree_mode) {
        out_dir = root / "examples" / lower_name;
    } else {
        out_dir = resolve_create_projects_base_dir(root) / lower_name;
    }

    if (in_tree_mode) {
        auto examples_root = root / "examples";
        if (!path_is_within(out_dir, examples_root)) {
            std::cerr << "Error: --in-tree projects must live under " << examples_root.string() << "\n";
            return 1;
        }
    } else if (!root.empty() && path_is_within(out_dir, root)) {
        std::cerr << "Error: standalone product projects must live outside the Pulp repo\n";
        std::cerr << "  Use --in-tree to scaffold under examples/, or choose --output outside\n";
        std::cerr << "  " << root.string() << "\n";
        return 1;
    }

    if (fs::exists(out_dir)) {
        std::cerr << "Error: " << out_dir.string() << " already exists\n";
        return 1;
    }

    // Quick doctor check
    log("Checking environment...\n");
    auto checks = run_doctor_checks(standalone_mode ? fs::path{} : root, standalone_mode);
    bool env_ok = true;
    for (auto& c : checks) {
        if (c.passed) continue;
        // Optional checks are advisory — they surface fix advice but
        // must not gate `pulp create`. The pulp-mcp row (#2067) is
        // optional because the binary is only needed for the Claude
        // Code plugin's MCP server, not for creating a Pulp project.
        if (c.optional) {
            log("  \xe2\x9a\xa0 " + c.name
                + (c.detail.empty() ? std::string{}
                                    : (" — " + c.detail))
                + "\n");
            continue;
        }
        std::cout << "  \xe2\x9c\x97 " << c.name;
        if (!c.fix.empty()) std::cout << " — fix: " << c.fix;
        std::cout << "\n";
        env_ok = false;
    }
    if (!env_ok) {
        std::cerr << "\nEnvironment issues found. Run `pulp doctor --fix` first.\n";
        return 1;
    }
    log("  \xe2\x9c\x93 Environment OK\n\n");

    if (standalone_mode && !root.empty()) {
        const bool needs_vst3 = type != "app" && type != "bare" && !checkout_supports_vst3(root);
#ifdef __APPLE__
        const bool needs_au = type != "app" && type != "bare" && !checkout_supports_au(root);
#else
        const bool needs_au = false;
#endif
        if (needs_vst3 || needs_au) {
            log("Preparing current checkout dependencies for standalone plugin formats...\n");
            if (ensure_checkout_dependencies(root) != 0) {
                std::cerr << "Error: could not prepare checkout dependencies.\n";
                return 1;
            }
            log("\n");
        }
    }

    auto formats = default_create_formats(root, type);
    if (standalone_mode && !root.empty() && type != "app" && type != "bare") {
        if (formats.find("VST3") == std::string::npos) {
            print_warn("VST3 SDK unavailable in current checkout — generating without VST3 support");
        }
#ifdef __APPLE__
        if (formats.find("AU") == std::string::npos) {
            print_warn("AudioUnitSDK unavailable in current checkout — generating without AU support");
        }
#endif
    }
#if defined(__APPLE__) || defined(_WIN32)
    if (formats.find("AAX") != std::string::npos && find_aax_sdk_root().empty()) {
        log("AAX is optional. Build the other formats now, or install the AAX SDK later and set PULP_AAX_SDK_DIR.\n");
        log("See https://developer.avid.com/aax/ for the AAX SDK and DigiShell/AAX Validator downloads.\n\n");
    }
#endif

    fs::path sdk_dir;
    std::string sdk_version = PULP_SDK_VERSION;
    fs::path templates_base = root.empty() ? fs::path{} : root / "tools" / "templates";

    if (standalone_mode && root.empty()) {
        log("Standalone mode — fetching SDK...\n");
        sdk_dir = ensure_sdk(sdk_version);
        if (sdk_dir.empty()) {
            std::cerr << "Error: could not obtain Pulp SDK. Check your internet connection.\n";
            return 1;
        }
        templates_base = sdk_dir / "templates";
    } else if (standalone_mode) {
        log("Standalone mode — using templates from the current checkout.\n");
    }

    std::string lower_name_underscored = replace_all_str(lower_name, "-", "_");

    // Template variables
    std::vector<std::pair<std::string,std::string>> vars = {
        {"PLUGIN_NAME", name},
        {"CLASS_NAME", class_name},
        {"LOWER_NAME", lower_name_underscored},
        {"PLUGIN_URI", "http://pulp.audio/plugins/" + lower_name},
        {"NAMESPACE", ns},
        {"FACTORY_NAME", factory},
        {"HEADER_NAME", header_name},
        {"TARGET_NAME", class_name},
        {"MANUFACTURER", manufacturer},
        {"MANUFACTURER_CODE", mfr_code},
        {"BUNDLE_ID", bundle_id},
        {"VERSION", "1.0.0"},
        {"PLUGIN_CODE", plugin_code},
        {"AAX_PRODUCT_CODE", aax_product_code},
        {"AAX_NATIVE_CODE", plugin_code},
        {"FORMATS", formats},
        {"DESCRIPTION", type == "app" ? "A standalone Pulp audio application" :
                        type == "bare" ? "A minimal Pulp project" :
                        "A Pulp audio " + type},
        {"VST3_UID", make_vst3_uid()},
        {"SDK_VERSION", sdk_version},
    };

    auto source_template_dir = templates_base / template_key;
    auto cmake_template_dir = standalone_mode
        ? templates_base / "standalone" / template_key
        : source_template_dir;

    if (standalone_mode && !root.empty()) {
        auto local_templates_base = root / "tools" / "templates";
        if (!fs::exists(source_template_dir) && fs::exists(local_templates_base / template_key)) {
            source_template_dir = local_templates_base / template_key;
        }
        if (!fs::exists(cmake_template_dir) &&
            fs::exists(local_templates_base / "standalone" / template_key)) {
            cmake_template_dir = local_templates_base / "standalone" / template_key;
        }
    }

    if (standalone_mode && !fs::exists(cmake_template_dir)) {
        cmake_template_dir = source_template_dir;
    }

    if (!fs::exists(source_template_dir)) {
        std::cerr << "Error: template directory not found at " << source_template_dir.string() << "\n";
        if (!tmpl.empty())
            std::cerr << "Available templates: effect, instrument, gain\n";
        return 1;
    }

    fs::create_directories(out_dir);
    if (standalone_mode) {
        log("Mode: standalone product project (default)\n");
        if (!root.empty()) {
            log("  Creating outside the repo so the generated project behaves like an end-user install.\n");
            log("  Use --in-tree to add an example under examples/ instead.\n");
        }
    } else {
        log("Mode: in-tree example project\n");
        log("  Using the local checkout and adding the project to examples/.\n");
    }
    log("Creating " + name + " (" + type + ") at " + out_dir.string() + "\n\n");

    struct FileMapping { std::string tmpl; std::string output; };
    std::string test_name = "test_" + replace_all_str(lower_name, "-", "_") + ".cpp";
    std::vector<FileMapping> file_map = {
        {"processor.hpp.template", header_name},
        {"CMakeLists.txt.template", "CMakeLists.txt"},
        {"clap_entry.cpp.template", "clap_entry.cpp"},
        {"vst3_entry.cpp.template", "vst3_entry.cpp"},
        {"lv2_entry.cpp.template", "lv2_entry.cpp"},
        {"au_v2_entry.cpp.template", "au_v2_entry.cpp"},
        {"aax_entry.cpp.template", "aax_entry.cpp"},
        {"test.cpp.template", test_name},
    };

    if (mpe_mode && type != "instrument") {
        std::cerr << "Warning: --mpe has no effect unless --type instrument; ignoring.\n";
        mpe_mode = false;
    }

    for (auto& [tmpl_file, outfile] : file_map) {
        if (tmpl_file == "clap_entry.cpp.template" && formats.find("CLAP") == std::string::npos) continue;
        if (tmpl_file == "vst3_entry.cpp.template" && formats.find("VST3") == std::string::npos) continue;
        if (tmpl_file == "lv2_entry.cpp.template" && formats.find("LV2") == std::string::npos) continue;
        if (tmpl_file == "au_v2_entry.cpp.template" && formats.find("AU") == std::string::npos) continue;
        if (tmpl_file == "aax_entry.cpp.template" && formats.find("AAX") == std::string::npos) continue;
        auto tmpl_path = (tmpl_file == "CMakeLists.txt.template")
            ? cmake_template_dir / tmpl_file
            : source_template_dir / tmpl_file;
        if (!fs::exists(tmpl_path)) continue;
        auto content = read_file_contents(tmpl_path);
        auto expanded = expand_template_str(content, vars);
        // --mpe post-processing: add supports_mpe = true to the descriptor
        // and insert an include for mpe_buffer.hpp in the processor header.
        if (mpe_mode && tmpl_file == "processor.hpp.template") {
            const std::string accepts_line = ".accepts_midi = true,";
            auto pos = expanded.find(accepts_line);
            if (pos != std::string::npos) {
                expanded.insert(pos + accepts_line.size(),
                    "\n        .supports_mpe = true,");
            }
            const std::string include_anchor = "#include <pulp/format/processor.hpp>";
            pos = expanded.find(include_anchor);
            if (pos != std::string::npos) {
                expanded.insert(pos + include_anchor.size(),
                    "\n#include <pulp/midi/mpe_buffer.hpp>");
            }
        }
        std::ofstream f(out_dir / outfile);
        f << expanded;
        log("  Created " + outfile + (mpe_mode && outfile == header_name ? " (MPE)" : "") + "\n");
    }

    // Standalone main.cpp
    if (formats.find("Standalone") != std::string::npos) {
        std::ofstream f(out_dir / "main.cpp");
        f << "#include \"" << header_name << "\"\n";
        f << "#include <pulp/format/standalone.hpp>\n\n";
        f << "int main() {\n";
        f << "    pulp::format::StandaloneApp app(" << ns << "::create_" << factory << ");\n";
        f << "    pulp::format::StandaloneConfig config;\n";
        if (type == "instrument") {
            f << "    config.input_channels = 0;\n";
        } else {
            f << "    config.input_channels = 2;\n";
        }
        f << "    config.output_channels = 2;\n";
        f << "    app.set_config(config);\n";
        f << "    return app.run_with_editor(false) ? 0 : 1;\n";
        f << "}\n";
        log("  Created main.cpp\n");
    }

    // Copy UI script directory if template includes one
    auto ui_template_dir = source_template_dir / "ui";
    if (fs::exists(ui_template_dir)) {
        auto ui_out_dir = out_dir / "ui";
        fs::create_directories(ui_out_dir);
        for (auto& entry : fs::directory_iterator(ui_template_dir)) {
            auto content = read_file_contents(entry.path());
            auto expanded = expand_template_str(content, vars);
            auto out_path = ui_out_dir / entry.path().filename();
            std::ofstream f(out_path);
            f << expanded;
            std::cout << "  Created ui/" << entry.path().filename().string() << "\n";
        }
    }

    // Generate pulp.toml for standalone projects
    if (standalone_mode) {
        std::ofstream f(out_dir / "pulp.toml");
        f << "[pulp]\n";
        // Pulp #2087: floating-SDK default. Unpinned projects track the
        // latest installed SDK on every rebuild, so users automatically
        // pick up framework fixes without remembering to run an
        // explicit upgrade. `--pin` writes the exact resolved version
        // for users who want reproducibility from day one.
        //
        // The `"latest"` marker is interpreted by version_diag.cpp and
        // the SDK-resolution path in cmd_build / cmd_run / ... — it
        // resolves to the newest sibling under ~/.pulp/sdk/<ver>/ at
        // command time, falling back to the CLI's own SDK_VERSION if
        // no installed SDK exists.
        if (pin_at_create) {
            f << "sdk_version = \"" << sdk_version << "\"\n";
        } else {
            f << "sdk_version = \"latest\"\n";
        }
        if (!root.empty()) {
            auto local_sdk_dir = local_sdk_cache_path(sdk_version);
            f << "sdk_path = \"" << local_sdk_dir.generic_string() << "\"\n";
            f << "sdk_checkout = \"" << root.generic_string() << "\"\n";
        }
        log("  Created pulp.toml\n");
        if (!pin_at_create) {
            log("  SDK mode: floating (tracks latest installed) — pin with `pulp project pin <version>`\n");
        }
    }

    // Android target scaffold (experimental — see issue #83)
    if (want_android) {
        auto android_tmpl_dir = templates_base / "android";
        if (!fs::exists(android_tmpl_dir) && !root.empty()) {
            android_tmpl_dir = root / "tools" / "templates" / "android";
        }
        if (!fs::exists(android_tmpl_dir)) {
            print_warn("Android template directory not found — skipping Android scaffold");
        } else {
            auto android_out = out_dir / "android";
            fs::create_directories(android_out);
            log("\nScaffolding Android project (experimental)...\n");

            // Walk the template tree and expand every .template file.
            for (auto& entry : fs::recursive_directory_iterator(android_tmpl_dir)) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), android_tmpl_dir);
                auto rel_str = rel.generic_string();
                std::string out_rel = rel_str;
                if (out_rel.size() > 9 && out_rel.substr(out_rel.size() - 9) == ".template") {
                    out_rel = out_rel.substr(0, out_rel.size() - 9);
                }
                // Rename MainActivity placeholder path to real namespace package.
                // Template lives at app/src/main/java/MainActivity.kt.template;
                // put it under app/src/main/java/<namespace>/MainActivity.kt
                if (out_rel == "app/src/main/java/MainActivity.kt") {
                    out_rel = "app/src/main/java/" + ns + "/MainActivity.kt";
                }
                auto out_path = android_out / out_rel;
                fs::create_directories(out_path.parent_path());
                auto content = read_file_contents(entry.path());
                auto expanded = expand_template_str(content, vars);
                std::ofstream f(out_path);
                f << expanded;
                log("  Created android/" + out_rel + "\n");
            }
            log("\n  Android scaffold is experimental. See android/README.md for\n"
                "  build instructions and current limitations.\n");
        }
    }

    // Add to examples/CMakeLists.txt (in-tree mode only)
    if (in_tree_mode) {
        auto examples_cmake = root / "examples" / "CMakeLists.txt";
        auto rel_dir = fs::relative(out_dir, root / "examples").generic_string();

        if (fs::exists(examples_cmake)) {
            std::string cmake_content = read_file_contents(examples_cmake);
            std::string add_line = "add_subdirectory(" + rel_dir + ")";
            if (cmake_content.find(add_line) == std::string::npos) {
                std::ofstream f(examples_cmake, std::ios::app);
                f << "\n# " << name << " (generated by pulp create)\n"
                  << add_line << "\n";
                log("  Added to examples/CMakeLists.txt\n");
            }
        }
    }

    // Register the new project in ~/.pulp/projects.json so
    // `pulp doctor --versions` can report per-project skew without an
    // opt-in disk scan. Issue #552 Slice 1b. The registry is a
    // best-effort diagnostic surface — a failure here is non-fatal
    // and logged only in non-CI mode so we don't clutter scaffold
    // output of CI-driven scaffolds.
    {
        auto reg = pulp::cli::projects_registry::registry_path();
        // Codex 2026-04-21 wave 2 P2 on #563: check the write result
        // rather than blindly printing "Registered" — otherwise a
        // failed `write_registry()` in an unwritable $PULP_HOME
        // surfaces as "registered" to the user but the project is
        // missing from the file. Scaffold itself still succeeds
        // (the project tree IS on disk) — we just report honestly
        // that the diagnostic registry didn't persist.
        bool wrote_ok = false;
        pulp::cli::projects_registry::add_project(reg, out_dir, name, &wrote_ok);
        if (wrote_ok) {
            log("  Registered in " + reg.string() + "\n");
        } else {
            log("  (note) could not write registry at " + reg.string() + "\n");
        }
    }

    log("\n");

    if (no_build) {
        log("Scaffolding complete. Run `pulp build` to build.\n");
        return 0;
    }

    auto build_targets = pulp::cli::create_default_build_targets(class_name, type, formats);
    auto append_build_targets = [&](std::string& cmd) {
        cmd += " --target";
        for (const auto& target : build_targets) {
            cmd += " " + target;
        }
    };

    // Build
    log("Building...\n");
    if (standalone_mode) {
        if (sdk_dir.empty()) {
            if (!root.empty()) {
                log("Preparing local SDK from the current checkout...\n");
                sdk_dir = ensure_checkout_sdk(root, sdk_version);
            } else {
                log("Fetching SDK for standalone build...\n");
                sdk_dir = ensure_sdk(sdk_version);
            }
            if (sdk_dir.empty()) {
                std::cerr << "Error: could not obtain Pulp SDK. ";
                if (!root.empty()) {
                    std::cerr << "Run with --no-build to scaffold only, or use --in-tree while developing Pulp.\n";
                } else {
                    std::cerr << "Check your internet connection.\n";
                }
                return 1;
            }
        }

        std::string configure_cmd = "cmake -S " + out_dir.string()
            + " -B " + (out_dir / "build").string()
            + " -DCMAKE_BUILD_TYPE=" + (debug_build ? "Debug" : "Release")
            + " -DCMAKE_PREFIX_PATH=" + sdk_dir.string();
        append_windows_visual_studio_generator_args(configure_cmd);
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        std::string build_cmd = "cmake --build " + shell_quote(out_dir / "build");
        append_build_targets(build_cmd);
        build_cmd += " --config " + pulp::cli::create_build_config(debug_build);
        rc = run_with_spinner(build_cmd, "Building");
        if (rc != 0) {
            std::cerr << "Build failed.\n";
            return rc;
        }

        log("\nRunning tests...\n");
        rc = run("ctest --test-dir " + (out_dir / "build").string()
                 + " -C " + pulp::cli::create_build_config(debug_build)
                 + " --output-on-failure");
        if (rc != 0) {
            std::cerr << "Tests failed.\n";
            return rc;
        }
    } else {
        auto example_rel_dir = fs::relative(out_dir, root / "examples");
        std::string configure_cmd = "cmake -S " + shell_quote(root) + " -B "
                                  + shell_quote(root / "build")
                                  + " -DCMAKE_BUILD_TYPE="
                                  + (debug_build ? "Debug" : "Release");
        append_windows_visual_studio_generator_args(configure_cmd);
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        std::string build_cmd = "cmake --build " + shell_quote(root / "build");
        append_build_targets(build_cmd);
        build_cmd += " --config " + pulp::cli::create_build_config(debug_build);
        rc = run_with_spinner(build_cmd, "Building");
        if (rc != 0) {
            std::cerr << "Build failed.\n";
            return rc;
        }

        log("\nRunning tests...\n");
        auto test_binary = root / "build" / "examples" / example_rel_dir / (class_name + "-test");
        if (fs::exists(test_binary)) {
            rc = run(test_binary.string());
        } else {
            rc = run("ctest --test-dir " + (root / "build").string()
                     + " -C " + pulp::cli::create_build_config(debug_build)
                     + " -R \"" + name + "\" --output-on-failure");
        }
        if (rc != 0) {
            std::cerr << "Tests failed.\n";
            return rc;
        }
    }

    std::string test_filter = replace_all_str(lower_name, "-", "_");

    // Success report
    std::cout << "\n  \xe2\x9c\x93 " << name << " is ready!\n\n";
    std::cout << "  Source:     " << out_dir.string() << "\n";

    if (standalone_mode) {
        std::cout << "  SDK:        " << sdk_dir.string() << "\n";
    }

    auto build_dir = standalone_mode ? (out_dir / "build") : (root / "build");
    auto matches_format_artifact = [&](const fs::path& path, const std::string& fmt) {
        const auto filename = path.filename().string();
        if (fmt == "VST3") {
            return filename == class_name + ".vst3"
                || filename == class_name + ".dll"
                || filename == "lib" + class_name + ".so";
        }
        if (fmt == "CLAP") return filename == class_name + ".clap";
        if (fmt == "LV2") return filename == class_name + ".lv2";
        if (fmt == "AU") return filename == class_name + ".component";
        if (fmt == "AAX") return filename == class_name + ".aaxplugin";
        return false;
    };
    auto find_format_artifact = [&](const std::string& fmt) -> fs::path {
        auto fmt_dir = build_dir / fmt;
        if (!fs::exists(fmt_dir)) return {};
        for (auto& entry : fs::recursive_directory_iterator(fmt_dir)) {
            if (matches_format_artifact(entry.path(), fmt)) {
                return entry.path();
            }
        }
        return {};
    };
    for (auto fmt : {"VST3", "CLAP", "LV2", "AU", "AAX"}) {
        if (auto artifact = find_format_artifact(fmt); !artifact.empty()) {
            std::cout << "  " << fmt << ":       " << artifact.string() << "\n";
        }
    }
    auto find_standalone_artifact = [&]() -> fs::path {
        auto standalone_app = build_dir / (class_name + ".app");
        if (fs::exists(standalone_app)) return standalone_app;
        auto standalone_bin = build_dir / class_name;
        if (fs::exists(standalone_bin)) return standalone_bin;
        for (auto& entry : fs::recursive_directory_iterator(build_dir)) {
            const auto filename = entry.path().filename().string();
            if (filename == class_name + ".exe" || filename == class_name || filename == class_name + ".app") {
                return entry.path();
            }
        }
        return {};
    };
    if (auto standalone = find_standalone_artifact(); !standalone.empty()) {
        std::cout << "  Standalone: " << standalone.string() << "\n";
    }

    std::cout << "\n  Next steps:\n";
    if (standalone_mode) {
        std::cout << "    cd " << out_dir.string() << "\n";
    }
    std::cout << "    pulp build              # rebuild after changes\n";
    std::cout << "    pulp test -R " << test_filter << "  # run tests\n";
    std::cout << "    pulp validate           # validate plugin formats\n";
    return 0;
}
