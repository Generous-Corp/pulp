#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/authority_navigation.hpp"
#include "../tools/cli/cli_common.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        path = fs::temp_directory_path() /
               ("pulp-cli-docs-command-test-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(n));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
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

#if !defined(_WIN32)
struct ScopedEnv {
    std::string name;
    bool had_value = false;
    std::string old_value;

    ScopedEnv(const char* n, const std::string& value) : name(n) {
        if (const char* value = std::getenv(n)) {
            had_value = true;
            old_value = value;
        }
        set(value);
    }

    ~ScopedEnv() {
        if (had_value) {
            (void)::setenv(name.c_str(), old_value.c_str(), 1);
        } else {
            (void)::unsetenv(name.c_str());
        }
    }

    void set(const std::string& value) {
        REQUIRE(::setenv(name.c_str(), value.c_str(), 1) == 0);
    }
};
#endif

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    REQUIRE(f.good());
    f << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    REQUIRE(position != std::string::npos);
    text.replace(position, from.size(), to);
}

std::string slash_normalized(std::string text) {
    for (char& c : text) {
        if (c == '\\')
            c = '/';
    }
    return text;
}

fs::path make_project(TempDir& tmp) {
    auto root = tmp.path / "repo";
    fs::create_directories(root / "core");
    write_file(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");

    write_file(root / "docs" / "status" / "docs-index.yaml", R"YAML(
- slug: getting-started
  path: guides/getting-started.md
  kind: guide
- slug: cli-reference
  path: reference/cli.md
  kind: reference
)YAML");

    write_file(root / "docs" / "guides" / "getting-started.md", R"MD(# Getting Started

Pulp plugins can render audio controls.
This document mentions a rare synthesizer workflow.
This document mentions a rare synthesizer workflow again.
This document mentions a rare synthesizer workflow for the third time.
This document mentions a rare synthesizer workflow for the fourth time.
This document mentions a rare synthesizer workflow for the fifth time.
This document mentions a rare synthesizer workflow for the sixth time.
)MD");
    write_file(root / "docs" / "reference" / "cli.md",
               "# CLI Reference\n\nUse pulp docs show command.\n");

    write_file(root / "docs" / "status" / "support-matrix.yaml", R"YAML(
platforms:
  macos:
    status: supported
    notes: CoreAudio path
formats:
  vst3:
    status: supported
audio_io:
  default_output: supported
)YAML");

    write_file(root / "docs" / "status" / "cli-commands.yaml", R"YAML(
commands:
  - name: audio
    status: supported
    summary: Audio model tooling
    docs: reference/cli.md
    args:
      - name: --json
        required: false
        description: Emit JSON
        kind: flag
    subcommands:
      - name: model
        summary: Manage models
        args:
          - name: list
            summary: List models
  - name: docs
    status: supported
    summary: Browse local docs
)YAML");

    write_file(root / "docs" / "status" / "cmake-functions.yaml", R"YAML(
- name: pulp_add_plugin
  status: supported
  summary: Register a plugin target
  arguments:
    - NAME
- name: pulp_app_icon
  status: usable
  summary: Attach app icon assets
  arguments:
    - target
    - SOURCE
  docs: reference/cmake.md#pulp_app_icon
)YAML");

    write_file(root / "docs" / "status" / "style-rules.yaml", R"YAML(
- id: public-history
  rule: Keep commits and filenames public-ready
  severity: error
)YAML");

    return root;
}

void write_authority_registry(const fs::path& root, const std::string& second_id = "offline-cli") {
    write_file(root / "CLAUDE.md", "# source authority fixture\n");
    write_file(root / "tools/cli/pulp_cli.cpp", "// source authority fixture\n");
    write_file(root / "docs/status/cli-commands.yaml", "commands: []\n");
    for (const auto* path :
         {"docs/status/agent.json", "docs/status/dsp.json", "docs/status/forge.json",
          "inspect/control.hpp", "docs/status/cli.yaml", "tools/mcp.cpp",
          "docs/status/sequencer.json", "core/timeline/schema.json"})
        write_file(root / path, "{}\n");
    fs::create_directories(root / "planning");
    write_file(
        root / "docs/status/authority-navigation.schema.json",
        read_file(fs::path(PULP_SOURCE_DIR) / "docs/status/authority-navigation.schema.json"));
    const auto row = [](const std::string& id, const std::string& alias, const std::string& plane,
                        const std::string& source, const std::string& installed,
                        const std::string& source_query, const std::string& installed_query,
                        const std::string& absence) {
        const auto owner = id == "dsp-survey-admission"
                               ? "danielraffel/pulp-planning:dsp-survey-claims/WORKER-PROMPT.md"
                               : "owner";
        return "{\"id\":\"" + id + "\",\"aliases\":[\"" + alias + "\"],\"plane\":\"" + plane +
               "\",\"native_owner\":\"" + owner + "\",\"source_location\":\"" + source +
               "\",\"installed_location\":" + installed + ",\"query_or_validator\":{\"source\":\"" +
               source_query + "\",\"installed\":" + installed_query +
               "},\"coverage_semantics\":\"covered\",\"absence_semantics\":\"" + absence +
               "\",\"does_not_prove\":[\"everything\"]}";
    };
    std::vector<std::string> rows = {
        row("agent-capabilities", "agent-api", "design_time_static", "docs/status/agent.json",
            "\"share/pulp/agent.json\"", "agent source query", "\"agent installed query\"",
            "unknown"),
        row("dsp-capabilities", "dsp", "dsp_static", "docs/status/dsp.json",
            "\"share/pulp/dsp.json\"", "dsp source query", "\"dsp installed query\"",
            "not_present_in_covered_forge_bake_catalog_headers"),
        row("dsp-survey-admission", "dsp-survey", "release_evidence", "planning", "null",
            "read dsp-survey-claims/WORKER-PROMPT.md from current danielraffel/pulp-planning main",
            "null", "unknown"),
        row("forge-catalog", "forge", "dsp_static", "docs/status/forge.json",
            "\"share/pulp/forge.json\"", "forge source query", "\"forge installed query\"",
            "not_present_in_joined_forge_catalog"),
        row("live-control", "control", "live_control", "inspect/control.hpp", "null",
            "pulp control status --instance <caller-supplied-exact-instance-id> --json",
            "\"pulp control status --instance <caller-supplied-exact-instance-id> --json\"",
            "requires_exact_live_instance"),
        row(second_id, "cli", "offline_command", "docs/status/cli.yaml", "null", "cli source query",
            "null", "not_registered_as_a_top_level_cli_command"),
        row("offline-mcp", "mcp", "offline_command", "tools/mcp.cpp", "\"bin/pulp-mcp\"",
            "mcp source query", "\"mcp installed query\"",
            "not_advertised_by_the_queried_mcp_server"),
        row("sequencer-exposure", "sequencer", "release_evidence", "docs/status/sequencer.json",
            "null", "sequencer source query", "null", "unknown_outside_reverified_rows"),
        row("timeline-schema", "timeline", "timeline_static", "core/timeline/schema.json", "null",
            "timeline source query", "null", "not_registered_in_builtin_timeline_schema_registry"),
    };
    std::ostringstream registry;
    registry << R"JSON({
  "$schema": "authority-navigation.schema.json",
  "schema": "pulp.authority-navigation.v1",
  "registry_revision": 1,
  "authorities": [
)JSON";
    for (std::size_t index = 0; index < rows.size(); ++index)
        registry << (index ? ",\n" : "") << rows[index];
    registry << R"JSON(
  ]
}
)JSON";
    write_file(root / "docs/status/authority-navigation.json", registry.str());
}

} // namespace

