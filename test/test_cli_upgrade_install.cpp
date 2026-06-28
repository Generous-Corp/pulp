// Regression coverage for the C++ `pulp upgrade` install helper.
//
// Older pre-cutover C++ CLIs can upgrade straight into a Rust CLI archive.
// That archive contains `pulp` plus sibling artifacts such as `pulp-cpp` and
// the wgpu runtime library; copying only `pulp` strands the new Rust CLI
// without its C++ fallthrough delegate (#1673).

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

void write_file_create_parent(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    write_file(path, body);
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

TEST_CASE("upgrade install skips directories, primary binary, and downloaded archive",
          "[cli][upgrade][issue-643]") {
    auto extracted = make_tmpdir("skip");
    auto install = make_tmpdir("skip-install");

    auto primary = extracted / ui::primary_binary_name();
    auto archive = extracted / "pulp-test.tar.gz";
    auto readme = extracted / "README.txt";
    auto nested = extracted / "nested";

    write_file(primary, "primary");
    write_file(archive, "archive");
    write_file(readme, "readme");
    fs::create_directories(nested);
    write_file_create_parent(nested / "ignored.txt", "ignored");

    auto installed = ui::install_sibling_payloads(extracted, install, primary, archive);

    REQUIRE(installed.size() == 1);
    REQUIRE(installed[0].filename() == "README.txt");
    REQUIRE(read_file(install / "README.txt") == "readme");
    REQUIRE_FALSE(fs::exists(install / primary.filename()));
    REQUIRE_FALSE(fs::exists(install / archive.filename()));
    REQUIRE_FALSE(fs::exists(install / "nested"));

    fs::remove_all(extracted);
    fs::remove_all(install);
}

TEST_CASE("upgrade install detects cpp delegate only by exact filename",
          "[cli][upgrade][issue-643]") {
    REQUIRE(ui::installed_cpp_delegate({fs::path{"bin"} / ui::cpp_binary_name()}));
    REQUIRE_FALSE(ui::installed_cpp_delegate({fs::path{"bin"} / "pulp-cpp-helper"}));
    REQUIRE_FALSE(ui::installed_cpp_delegate({}));
}

TEST_CASE("upgrade install same_path falls back to lexical normalization",
          "[cli][upgrade][issue-643]") {
    auto root = make_tmpdir("same-path");
    auto path = root / "a" / ".." / "artifact";
    auto normalized = root / "artifact";

    REQUIRE(ui::same_path(path, normalized));
    REQUIRE_FALSE(ui::same_path(root / "artifact-one", root / "artifact-two"));

    fs::remove_all(root);
}

TEST_CASE("upgrade install executable permission policy covers binary names and source bits",
          "[cli][upgrade][issue-643]") {
    auto root = make_tmpdir("perms");
    auto primary = root / ui::primary_binary_name();
    auto cpp = root / ui::cpp_binary_name();
    auto plain = root / "plain.txt";
    auto executable = root / "helper";

    write_file(primary, "primary");
    write_file(cpp, "cpp");
    write_file(plain, "plain");
    write_file(executable, "helper");

    REQUIRE(ui::should_add_exec_permissions(primary));
    REQUIRE(ui::should_add_exec_permissions(cpp));

#ifndef _WIN32
    REQUIRE_FALSE(ui::should_add_exec_permissions(plain));
    fs::permissions(executable, fs::perms::owner_exec, fs::perm_options::add);
    REQUIRE(ui::has_any_exec_bit(executable));
    REQUIRE(ui::should_add_exec_permissions(executable));

    ui::add_exec_permissions(plain);
    REQUIRE(ui::has_any_exec_bit(plain));
#else
    REQUIRE_FALSE(ui::should_add_exec_permissions(plain));
    REQUIRE_FALSE(ui::has_any_exec_bit(executable));
#endif

    fs::remove_all(root);
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

// ── ensure_dir_on_path: `pulp upgrade` self-heals PATH ──────────────────────
// Covers the gap where a CLI first installed via a source/SDK-prefix install
// (e.g. ~/pulp-sdk/bin) upgraded successfully yet still hit "command not found"
// in a fresh shell because nothing added the dir to a shell profile.

using PS = ui::PathEnsureOutcome::Status;

TEST_CASE("ensure_dir_on_path: appends an export line for a fresh zsh profile",
          "[cli][upgrade][path]") {
    auto home = make_tmpdir("path-zsh-home");
    fs::path dir = "/Users/someone/pulp-sdk/bin";
    auto r = ui::ensure_dir_on_path(dir, "/usr/bin:/bin", "zsh", home, false);
    REQUIRE(r.status == PS::added);
    REQUIRE(r.profile == home / ".zshrc");
    auto body = read_file(home / ".zshrc");
    REQUIRE(body.find("export PATH=\"" + dir.string() + ":$PATH\"") != std::string::npos);

    // Idempotent: a second call must not double-add.
    auto r2 = ui::ensure_dir_on_path(dir, "/usr/bin:/bin", "zsh", home, false);
    REQUIRE(r2.status == PS::already_in_profile);
    fs::remove_all(home);
}

TEST_CASE("ensure_dir_on_path: no-op when the dir is already on PATH",
          "[cli][upgrade][path]") {
    auto home = make_tmpdir("path-onpath-home");
    fs::path dir = "/opt/pulp/bin";
    auto r = ui::ensure_dir_on_path(dir, "/usr/bin:" + dir.string() + ":/bin",
                                    "zsh", home, false);
    REQUIRE(r.status == PS::already_on_path);
    REQUIRE_FALSE(fs::exists(home / ".zshrc"));
    fs::remove_all(home);
}

TEST_CASE("ensure_dir_on_path: PULP_NO_MODIFY_PATH opts out",
          "[cli][upgrade][path]") {
    auto home = make_tmpdir("path-optout-home");
    auto r = ui::ensure_dir_on_path("/opt/pulp/bin", "/usr/bin", "zsh", home, true);
    REQUIRE(r.status == PS::skipped_opt_out);
    REQUIRE_FALSE(fs::exists(home / ".zshrc"));
    fs::remove_all(home);
}

TEST_CASE("ensure_dir_on_path: bash prefers an existing .bash_profile",
          "[cli][upgrade][path]") {
    auto home = make_tmpdir("path-bash-home");
    write_file(home / ".bash_profile", "# existing\n");
    auto r = ui::ensure_dir_on_path("/opt/pulp/bin", "/usr/bin", "bash", home, false);
    REQUIRE(r.status == PS::added);
    REQUIRE(r.profile == home / ".bash_profile");
    fs::remove_all(home);
}

TEST_CASE("ensure_dir_on_path: empty dir and missing HOME are handled",
          "[cli][upgrade][path]") {
    auto r_empty = ui::ensure_dir_on_path("", "/usr/bin", "zsh", "/home/x", false);
    REQUIRE(r_empty.status == PS::empty_dir);
    auto r_nohome = ui::ensure_dir_on_path("/opt/pulp/bin", "/usr/bin", "zsh", "", false);
    REQUIRE(r_nohome.status == PS::no_home);
}
