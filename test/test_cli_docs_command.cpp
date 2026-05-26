#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/cli_common.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdint>
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
        path = fs::temp_directory_path()
             / ("pulp-cli-docs-command-test-"
                + std::to_string(reinterpret_cast<std::uintptr_t>(this))
                + "-" + std::to_string(n));
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

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    REQUIRE(f.good());
    f << body;
}

std::string slash_normalized(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
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
    write_file(root / "docs" / "reference" / "cli.md", "# CLI Reference\n\nUse pulp docs show command.\n");

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
)YAML");

    write_file(root / "docs" / "status" / "style-rules.yaml", R"YAML(
- id: public-history
  rule: Keep commits and filenames public-ready
  severity: error
)YAML");

    return root;
}

} // namespace

TEST_CASE("docs command reports usage outside and inside projects",
          "[cli][docs][coverage]") {
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
}

TEST_CASE("docs command indexes opens and searches local docs",
          "[cli][docs][coverage]") {
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
        REQUIRE(slash_normalized(output.out.str()).find("docs/guides/getting-started.md")
                != std::string::npos);
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

TEST_CASE("docs command shows support matrix entries sections and scalars",
          "[cli][docs][coverage]") {
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

TEST_CASE("docs command shows command cmake and style manifests",
          "[cli][docs][coverage]") {
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

TEST_CASE("docs command reports missing manifest and script paths",
          "[cli][docs][coverage]") {
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
        REQUIRE(cmd_docs({"build-api"}) == 1);
    }
}