TEST_CASE("docs command reports usage outside and inside projects", "[cli][docs]") {
    TempDir tmp;
    {
        ScopedCurrentPath cwd{tmp.path};
        ScopedOutput output;
        REQUIRE(cmd_docs({}) == 1);
        REQUIRE(output.err.str().find("not in a Pulp project") != std::string::npos);
    }

    auto root = make_project(tmp);
    ScopedCurrentPath cwd{root};
    ScopedOutput output;
    REQUIRE(cmd_docs({}) == 0);
    REQUIRE(output.out.str().find("pulp docs") != std::string::npos);

    SECTION("authority registry resolves from source and installed contexts") {
        TempDir authority_tmp;
        auto source = authority_tmp.path / "source";
        fs::create_directories(source / "core");
        write_file(source / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");
        write_authority_registry(source);

        auto source_resolution = pulp::cli::authority::resolve_registry(
            source / "docs", authority_tmp.path / "unrelated/pulp-cpp");
        REQUIRE(source_resolution);
        REQUIRE(source_resolution->context == pulp::cli::authority::Context::source);
        auto source_registry = pulp::cli::authority::load_registry(*source_resolution);
        REQUIRE(source_registry.registry);
        REQUIRE(pulp::cli::authority::find(*source_registry.registry, "agent-api") != nullptr);
        REQUIRE(pulp::cli::authority::render_query(*source_registry.registry, "agent-api", true)
                    ->find("\"matched_token\":\"agent-api\"") != std::string::npos);
        const auto source_list =
            pulp::cli::authority::render_list(*source_registry.registry, false);
        REQUIRE(source_list.find("Authority routes (source)") != std::string::npos);
        REQUIRE(source_list.find("agent-capabilities [design_time_static]") != std::string::npos);
        const auto source_query =
            pulp::cli::authority::render_query(*source_registry.registry, "agent-api", false);
        REQUIRE(source_query);
        REQUIRE(source_query->find("Authority: agent-capabilities") != std::string::npos);
        REQUIRE(source_query->find("Matched token: agent-api") != std::string::npos);
        REQUIRE(source_query->find("Context artifact:") != std::string::npos);
        REQUIRE(source_query->find("Query or validate: agent source query") != std::string::npos);
        REQUIRE(source_query->find("Does not prove:") != std::string::npos);
        REQUIRE_FALSE(pulp::cli::authority::render_query(*source_registry.registry,
                                                         "not-an-authority", false));

        auto prefix = authority_tmp.path / "sdk";
        write_file(prefix / "share/pulp/agent.json", "{}\n");
        write_file(prefix / "share/pulp/authority-navigation.schema.json",
                   read_file(source / "docs/status/authority-navigation.schema.json"));
        write_file(prefix / "share/pulp/authority-navigation.json",
                   read_file(source / "docs/status/authority-navigation.json"));
        auto installed_resolution = pulp::cli::authority::resolve_registry(
            authority_tmp.path / "outside", prefix / "bin/pulp-cpp");
        REQUIRE(installed_resolution);
        REQUIRE(installed_resolution->context == pulp::cli::authority::Context::installed);
        auto installed_registry = pulp::cli::authority::load_registry(*installed_resolution);
        REQUIRE(installed_registry.registry);
        auto installed_json = pulp::cli::authority::render_query(*installed_registry.registry,
                                                                 "agent-capabilities", true);
        REQUIRE(installed_json);
        REQUIRE(installed_json->find("\"registry_context\":\"installed\"") != std::string::npos);
        REQUIRE(installed_json->find("agent installed query") != std::string::npos);
        const auto installed_list =
            pulp::cli::authority::render_list(*installed_registry.registry, false);
        REQUIRE(installed_list.find("Authority routes (installed)") != std::string::npos);
        const auto installed_query = pulp::cli::authority::render_query(
            *installed_registry.registry, "agent-capabilities", false);
        REQUIRE(installed_query);
        REQUIRE(installed_query->find("Registry context: installed") != std::string::npos);
        REQUIRE(installed_query->find("Query or validate: agent installed query") !=
                std::string::npos);

        auto missing_selected = authority_tmp.path / "missing-selected";
        auto source_over_selected = pulp::cli::authority::resolve_registry(
            source / "docs", authority_tmp.path / "unrelated/pulp-cpp", missing_selected, "99.0.0",
            "pulp.toml:sdk_version");
        REQUIRE(source_over_selected);
        REQUIRE(source_over_selected->context == pulp::cli::authority::Context::source);

        auto downstream = authority_tmp.path / "downstream";
        fs::create_directories(downstream / "core");
        write_file(downstream / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");
        write_file(downstream / "pulp.toml", "[pulp]\nsdk_version = \"7.8.9\"\n");
        auto downstream_resolution = pulp::cli::authority::resolve_registry(
            downstream / "core", authority_tmp.path / "unrelated/pulp-cpp", prefix, "7.8.9",
            "pulp.toml:sdk_version");
        REQUIRE(downstream_resolution);
        REQUIRE(downstream_resolution->context == pulp::cli::authority::Context::installed);
        REQUIRE(downstream_resolution->context_root == prefix);

        auto source_build = authority_tmp.path / "source-build";
        write_file(source_build / "CMakeCache.txt",
                   "CMAKE_HOME_DIRECTORY:INTERNAL=" + source.generic_string() + "\n");
        auto selected_over_source_build = pulp::cli::authority::resolve_registry(
            downstream / "core", source_build / "tools/cli/pulp-cpp", prefix, "7.8.9",
            "pulp.toml:sdk_version");
        REQUIRE(selected_over_source_build);
        REQUIRE(selected_over_source_build->context == pulp::cli::authority::Context::installed);
        REQUIRE(selected_over_source_build->context_root == prefix);

        auto unrelated_ancestor = authority_tmp.path / "ancestor";
        write_file(unrelated_ancestor / "share/pulp/authority-navigation.json", "{}\n");
        auto nested_binary = unrelated_ancestor / "nested/bin/pulp-cpp";
        REQUIRE_FALSE(
            pulp::cli::authority::resolve_registry(authority_tmp.path / "outside", nested_binary));

        auto malformed_selected = authority_tmp.path / "malformed-selected";
        write_file(malformed_selected / "share/pulp/authority-navigation.schema.json",
                   read_file(source / "docs/status/authority-navigation.schema.json"));
        write_file(malformed_selected / "share/pulp/authority-navigation.json", "{}\n");
        auto selected_over_adjacent = pulp::cli::authority::resolve_registry(
            authority_tmp.path / "outside", prefix / "bin/pulp-cpp", malformed_selected, "8.0.0",
            "pulp.toml:sdk_path");
        REQUIRE(selected_over_adjacent);
        REQUIRE(selected_over_adjacent->context_root == malformed_selected);
        REQUIRE_FALSE(pulp::cli::authority::load_registry(*selected_over_adjacent).registry);
    }

    SECTION("authority registry rejects ordering mutation") {
        TempDir authority_tmp;
        auto source = authority_tmp.path / "source";
        fs::create_directories(source / "core");
        write_file(source / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");
        write_authority_registry(source, "a-before-agent");
        auto resolution = pulp::cli::authority::resolve_registry(source, {});
        REQUIRE(resolution);
        auto loaded = pulp::cli::authority::load_registry(*resolution);
        REQUIRE_FALSE(loaded.registry);
        REQUIRE(loaded.error.find("strictly bytewise sorted") != std::string::npos);
    }

    SECTION("authority registry rejects semantic schema and bound mutations") {
        TempDir authority_tmp;
        int fixture_index = 0;
        const auto expect_registry_error = [&](const auto& mutate, const std::string& expected) {
            const auto source = authority_tmp.path / ("source-" + std::to_string(fixture_index++));
            fs::create_directories(source / "core");
            write_file(source / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");
            write_authority_registry(source);
            auto registry = read_file(source / "docs/status/authority-navigation.json");
            mutate(registry, source);
            write_file(source / "docs/status/authority-navigation.json", registry);
            const auto resolution = pulp::cli::authority::resolve_registry(source, {});
            REQUIRE(resolution);
            const auto loaded = pulp::cli::authority::load_registry(*resolution);
            REQUIRE_FALSE(loaded.registry);
            INFO(loaded.error);
            REQUIRE(loaded.error.find(expected) != std::string::npos);
        };

        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                replace_once(registry, "\"id\":\"timeline-schema\"",
                             "\"id\":\"unexpected-authority\"");
            },
            "finite v1 authority ids");
        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                const auto first = registry.find("{\"id\":");
                const auto end = registry.find("},\n", first);
                REQUIRE(first != std::string::npos);
                REQUIRE(end != std::string::npos);
                auto extra = registry.substr(first, end + 1 - first);
                replace_once(extra, "\"id\":\"agent-capabilities\"",
                             "\"id\":\"zz-extra-authority\"");
                replace_once(extra, "\"aliases\":[\"agent-api\"]", "\"aliases\":[\"zz-extra\"]");
                const auto close = registry.rfind("\n  ]");
                REQUIRE(close != std::string::npos);
                registry.insert(close, ",\n" + extra);
            },
            "finite v1 authority ids");
        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                replace_once(registry, "{\"id\":\"agent-capabilities\"",
                             "{\"operations\":[],\"id\":\"agent-capabilities\"");
            },
            "unknown field 'operations'");
        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                replace_once(registry, "\"aliases\":[\"dsp\"]", "\"aliases\":[\"agent-api\"]");
            },
            "duplicate authority id or alias");
        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                replace_once(registry, "agent source query",
                             "pulp authority query agent-capabilities");
            },
            "cannot route back to the navigator");
        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                replace_once(registry,
                             "\"source_location\":\"planning\",\"installed_location\":null",
                             "\"source_location\":\"planning\",\"installed_location\":"
                             "\"share/pulp/dsp-survey.json\"");
                replace_once(registry,
                             "current danielraffel/pulp-planning main\",\"installed\":null",
                             "current danielraffel/pulp-planning main\",\"installed\":"
                             "\"pulp dsp-survey validate\"");
            },
            "must remain source-only");
        expect_registry_error(
            [](std::string& registry, const fs::path&) {
                replace_once(registry, "\"registry_revision\": 1",
                             "\"registry_revision\": 2147483648");
            },
            "unsupported authority-navigation schema or revision");
        expect_registry_error(
            [](std::string&, const fs::path& source) {
                const auto schema_path = source / "docs/status/authority-navigation.schema.json";
                write_file(schema_path, read_file(schema_path) + " ");
            },
            "canonical v1 schema");

        const auto crlf_source = authority_tmp.path / "source-crlf";
        fs::create_directories(crlf_source / "core");
        write_file(crlf_source / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");
        write_authority_registry(crlf_source);
        auto crlf_schema = read_file(crlf_source / "docs/status/authority-navigation.schema.json");
        std::string converted;
        for (const char character : crlf_schema)
            converted += character == '\n' ? "\r\n" : std::string(1, character);
        write_file(crlf_source / "docs/status/authority-navigation.schema.json", converted);
        const auto crlf_resolution = pulp::cli::authority::resolve_registry(crlf_source, {});
        REQUIRE(crlf_resolution);
        REQUIRE(pulp::cli::authority::load_registry(*crlf_resolution).registry);
    }
}

