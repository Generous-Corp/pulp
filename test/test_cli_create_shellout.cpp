#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_old_ = true;
            old_value_ = old;
        }
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    ~ScopedEnvVar() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), had_old_ ? old_value_.c_str() : "");
#else
        if (had_old_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    bool had_old_ = false;
    std::string old_value_;
};

struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& prefix) {
        path = fs::temp_directory_path() /
               (prefix + "-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path source_root() {
    return fs::path(PULP_SOURCE_DIR);
}

std::string native_path_string(fs::path path) {
#if defined(_WIN32)
    path.make_preferred();
#endif
    return path.string();
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    std::ostringstream out;
    out << f.rdbuf();
    return out.str();
}

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << text;
}

std::vector<fs::path> pulp_binary_candidates() {
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        candidates.emplace_back(env);
    }
#if defined(PULP_CLI_BINARY)
    candidates.emplace_back(PULP_CLI_BINARY);
#endif
    auto build_dir = fs::path(PULP_BUILD_DIR);
    candidates.emplace_back(build_dir / "pulp");
    auto cli_dir = build_dir / "tools" / "cli";
#if defined(_WIN32)
    candidates.emplace_back(build_dir / "Release" / "pulp.exe");
    candidates.emplace_back(build_dir / "RelWithDebInfo" / "pulp.exe");
    candidates.emplace_back(build_dir / "Debug" / "pulp.exe");
    candidates.emplace_back(build_dir / "MinSizeRel" / "pulp.exe");
    candidates.emplace_back(cli_dir / "Release" / "pulp.exe");
    candidates.emplace_back(cli_dir / "RelWithDebInfo" / "pulp.exe");
    candidates.emplace_back(cli_dir / "Debug" / "pulp.exe");
    candidates.emplace_back(cli_dir / "MinSizeRel" / "pulp.exe");
    candidates.emplace_back(cli_dir / "pulp.exe");
    candidates.emplace_back(cli_dir / "pulp-cpp.exe");
#else
    candidates.emplace_back(cli_dir / "pulp");
    candidates.emplace_back(cli_dir / "pulp-cpp");
#endif
    return candidates;
}

fs::path pulp_binary() {
    for (const auto& candidate : pulp_binary_candidates()) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    auto candidates = pulp_binary_candidates();
    return candidates.empty() ? fs::path{} : candidates.front();
}

bool pulp_binary_exists() {
    auto binary = pulp_binary();
    std::error_code ec;
    return fs::exists(binary, ec) && fs::is_regular_file(binary, ec);
}

bool pulp_binary_available() {
    if (const char* env = std::getenv("PULP_CLI_PATH")) {
        // Empty override is the explicit absent-CLI sentinel used by skip-path tests.
        if (*env == '\0') {
            return false;
        }
        const fs::path configured(env);
        std::error_code ec;
        if (fs::exists(configured, ec) && fs::is_regular_file(configured, ec)) {
            return true;
        }
        FAIL("pulp binary configured but missing at " + native_path_string(configured));
    }
    return pulp_binary_exists();
}

std::string pulp_binary_diagnostics(const fs::path& selected) {
    std::ostringstream out;
    out << "selected pulp binary: " << native_path_string(selected) << "\n";
    for (const auto& candidate : pulp_binary_candidates()) {
        std::error_code ec;
        out << "candidate: " << native_path_string(candidate)
            << " exists=" << fs::exists(candidate, ec)
            << " regular=" << fs::is_regular_file(candidate, ec) << "\n";
    }
    return out.str();
}

pulp::platform::ProcessResult run_create(const std::vector<std::string>& args,
                                         const fs::path& working_dir) {
    pulp::platform::ProcessOptions options;
    options.working_directory = native_path_string(working_dir);
    // Generous budget, not a latency assertion. `pulp create` runs in ~1-2 s,
    // but the local-template-kit path shells `pulp` → `pulp-cpp` (two cold
    // binary loads + dylib resolution + file I/O) for byte-for-byte parity.
    // On a freshly-cloned ephemeral CI VM with a cold page cache that double
    // cold-start intermittently blew past a 10 s budget — the create itself
    // succeeded, but `ChildProcess::run` killed it and `timed_out` tripped
    // (only the delegating kit cases, never the single-process reject cases).
    // 60 s tolerates the cold-VM start while still catching a genuine hang.
    options.timeout_ms = 60000;

    ScopedEnvVar disable_update("PULP_UPDATE_CHECK_DISABLED", "1");
    auto binary = pulp_binary();
    auto result =
        pulp::platform::ChildProcess::run(native_path_string(binary), args, options);
    if (result.exit_code == -1 && result.stderr_output.empty()) {
        result.stderr_output = "failed to start pulp create shell-out\n" +
                               pulp_binary_diagnostics(binary);
    }
    return result;
}

}  // namespace

