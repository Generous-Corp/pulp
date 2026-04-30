// test_cli_tool_registry.cpp - deterministic tests for
// `tools/cli/tool_registry.cpp`.
//
// Coverage tranche for issue #643. These tests stay on local registry,
// discovery, uninstall, and command dispatch paths. They intentionally avoid
// network downloads, archive extraction, Python package installs, and running
// external tools from the registry.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/tool_registry.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::cli::tools;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        path = base / ("pulp-cli-tool-registry-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedEnv {
    std::string key;
    bool had_value = false;
    std::string old_value;

    ScopedEnv(std::string k, const fs::path& value) : key(std::move(k)) {
        if (const char* existing = std::getenv(key.c_str())) {
            had_value = true;
            old_value = existing;
        }
        set(value.string());
    }

    ~ScopedEnv() {
#ifdef _WIN32
        _putenv_s(key.c_str(), had_value ? old_value.c_str() : "");
#else
        if (had_value)
            setenv(key.c_str(), old_value.c_str(), 1);
        else
            unsetenv(key.c_str());
#endif
    }

    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }
};

struct ScopedCurrentPath {
    fs::path old_path = fs::current_path();

    explicit ScopedCurrentPath(const fs::path& path) {
        fs::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        fs::current_path(old_path, ec);
    }
};

struct ScopedOutput {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());

    ~ScopedOutput() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    }
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << body;
}

fs::path touch_file(const fs::path& path) {
    write_file(path, "fixture\n");
    return path;
}

std::string system_shell_tool_id() {
#ifdef _WIN32
    return "cmd.exe";
#else
    return "sh";
#endif
}

std::string quote_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

fs::path write_registry_fixture(const fs::path& root) {
    const auto platform = current_platform_key();
    auto path = root / "tool-registry.json";
    write_file(path, std::string(R"({
  "schema_version": 3,
  "tools": {
    "fixture-bin": {
      "display_name": "Fixture Binary",
      "category": "binary",
      "description": "Local binary fixture",
      "license": "MIT",
      "install_method": "binary_download",
      "pinned_version": "1.2.3",
      "requires_tools": ["uv"],
      "managed_by_pulp": false,
      "bundleable": true,
      "binary_sources": {
        ")") + quote_json(platform) + R"(": {
          "url_template": "https://example.invalid/fixture-${version}.tar.gz",
          "archive_format": "tar.gz",
          "binary_name": "fixture-bin"
        }
      }
    },
    "fixture-py": {
      "display_name": "Fixture Python",
      "category": "python_tool",
      "description": "Local Python fixture",
      "license": "Apache-2.0",
      "install_method": "python_pip",
      "pip_package": "fixture_tool",
      "pinned_version": "4.5.6",
      "requires_tools": ["uv"]
    }
  }
}
)");
    return path;
}

ToolDescriptor binary_tool(std::string id, std::string binary_name = {}) {
    ToolDescriptor tool;
    tool.id = std::move(id);
    tool.display_name = tool.id;
    tool.install_method = "binary_download";
    tool.pinned_version = "1.2.3";
    if (binary_name.empty()) binary_name = tool.id;
    tool.binary_sources[current_platform_key()] = {
        "https://example.invalid/" + tool.id + "-${version}.tar.gz",
        "tar.gz",
        std::move(binary_name),
    };
    return tool;
}

fs::path managed_binary_path(const fs::path& home,
                             const std::string& id,
                             const std::string& version,
                             const std::string& binary_name) {
    return home / "tools" / id / version / binary_name;
}

}  // namespace