TEST_CASE("docs command indexes opens and searches local docs", "[cli][docs]") {
    TempDir tmp;
    auto root = make_project(tmp);
    ScopedCurrentPath cwd{root / "docs" / "guides"};

    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"index"}) == 0);
        REQUIRE(output.out.str().find("getting-started (guide)") != std::string::npos);
        REQUIRE(output.out.str().find("cli-reference (reference)") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"open", "getting-started"}) == 0);
        REQUIRE(output.out.str().find("# Getting Started") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"open"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"open", "missing"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"search", "rare", "synthesizer"}) == 0);
        REQUIRE(slash_normalized(output.out.str()).find("docs/guides/getting-started.md") !=
                std::string::npos);
        REQUIRE(output.out.str().find("6 match(es) found") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"search", "CLIR"}) == 0);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"search", "missingphrase"}) == 0);
        REQUIRE(output.out.str().find("No matches") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"search"}) == 1);
    }
}

TEST_CASE("docs command shows support matrix entries sections and scalars", "[cli][docs]") {
    TempDir tmp;
    auto root = make_project(tmp);
    ScopedCurrentPath cwd{root};

    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "support", "macos"}) == 0);
        REQUIRE(output.out.str().find("Status: supported") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "support", "default_output"}) == 0);
        REQUIRE(output.out.str().find("default_output: supported") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "support", "formats"}) == 0);
        REQUIRE(output.out.str().find("[formats]") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "support", "unknown"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "support"}) == 1);
    }
}