TEST_CASE("pulp create without a name fails before project resolution",
          "[cli][create][shellout][issue-643]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    auto r = run_create({"create", "--ci"}, source_root());

    INFO(r.stderr_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE((r.exit_code == 1 || r.exit_code == 2));
    REQUIRE(r.stderr_output.find("Usage: pulp create <name>") != std::string::npos);
}

TEST_CASE("pulp create rejects standalone output paths inside the checkout",
          "[cli][create][shellout][issue-643]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    auto out_dir = source_root() / "build" / "pulp-create-standalone-policy-reject";
    std::error_code ec;
    fs::remove_all(out_dir, ec);

    auto r = run_create({"create", "Inside Repo",
                         "--output", out_dir.string(),
                         "--no-build", "--ci"},
                        source_root());

    const bool created = fs::exists(out_dir);
    fs::remove_all(out_dir, ec);

    INFO(r.stderr_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE((r.exit_code == 1 || r.exit_code == 2));
    REQUIRE_FALSE(created);
    REQUIRE(r.stderr_output.find("standalone product projects must live outside the Pulp repo")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("Use --in-tree to scaffold under examples/")
            != std::string::npos);
}

TEST_CASE("pulp create --in-tree rejects output paths outside examples",
          "[cli][create][shellout][issue-643]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-in-tree-policy");
    auto out_dir = tmp.path / "OutsideExamples";

    auto r = run_create({"create", "Outside Examples",
                         "--in-tree",
                         "--output", out_dir.string(),
                         "--no-build", "--ci"},
                        source_root());

    INFO(r.stderr_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE((r.exit_code == 1 || r.exit_code == 2));
    REQUIRE_FALSE(fs::exists(out_dir));
    REQUIRE(r.stderr_output.find("--in-tree projects must live under")
            != std::string::npos);
    REQUIRE(r.stderr_output.find(native_path_string(source_root() / "examples"))
            != std::string::npos);
}

TEST_CASE("pulp create accepts local template kit paths without executing package code",
          "[cli][create][shellout][kit]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-template-kit");
    ScopedEnvVar pulp_home("PULP_HOME", native_path_string(tmp.path / "pulp-home").c_str());

    const auto out_dir = tmp.path / "GeneratedFromKit";
    const auto kit_dir = source_root() / "fixtures" / "packages" / "simple-plugin-template";

    auto r = run_create({"create", "Kit Gain",
                         "--template", native_path_string(kit_dir),
                         "--output", native_path_string(out_dir),
                         "--no-build", "--ci"},
                        source_root());

    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out_dir / "kit_gain.hpp"));
    REQUIRE(fs::exists(out_dir / "CMakeLists.txt"));
    REQUIRE(fs::exists(out_dir / "clap_entry.cpp"));
    REQUIRE_FALSE(fs::exists(out_dir / "vst3_entry.cpp"));
    REQUIRE_FALSE(fs::exists(out_dir / "au_v2_entry.cpp"));
    REQUIRE(fs::exists(out_dir / "test_kit_gain.cpp"));

    const auto golden = read_file(kit_dir / "validation" / "generated-project.diff");
    REQUIRE(golden.find("diff --git a/CMakeLists.txt b/CMakeLists.txt") != std::string::npos);
    REQUIRE(golden.find("diff --git a/kit_gain.hpp b/kit_gain.hpp") != std::string::npos);
    REQUIRE(golden.find("diff --git a/test_kit_gain.cpp b/test_kit_gain.cpp") != std::string::npos);

    auto require_anchor = [&](const fs::path& file, const std::string& plus_line) {
        INFO(file);
        INFO(plus_line);
        REQUIRE(golden.find("+" + plus_line) != std::string::npos);
        REQUIRE(read_file(file).find(plus_line) != std::string::npos);
    };

    require_anchor(out_dir / "CMakeLists.txt", "find_package(Pulp REQUIRED CONFIG)");
    require_anchor(out_dir / "CMakeLists.txt", "add_executable(KitGain-test test_kit_gain.cpp)");
    require_anchor(out_dir / "CMakeLists.txt", "    FORMATS         CLAP Standalone");
    require_anchor(out_dir / "kit_gain.hpp", "class KitGain : public pulp::format::Processor {");
    require_anchor(out_dir / "kit_gain.hpp", "    void prepare(const pulp::format::PrepareContext&) override {}");
    require_anchor(out_dir / "kit_gain.hpp", "inline std::unique_ptr<pulp::format::Processor> create_kit_gain() {");
    REQUIRE(read_file(out_dir / "clap_entry.cpp").find("PULP_CLAP_PLUGIN(kit_gain::create_kit_gain)") != std::string::npos);
    require_anchor(out_dir / "test_kit_gain.cpp", "TEST_CASE(\"KitGain descriptor is generated from template kit\") {");
}

