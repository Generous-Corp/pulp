#include "../tools/cli/configure_defaults.hpp"
#include "test_cli_shellout_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using pulp::cli::ConfigureDefaults;
using pulp::cli::cmake_cache_value;
using pulp::cli::configure_default_args;

namespace {
bool has(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}
}  // namespace

TEST_CASE("fresh source configure pins Ninja, Release, and examples off",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.ninja_available = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in)
            == std::vector<std::string>{"-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                        "-DPULP_BUILD_EXAMPLES=OFF"});
}

TEST_CASE("fresh configure without ninja leaves the generator to CMake",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.source_checkout = true;
    const auto args = configure_default_args(in);
    REQUIRE_FALSE(has(args, "-G"));
    REQUIRE(has(args, "-DCMAKE_BUILD_TYPE=Release"));
}

TEST_CASE("PULP_BUILD_TYPE overrides the Release default; empty means unset",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.build_type_env = "Debug";
    REQUIRE(has(configure_default_args(in), "-DCMAKE_BUILD_TYPE=Debug"));
    REQUIRE_FALSE(has(configure_default_args(in), "-DCMAKE_BUILD_TYPE=Release"));
    in.build_type_env = "";
    REQUIRE(has(configure_default_args(in), "-DCMAKE_BUILD_TYPE=Release"));
}

TEST_CASE("an existing cache keeps its generator, build type, and examples",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.existing_cache = "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n"
                        "CMAKE_BUILD_TYPE:STRING=Debug\n"
                        "PULP_BUILD_EXAMPLES:BOOL=ON\n";
    in.ninja_available = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in).empty());
}

TEST_CASE("an existing cache with an empty build type is filled with Release",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.existing_cache = "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\r\nCMAKE_BUILD_TYPE:STRING=\r\n";
    in.ninja_available = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in) == std::vector<std::string>{"-DCMAKE_BUILD_TYPE=Release"});
}

TEST_CASE("the examples flag opts in; standalone projects never get the option",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.existing_cache = "PULP_BUILD_EXAMPLES:BOOL=OFF\nCMAKE_BUILD_TYPE:STRING=Release\n";
    in.examples = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in) == std::vector<std::string>{"-DPULP_BUILD_EXAMPLES=ON"});

    ConfigureDefaults standalone;
    standalone.ninja_available = true;
    standalone.examples = true;
    for (const auto& arg : configure_default_args(standalone)) {
        REQUIRE(arg.rfind("-DPULP_BUILD_EXAMPLES", 0) != 0);
    }
}

TEST_CASE("cmake_cache_value reads typed entries", "[cli][configure-defaults]") {
    const std::string cache = "// comment\nCMAKE_BUILD_TYPE:STRING=Release\nFOO:BOOL=ON";
    REQUIRE(cmake_cache_value(cache, "CMAKE_BUILD_TYPE") == "Release");
    REQUIRE(cmake_cache_value(cache, "FOO") == "ON");
    REQUIRE_FALSE(cmake_cache_value(cache, "MISSING").has_value());
    REQUIRE_FALSE(cmake_cache_value(cache, "CMAKE_BUILD").has_value());
}

// ── helpers that read a real build dir ─────────────────────────────────────

namespace {
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix) {
        path = fs::temp_directory_path()
             / (prefix + "-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
}  // namespace

TEST_CASE("build-dir helpers read the cache the configure decision depends on",
          "[cli][configure-defaults]") {
    TempDir tmp("pulp-configure-defaults");
    const auto build = tmp.path / "build";
    REQUIRE_FALSE(pulp::cli::read_cmake_cache(build).has_value());
    REQUIRE_FALSE(pulp::cli::build_dir_has_examples_off(build));
    // A fresh dir gets the full default set.
    REQUIRE(has(pulp::cli::configure_default_args_for(build, true, false, true), "-G"));

    write_text(build / "CMakeCache.txt",
               "CMAKE_BUILD_TYPE:STRING=Debug\nPULP_BUILD_EXAMPLES:BOOL=OFF\n");
    REQUIRE(pulp::cli::build_dir_has_examples_off(build));
    // An existing Debug dir is left alone unless examples are requested.
    REQUIRE(pulp::cli::configure_default_args_for(build, true, false, true).empty());
    REQUIRE(pulp::cli::configure_default_args_for(build, true, true, true)
            == std::vector<std::string>{"-DPULP_BUILD_EXAMPLES=ON"});
}

#if !defined(_WIN32)
namespace {
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_ = true;
            old_ = old;
        }
        if (value) setenv(name, value, 1);
        else unsetenv(name);
    }
    ~ScopedEnv() {
        if (had_) setenv(name_.c_str(), old_.c_str(), 1);
        else unsetenv(name_.c_str());
    }

private:
    std::string name_;
    bool had_ = false;
    std::string old_;
};

