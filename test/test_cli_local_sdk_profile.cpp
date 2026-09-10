#include "tools/cli/local_sdk_profile.hpp"
#include "tools/cli/ship_tracing_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace local_sdk = pulp::cli::local_sdk;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("pulp-local-sdk-profile-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out << text;
    REQUIRE(out.good());
}

local_sdk::Identity identity(std::string sha = std::string(40, 'a')) {
    return {
        .sdk_version = "1.2.3",
        .source_git_sha = std::move(sha),
        .platform = "darwin-arm64",
        .cmake_version = "cmake version 4.1.0",
        .generator = "Ninja",
        .compiler = "Apple clang version 17.0.0",
        .macos_sdk = "26.0",
        .deployment_target = "13.4",
        .skia_identity = "chrome/m152",
        .vst3_git_sha = std::string(40, 'b'),
        .ausdk_git_sha = std::string(40, 'c'),
    };
}

void plant_install(const fs::path& prefix, const fs::path& build,
                   const local_sdk::Identity& expected) {
    write_file(prefix / "version.txt", expected.sdk_version + "\n");
    write_file(prefix / "sdk_build_type.txt", "Release\n");
    write_file(prefix / "lib/cmake/Pulp/PulpConfig.cmake", "# config\n");
    write_file(
        prefix / "lib/cmake/Pulp/PulpTargets.cmake",
        "Pulp::format Pulp::standalone Pulp::render Pulp::clap Pulp::vst3-sdk Pulp::ausdk\n");
    write_file(prefix / "include/pulp/runtime/build_info.hpp",
               "kBuildType   = \"Release\";\n"
               "kGitSha      = \"" +
                   expected.source_git_sha.substr(0, 7) +
                   "\";\n"
                   "kGitDirty                = false;\n");
    write_file(build / "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Release\n"
                                         "CMAKE_OSX_ARCHITECTURES:STRING=arm64\n"
                                         "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=13.4\n"
                                         "SKIA_DIR:PATH=/source/external/skia-build\n"
                                         "PULP_ENABLE_GPU:BOOL=ON\n"
                                         "PULP_HAS_SKIA:INTERNAL=TRUE\n"
                                         "PULP_ENABLE_DESIGN_IMPORT:BOOL=ON\n"
                                         "PULP_BUILD_WEBVIEW:BOOL=ON\n"
                                         "PULP_HAS_VST3:INTERNAL=TRUE\n"
                                         "PULP_HAS_AUSDK:INTERNAL=TRUE\n"
                                         "PULP_HAS_CLAP:INTERNAL=TRUE\n"
                                         "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
                                         "PULP_ENABLE_INSPECTOR:BOOL=OFF\n");
}

} // namespace

TEST_CASE("forge development SDK paths are immutable per source and toolchain",
          "[cli][sdk][forge-dev]") {
    const auto first = identity();
    auto second = first;
    second.source_git_sha = std::string(40, 'd');
    auto toolchain_change = first;
    toolchain_change.compiler = "Apple clang version 18.0.0";

    const auto a = local_sdk::profile_paths("/tmp/pulp-home", first);
    const auto b = local_sdk::profile_paths("/tmp/pulp-home", second);
    const auto c = local_sdk::profile_paths("/tmp/pulp-home", toolchain_change);

    REQUIRE(a.install_prefix != b.install_prefix);
    REQUIRE(a.install_prefix != c.install_prefix);
    REQUIRE(a.install_prefix.string().find(first.source_git_sha) != std::string::npos);
    REQUIRE(a.install_prefix.string().find("sdk-dev/forge-v1/darwin-arm64") != std::string::npos);
    REQUIRE(a.input_fingerprint.size() == 12);
}