TEST_CASE("tool registry parses descriptors and reports malformed roots",
          "[cli][tool-registry][issue-643]") {
    TempDir tmp;
    auto registry_path = write_registry_fixture(tmp.path);

    auto loaded = load_tool_registry(registry_path);
    REQUIRE(loaded.error.empty());
    REQUIRE(loaded.registry.schema_version == 3);
    REQUIRE(loaded.registry.tools.size() == 2);

    const auto& bin = loaded.registry.tools.at("fixture-bin");
    REQUIRE(bin.id == "fixture-bin");
    REQUIRE(bin.display_name == "Fixture Binary");
    REQUIRE(bin.category == "binary");
    REQUIRE(bin.description == "Local binary fixture");
    REQUIRE(bin.license == "MIT");
    REQUIRE(bin.install_method == "binary_download");
    REQUIRE(bin.pinned_version == "1.2.3");
    REQUIRE(bin.requires_tools == std::vector<std::string>{"uv"});
    REQUIRE_FALSE(bin.managed_by_pulp);
    REQUIRE(bin.bundleable);
    REQUIRE(bin.binary_sources.at(current_platform_key()).archive_format == "tar.gz");
    REQUIRE(bin.binary_sources.at(current_platform_key()).binary_name == "fixture-bin");

    const auto& py = loaded.registry.tools.at("fixture-py");
    REQUIRE(py.install_method == "python_pip");
    REQUIRE(py.pip_package == "fixture_tool");
    REQUIRE(py.requires_tools == std::vector<std::string>{"uv"});
    REQUIRE(py.managed_by_pulp);
    REQUIRE_FALSE(py.bundleable);

    auto missing = load_tool_registry(tmp.path / "missing.json");
    REQUIRE(missing.registry.tools.empty());
    REQUIRE(missing.error.find("Cannot read tool registry") != std::string::npos);

    write_file(tmp.path / "array.json", "[]");
    auto malformed = load_tool_registry(tmp.path / "array.json");
    REQUIRE(malformed.registry.tools.empty());
    REQUIRE(malformed.error == "Tool registry is not a valid JSON object");
}

TEST_CASE("tool registry accepts empty and partial descriptor shapes",
          "[cli][tool-registry][issue-643]") {
    TempDir tmp;

    write_file(tmp.path / "empty-tools.json", R"({
  "schema_version": 4
}
)");
    auto empty_tools = load_tool_registry(tmp.path / "empty-tools.json");
    REQUIRE(empty_tools.error.empty());
    REQUIRE(empty_tools.registry.schema_version == 4);
    REQUIRE(empty_tools.registry.tools.empty());

    write_file(tmp.path / "array-tools.json", R"({
  "schema_version": 5,
  "tools": []
}
)");
    auto array_tools = load_tool_registry(tmp.path / "array-tools.json");
    REQUIRE(array_tools.error.empty());
    REQUIRE(array_tools.registry.schema_version == 5);
    REQUIRE(array_tools.registry.tools.empty());

    write_file(tmp.path / "partial.json", std::string(R"({
  "tools": {
    "partial-tool": {
      "binary_sources": {
        ")") + quote_json(current_platform_key()) + R"(": {}
      }
    }
  }
}
)");
    auto partial = load_tool_registry(tmp.path / "partial.json");
    REQUIRE(partial.error.empty());
    REQUIRE(partial.registry.tools.size() == 1);
    const auto& tool = partial.registry.tools.at("partial-tool");
    REQUIRE(tool.id == "partial-tool");
    REQUIRE(tool.display_name.empty());
    REQUIRE(tool.managed_by_pulp);
    REQUIRE_FALSE(tool.bundleable);
    REQUIRE(tool.binary_sources.count(current_platform_key()) == 1);
    REQUIRE(tool.binary_sources.at(current_platform_key()).binary_name.empty());
}

TEST_CASE("tool lookup prefers pulp-managed binaries and python wrappers",
          "[cli][tool-registry][locate][issue-643]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", tmp.path / "home"};

    auto managed = binary_tool("managed-fixture", "managed-bin");
    auto managed_path = managed_binary_path(pulp_home(), managed.id, "1.2.3", "managed-bin");
    touch_file(managed_path);

    auto located = locate_tool(managed);
    REQUIRE(located.found);
    REQUIRE(located.source == "pulp-managed");
    REQUIRE(located.path == managed_path);

    auto default_name = binary_tool("default-name-fixture");
    auto default_path = managed_binary_path(pulp_home(), default_name.id, "1.2.3", default_name.id);
    touch_file(default_path);
    auto located_default = locate_tool(default_name);
    REQUIRE(located_default.found);
    REQUIRE(located_default.path == default_path);

    ToolDescriptor py;
    py.id = "python-fixture";
    py.install_method = "python_pip";
