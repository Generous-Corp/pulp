#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

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

std::vector<fs::path> pulp_binary_candidates() {
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        candidates.emplace_back(env);
    }
#if defined(PULP_CLI_BINARY)
    candidates.emplace_back(PULP_CLI_BINARY);
#endif
    auto build_dir = fs::path(PULP_BUILD_DIR);
    auto cli_dir = build_dir / "tools" / "cli";
#if defined(_WIN32)
    candidates.emplace_back(cli_dir / "Release" / "pulp.exe");
    candidates.emplace_back(cli_dir / "RelWithDebInfo" / "pulp.exe");
    candidates.emplace_back(cli_dir / "Debug" / "pulp.exe");
    candidates.emplace_back(cli_dir / "MinSizeRel" / "pulp.exe");
    candidates.emplace_back(cli_dir / "pulp.exe");
#else
    candidates.emplace_back(cli_dir / "pulp");
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
    options.timeout_ms = 10000;

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
    if (!pulp_binary_exists()) { SUCCEED("pulp binary not built"); return; }

    auto r = run_create({"create", "--ci"}, source_root());

    INFO(r.stderr_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stderr_output.find("Usage: pulp create <name>") != std::string::npos);
}

TEST_CASE("pulp create rejects standalone output paths inside the checkout",
          "[cli][create][shellout][issue-643]") {
    if (!pulp_binary_exists()) { SUCCEED("pulp binary not built"); return; }

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
    REQUIRE(r.exit_code == 1);
    REQUIRE_FALSE(created);
    REQUIRE(r.stderr_output.find("standalone product projects must live outside the Pulp repo")
            != std::string::npos);
    REQUIRE(r.stderr_output.find("Use --in-tree to scaffold under examples/")
            != std::string::npos);
}

TEST_CASE("pulp create --in-tree rejects output paths outside examples",
          "[cli][create][shellout][issue-643]") {
    if (!pulp_binary_exists()) { SUCCEED("pulp binary not built"); return; }

    TempDir tmp("pulp-create-in-tree-policy");
    auto out_dir = tmp.path / "OutsideExamples";

    auto r = run_create({"create", "Outside Examples",
                         "--in-tree",
                         "--output", out_dir.string(),
                         "--no-build", "--ci"},
                        source_root());

    INFO(r.stderr_output);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE_FALSE(fs::exists(out_dir));
    REQUIRE(r.stderr_output.find("--in-tree projects must live under")
            != std::string::npos);
    REQUIRE(r.stderr_output.find(native_path_string(source_root() / "examples"))
            != std::string::npos);
}
