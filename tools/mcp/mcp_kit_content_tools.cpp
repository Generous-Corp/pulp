// mcp_kit_content_tools.cpp — Kit and installed-content MCP handlers.

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"

#include <string>

namespace pulp_mcp {

std::string handle_kit_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit validate " +
                      shell_quote(path) + " --json";
    if (extract_bool(params_json, "strict", false)) {
        cmd += " --strict";
    }
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto subcommand = extract_string(params_json, "subcommand");
    if (subcommand.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: subcommand is required\"}]}";
    }
    if (subcommand == "search")
        return handle_kit_search(params_json);
    if (subcommand == "validate")
        return handle_kit_validate(params_json);
    if (subcommand == "inspect" || subcommand == "show")
        return handle_kit_inspect(params_json);
    if (subcommand == "plan")
        return handle_kit_plan(params_json);
    if (subcommand == "preview") {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: use kit plan; preview is "
               "reserved for content compatibility checks\"}]}";
    }
    if (subcommand == "verify")
        return handle_kit_verify(params_json);
    if (subcommand == "apply")
        return handle_kit_apply(params_json);
    if (subcommand == "remove" || subcommand == "uninstall")
        return handle_kit_remove(params_json);
    if (subcommand == "pack")
        return handle_kit_pack(params_json);
    if (subcommand == "publish" || subcommand == "publish-check")
        return handle_kit_publish_check(params_json);
    if (subcommand == "init")
        return handle_kit_init(params_json);
    return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: unsupported kit subcommand\"}]}";
}