#ifdef _WIN32
    auto wrapper = pulp_home() / "tools" / "python-envs" / py.id / "run.bat";
#else
    auto wrapper = pulp_home() / "tools" / "python-envs" / py.id / "run.sh";
#endif
    touch_file(wrapper);

    auto located_py = locate_tool(py);
    REQUIRE(located_py.found);
    REQUIRE(located_py.source == "pulp-managed");
    REQUIRE(located_py.path == wrapper);

    auto missing = binary_tool("pulp-tool-registry-missing-binary-xyz");
    auto located_missing = locate_tool(missing);
    REQUIRE_FALSE(located_missing.found);
    REQUIRE(located_missing.source == "not-found");

    ToolDescriptor system_shell;
    system_shell.id = system_shell_tool_id();
    auto located_system = locate_tool(system_shell);
    REQUIRE(located_system.found);
    REQUIRE(located_system.source == "system-path");
    REQUIRE_FALSE(located_system.path.empty());
}

TEST_CASE("tool install helpers have deterministic local exits",
          "[cli][tool-registry][install][issue-643]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", tmp.path / "home"};

    auto existing = binary_tool("cached-tool", "cached-bin");
    auto cached_path = managed_binary_path(pulp_home(), existing.id, existing.pinned_version, "cached-bin");
    touch_file(cached_path);
    write_file(cached_path.parent_path() / "manifest.json",
               "{\"version\":\"1.2.3\",\"tool_id\":\"cached-tool\"}\n");

    auto cached = install_binary_tool(existing, /*force=*/false);
    REQUIRE(cached.ok);
    REQUIRE(cached.binary_path == cached_path);
    REQUIRE(cached.installed_version == existing.pinned_version);

    ToolDescriptor cached_without_manifest = binary_tool("cached-no-manifest");
    auto no_manifest_path = managed_binary_path(pulp_home(),
                                                cached_without_manifest.id,
                                                cached_without_manifest.pinned_version,
                                                cached_without_manifest.id);
    touch_file(no_manifest_path);
    auto no_manifest = install_binary_tool(cached_without_manifest, /*force=*/false);
    REQUIRE(no_manifest.ok);
    REQUIRE(no_manifest.binary_path == no_manifest_path);

    ToolDescriptor stale_manifest;
    stale_manifest.id = "stale-manifest-tool";
    stale_manifest.display_name = "Stale Manifest Tool";
    stale_manifest.install_method = "binary_download";
    stale_manifest.pinned_version = "2.0.0";
    auto stale_path = managed_binary_path(pulp_home(), stale_manifest.id,
                                          stale_manifest.pinned_version,
                                          stale_manifest.id);
    touch_file(stale_path);
    write_file(stale_path.parent_path() / "manifest.json",
               "{\"version\":\"1.0.0\",\"tool_id\":\"stale-manifest-tool\"}\n");
    auto stale = install_binary_tool(stale_manifest, /*force=*/false);
    REQUIRE_FALSE(stale.ok);
    REQUIRE(stale.error.find(std::string("is not available for ") + current_platform_key()) !=
            std::string::npos);

    ToolDescriptor unavailable;
    unavailable.id = "unavailable-tool";
    unavailable.display_name = "Unavailable Tool";
    unavailable.install_method = "binary_download";
    unavailable.pinned_version = "9.9.9";
    auto unavailable_result = install_binary_tool(unavailable, /*force=*/true);
    REQUIRE_FALSE(unavailable_result.ok);
    REQUIRE(unavailable_result.error.find(std::string("is not available for ") +
                                          current_platform_key()) !=
            std::string::npos);

    ToolRegistry missing_uv;
    ToolDescriptor py;
    py.id = "py-tool";
    py.display_name = "Python Tool";
    py.install_method = "python_pip";
    py.pip_package = "py_tool";
    py.pinned_version = "1.0.0";
    auto no_uv = install_python_tool(py, missing_uv, /*force=*/false);
    REQUIRE_FALSE(no_uv.ok);
    REQUIRE(no_uv.error == "UV not found in tool registry");

    ToolRegistry with_uv;
    auto uv = binary_tool("uv", "uv-bin");
    with_uv.tools["uv"] = uv;
    touch_file(managed_binary_path(pulp_home(), "uv", "1.2.3", "uv-bin"));

    auto venv_dir = pulp_home() / "tools" / "python-envs" / py.id;
    fs::create_directories(venv_dir / ".venv");
