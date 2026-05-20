// cmd_design.cpp — pulp design command

#include "cli_common.hpp"
#include "design_binding.hpp"

#include <pulp/view/design_import.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

namespace {

std::string read_text_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// `pulp design lint <DESIGN.md>` — runs the seven Google design.md
// lint rules + section-order. Exit 0 if no error findings; exit 1
// if any error-severity finding is present.
int run_lint(const std::vector<std::string>& rest) {
    if (rest.empty()) {
        std::cerr << "Usage: pulp design lint <path/to/DESIGN.md>\n";
        return 1;
    }
    auto text = read_text_file(rest.front());
    if (text.empty()) {
        std::cerr << "Error: cannot read " << rest.front() << "\n";
        return 1;
    }
    auto parsed = pulp::view::parse_designmd(text);
    auto findings = pulp::view::lint_designmd(parsed);
    int errors = 0;
    for (const auto& d : findings) {
        const char* sev = (d.severity == pulp::view::DesignMdSeverity::error)   ? "error" :
                          (d.severity == pulp::view::DesignMdSeverity::warning) ? "warning" : "info";
        std::cout << "[" << sev << "] " << d.code
                  << " at " << (d.path.empty() ? "<root>" : d.path);
        if (d.line > 0) std::cout << " (line " << d.line << ":" << d.column << ")";
        std::cout << ": " << d.message << "\n";
        if (d.severity == pulp::view::DesignMdSeverity::error) ++errors;
    }
    std::cout << "Lint summary: " << findings.size() << " finding(s), "
              << errors << " error(s).\n";
    return errors > 0 ? 1 : 0;
}

// `pulp design diff <before.md> <after.md>` — token-level diff plus
// regression flag. Exit 0 if no regression; exit 1 if regression.
int run_diff(const std::vector<std::string>& rest) {
    if (rest.size() < 2) {
        std::cerr << "Usage: pulp design diff <before.md> <after.md>\n";
        return 1;
    }
    auto before_text = read_text_file(rest[0]);
    auto after_text  = read_text_file(rest[1]);
    if (before_text.empty() || after_text.empty()) {
        std::cerr << "Error: cannot read one of the input files\n";
        return 1;
    }
    auto before = pulp::view::parse_designmd(before_text);
    auto after  = pulp::view::parse_designmd(after_text);
    auto diff = pulp::view::diff_designmd(before, after);
    auto report = [](const char* group, const pulp::view::DesignMdTokenDiff& d) {
        std::cout << group << ": +" << d.added.size()
                  << " -" << d.removed.size()
                  << " ~" << d.modified.size() << "\n";
        for (const auto& k : d.added)    std::cout << "  + " << k << "\n";
        for (const auto& k : d.removed)  std::cout << "  - " << k << "\n";
        for (const auto& k : d.modified) std::cout << "  ~ " << k << "\n";
    };
    report("colors",     diff.colors);
    report("dimensions", diff.dimensions);
    report("strings",    diff.strings);
    std::cout << "regression: " << (diff.regression ? "true" : "false") << "\n";
    return diff.regression ? 1 : 0;
}

} // namespace