TEST_CASE("forge development SDK install arguments fail closed", "[cli][sdk][forge-dev]") {
    const auto valid = local_sdk::parse_install_arguments(
        {"--local", "--profile", "forge-dev", "--print-path"}, "1.0.0");
    REQUIRE(valid.ok);
    REQUIRE(valid.from_local);
    REQUIRE(valid.print_path);
    REQUIRE(valid.profile == "forge-dev");
    REQUIRE(valid.version == "1.0.0");

    REQUIRE_FALSE(local_sdk::parse_install_arguments({"--profile", "forge-dev"}, "1.0.0").ok);
    REQUIRE_FALSE(local_sdk::parse_install_arguments({"--local", "--print-path"}, "1.0.0").ok);
    REQUIRE_FALSE(
        local_sdk::parse_install_arguments({"--local", "--profile", "release"}, "1.0.0").ok);
    REQUIRE_FALSE(local_sdk::parse_install_arguments({"--local", "--profile"}, "1.0.0").ok);
    REQUIRE_FALSE(local_sdk::parse_install_arguments({"--local", "--unknown"}, "1.0.0").ok);
    REQUIRE_FALSE(local_sdk::parse_install_arguments(
                      {"--local", "--profile", "forge-dev", "--version", "1.0.0"}, "1.0.0")
                      .ok);
}

TEST_CASE("forge development SDK normalizes only archives containing arm64",
          "[cli][sdk][forge-dev]") {
    using Action = local_sdk::ArchiveSliceAction;
    REQUIRE(local_sdk::archive_slice_action("arm64") == Action::Keep);
    REQUIRE(local_sdk::archive_slice_action("x86_64 arm64") == Action::ThinToArm64);
    REQUIRE(local_sdk::archive_slice_action("arm64 x86_64") == Action::ThinToArm64);
    REQUIRE(local_sdk::archive_slice_action("x86_64") == Action::Reject);
    REQUIRE(local_sdk::archive_slice_action("") == Action::Reject);
}

TEST_CASE("forge development SDK configure profile pins required capabilities",
          "[cli][sdk][forge-dev]") {
    const auto args = local_sdk::configure_arguments("/source with spaces", "/build with spaces",
                                                     "/stage with spaces", identity(),
                                                     std::nullopt);
    const auto contains = [&](const std::string& value) {
        return std::find(args.begin(), args.end(), value) != args.end();
    };

    REQUIRE(contains("-DCMAKE_BUILD_TYPE=Release"));
    REQUIRE(contains("-DCMAKE_C_COMPILER=/usr/bin/clang"));
    REQUIRE(contains("-DCMAKE_CXX_COMPILER=/usr/bin/clang++"));
    REQUIRE(contains("-DPULP_MACOS_ARCH=arm64"));
    REQUIRE(contains("-DPULP_ENABLE_GPU=ON"));
    REQUIRE(contains("-DPULP_REQUIRE_GPU_FOR_SDK=ON"));
    REQUIRE(contains("-DPULP_ENABLE_DESIGN_IMPORT=ON"));
    REQUIRE(contains("-DPULP_BUILD_WEBVIEW=ON"));
    REQUIRE(contains("-DPULP_ENABLE_AUDIO_PROBES=OFF"));
    REQUIRE(contains("-DPULP_ENABLE_INSPECTOR=OFF"));
    REQUIRE(contains("-DPULP_BUILD_TESTS=OFF"));
    REQUIRE(contains("-DPULP_BUILD_EXAMPLES=OFF"));
    REQUIRE(contains("-DCMAKE_INSTALL_PREFIX=/stage with spaces"));
    REQUIRE(contains("-DCMAKE_OSX_DEPLOYMENT_TARGET=13.4"));
    REQUIRE(contains("-DSKIA_DIR=/source with spaces/external/skia-build"));

    // Design import is ON above, so the browser-capture scripts are installed.
    // Without a Node runtime staged beside them they have no interpreter, and a
    // Forge configure against the resulting SDK fails on the missing payload
    // rather than degrading — so the flag has to reach CMake whenever a runtime
    // was resolved, and must be absent when one was not.
    const auto has_node_flag = [&] {
        return std::any_of(args.begin(), args.end(), [](const std::string& value) {
            return value.rfind("-DPULP_NODE_RUNTIME_EXECUTABLE=", 0) == 0;
        });
    };
    REQUIRE_FALSE(has_node_flag());

    const auto staged = local_sdk::configure_arguments("/source with spaces", "/build with spaces",
                                                       "/stage with spaces", identity(),
                                                       std::filesystem::path("/node with spaces/node"));
    REQUIRE(std::find(staged.begin(), staged.end(),
                      "-DPULP_NODE_RUNTIME_EXECUTABLE=/node with spaces/node") != staged.end());
}

