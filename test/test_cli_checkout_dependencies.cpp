#include "../tools/cli/cli_common.hpp"
#include "../tools/cli/cli_sdk.hpp"
#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {

struct TempCheckout {
    fs::path root = pulp::test::make_unique_temp_dir("pulp-checkout-deps");

    TempCheckout() {
        fs::create_directories(root / "tools/deps");
        fs::create_directories(root / "external/vst3sdk/pluginterfaces");
        fs::create_directories(root / "external/AudioUnitSDK/include/AudioUnitSDK");
        std::ofstream(root / "external/AudioUnitSDK/include/AudioUnitSDK/AUBase.h") << "fixture\n";
    }
    ~TempCheckout() { std::error_code ec; fs::remove_all(root, ec); }
};

struct ScopedEnv {
    std::string key;
    std::string previous;
    bool had_previous = false;

    ScopedEnv(std::string name, const char* value) : key(std::move(name)) {
        if (const char* old = std::getenv(key.c_str())) {
            previous = old;
            had_previous = true;
        }
#ifdef _WIN32
        _putenv_s(key.c_str(), value);
#else
        setenv(key.c_str(), value, 1);
#endif
    }

    ~ScopedEnv() {
#ifdef _WIN32
        _putenv_s(key.c_str(), had_previous ? previous.c_str() : "");
#else
        if (had_previous) setenv(key.c_str(), previous.c_str(), 1);
        else unsetenv(key.c_str());
#endif
    }
};

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path);
    REQUIRE(out.good());
    out << text;
}

std::string ready_cache(const std::string& contract, bool requires_ausdk) {
    return "PULP_CHECKOUT_DEPENDENCY_CONTRACT:INTERNAL=" + contract + "\n"
        + "PULP_REQUIRE_CHECKOUT_DEPENDENCIES:BOOL=TRUE\n"
        + "PULP_HAS_VST3:INTERNAL=1\n"
        + "PULP_CHECKOUT_REQUIRES_AUSDK:INTERNAL="
        + (requires_ausdk ? "TRUE\nPULP_HAS_AUSDK:INTERNAL=TRUE\n" : "FALSE\n");
}

} // namespace

TEST_CASE("checkout dependency readiness includes pins and configured target",
          "[cli][dependencies]") {
    TempCheckout checkout;
    const auto contract = checkout.root / "tools/deps/shared-source-contract.txt";
    const auto cache = checkout.root / "CMakeCache.txt";
    write_text(contract, "fixture-v1\n");

    SECTION("non-macOS target needs VST3 but not AudioUnitSDK") {
        write_text(cache, ready_cache("fixture-v1", false));
        REQUIRE(source_checkout_dependencies_enabled(checkout.root, cache));
    }

    SECTION("macOS target needs AudioUnitSDK") {
        auto text = ready_cache("fixture-v1", true);
        text.replace(text.find("PULP_HAS_AUSDK:INTERNAL=TRUE"),
                     std::string("PULP_HAS_AUSDK:INTERNAL=TRUE").size(),
                     "PULP_HAS_AUSDK:INTERNAL=FALSE");
        write_text(cache, text);
        REQUIRE_FALSE(source_checkout_dependencies_enabled(checkout.root, cache));
    }

    SECTION("macOS target is ready when AudioUnitSDK is present") {
        write_text(cache, ready_cache("fixture-v1", true));
        REQUIRE(source_checkout_dependencies_enabled(checkout.root, cache));
    }

    SECTION("pin contract drift invalidates an otherwise complete cache") {
        write_text(cache, ready_cache("fixture-v0", false));
        REQUIRE_FALSE(source_checkout_dependencies_enabled(checkout.root, cache));
    }

    SECTION("a deleted dependency link invalidates an otherwise complete cache") {
        write_text(cache, ready_cache("fixture-v1", false));
        fs::remove_all(checkout.root / "external/vst3sdk");
        REQUIRE_FALSE(source_checkout_dependencies_enabled(checkout.root, cache));
    }
}

TEST_CASE("checkout dependency bootstrap has an explicit emergency bypass",
          "[cli][dependencies]") {
    ScopedEnv bypass("PULP_SKIP_DEPENDENCY_BOOTSTRAP", "1");
    REQUIRE(ensure_checkout_dependencies(fs::path("/definitely/not/a/pulp/checkout")) == 0);
}