TEST_CASE("docs command shows command cmake and style manifests", "[cli][docs]") {
    TempDir tmp;
    auto root = make_project(tmp);
    ScopedCurrentPath cwd{root};

    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "command", "audio"}) == 0);
        REQUIRE(output.out.str().find("Command: audio") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "command", "missing"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "cmake", "pulp_add_plugin"}) == 0);
        REQUIRE(output.out.str().find("CMake function: pulp_add_plugin") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "cmake", "pulp_app_icon"}) == 0);
        REQUIRE(output.out.str().find("CMake function: pulp_app_icon") != std::string::npos);
        REQUIRE(output.out.str().find("reference/cmake.md#pulp_app_icon") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "cmake", "missing"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "style"}) == 0);
        REQUIRE(output.out.str().find("Style Rules") != std::string::npos);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "unknown"}) == 1);
    }
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"unknown"}) == 1);
    }
}

TEST_CASE("docs command reports missing manifest and script paths", "[cli][docs]") {
    TempDir tmp;
    auto root = make_project(tmp);
    ScopedCurrentPath cwd{root};

    fs::remove(root / "docs" / "status" / "docs-index.yaml");
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"index"}) == 1);
        REQUIRE(output.err.str().find("docs index not found") != std::string::npos);
    }
    fs::remove(root / "docs" / "status" / "support-matrix.yaml");
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "support", "macos"}) == 1);
    }

    fs::remove(root / "docs" / "status" / "cli-commands.yaml");
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "command", "audio"}) == 1);
        REQUIRE(output.err.str().find("CLI commands manifest not found") != std::string::npos);
    }

    fs::remove(root / "docs" / "status" / "cmake-functions.yaml");
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "cmake", "pulp_add_plugin"}) == 1);
    }

    fs::remove(root / "docs" / "status" / "style-rules.yaml");
    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"show", "style"}) == 1);
        REQUIRE(output.err.str().find("style rules not found") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"check"}) == 1);
        REQUIRE(output.err.str().find("check script not found") != std::string::npos);
    }

    {
        ScopedOutput output;
        REQUIRE(cmd_docs({"build-api"}) == 1);
        REQUIRE(output.err.str().find("build-api-docs.sh") != std::string::npos);
    }
}