#ifdef _WIN32
    auto wrapper = venv_dir / "run.bat";
#else
    auto wrapper = venv_dir / "run.sh";
#endif
    touch_file(wrapper);

    auto existing_py = install_python_tool(py, with_uv, /*force=*/false);
    REQUIRE(existing_py.ok);
    REQUIRE(existing_py.binary_path == wrapper);
    REQUIRE(existing_py.installed_version == py.pinned_version);

    ToolRegistry uv_unavailable;
    ToolDescriptor uv_without_source;
    uv_without_source.id = "uv";
    uv_without_source.display_name = "UV";
    uv_without_source.install_method = "binary_download";
    uv_without_source.pinned_version = "9.9.9";
    uv_unavailable.tools["uv"] = uv_without_source;
    {
        ScopedEnv path{"PATH", tmp.path / "empty-path"};
        ScopedOutput output;
        auto uv_bootstrap_failed = install_python_tool(py, uv_unavailable, /*force=*/false);
        REQUIRE_FALSE(uv_bootstrap_failed.ok);
        REQUIRE(output.out.str().find("Installing UV") != std::string::npos);
        REQUIRE(uv_bootstrap_failed.error.find("Failed to install UV: UV is not available for ") ==
                0);
    }

    fs::remove(wrapper);
    auto missing_wrapper = install_python_tool(py, with_uv, /*force=*/false);
    REQUIRE_FALSE(missing_wrapper.ok);
    REQUIRE(missing_wrapper.binary_path == wrapper);
    REQUIRE(missing_wrapper.installed_version == py.pinned_version);
}

TEST_CASE("tool uninstall removes managed binary and python tool roots",
          "[cli][tool-registry][uninstall][issue-643]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", tmp.path / "home"};

    auto binary_root = pulp_home() / "tools" / "binary-tool";
    touch_file(binary_root / "1.0.0" / "binary-tool");
    REQUIRE(uninstall_tool("binary-tool"));
    REQUIRE_FALSE(fs::exists(binary_root));

    auto py_root = pulp_home() / "tools" / "python-envs" / "py-tool";
    touch_file(py_root / "run.sh");
    REQUIRE(uninstall_tool("py-tool"));
    REQUIRE_FALSE(fs::exists(py_root));

    REQUIRE_FALSE(uninstall_tool("missing-tool"));
}

