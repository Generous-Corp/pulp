#include "../tools/cli/cli_common.hpp"
#include "../tools/cli/cli_sdk.hpp"
#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

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
