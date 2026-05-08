// Regression coverage for the C++ `pulp upgrade` install helper.
//
// Older pre-cutover C++ CLIs can upgrade straight into a Phase 8 Rust
// archive. That archive contains `pulp` plus sibling artifacts such as
// `pulp-cpp` and the wgpu runtime library; copying only `pulp` strands
// the new Rust CLI without its C++ fallthrough delegate (#1673).

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/upgrade_install.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define pulp_test_pid() _getpid()
#else
#  include <unistd.h>
#  define pulp_test_pid() ::getpid()
#endif

namespace fs = std::filesystem;
namespace ui = pulp::cli::upgrade_install;

namespace {

fs::path make_tmpdir(const std::string& tag) {
    auto dir = fs::temp_directory_path() /
               ("pulp-test-upgrade-install-" + tag + "-" +
                std::to_string(pulp_test_pid()) + "-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
    fs::create_directories(dir);
    return dir;
}

const char* runtime_library_name() {
#ifdef _WIN32
    return "wgpu_native.dll";
#elif defined(__APPLE__)
    return "libwgpu_native.dylib";
#else
    return "libwgpu_native.so";
#endif
}

void write_file(const fs::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary);
    out << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("upgrade install copies sibling payloads before self replacement",
          "[cli][upgrade][issue-1673]") {
    auto extracted = make_tmpdir("extracted");
    auto install = make_tmpdir("install");

    auto primary = extracted / ui::primary_binary_name();
    auto cpp = extracted / ui::cpp_binary_name();
    auto runtime = extracted / runtime_library_name();
    auto archive = extracted / "pulp-darwin-arm64.tar.gz";

    write_file(primary, "new-rust-pulp");
    write_file(cpp, "new-cpp-delegate");
    write_file(runtime, "new-runtime-lib");
    write_file(archive, "downloaded-archive");

    auto installed_primary = install / ui::primary_binary_name();
    write_file(installed_primary, "old-cpp-pulp");

    auto installed = ui::install_sibling_payloads(extracted, install, primary, archive);

    REQUIRE(ui::installed_cpp_delegate(installed));
    REQUIRE(read_file(installed_primary) == "old-cpp-pulp");
    REQUIRE(read_file(install / ui::cpp_binary_name()) == "new-cpp-delegate");
    REQUIRE(read_file(install / runtime.filename()) == "new-runtime-lib");
    REQUIRE_FALSE(fs::exists(install / archive.filename()));

#ifndef _WIN32
    const auto cpp_perms = fs::status(install / ui::cpp_binary_name()).permissions();
    REQUIRE((cpp_perms & fs::perms::owner_exec) != fs::perms::none);
#endif

    fs::remove_all(extracted);
    fs::remove_all(install);
}

TEST_CASE("upgrade install tolerates archives without a cpp delegate",
          "[cli][upgrade][issue-1673]") {
    auto extracted = make_tmpdir("single");
    auto install = make_tmpdir("single-install");

    auto primary = extracted / ui::primary_binary_name();
    auto archive = extracted / "pulp-linux-x64.tar.gz";
    write_file(primary, "new-pulp");
    write_file(archive, "downloaded-archive");

    auto installed = ui::install_sibling_payloads(extracted, install, primary, archive);

    REQUIRE(installed.empty());
    REQUIRE_FALSE(fs::exists(install / ui::cpp_binary_name()));

    fs::remove_all(extracted);
    fs::remove_all(install);
}