TEST_CASE("forge development SDK validation accepts only the complete arm64 profile",
          "[cli][sdk][forge-dev]") {
    TempDir tmp;
    const auto expected = identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);

    auto validation = local_sdk::validate_staged_install(
        prefix, build, expected, "/source/external/skia-build");
    REQUIRE(validation.ok);
    REQUIRE(validation.errors.empty());

    write_file(build / "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Debug\n"
                                         "CMAKE_OSX_ARCHITECTURES:STRING=x86_64\n");
    validation = local_sdk::validate_staged_install(
        prefix, build, expected, "/source/external/skia-build");
    REQUIRE_FALSE(validation.ok);
    REQUIRE(validation.errors.size() >= 6);
}

TEST_CASE("forge development SDK validation rejects missing format and GPU targets",
          "[cli][sdk][forge-dev]") {
    TempDir tmp;
    const auto expected = identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);
    write_file(prefix / "lib/cmake/Pulp/PulpTargets.cmake", "Pulp::format Pulp::clap\n");

    const auto validation = local_sdk::validate_staged_install(
        prefix, build, expected, "/source/external/skia-build");
    REQUIRE_FALSE(validation.ok);
    REQUIRE(validation.errors.size() == 4);
}

TEST_CASE("forge development SDK validation rejects environment-selected build inputs",
          "[cli][sdk][forge-dev]") {
    TempDir tmp;
    const auto expected = identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);
    auto cache = local_sdk::parse_cmake_cache(build / "CMakeCache.txt");
    REQUIRE(cache.at("SKIA_DIR") == "/source/external/skia-build");

    write_file(build / "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Release\n"
                                         "CMAKE_OSX_ARCHITECTURES:STRING=arm64\n"
                                         "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=15.0\n"
                                         "SKIA_DIR:PATH=/tmp/unpinned-skia\n"
                                         "PULP_ENABLE_GPU:BOOL=ON\n"
                                         "PULP_HAS_SKIA:INTERNAL=TRUE\n"
                                         "PULP_ENABLE_DESIGN_IMPORT:BOOL=ON\n"
                                         "PULP_BUILD_WEBVIEW:BOOL=ON\n"
                                         "PULP_HAS_VST3:INTERNAL=TRUE\n"
                                         "PULP_HAS_AUSDK:INTERNAL=TRUE\n"
                                         "PULP_HAS_CLAP:INTERNAL=TRUE\n"
                                         "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
                                         "PULP_ENABLE_INSPECTOR:BOOL=OFF\n");
    const auto validation = local_sdk::validate_staged_install(
        prefix, build, expected, "/source/external/skia-build");
    REQUIRE_FALSE(validation.ok);
    REQUIRE(std::find(validation.errors.begin(), validation.errors.end(),
                      "configured macOS deployment target does not match the immutable profile") !=
            validation.errors.end());
    REQUIRE(std::find(validation.errors.begin(), validation.errors.end(),
                      "configured Skia directory does not match the immutable profile") !=
            validation.errors.end());
}

TEST_CASE("forge development SDK provenance is explicitly non-distributable",
          "[cli][sdk][forge-dev]") {
    TempDir tmp;
    const auto expected = identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);
    const auto fingerprint = local_sdk::input_fingerprint(expected);
    const auto json = local_sdk::serialize_provenance(expected, fingerprint);

    REQUIRE(json.find("\"kind\": \"development\"") != std::string::npos);
    REQUIRE(json.find("\"profile\": \"forge-dev\"") != std::string::npos);
    REQUIRE(json.find("\"distribution_eligible\": false") != std::string::npos);
    REQUIRE(json.find("\"source_git_dirty\": false") != std::string::npos);
    REQUIRE(json.find("\"architectures\": [\"arm64\"]") != std::string::npos);
    REQUIRE(json.find("\"webview\": true") != std::string::npos);
    REQUIRE(json.find(tmp.path.string()) == std::string::npos);

    std::string error;
    REQUIRE(local_sdk::write_file_atomically(prefix / "sdk-provenance.json", json, error));
    REQUIRE(error.empty());
    REQUIRE(local_sdk::validate_published_install(prefix, expected).ok);
}