#if !defined(_WIN32)
TEST_CASE("checkout SDK installation uses the governed bounded build command",
          "[cli][dependencies][build-governance]") {
    TempCheckout checkout;
    write_text(checkout.root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.24)\n");

    const auto home = checkout.root / "home with spaces";
    const auto bin = checkout.root / "fake-bin";
    const auto log = checkout.root / "cmake-argv.log";
    fs::create_directories(bin);
    write_text(bin / "cmake", "#!/bin/sh\n"
                              "printf '%s\\n' \"$@\" >> \"$FAKE_CMAKE_LOG\"\n"
                              "for arg in \"$@\"; do\n"
                              "  case \"$arg\" in\n"
                              "    -DCMAKE_INSTALL_PREFIX=*) prefix=\"${arg#*=}\" ;;\n"
                              "  esac\n"
                              "done\n"
                              "if [ -n \"${prefix:-}\" ]; then\n"
                              "  mkdir -p \"$prefix/lib/cmake/Pulp\"\n"
                              "  : > \"$prefix/lib/cmake/Pulp/PulpConfig.cmake\"\n"
                              "  printf '0.9.0\\n' > \"$prefix/version.txt\"\n"
                              "fi\n"
                              "exit 0\n");
    fs::permissions(bin / "cmake", fs::perms::owner_all);

    const auto old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    const auto path = bin.string() + ":" + old_path;
    ScopedEnv pulp_home("PULP_HOME", home.string().c_str());
    ScopedEnv path_env("PATH", path.c_str());
    ScopedEnv cmake_log("FAKE_CMAKE_LOG", log.string().c_str());
    ScopedEnv skip_bootstrap("PULP_SKIP_DEPENDENCY_BOOTSTRAP", "1");
    ScopedEnv leases_off("PULP_TARTCI_LEASES", "0");
    ScopedEnv jobs("PULP_BUILD_JOBS", "3");
    ScopedEnv lock_off("PULP_BUILD_DIR_LOCK", "0");

    const auto tartci = bin / "tartci";
    write_text(tartci,
               "#!/bin/sh\n"
               "case \"$1 $2\" in\n"
               "  'host-profile ') printf 'PULP_BUILD_JOBS=3\\nTARTCI_GOVERNOR_SCHEMA=1\\n' ;;\n"
               "  'leases acquire') exit 75 ;;\n"
               "esac\n");
    fs::permissions(tartci, fs::perms::owner_all);
    ScopedEnv fake_tartci("PULP_TARTCI_BIN", tartci.string().c_str());
    ScopedEnv no_parent_lease("PULP_TARTCI_LEASE_HELD", "0");
    ScopedEnv interactive("PULP_BUILD_CLASS", "interactive");

    SECTION("capacity denial prevents the install build") {
        ScopedEnv leases_on("PULP_TARTCI_LEASES", "1");
        REQUIRE(ensure_checkout_sdk(checkout.root, "0.9.0").empty());
        std::ifstream input(log);
        std::string line;
        while (std::getline(input, line))
            REQUIRE(line != "--build");
        return;
    }

    SECTION("no lease uses the explicit local cap and preserves path arguments") {

        const auto sdk = ensure_checkout_sdk(checkout.root, "0.9.0");
        REQUIRE(sdk == home / "sdk-local" / detect_platform() / "0.9.0");

        std::ifstream input(log);
        REQUIRE(input.good());
        std::vector<std::string> invocations;
        std::string line;
        while (std::getline(input, line))
            invocations.push_back(line);
        REQUIRE(std::find(invocations.begin(), invocations.end(), "--build") != invocations.end());
        REQUIRE(std::find(invocations.begin(), invocations.end(), "--parallel") !=
                invocations.end());
        REQUIRE(std::find(invocations.begin(), invocations.end(), "3") != invocations.end());
        REQUIRE(std::find(invocations.begin(), invocations.end(),
                          (home / "sdk-build" / (detect_platform() + "-0.9.0")).string()) !=
                invocations.end());
    }
}
#endif

TEST_CASE("opt-in Shipyard targets are read from the checkout config", "[cli][shipyard]") {
    TempCheckout checkout;
    fs::create_directories(checkout.root / ".shipyard");
    const auto config = checkout.root / ".shipyard" / "config.toml";

    SECTION("a missing config reports no opt-in target") {
        REQUIRE(read_opt_in_shipyard_targets(checkout.root).empty());
    }

    SECTION("only `default = false` directly under [targets.<name>] counts") {
        std::ofstream(config) << "[project]\n"
                                 "default = false\n"
                                 "\n"
                                 "[targets.mac]\n"
                                 "backend = \"local\"\n"
                                 "default  = false   # opt-in\n"
                                 "\n"
                                 "[targets.mac.changed_surface_selection]\n"
                                 "default = false\n"
                                 "\n"
                                 "[targets.linux]\n"
                                 "default = true\n"
                                 "\n"
                                 "[targets.win]\n"
                                 "# default = false\n"
                                 "\n"
                                 "[targets.ssh]\n"
                                 "default = false\n";
        REQUIRE(read_opt_in_shipyard_targets(checkout.root) ==
                std::vector<std::string>{"mac", "ssh"});
    }

    SECTION("a sub-table cannot make its parent target opt-in") {
        std::ofstream(config) << "[targets.mac]\n"
                                 "backend = \"local\"\n"
                                 "[targets.mac.changed_surface_selection]\n"
                                 "default = false\n";
        REQUIRE(read_opt_in_shipyard_targets(checkout.root).empty());
    }
}