fs::path cpp_cli() { return fs::path(PULP_BUILD_DIR) / "tools" / "cli" / "pulp-cpp"; }

// A minimal source checkout plus a fake `cmake` (and optionally `ninja`) that
// records each invocation's argv, one line per call.
struct FakeCheckout {
    TempDir tmp{"pulp-build-configure-shellout"};
    fs::path root = tmp.path / "checkout";
    fs::path bin = tmp.path / "bin";
    fs::path log = tmp.path / "cmake-argv.log";

    explicit FakeCheckout(bool with_ninja) {
        fs::create_directories(root / "core");
        write_text(root / "CMakeLists.txt", "project(Pulp)\n");
        write_text(bin / "cmake", "#!/bin/sh\nprintf '%s ' \"$@\" >> '" + log.string()
                                      + "'\nprintf '\\n' >> '" + log.string() + "'\nexit 0\n");
        fs::permissions(bin / "cmake", fs::perms::owner_all);
        if (with_ninja) {
            write_text(bin / "ninja", "#!/bin/sh\nexit 0\n");
            fs::permissions(bin / "ninja", fs::perms::owner_all);
        }
    }

    // Runs `pulp-cpp build <args>` and returns the first cmake argv line.
    std::string configure_line(const std::vector<std::string>& args) {
        const std::string path = bin.string() + ":/usr/bin:/bin";
        ScopedEnv p("PATH", path.c_str());
        ScopedEnv s("PULP_SKIP_DEPENDENCY_BOOTSTRAP", "1");
        ScopedEnv c("PULP_SKIP_CACHE_PREFLIGHT", "1");
        ScopedEnv l("PULP_TARTCI_LEASES", "0");
        ScopedEnv u("PULP_UPDATE_CHECK_DISABLED", "1");
        ScopedEnv b("PULP_BUILD_TYPE", nullptr);
        pulp::platform::ProcessOptions options;
        options.working_directory = root.string();
        options.timeout_ms = pulp_test_cli::shellout_timeout_ms();
        std::vector<std::string> argv{"build"};
        argv.insert(argv.end(), args.begin(), args.end());
        auto r = pulp::platform::ChildProcess::run(cpp_cli().string(), argv, options);
        INFO(r.stdout_output << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        std::istringstream lines(read_text(log));
        std::string first;
        std::getline(lines, first);
        return first;
    }
};
}  // namespace

TEST_CASE("pulp-cpp build configures a fresh checkout with Ninja, Release, examples off",
          "[cli][configure-defaults][shellout]") {
    if (!fs::exists(cpp_cli())) SKIP("pulp-cpp not built");
    FakeCheckout checkout(/*with_ninja=*/true);
    const auto line = checkout.configure_line({});
    INFO(line);
    REQUIRE(line.find("-G Ninja") != std::string::npos);
    REQUIRE(line.find("-DCMAKE_BUILD_TYPE=Release") != std::string::npos);
    REQUIRE(line.find("-DPULP_BUILD_EXAMPLES=OFF") != std::string::npos);
}

TEST_CASE("pulp-cpp build --examples reconfigures a cache that has examples off",
          "[cli][configure-defaults][shellout]") {
    if (!fs::exists(cpp_cli())) SKIP("pulp-cpp not built");
    FakeCheckout checkout(/*with_ninja=*/false);
    // An otherwise complete cache, so only the examples request can trigger
    // a reconfigure (an incomplete one is healed by reconfiguring anyway).
    write_text(checkout.root / "tools/deps/shared-source-contract.txt", "fixture-contract\n");
    fs::create_directories(checkout.root / "external/vst3sdk/pluginterfaces");
    write_text(checkout.root / "build" / "CMakeCache.txt",
               "CMAKE_BUILD_TYPE:STRING=Release\nPULP_BUILD_EXAMPLES:BOOL=OFF\n"
               "PULP_REQUIRE_CHECKOUT_DEPENDENCIES:BOOL=ON\nPULP_HAS_VST3:INTERNAL=ON\n"
               "PULP_CHECKOUT_REQUIRES_AUSDK:INTERNAL=OFF\n"
               "PULP_CHECKOUT_DEPENDENCY_CONTRACT:INTERNAL=fixture-contract\n");
    // Control: without --examples the complete cache goes straight to the build.
    REQUIRE(checkout.configure_line({}).rfind("--build ", 0) == 0);
    fs::remove(checkout.log);

    const auto line = checkout.configure_line({"--examples"});
    INFO(line);
    REQUIRE(line.rfind("-B ", 0) == 0);  // a configure, not a build
    REQUIRE(line.find("-DPULP_BUILD_EXAMPLES=ON") != std::string::npos);
    REQUIRE(line.find("-G") == std::string::npos);
    REQUIRE(line.find("--examples") == std::string::npos);
}
#endif
