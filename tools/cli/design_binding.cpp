#include "design_binding.hpp"

#include <system_error>

namespace pulp::cli {

static bool same_path(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;

    std::error_code ec;
    auto norm_a = fs::weakly_canonical(a, ec);
    if (ec) norm_a = a.lexically_normal();
    ec.clear();
    auto norm_b = fs::weakly_canonical(b, ec);
    if (ec) norm_b = b.lexically_normal();
    return norm_a == norm_b;
}

std::string design_binding_autobind_error() {
    return "Auto-binding only works from inside a Pulp checkout or when pulp lives inside a "
           "Pulp build tree; otherwise pass --build-dir and --script.";
}

DesignBindingResult resolve_design_binding(const DesignBindingInput& input) {
    DesignBindingResult result;
    result.root = input.cwd_root;
    if (!input.cwd_root.empty()) result.root_reason = "current checkout";

    if (!input.script_path.empty()) {
        result.script_path = input.script_path;
        result.script_reason = input.script_explicit ? "explicit --script"
                                                     : "positional script argument";
        if (!input.script_root.empty() && !same_path(input.script_root, result.root)) {
            result.root = input.script_root;
            result.root_reason = "script path";
        }
    }

    if (result.root.empty() && input.build_dir_explicit && !input.build_dir_cache_root.empty()) {
        result.root = input.build_dir_cache_root;
        result.root_reason = "build dir cache";
    }

    if (result.root.empty() && !input.binary_root.empty()) {
        result.root = input.binary_root;
        result.root_reason = "CLI-adjacent build tree";
    }

    if (result.root.empty()) {
        result.error = design_binding_autobind_error();
        return result;
    }

    if (input.build_dir_explicit) {
        result.build_dir = input.build_dir;
        result.build_reason = "explicit --build-dir";
    } else if (!input.binary_build_dir.empty() && same_path(input.binary_root, result.root)) {
        result.build_dir = input.binary_build_dir;
        result.build_reason = "CLI-adjacent build tree";
    } else {
        result.build_dir = result.root / "build";
        result.build_reason = "default build dir under selected root";
    }

    if (!input.build_dir_cache_root.empty() && !same_path(input.build_dir_cache_root, result.root)) {
        result.error = "Build dir " + result.build_dir.string() + " is configured for "
                     + input.build_dir_cache_root.string() + ", but the selected design project is "
                     + result.root.string() + ". Use a matching --build-dir, or remove CMakeCache.txt and reconfigure.";
        return result;
    }

    if (result.script_path.empty()) {
        // The design-tool UI is split across design-tool-*.js concern modules
        // (P8-NEW). The entry module is the path handed to pulp-design-tool;
        // the binary loads its sibling modules in order from the same dir.
        result.script_path = result.root / "examples" / "design-tool" / "design-tool-core.js";
        result.script_reason = "default design tool script";
    }

    result.ok = true;
    return result;
}

}  // namespace pulp::cli