TEST_CASE("forge development SDK provenance rejects a different source commit",
          "[cli][sdk][forge-dev]") {
    TempDir tmp;
    const auto expected = identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);
    std::string error;
    REQUIRE(local_sdk::write_file_atomically(
        prefix / "sdk-provenance.json",
        local_sdk::serialize_provenance(expected, local_sdk::input_fingerprint(expected)), error));

    auto other = expected;
    other.source_git_sha = std::string(40, 'e');
    const auto validation = local_sdk::validate_published_install(prefix, other);
    REQUIRE_FALSE(validation.ok);
}

namespace {

local_sdk::Identity trace_identity() {
    auto i = identity();
    i.tracing = true;
    return i;
}

// Upgrade a release-shaped planted install into a traced one: the CMake cache
// says tracing was on, the exported package declares the tracing target, and
// the runtime archive carries the retained ship sentinel that
// `core/runtime/src/trace.cpp` emits only under PULP_TRACING.
void plant_tracing_evidence(const fs::path& prefix, const fs::path& build) {
    auto cache = std::string(
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "CMAKE_OSX_ARCHITECTURES:STRING=arm64\n"
        "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=13.4\n"
        "SKIA_DIR:PATH=/source/external/skia-build\n"
        "PULP_ENABLE_GPU:BOOL=ON\n"
        "PULP_HAS_SKIA:INTERNAL=TRUE\n"
        "PULP_ENABLE_DESIGN_IMPORT:BOOL=ON\n"
        "PULP_BUILD_WEBVIEW:BOOL=ON\n"
        "PULP_HAS_VST3:INTERNAL=TRUE\n"
        "PULP_HAS_AUSDK:INTERNAL=TRUE\n"
        "PULP_HAS_CLAP:INTERNAL=TRUE\n"
        "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
        "PULP_ENABLE_INSPECTOR:BOOL=OFF\n"
        "PULP_TRACING:BOOL=ON\n");
    write_file(build / "CMakeCache.txt", cache);
    write_file(prefix / "lib/cmake/Pulp/PulpTargets.cmake",
               "Pulp::format Pulp::standalone Pulp::render Pulp::clap Pulp::vst3-sdk "
               "Pulp::ausdk Pulp::tracing\n");
    write_file(prefix / "lib/libpulp-runtime.a",
               std::string("\x21<arch>\npadding") + std::string(pulp::cli::kTracingShipSentinel) +
                   "more padding");
}

} // namespace

TEST_CASE("trace SDK install arguments parse beside forge-dev", "[cli][sdk][trace]") {
    const auto valid =
        local_sdk::parse_install_arguments({"--local", "--profile", "trace", "--print-path"},
                                           "1.0.0");
    REQUIRE(valid.ok);
    REQUIRE(valid.from_local);
    REQUIRE(valid.print_path);
    REQUIRE(valid.profile == local_sdk::kTraceProfileName);

    // Control: the same shape without --local is still rejected, so a passing
    // case above is the parser accepting the profile rather than accepting
    // everything.
    REQUIRE_FALSE(local_sdk::parse_install_arguments({"--profile", "trace"}, "1.0.0").ok);
    REQUIRE_FALSE(
        local_sdk::parse_install_arguments({"--local", "--profile", "tracing"}, "1.0.0").ok);
    REQUIRE_FALSE(local_sdk::parse_install_arguments(
                      {"--local", "--profile", "trace", "--version", "1.0.0"}, "1.0.0")
                      .ok);
}

TEST_CASE("trace SDK installs under its own immutable root", "[cli][sdk][trace]") {
    const auto traced = local_sdk::profile_paths("/tmp/pulp-home", trace_identity());
    const auto forge = local_sdk::profile_paths("/tmp/pulp-home", identity());

    REQUIRE(traced.install_prefix.string().find("sdk-dev/trace-v1/darwin-arm64") !=
            std::string::npos);
    REQUIRE(forge.install_prefix.string().find("sdk-dev/forge-v1/darwin-arm64") !=
            std::string::npos);
    // Same source and toolchain, different compiled output: the prefixes must
    // not collide, or one profile would silently serve the other's archives.
    REQUIRE(traced.install_prefix != forge.install_prefix);
    REQUIRE(traced.input_fingerprint != forge.input_fingerprint);
    REQUIRE(traced.build_dir != forge.build_dir);
}