TEST_CASE("docs command runs project docs check script", "[cli][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir tmp;
    auto root = make_project(tmp);
    auto marker = root / "docs-check-ran.txt";
    write_file(root / "tools" / "check-docs.sh", "#!/bin/sh\n"
                                                 "printf 'cli-docs-check-ran\\n' > \"" +
                                                     marker.string() + "\"\n");

    ScopedCurrentPath cwd{root / "docs" / "guides"};
    REQUIRE(cmd_docs({"check"}) == 0);
    REQUIRE(fs::exists(marker));
    REQUIRE(read_file_contents(marker).find("cli-docs-check-ran") != std::string::npos);

    fs::remove(marker);
    write_file(root / "tools" / "check-docs.sh", "#!/bin/sh\n"
                                                 "exit 7\n");
    REQUIRE(cmd_docs({"check"}) == 7);
    REQUIRE_FALSE(fs::exists(marker));
#endif
}

TEST_CASE("docs command runs project docs site build through mkdocs", "[cli][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake mkdocs assertions are only used on non-Windows");
#else
    TempDir tmp;
    auto root = make_project(tmp);
    write_file(root / "mkdocs.yml", "site_name: Test\n");

    auto fake_bin = tmp.path / "bin";
    auto fake_mkdocs = fake_bin / "mkdocs";
    auto args_log = tmp.path / "mkdocs-args.txt";
    write_file(fake_mkdocs, "#!/bin/sh\n"
                            "for arg in \"$@\"; do\n"
                            "  printf '%s\\n' \"$arg\"\n"
                            "done > \"$PULP_FAKE_MKDOCS_ARGS\"\n"
                            "exit \"${PULP_FAKE_MKDOCS_EXIT:-0}\"\n");
    fs::permissions(fake_mkdocs,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    const char* old_path = std::getenv("PATH");
    ScopedEnv path_env("PATH", fake_bin.string() + ":" + (old_path ? old_path : ""));
    ScopedEnv args_env("PULP_FAKE_MKDOCS_ARGS", args_log.string());
    ScopedEnv exit_env("PULP_FAKE_MKDOCS_EXIT", "0");

    auto site_dir = root / "build" / "site dir with spaces";
    ScopedCurrentPath cwd{root / "docs" / "guides"};
    REQUIRE(cmd_docs({"build-site", "--site-dir", site_dir.string(), "--strict"}) == 0);

    auto args = read_file_contents(args_log);
    const auto root_config = fs::weakly_canonical(root / "mkdocs.yml");
    const auto expected_args = "build\n"
                               "-f\n" +
                               root_config.string() +
                               "\n"
                               "--site-dir\n" +
                               site_dir.string() +
                               "\n"
                               "--strict\n";
    REQUIRE(args == expected_args);

    exit_env.set("11");
    ScopedOutput output;
    REQUIRE(cmd_docs({"build-site"}) == 11);
    REQUIRE(output.err.str().find("pip install -r requirements-docs.txt") != std::string::npos);
#endif
}

TEST_CASE("docs command runs project API docs build script", "[cli][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir tmp;
    auto root = make_project(tmp);
    auto marker = root / "api-docs-build-ran.txt";
    write_file(root / "tools" / "build-api-docs.sh", "#!/bin/sh\n"
                                                     "printf 'cli-docs-build-api-ran\\n' > \"" +
                                                         marker.string() + "\"\n");

    ScopedCurrentPath cwd{root / "docs" / "guides"};
    REQUIRE(cmd_docs({"build-api"}) == 0);
    REQUIRE(fs::exists(marker));
    REQUIRE(read_file_contents(marker).find("cli-docs-build-api-ran") != std::string::npos);

    fs::remove(marker);
    write_file(root / "tools" / "build-api-docs.sh", "#!/bin/sh\n"
                                                     "exit 9\n");
    REQUIRE(cmd_docs({"build-api"}) == 9);
    REQUIRE_FALSE(fs::exists(marker));
#endif
}