TEST_CASE("tool command handles local list path doctor and error branches",
          "[cli][tool-registry][command][issue-643]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", tmp.path / "home"};
    fs::create_directories(tmp.path / "repo" / "tools" / "packages");
    ScopedCurrentPath cwd{tmp.path / "repo"};

    const auto platform = current_platform_key();
    write_file(tmp.path / "repo" / "tools" / "packages" / "tool-registry.json",
               std::string(R"({
  "schema_version": 1,
  "tools": {
    "managed-cmd": {
      "display_name": "Managed Command",
      "description": "Already installed fixture",
      "install_method": "binary_download",
      "pinned_version": "1.0.0",
      "binary_sources": {
        ")") + quote_json(platform) + R"(": {
          "url_template": "https://example.invalid/managed-${version}.zip",
          "archive_format": "zip",
          "binary_name": "managed-cmd-bin"
        }
      }
    },
    "unavailable-cmd": {
      "display_name": "Unavailable Command",
      "description": "No source fixture",
      "install_method": "binary_download",
      "pinned_version": "1.0.0"
    }
  }
}
)");
    auto managed_path = managed_binary_path(pulp_home(), "managed-cmd", "1.0.0",
                                            "managed-cmd-bin");
    touch_file(managed_path);

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({}) == 0);
        REQUIRE(output.out.str().find("Usage: pulp tool") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"list"}) == 0);
        REQUIRE(output.out.str().find("managed-cmd") != std::string::npos);
        REQUIRE(output.out.str().find("unavailable-cmd") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"path", "managed-cmd"}) == 0);
        REQUIRE(output.out.str().find(managed_path.string()) != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"path", "unavailable-cmd"}) == 1);
        REQUIRE(output.err.str().find("unavailable-cmd not installed") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"path", "missing-cmd"}) == 1);
        REQUIRE(output.err.str().find("Tool 'missing-cmd' not found") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"doctor"}) == 1);
        REQUIRE(output.out.str().find("Managed Command") != std::string::npos);
        REQUIRE(output.out.str().find(std::string("not available for ") + platform) !=
                std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"install"}) == 1);
        REQUIRE(output.err.str().find("Usage: pulp tool install") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"install", "missing-cmd"}) == 1);
        REQUIRE(output.err.str().find("Tool 'missing-cmd' not found") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"run"}) == 1);
        REQUIRE(output.err.str().find("Usage: pulp tool run") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"run", "missing-cmd"}) == 1);
        REQUIRE(output.err.str().find("Tool 'missing-cmd' not found") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"run", "unavailable-cmd"}) == 1);
        REQUIRE(output.err.str().find("unavailable-cmd not installed") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"uninstall", "managed-cmd"}) == 0);
        REQUIRE(output.out.str().find("Uninstalled managed-cmd") != std::string::npos);
    }
    REQUIRE_FALSE(fs::exists(pulp_home() / "tools" / "managed-cmd"));

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"uninstall", "managed-cmd"}) == 1);
        REQUIRE(output.err.str().find("managed-cmd is not installed") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"unknown"}) == 1);
        REQUIRE(output.err.str().find("Unknown tool subcommand: unknown") != std::string::npos);
    }
}

TEST_CASE("tool command reports and runs system path tools",
          "[cli][tool-registry][command][issue-643]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", tmp.path / "home"};
    fs::create_directories(tmp.path / "repo" / "tools" / "packages");
    ScopedCurrentPath cwd{tmp.path / "repo"};

    const auto shell_id = system_shell_tool_id();
    write_file(tmp.path / "repo" / "tools" / "packages" / "tool-registry.json",
               std::string(R"({
  "schema_version": 1,
  "tools": {
    ")") + quote_json(shell_id) + R"(": {
      "display_name": "System Shell",
      "description": "Known shell on PATH",
      "install_method": "binary_download",
      "pinned_version": "1.0.0"
    }
  }
}
)");

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"list"}) == 0);
        REQUIRE(output.out.str().find("system (") != std::string::npos);
        REQUIRE(output.out.str().find(shell_id) != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"path", shell_id}) == 0);
        REQUIRE(output.out.str().find(shell_id) != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"doctor"}) == 0);
        REQUIRE(output.out.str().find("System Shell") != std::string::npos);
        REQUIRE(output.out.str().find("system-path") != std::string::npos);
    }

    {
        ScopedOutput output;
#ifdef _WIN32
        REQUIRE(cmd_tool({"run", shell_id, "/c",
                          "echo tool-system-out&& echo tool-system-err 1>&2&& exit /b 7"}) == 7);
#else
        REQUIRE(cmd_tool({"run", shell_id, "-c",
                          "printf tool-system-out; printf tool-system-err >&2; exit 7"}) == 7);
#endif
        REQUIRE(output.out.str().find("tool-system-out") != std::string::npos);
        REQUIRE(output.err.str().find("tool-system-err") != std::string::npos);
    }
}

