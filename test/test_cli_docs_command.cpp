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

} // namespace

TEST_CASE("docs command reports usage outside and inside projects",
          "[cli][docs]") {
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
          "[cli][docs]") {
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
          "[cli][docs]") {
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
          "[cli][docs]") {
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

TEST_CASE("docs command reports missing manifest and script paths",
          "[cli][docs]") {
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

TEST_CASE("docs command runs project docs check script",
          "[cli][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir tmp;
    auto root = make_project(tmp);
    auto marker = root / "docs-check-ran.txt";
    write_file(root / "tools" / "check-docs.sh",
               "#!/bin/sh\n"
               "printf 'cli-docs-check-ran\\n' > \"" + marker.string() + "\"\n");

    ScopedCurrentPath cwd{root / "docs" / "guides"};
    REQUIRE(cmd_docs({"check"}) == 0);
    REQUIRE(fs::exists(marker));
    REQUIRE(read_file_contents(marker).find("cli-docs-check-ran") != std::string::npos);

    fs::remove(marker);
    write_file(root / "tools" / "check-docs.sh",
               "#!/bin/sh\n"
               "exit 7\n");
    REQUIRE(cmd_docs({"check"}) == 7);
    REQUIRE_FALSE(fs::exists(marker));
#endif
}

TEST_CASE("docs command runs project docs site build through mkdocs",
          "[cli][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake mkdocs assertions are only used on non-Windows");
#else
    TempDir tmp;
    auto root = make_project(tmp);
    write_file(root / "mkdocs.yml", "site_name: Test\n");

    auto fake_bin = tmp.path / "bin";
    auto fake_mkdocs = fake_bin / "mkdocs";
    auto args_log = tmp.path / "mkdocs-args.txt";
    write_file(fake_mkdocs,
               "#!/bin/sh\n"
               "for arg in \"$@\"; do\n"
               "  printf '%s\\n' \"$arg\"\n"
               "done > \"$PULP_FAKE_MKDOCS_ARGS\"\n"
               "exit \"${PULP_FAKE_MKDOCS_EXIT:-0}\"\n");
    fs::permissions(fake_mkdocs,
                    fs::perms::owner_exec | fs::perms::owner_read |
                        fs::perms::owner_write,
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
    const auto expected_args =
        "build\n"
        "-f\n" + root_config.string() + "\n"
        "--site-dir\n" + site_dir.string() + "\n"
        "--strict\n";
    REQUIRE(args == expected_args);

    exit_env.set("11");
    ScopedOutput output;
    REQUIRE(cmd_docs({"build-site"}) == 11);
    REQUIRE(output.err.str().find("pip install -r requirements-docs.txt")
            != std::string::npos);
#endif
}

TEST_CASE("docs command runs project API docs build script",
          "[cli][docs]") {
#if defined(_WIN32)
    SKIP("POSIX fake script assertions are only used on non-Windows");
#else
    TempDir tmp;
    auto root = make_project(tmp);
    auto marker = root / "api-docs-build-ran.txt";
    write_file(root / "tools" / "build-api-docs.sh",
               "#!/bin/sh\n"
               "printf 'cli-docs-build-api-ran\\n' > \"" + marker.string() + "\"\n");

    ScopedCurrentPath cwd{root / "docs" / "guides"};
    REQUIRE(cmd_docs({"build-api"}) == 0);
    REQUIRE(fs::exists(marker));
    REQUIRE(read_file_contents(marker).find("cli-docs-build-api-ran") != std::string::npos);

    fs::remove(marker);
    write_file(root / "tools" / "build-api-docs.sh",
               "#!/bin/sh\n"
               "exit 9\n");
    REQUIRE(cmd_docs({"build-api"}) == 9);
    REQUIRE_FALSE(fs::exists(marker));
#endif
}