TEST_CASE("pulp create accepts bare relative local template kit directories",
          "[cli][create][shellout][kit]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-template-kit-relative");
    ScopedEnvVar pulp_home("PULP_HOME", native_path_string(tmp.path / "pulp-home").c_str());

    const auto out_dir = tmp.path / "GeneratedFromRelativeKit";
    auto r = run_create({"create", "Relative Kit Gain",
                         "--template", "simple-plugin-template",
                         "--output", native_path_string(out_dir),
                         "--no-build", "--ci"},
                        source_root() / "fixtures" / "packages");

    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out_dir / "relative_kit_gain.hpp"));
    REQUIRE(fs::exists(out_dir / "CMakeLists.txt"));
    REQUIRE(r.stderr_output.find("template directory not found") == std::string::npos);
}

TEST_CASE("pulp create accepts template kits when curated dependencies are installed",
          "[cli][create][shellout][kit]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-template-kit-deps");
    ScopedEnvVar pulp_home("PULP_HOME", native_path_string(tmp.path / "pulp-home").c_str());

    const auto project = tmp.path / "project";
    const auto kit_dir = tmp.path / "template-kit";
    fs::copy(source_root() / "fixtures" / "packages" / "simple-plugin-template",
             kit_dir,
             fs::copy_options::recursive);
    auto manifest = read_file(kit_dir / "pulp.package.json");
    const auto needle = R"JSON("packages": [])JSON";
    const auto pos = manifest.find(needle);
    REQUIRE(pos != std::string::npos);
    manifest.replace(pos, std::string(needle).size(), R"JSON("packages": ["rubber-band"])JSON");
    write_file(kit_dir / "pulp.package.json", manifest);

    write_file(project / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.22)\nproject(Project)\n");
    fs::create_directories(project / "tools" / "packages");
    fs::copy_file(source_root() / "tools" / "packages" / "registry.json",
                  project / "tools" / "packages" / "registry.json");
    write_file(project / "packages.lock.json", R"JSON({
  "lockfile_version": 1,
  "packages": {
    "rubber-band": {
      "version": "v3.3.0",
      "resolved": "registry:rubber-band",
      "integrity": "",
      "commit": "v3.3.0"
    }
  }
})JSON");

    const auto out_dir = tmp.path / "GeneratedFromDependencyKit";
    auto r = run_create({"create", "Dependency Kit Gain",
                         "--template", native_path_string(kit_dir),
                         "--output", native_path_string(out_dir),
                         "--no-build", "--ci"},
                        project);

    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out_dir / "CMakeLists.txt"));
    REQUIRE(fs::exists(out_dir / "dependency_kit_gain.hpp"));
}

TEST_CASE("pulp create rejects template kits missing core scaffold templates",
          "[cli][create][shellout][kit]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-template-kit-missing-cmake");
    ScopedEnvVar pulp_home("PULP_HOME", native_path_string(tmp.path / "pulp-home").c_str());

    const auto kit_dir = tmp.path / "template-kit";
    fs::copy(source_root() / "fixtures" / "packages" / "simple-plugin-template",
             kit_dir,
             fs::copy_options::recursive);
    fs::remove(kit_dir / "templates" / "basic-plugin" / "CMakeLists.txt.template");

    const auto out_dir = tmp.path / "GeneratedMissingCMake";
    auto r = run_create({"create", "Missing CMake Kit",
                         "--template", native_path_string(kit_dir),
                         "--output", native_path_string(out_dir),
                         "--no-build", "--ci"},
                        source_root());

    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE_FALSE(fs::exists(out_dir));
    REQUIRE(r.stderr_output.find("CMakeLists.txt.template") != std::string::npos);
}

TEST_CASE("pulp create resolves built-in template names before same-named local paths",
          "[cli][create][shellout][kit]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-template-shadow");
    ScopedEnvVar pulp_home("PULP_HOME", native_path_string(tmp.path / "pulp-home").c_str());

    const auto work_dir = source_root() / "build" / ("pulp-create-template-shadow-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(work_dir);
    write_file(work_dir / "gain", "not a template kit\n");

    const auto out_dir = tmp.path / "GeneratedGain";
    auto r = run_create({"create", "Generated Gain",
                         "--template", "gain",
                         "--output", native_path_string(out_dir),
                         "--no-build", "--ci"},
                        work_dir);
    std::error_code ec;
    fs::remove_all(work_dir, ec);

    INFO(r.stderr_output);
    INFO(r.stdout_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out_dir / "CMakeLists.txt"));
    REQUIRE(r.stderr_output.find("template kit manifest is invalid") == std::string::npos);
}

TEST_CASE("pulp create rejects non-template kits passed as template paths",
          "[cli][create][shellout][kit]") {
    if (!pulp_binary_available()) { SKIP("pulp binary not built"); }

    TempDir tmp("pulp-create-template-kit-reject");
    const auto out_dir = tmp.path / "Rejected";
    const auto kit_dir = source_root() / "fixtures" / "packages" / "gain-dsp-kit";

    auto r = run_create({"create", "Rejected Kit",
                         "--template", native_path_string(kit_dir),
                         "--output", native_path_string(out_dir),
                         "--no-build", "--ci"},
                        source_root());

    INFO(r.stderr_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE_FALSE(fs::exists(out_dir));
    REQUIRE(r.stderr_output.find("does not declare kind `template`") != std::string::npos);
}