TEST_CASE("tool command reports registry and argument failures deterministically",
          "[cli][tool-registry][command][issue-643]") {
    TempDir tmp;
    ScopedEnv home{"PULP_HOME", tmp.path / "home"};
    auto repo = tmp.path / "repo";
    fs::create_directories(repo / "subdir");
    ScopedCurrentPath cwd{repo / "subdir"};

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"list"}) == 1);
        REQUIRE(output.err.str().find("Tool registry not found") != std::string::npos);
    }

    fs::create_directories(repo / "tools" / "packages");
    write_file(repo / "tools" / "packages" / "tool-registry.json", "[]");
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"list"}) == 1);
        REQUIRE(output.err.str().find("Tool registry is not a valid JSON object") !=
                std::string::npos);
    }

    const auto platform = quote_json(current_platform_key());
    write_file(repo / "tools" / "packages" / "tool-registry.json",
               std::string(R"({
  "schema_version": 1,
  "tools": {
    "needs-unavailable-dep": {
      "display_name": "Needs Unavailable Dependency",
      "description": "Dependency failure fixture",
      "install_method": "binary_download",
      "pinned_version": "1.0.0",
      "requires_tools": ["unavailable-tool"],
      "binary_sources": {
        ")" + platform + R"(": {
          "url_template": "https://example.invalid/needs-dep-${version}.tar.gz",
          "archive_format": "tar.gz",
          "binary_name": "needs-unavailable-dep"
        }
      }
    },
    "available-tool-fixture": {
      "display_name": "Available Tool",
      "description": "Available but not installed",
      "install_method": "binary_download",
      "pinned_version": "1.0.0",
      "binary_sources": {
        ")") + platform + R"(": {
          "url_template": "https://example.invalid/available-${version}.tar.gz",
          "archive_format": "tar.gz",
          "binary_name": "available-tool-fixture"
        }
      }
    },
    "manual-tool": {
      "display_name": "Manual Tool",
      "description": "Unsupported install method fixture",
      "install_method": "manual",
      "pinned_version": "1.0.0"
    },
    "unavailable-tool": {
      "display_name": "Unavailable Tool",
      "description": "No source fixture",
      "install_method": "binary_download",
      "pinned_version": "1.0.0"
    }
  }
}
)");

    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"path"}) == 1);
        REQUIRE(output.err.str().find("Usage: pulp tool path") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"uninstall"}) == 1);
        REQUIRE(output.err.str().find("Usage: pulp tool uninstall") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"doctor"}) == 1);
        REQUIRE(output.out.str().find("Available Tool") != std::string::npos);
        REQUIRE(output.out.str().find("not installed") != std::string::npos);
        REQUIRE(output.out.str().find("Unavailable Tool") != std::string::npos);
        REQUIRE(output.out.str().find(std::string("not available for ") + current_platform_key()) !=
                std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"install", "manual-tool"}) == 1);
        REQUIRE(output.err.str().find("Unknown install method: manual") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"install", "unavailable-tool"}) == 1);
        REQUIRE(output.err.str().find(std::string("is not available for ") +
                                      current_platform_key()) != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"install", "needs-unavailable-dep"}) == 1);
        REQUIRE(output.err.str().find("Failed to install dependency unavailable-tool") !=
                std::string::npos);
    }
    auto available_path = managed_binary_path(pulp_home(), "available-tool-fixture", "1.0.0",
                                              "available-tool-fixture");
    touch_file(available_path);
    write_file(available_path.parent_path() / "manifest.json",
               "{\"version\":\"1.0.0\",\"tool_id\":\"available-tool-fixture\"}\n");
    {
        ScopedOutput output;
        REQUIRE(cmd_tool({"install", "--all"}) == 1);
        REQUIRE(output.err.str().find("Unknown install method: manual") != std::string::npos);
    }
}