TEST_CASE("trace SDK configure enables tracing and forge-dev does not", "[cli][sdk][trace]") {
    const auto traced = local_sdk::configure_arguments("/source", "/build", "/stage",
                                                       trace_identity(), std::nullopt);
    const auto forge =
        local_sdk::configure_arguments("/source", "/build", "/stage", identity(), std::nullopt);
    const auto has = [](const std::vector<std::string>& args, const std::string& value) {
        return std::find(args.begin(), args.end(), value) != args.end();
    };

    REQUIRE(has(traced, "-DPULP_TRACING=ON"));
    REQUIRE_FALSE(has(forge, "-DPULP_TRACING=ON"));
    // Control: both still pin the shared capability set, so the assertion above
    // is about tracing rather than about one list being empty.
    REQUIRE(has(traced, "-DPULP_ENABLE_GPU=ON"));
    REQUIRE(has(forge, "-DPULP_ENABLE_GPU=ON"));
}

TEST_CASE("trace SDK validation accepts a build that really carries Perfetto",
          "[cli][sdk][trace]") {
    TempDir tmp;
    const auto expected = trace_identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);
    plant_tracing_evidence(prefix, build);

    const auto validation =
        local_sdk::validate_staged_install(prefix, build, expected, "/source/external/skia-build");
    INFO((validation.errors.empty() ? std::string{} : validation.errors.front()));
    REQUIRE(validation.ok);
}

TEST_CASE("trace SDK validation rejects a release build wearing the trace profile",
          "[cli][sdk][trace]") {
    // The mislabeled-SDK regression. A directory that claims the trace profile
    // while holding an ordinary release build must never publish: this is
    // exactly the state a hand-copied `<version>-trace` prefix was in.
    TempDir tmp;
    const auto expected = trace_identity();
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, expected);
    // Deliberately no plant_tracing_evidence: release-shaped contents.

    const auto validation =
        local_sdk::validate_staged_install(prefix, build, expected, "/source/external/skia-build");
    REQUIRE_FALSE(validation.ok);

    const auto says = [&](const std::string& needle) {
        return std::any_of(validation.errors.begin(), validation.errors.end(),
                           [&](const std::string& e) { return e.find(needle) != std::string::npos; });
    };
    REQUIRE(says("PULP_TRACING was not enabled"));
    REQUIRE(says("no tracing sentinel"));
    REQUIRE(says("does not declare Pulp::tracing"));

    // Control: the identical fixture validates clean under the forge-dev
    // identity, proving these three errors come from the tracing contract and
    // not from a broken fixture.
    REQUIRE(local_sdk::validate_staged_install(prefix, build, identity(),
                                               "/source/external/skia-build")
                .ok);
}

TEST_CASE("forge-dev validation rejects a build that quietly enabled tracing",
          "[cli][sdk][trace]") {
    TempDir tmp;
    const auto prefix = tmp.path / "prefix";
    const auto build = tmp.path / "build";
    plant_install(prefix, build, identity());
    plant_tracing_evidence(prefix, build);

    const auto validation =
        local_sdk::validate_staged_install(prefix, build, identity(), "/source/external/skia-build");
    REQUIRE_FALSE(validation.ok);
    REQUIRE(std::any_of(validation.errors.begin(), validation.errors.end(),
                        [](const std::string& e) {
                            return e.find("tracing must stay disabled") != std::string::npos;
                        }));
}

TEST_CASE("trace SDK provenance records the profile and stays non-distributable",
          "[cli][sdk][trace]") {
    const auto expected = trace_identity();
    const auto json =
        local_sdk::serialize_provenance(expected, local_sdk::input_fingerprint(expected));

    REQUIRE(json.find("\"kind\": \"development\"") != std::string::npos);
    REQUIRE(json.find("\"profile\": \"trace\"") != std::string::npos);
    REQUIRE(json.find("\"tracing\": true") != std::string::npos);
    REQUIRE(json.find("\"distribution_eligible\": false") != std::string::npos);

    // Control: the forge-dev marker is byte-stable and carries no tracing key,
    // so existing prefixes are not invalidated by this profile's arrival.
    const auto forge =
        local_sdk::serialize_provenance(identity(), local_sdk::input_fingerprint(identity()));
    REQUIRE(forge.find("\"profile\": \"forge-dev\"") != std::string::npos);
    REQUIRE(forge.find("\"tracing\"") == std::string::npos);
}
