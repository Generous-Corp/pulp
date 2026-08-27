#include "authority_navigation.hpp"
#include "cli_common.hpp"
#include "json_writer.hpp"

#include <iostream>

namespace {

void print_authority_usage() {
    std::cout << "pulp authority — find the native source of truth\n\n"
              << "Usage:\n"
              << "  pulp authority list [--json]\n"
              << "  pulp authority query <id-or-alias> [--json]\n\n"
              << "The navigator prints routing and provenance only. It does not copy\n"
              << "capability rows, execute the reported command, start a live instance,\n"
              << "or infer support from an absent row.\n";
}

fs::path find_pulp_toml_root(fs::path path) {
    while (!path.empty()) {
        if (fs::is_regular_file(path / "pulp.toml"))
            return path;
        const auto parent = path.parent_path();
        if (parent == path)
            break;
        path = parent;
    }
    return {};
}

bool same_path(const fs::path& lhs, const fs::path& rhs) {
    std::error_code lhs_error;
    std::error_code rhs_error;
    const auto canonical_lhs = fs::weakly_canonical(lhs, lhs_error);
    const auto canonical_rhs = fs::weakly_canonical(rhs, rhs_error);
    return !lhs_error && !rhs_error && canonical_lhs == canonical_rhs;
}

void print_authority_error(bool json, const std::string& code, const std::string& message) {
    if (json) {
        std::cerr << "{\"schema\":\"pulp.authority-navigation.error.v1\",\"error\":{\"code\":"
                  << pulp::cli::json_string(code)
                  << ",\"message\":" << pulp::cli::json_string(message) << "}}\n";
    } else {
        std::cerr << "Error [" << code << "]: " << message << "\n";
    }
}

} // namespace

int cmd_authority(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_authority_usage();
        return 0;
    }
    bool json = false;
    std::vector<std::string> positional;
    for (const auto& arg : args) {
        if (arg == "--json")
            json = true;
        else
            positional.push_back(arg);
    }
    const bool list = positional.size() == 1 && positional[0] == "list";
    const bool query = positional.size() == 2 && positional[0] == "query";
    if (!list && !query) {
        std::cerr << "Error: expected `pulp authority list` or "
                     "`pulp authority query <id-or-alias>`\n\n";
        print_authority_usage();
        return 2;
    }

    const auto cwd = fs::current_path();
    const auto executable = current_executable_path();
    auto resolved = pulp::cli::authority::resolve_registry(cwd, executable);
    if (resolved && resolved->context == pulp::cli::authority::Context::source) {
        // A real Pulp checkout/source build is the highest-precedence authority.
    } else if (const auto project = find_pulp_toml_root(cwd); !project.empty()) {
        const auto sdk = resolve_standalone_sdk(project, false);
        if (sdk.resolved_sdk_dir.empty()) {
            print_authority_error(
                json, "authority_registry_unavailable",
                "exact selected SDK v" + sdk.requested_version +
                    " is not installed; authority navigation will not fall back to another SDK");
            return 1;
        }
        const auto selected_sdk = sdk.resolved_sdk_dir;
        const auto selected_sdk_version =
            sdk.sdk_path_custom_unverifiable
                ? std::string{}
                : (sdk.used_sdk_path_hint ? sdk.sdk_path_version : sdk.requested_version);
        std::string selected_sdk_source;
        if (sdk.sdk_path_custom_unverifiable) {
            selected_sdk_source = "pulp.toml:sdk_path:version_unverified";
        } else if (sdk.used_sdk_path_hint) {
            selected_sdk_source = "pulp.toml:sdk_path";
        } else if (!sdk.sdk_checkout_hint.empty() &&
                   same_path(selected_sdk, local_sdk_cache_path(sdk.requested_version))) {
            selected_sdk_source = "pulp.toml:sdk_checkout_cache";
        } else {
            selected_sdk_source = "pulp.toml:sdk_version";
        }
        resolved = pulp::cli::authority::resolve_registry(
            cwd, executable, selected_sdk, selected_sdk_version, selected_sdk_source);
    }
    if (!resolved) {
        print_authority_error(
            json, "authority_registry_unavailable",
            "authority navigation registry not found in this source checkout, exact selected "
            "SDK, or adjacent SDK; CLI-only archives do not carry this registry");
        return 1;
    }
    auto loaded = pulp::cli::authority::load_registry(*resolved);
    if (!loaded.registry) {
        print_authority_error(json, "authority_registry_invalid", loaded.error);
        return 1;
    }
    if (list) {
        std::cout << pulp::cli::authority::render_list(*loaded.registry, json);
        return 0;
    }
    auto rendered = pulp::cli::authority::render_query(*loaded.registry, positional[1], json);
    if (!rendered) {
        print_authority_error(json, "unknown_authority",
                              "unknown authority id or alias: " + positional[1]);
        return 1;
    }
    std::cout << *rendered;
    return 0;
}