std::string handle_kit_search(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit search";
    auto query = extract_string(params_json, "query");
    if (!query.empty())
        cmd += " " + shell_quote(query);
    auto search_root = extract_string(params_json, "root");
    if (!search_root.empty())
        cmd += " --root " + shell_quote(search_root);
    auto kind = extract_string(params_json, "kind");
    if (!kind.empty())
        cmd += " --kind " + shell_quote(kind);
    auto lane = extract_string(params_json, "lane");
    if (!lane.empty())
        cmd += " --lane " + shell_quote(lane);
    cmd += " --json 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_inspect(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit inspect " +
                      shell_quote(path) + " --json 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_plan(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit plan " +
                      shell_quote(path) + " --project " + shell_quote(root.string()) +
                      " --json 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_verify(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit verify " +
                      shell_quote(path) + " --project " + shell_quote(root.string()) +
                      " --json 2>&1";
    if (extract_bool(params_json, "execute_screenshots", false)) {
        cmd = shell_quote(resolve_cli_binary(root).string()) + " kit verify " + shell_quote(path) +
              " --project " + shell_quote(root.string()) + " --json --execute-screenshots";
        auto backend = extract_string(params_json, "screenshot_backend");
        if (!backend.empty())
            cmd += " --screenshot-backend " + shell_quote(backend);
        auto output_dir = extract_string(params_json, "screenshot_output_dir");
        if (!output_dir.empty())
            cmd += " --screenshot-output-dir " + shell_quote(output_dir);
        cmd += " 2>&1";
    }

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_apply(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after "
               "reviewing the kit plan\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit apply " +
                      shell_quote(path) + " --project " + shell_quote(root.string()) +
                      " --yes 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_remove(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto id = extract_string(params_json, "id");
    if (id.empty()) {
        id = extract_string(params_json, "kit_id");
    }
    if (id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: id is required\"}]}";
    }
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after "
               "reviewing installed kit ownership\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit remove " +
                      shell_quote(id) + " --project " + shell_quote(root.string()) + " --yes 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_pack(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit pack " +
                      shell_quote(path) + " --json";
    auto output = extract_string(params_json, "output");
    if (!output.empty())
        cmd += " --output " + shell_quote(output);
    cmd += " 2>&1";

    auto result = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(result) + "}]}";
}

std::string handle_kit_publish_check(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit publish " +
                      shell_quote(path) + " --dry-run --json";
    auto registry_manifest = extract_string(params_json, "registry_manifest");
    if (registry_manifest.empty())
        registry_manifest = extract_string(params_json, "registryManifest");
    if (!registry_manifest.empty())
        cmd += " --registry-manifest " + shell_quote(registry_manifest);
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_init(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto kind = extract_string(params_json, "kind");
    auto id = extract_string(params_json, "id");
    if (kind.empty() || id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: kind and id are required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit init --kind " +
                      shell_quote(kind) + " --id " + shell_quote(id);
    auto name = extract_string(params_json, "name");
    if (!name.empty())
        cmd += " --name " + shell_quote(name);
    auto dir = extract_string(params_json, "dir");
    if (!dir.empty())
        cmd += " --dir " + shell_quote(dir);
    if (extract_bool(params_json, "force", false))
        cmd += " --force";
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content(const std::string& params_json) {
    auto subcommand = extract_string(params_json, "subcommand");
    if (subcommand.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: subcommand is required\"}]}";
    }
    if (subcommand == "validate")
        return handle_content_validate(params_json);
    if (subcommand == "preview")
        return handle_content_preview(params_json);
    if (subcommand == "install")
        return handle_content_install(params_json);
    if (subcommand == "update")
        return handle_content_update(params_json);
    if (subcommand == "list")
        return handle_content_list(params_json);
    if (subcommand == "rescan")
        return handle_content_rescan(params_json);
    if (subcommand == "remove" || subcommand == "uninstall")
        return handle_content_remove(params_json);
    if (subcommand == "reveal")
        return handle_content_reveal(params_json);
    return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: unsupported content subcommand\"}]}";
}

std::string handle_content_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content validate " +
                      shell_quote(path) + " --json 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_preview(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    auto plugin_runtime = extract_string(params_json, "plugin_runtime");
    if (plugin_runtime.empty())
        plugin_runtime = extract_string(params_json, "pluginRuntime");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }
    if (plugin_runtime.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin_runtime is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content preview " +
                      shell_quote(path) + " --plugin-runtime " + shell_quote(plugin_runtime) +
                      " --json";
    auto plugin = extract_string(params_json, "plugin");
    if (!plugin.empty())
        cmd += " --plugin " + shell_quote(plugin);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_install(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    auto plugin = extract_string(params_json, "plugin");
    if (path.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    if (plugin.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after "
               "reviewing the content install target\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content install " +
                      shell_quote(path) + " --plugin " + shell_quote(plugin) + " --yes";
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty())
        cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_update(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    auto plugin = extract_string(params_json, "plugin");
    if (path.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    if (plugin.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after "
               "reviewing the content update target\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content update " +
                      shell_quote(path) + " --plugin " + shell_quote(plugin) + " --yes";
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty())
        cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_list(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content list --json";
    auto plugin = extract_string(params_json, "plugin");
    if (!plugin.empty())
        cmd += " --plugin " + shell_quote(plugin);
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty())
        cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_rescan(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content rescan --json";
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty())
        cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_remove(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto id = extract_string(params_json, "id");
    if (id.empty())
        id = extract_string(params_json, "package_id");
    auto plugin = extract_string(params_json, "plugin");
    if (id.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: id is required\"}]}";
    if (plugin.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after "
               "reviewing installed content\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content remove " +
                      shell_quote(id) + " --plugin " + shell_quote(plugin) + " --yes";
    auto version = extract_string(params_json, "version");
    if (!version.empty())
        cmd += " --version " + shell_quote(version);
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty())
        cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_reveal(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto id = extract_string(params_json, "id");
    if (id.empty())
        id = extract_string(params_json, "package_id");
    auto plugin = extract_string(params_json, "plugin");
    if (id.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: id is required\"}]}";
    if (plugin.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content reveal " +
                      shell_quote(id) + " --plugin " + shell_quote(plugin);
    auto version = extract_string(params_json, "version");
    if (!version.empty())
        cmd += " --version " + shell_quote(version);
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty())
        cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

} // namespace pulp_mcp