int cmd_design(const std::vector<std::string>& args) {
    // ── Phase 2: `pulp design lint` and `pulp design diff` ─────────────
    // Both verbs operate on DESIGN.md files and do NOT launch the live
    // design tool. They short-circuit before the design-tool build path.
    if (!args.empty()) {
        if (args[0] == "lint") {
            return run_lint(std::vector<std::string>(args.begin() + 1, args.end()));
        }
        if (args[0] == "diff") {
            return run_diff(std::vector<std::string>(args.begin() + 1, args.end()));
        }
    }

    fs::path cwd_root = find_project_root();
    fs::path build_dir;
    fs::path script_path;
    std::vector<std::string> pass_through;
    bool build_dir_explicit = false;
    std::string root_reason = cwd_root.empty() ? "" : "current checkout";
    std::string build_reason;
    std::string script_reason;

    bool watch_mode = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--watch" || args[i] == "-w") {
            watch_mode = true;
            continue;
        }
        if (args[i] == "--build-dir") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp design: --build-dir requires a value\n";
                return 2;
            }
            build_dir = fs::absolute(args[++i]);
            build_dir_explicit = true;
            build_reason = "explicit --build-dir";
            continue;
        }
        if (args[i] == "--script") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp design: --script requires a value\n";
                return 2;
            }
            script_path = fs::absolute(args[++i]);
            script_reason = "explicit --script";
            continue;
        }
        pass_through.push_back(args[i]);
    }

    if (script_path.empty() && !pass_through.empty() && !pass_through.front().empty()
        && pass_through.front()[0] != '-') {
        fs::path candidate = pass_through.front();
        auto ext = candidate.extension().string();
        if (ext == ".js" || ext == ".mjs" || ext == ".cjs") {
            script_path = candidate.is_absolute() ? candidate : fs::absolute(candidate);
            script_reason = "positional script argument";
            pass_through.erase(pass_through.begin());
        }
    }

    auto binary_build_dir = build_dir_from_current_binary();
    auto binary_root = cmake_home_directory(binary_build_dir);
    auto cache_root = cmake_home_directory(build_dir);
    pulp::cli::DesignBindingInput binding_input;
    binding_input.cwd_root = cwd_root;
    binding_input.build_dir = build_dir;
    binding_input.script_path = script_path;
    binding_input.script_root = script_path.empty() ? fs::path{} : find_project_root_from(script_path.parent_path());
    binding_input.build_dir_cache_root = cache_root;
    binding_input.binary_build_dir = binary_build_dir;
    binding_input.binary_root = binary_root;
    binding_input.build_dir_explicit = build_dir_explicit;
    binding_input.script_explicit = !script_reason.empty() && script_reason != "positional script argument";

    auto binding = pulp::cli::resolve_design_binding(binding_input);
    if (!binding.ok) {
        std::cerr << "Error: " << binding.error << "\n";
        return 1;
    }

    auto root = binding.root;
    build_dir = binding.build_dir;
    script_path = binding.script_path;
    root_reason = binding.root_reason;
    build_reason = binding.build_reason;
    script_reason = binding.script_reason;

    if (!fs::exists(script_path)) {
        std::cerr << "Error: design tool script not found at " << script_path << "\n";
        return 1;
    }

    std::cout << "Design root:  " << root << " (" << root_reason << ")\n";
    std::cout << "Build dir:    " << build_dir << " (" << build_reason << ")\n";
    std::cout << "Script:       " << script_path << " (" << script_reason << ")\n";

    int rc = ensure_repo_build_configured(root, build_dir);
    if (rc != 0) return rc;

    rc = run_with_spinner("cmake --build " + shell_quote(build_dir) + " --target pulp-design-tool",
                          "Building design tool");
    if (rc != 0) return rc;

    std::vector<fs::path> candidates = {
        platform_executable(build_dir / "tools" / "design" / "pulp-design"),
        platform_executable(build_dir / "examples" / "design-tool" / "pulp-design-tool"),
    };

    fs::path design_bin;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            design_bin = candidate;
            break;
        }
    }

    if (design_bin.empty()) {
        std::cerr << "Error: pulp-design-tool not found after build in " << build_dir << "\n";
        return 1;
    }

    if (watch_mode) {
        // Watch mode: rebuild design tool on C++ changes, auto-relaunch.
        // JS hot-reload is handled by the design tool's internal HotReloader.
        WatchOptions opts;
        opts.root = root;
        opts.build_dir = build_dir;
        opts.build_args = {"--target", "pulp-design-tool"};
        opts.launch_target = design_bin.string();
        opts.launch_args = {script_path.string()};
        for (const auto& arg : pass_through) opts.launch_args.push_back(arg);
        return watch_loop(opts);
    }

    std::string cmd = shell_quote(design_bin) + " " + shell_quote(script_path);
    for (const auto& arg : pass_through) {
        cmd += " " + shell_quote(arg);
    }
    return run(cmd);
}
