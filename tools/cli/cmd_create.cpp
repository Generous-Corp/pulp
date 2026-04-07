// cmd_create.cpp — pulp create command

#include "cli_common.hpp"
#include "create_targets.hpp"

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
    std::string name, type = "effect", manufacturer = "Pulp", output_path, tmpl;
    bool no_build = false;
    bool ci_mode = false;
    bool in_tree_mode = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--type" && i + 1 < args.size()) { type = args[++i]; continue; }
        if (args[i] == "--template" && i + 1 < args.size()) { tmpl = args[++i]; continue; }
        if (args[i] == "--manufacturer" && i + 1 < args.size()) { manufacturer = args[++i]; continue; }
        if (args[i] == "--output" && i + 1 < args.size()) { output_path = args[++i]; continue; }
        if (args[i] == "--in-tree" || args[i] == "--example") { in_tree_mode = true; continue; }
        if (args[i] == "--no-build") { no_build = true; continue; }
        if (args[i] == "--no-interactive" || args[i] == "--ci") { ci_mode = true; continue; }
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp create — create a new plugin project\n\n";
            std::cout << "Usage: pulp create <name> [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --type <effect|instrument|app|bare>  Plugin type (default: effect)\n";
            std::cout << "  --template <name>                    Use named template (e.g. gain)\n";
            std::cout << "  --manufacturer <name>                Manufacturer (default: Pulp)\n";
            std::cout << "  --output <dir>                       Override output directory\n";
            std::cout << "  --in-tree, --example                 Add the project to examples/ using the local Pulp checkout\n";
            std::cout << "  --no-build                           Skip build after scaffolding\n";
            std::cout << "  --no-interactive, --ci               Non-interactive mode (use defaults)\n";
            std::cout << "\nDefault behavior: create a standalone product project, even inside the Pulp repo.\n";
            std::cout << "Inside the repo, the default location is next to the repo root unless\n";
            std::cout << "PULP_PROJECTS_DIR or ~/.pulp/config.toml overrides it.\n";
            return 0;
        }
        if (name.empty() && !args[i].empty() && args[i][0] != '-') { name = args[i]; continue; }
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
        if (!c.passed) {
            std::cout << "  \xe2\x9c\x97 " << c.name;
            if (!c.fix.empty()) std::cout << " — fix: " << c.fix;
            std::cout << "\n";
            env_ok = false;
        }
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
        std::ofstream f(out_dir / outfile);
        f << expanded;
        log("  Created " + outfile + "\n");
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
        f << "sdk_version = \"" << sdk_version << "\"\n";
        if (!root.empty()) {
            auto local_sdk_dir = local_sdk_cache_path(sdk_version);
            f << "sdk_path = \"" << local_sdk_dir.generic_string() << "\"\n";
            f << "sdk_checkout = \"" << root.generic_string() << "\"\n";
        }
        log("  Created pulp.toml\n");
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
            + " -DCMAKE_BUILD_TYPE=Debug"
            + " -DCMAKE_PREFIX_PATH=" + sdk_dir.string();
        append_windows_visual_studio_generator_args(configure_cmd);
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        std::string build_cmd = "cmake --build " + shell_quote(out_dir / "build");
        append_build_targets(build_cmd);
        rc = run_with_spinner(build_cmd, "Building");
        if (rc != 0) {
            std::cerr << "Build failed.\n";
            return rc;
        }

        log("\nRunning tests...\n");
        rc = run("ctest --test-dir " + (out_dir / "build").string() + " --output-on-failure");
        if (rc != 0) {
            std::cerr << "Tests failed.\n";
            return rc;
        }
    } else {
        auto example_rel_dir = fs::relative(out_dir, root / "examples");
        std::string configure_cmd = "cmake -S " + shell_quote(root) + " -B "
                                  + shell_quote(root / "build")
                                  + " -DCMAKE_BUILD_TYPE=Debug";
        append_windows_visual_studio_generator_args(configure_cmd);
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        std::string build_cmd = "cmake --build " + shell_quote(root / "build");
        append_build_targets(build_cmd);
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
            rc = run("ctest --test-dir " + (root / "build").string() + " -R \"" + name + "\" --output-on-failure");
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
