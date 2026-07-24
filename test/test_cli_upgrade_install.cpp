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

TEST_CASE("profile values safely quote shell metacharacters",
          "[cli][upgrade][mcp]") {
    const std::string value = "a$`\"\\b'c";
    REQUIRE(ui::quote_sh_profile_value(value) ==
            R"('a$`"\b'\''c')");
    REQUIRE(ui::quote_fish_profile_value(value) ==
            R"('a$`"\\b\'c')");

    auto home = make_tmpdir("quoted-path-home");
    const fs::path install_dir = home / value;
    auto path =
        ui::ensure_dir_on_path(install_dir, "/usr/bin", "zsh", home, false);
    REQUIRE(path.line ==
            "export PATH=" + ui::quote_sh_profile_value(install_dir.string()) +
                ":$PATH");
    fs::remove_all(home);
}

TEST_CASE("active PATH detection accepts legacy ordering but ignores comments",
          "[cli][upgrade][mcp]") {
    const std::string dir = "/opt/Pulp SDK/bin";
    REQUIRE(ui::has_active_path_assignment(
        "export PATH=\"" + dir + ":$PATH\"\n", dir));
    REQUIRE(ui::has_active_path_assignment(
        "export PATH=\"$PATH:" + dir + "\"\n", dir));
    REQUIRE(ui::has_active_path_assignment(
        "set -gx PATH $PATH '" + dir + "'\n", dir));
    REQUIRE_FALSE(ui::has_active_path_assignment(
        "# export PATH=\"" + dir + ":$PATH\"\n", dir));
    REQUIRE_FALSE(ui::has_active_path_assignment(
        "echo 'PATH=" + dir + "'\n", dir));
}

TEST_CASE("active MCP assignment detection ignores comments, docs, and unsets",
          "[cli][upgrade][mcp]") {
    REQUIRE_FALSE(ui::has_active_mcp_assignment(
        "# export PULP_MCP_BINARY=/commented\n"
        "unset PULP_MCP_BINARY\n"
        "echo 'set PULP_MCP_BINARY for Claude'\n"
        "set -e PULP_MCP_BINARY\n"
        "set -q PULP_MCP_BINARY\n"
        "set OTHER PULP_MCP_BINARY\n"
        "set -gx PATH /usr/bin\n"));
    REQUIRE(ui::has_active_mcp_assignment(
        "export PULP_MCP_BINARY='/custom/pulp-mcp'\n"));
    REQUIRE(ui::has_active_mcp_assignment(
        "PULP_MCP_BINARY=/custom/pulp-mcp\n"));
    REQUIRE(ui::has_active_mcp_assignment(
        "set -gx PATH /usr/bin\n"
        "set -gx PULP_MCP_BINARY /custom/pulp-mcp\n"));

    auto home = make_tmpdir("mcp-env-text-mention-home");
    const auto profile = home / ".zshrc";
    write_file(profile,
               "# export PULP_MCP_BINARY=/commented\n"
               "unset PULP_MCP_BINARY\n");
    const auto result = ui::ensure_mcp_binary_env(
        "/installed/pulp-mcp", "zsh", home, false);
    REQUIRE(result.status == PS::added);
    REQUIRE(read_file(profile).find(
                "export PULP_MCP_BINARY='/installed/pulp-mcp'") !=
            std::string::npos);
    fs::remove_all(home);
}

TEST_CASE("Windows MCP ownership distinguishes fresh managed and user paths",
          "[cli][upgrade][mcp][windows]") {
    using A = ui::WindowsMcpEnvAction;
    REQUIRE(ui::decide_windows_mcp_env_action(L"", L"") ==
            A::configure_fresh);
    REQUIRE(ui::decide_windows_mcp_env_action(
                L"C:\\Pulp\\old\\pulp-mcp.exe",
                L"C:\\Pulp\\old\\pulp-mcp.exe") ==
            A::update_managed);
    REQUIRE(ui::decide_windows_mcp_env_action(
                L"D:\\custom\\pulp-mcp.exe", L"") ==
            A::preserve_user_override);
    REQUIRE(ui::decide_windows_mcp_env_action(
                L"D:\\custom\\pulp-mcp.exe",
                L"C:\\Pulp\\old\\pulp-mcp.exe") ==
            A::preserve_user_override);
}

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

TEST_CASE("ensure_mcp_binary_env: appends an exact source path and is idempotent",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-home");
    fs::path binary = home / "build" / "tools" / "mcp" / ui::mcp_binary_name();
    auto r = ui::ensure_mcp_binary_env(binary, "zsh", home, false);
    REQUIRE(r.status == PS::added);
    REQUIRE(r.line == "export PULP_MCP_BINARY='" + binary.string() + "'");
    REQUIRE(read_file(home / ".zshrc").find(r.line) != std::string::npos);

    auto r2 = ui::ensure_mcp_binary_env(binary, "zsh", home, false);
    REQUIRE(r2.status == PS::added);
    auto content = read_file(home / ".zshrc");
    REQUIRE(content.find(r.line) != std::string::npos);
    REQUIRE(content.find(r.line, content.find(r.line) + 1) == std::string::npos);

    fs::path moved = home / "custom path" / ui::mcp_binary_name();
    auto r3 = ui::ensure_mcp_binary_env(moved, "zsh", home, false);
    REQUIRE(r3.status == PS::added);
    content = read_file(home / ".zshrc");
    REQUIRE(content.find(moved.string()) != std::string::npos);
    REQUIRE(content.find(binary.string()) == std::string::npos);
    fs::remove_all(home);
}

TEST_CASE("ensure_mcp_binary_env: fish, opt-out, and missing HOME are explicit",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-fish-home");
    fs::path binary = "/opt/Pulp SDK/bin/pulp-mcp";
    auto fish = ui::ensure_mcp_binary_env(binary, "fish", home, false);
    REQUIRE(fish.status == PS::added);
    REQUIRE(fish.line ==
            "set -gx PULP_MCP_BINARY '/opt/Pulp SDK/bin/pulp-mcp'");
    REQUIRE(fish.profile == home / ".config" / "fish" / "config.fish");

    auto skipped = ui::ensure_mcp_binary_env(binary, "zsh", home, true);
    REQUIRE(skipped.status == PS::skipped_opt_out);
    auto no_home = ui::ensure_mcp_binary_env(binary, "zsh", "", false);
    REQUIRE(no_home.status == PS::no_home);
    fs::remove_all(home);
}

TEST_CASE("ensure_mcp_binary_env configures login and non-login Bash profiles once",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-bash-home");
    write_file(home / ".bash_profile", "# login profile\n");
    write_file(home / ".bashrc", "# interactive profile\n");
    const fs::path binary = home / "bin" / ui::mcp_binary_name();

    const auto first =
        ui::ensure_mcp_binary_env(binary, "bash", home, false);
    const auto second =
        ui::ensure_mcp_binary_env(binary, "bash", home, false);

    REQUIRE(first.status == PS::added);
    REQUIRE(second.status == PS::added);
    const auto expected =
        "if [ -z \"${PULP_MCP_BINARY+x}\" ]; then export "
        "PULP_MCP_BINARY='" + binary.string() + "'; fi";
    for (const auto& profile :
         {home / ".bash_profile", home / ".bashrc"}) {
        const auto content = read_file(profile);
        REQUIRE(content.find(expected) != std::string::npos);
        REQUIRE(content.find(expected, content.find(expected) + 1) ==
                std::string::npos);
        const std::string marker =
            "# >>> Pulp MCP (managed by installer) >>>";
        REQUIRE(content.find(marker, content.find(marker) + 1) ==
                std::string::npos);
    }
    fs::remove_all(home);
}

TEST_CASE("ensure_mcp_binary_env uses profile fallback for Bash login shells",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-bash-profile-fallback-home");
    const auto binary = home / "bin" / ui::mcp_binary_name();

    const auto result =
        ui::ensure_mcp_binary_env(binary, "bash", home, false);

    REQUIRE(result.status == PS::added);
    const auto expected =
        "if [ -z \"${PULP_MCP_BINARY+x}\" ]; then export "
        "PULP_MCP_BINARY='" + binary.string() + "'; fi";
    REQUIRE(read_file(home / ".bashrc").find(expected) !=
            std::string::npos);
    REQUIRE(read_file(home / ".profile").find(expected) !=
            std::string::npos);
    REQUIRE_FALSE(fs::exists(home / ".bash_profile"));
    fs::remove_all(home);
}

TEST_CASE("ensure_mcp_binary_env preserves a Bash override across sourced profiles",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-bash-cross-profile-override-home");
    write_file(home / ".bashrc",
               "export PULP_MCP_BINARY='/user/source-build/pulp-mcp'\n");
    write_file(home / ".bash_profile", ". \"$HOME/.bashrc\"\n");
    const auto binary = home / "bin" / ui::mcp_binary_name();

    const auto result =
        ui::ensure_mcp_binary_env(binary, "bash", home, false);

    REQUIRE(result.status == PS::added);
    REQUIRE(read_file(home / ".bashrc").find(
                "# >>> Pulp MCP (managed by installer) >>>") ==
            std::string::npos);
    REQUIRE(read_file(home / ".bash_profile").find(
                "if [ -z \"${PULP_MCP_BINARY+x}\" ]") !=
            std::string::npos);
    fs::remove_all(home);
}

#ifndef _WIN32
TEST_CASE("ensure_mcp_binary_env updates a symlink target without replacing the link",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-symlink-home");
    const auto dotfiles = home / "dotfiles";
    fs::create_directories(dotfiles);
    const auto target = dotfiles / "zshrc";
    write_file(target,
               "# >>> Pulp MCP (managed by installer) >>>\n"
               "export PULP_MCP_BINARY=\"/old/pulp-mcp\"\n"
               "# <<< Pulp MCP (managed by installer) <<<\n"
               "tail-must-survive\n");
    fs::create_symlink(fs::path("dotfiles") / "zshrc", home / ".zshrc");

    const auto binary = home / "custom path" / ui::mcp_binary_name();
    auto result = ui::ensure_mcp_binary_env(binary, "zsh", home, false);

    REQUIRE(result.status == PS::added);
    REQUIRE(fs::is_symlink(home / ".zshrc"));
    const auto content = read_file(target);
    REQUIRE(content.find(binary.string()) != std::string::npos);
    REQUIRE(content.find("/old/pulp-mcp") == std::string::npos);
    REQUIRE(content.find("tail-must-survive") != std::string::npos);
    fs::remove_all(home);
}
#endif

TEST_CASE("ensure_mcp_binary_env preserves a malformed managed block and tail",
          "[cli][upgrade][mcp]") {
    const std::vector<std::string> malformed_profiles{
        "before\n"
        "# >>> Pulp MCP (managed by installer) >>>\n"
        "export PULP_MCP_BINARY=\"/old/pulp-mcp\"\n"
        "tail-must-survive\n",
        "before\n"
        "# <<< Pulp MCP (managed by installer) <<<\n"
        "export PULP_MCP_BINARY=\"/old/pulp-mcp\"\n"
        "# >>> Pulp MCP (managed by installer) >>>\n"
        "tail-must-survive\n",
        "before\n"
        "# >>> Pulp MCP (managed by installer) >>>\n"
        "# >>> Pulp MCP (managed by installer) >>>\n"
        "export PULP_MCP_BINARY=\"/old/pulp-mcp\"\n"
        "# <<< Pulp MCP (managed by installer) <<<\n"
        "tail-must-survive\n",
        "before\n"
        "# >>> Pulp MCP (managed by installer) >>>\n"
        "export PULP_MCP_BINARY=\"/old/pulp-mcp\"\n"
        "# <<< Pulp MCP (managed by installer) <<<\n"
        "# <<< Pulp MCP (managed by installer) <<<\n"
        "tail-must-survive\n",
    };

    std::size_t index = 0;
    for (const auto& original : malformed_profiles) {
        auto home =
            make_tmpdir("mcp-env-malformed-home-" + std::to_string(index++));
        const auto profile = home / ".zshrc";
        write_file(profile, original);
        auto result = ui::ensure_mcp_binary_env(
            "/new/pulp-mcp", "zsh", home, false);
        REQUIRE(result.status == PS::malformed_profile);
        REQUIRE(read_file(profile) == original);
        fs::remove_all(home);
    }
}

TEST_CASE("ensure_mcp_binary_env surfaces malformed Bash profile over successful sibling",
          "[cli][upgrade][mcp]") {
    auto home = make_tmpdir("mcp-env-malformed-bash-home");
    write_file(home / ".bashrc", "# interactive profile\n");
    const std::string malformed =
        "# >>> Pulp MCP (managed by installer) >>>\n"
        "export PULP_MCP_BINARY=\"/old/pulp-mcp\"\n"
        "tail-must-survive\n";
    write_file(home / ".bash_profile", malformed);

    const auto result = ui::ensure_mcp_binary_env(
        "/new/pulp-mcp", "bash", home, false);

    REQUIRE(result.status == PS::malformed_profile);
    REQUIRE(result.profile == home / ".bash_profile");
    REQUIRE(read_file(home / ".bash_profile") == malformed);
    REQUIRE(read_file(home / ".bashrc").find("/new/pulp-mcp") !=
            std::string::npos);
    fs::remove_all(home);
}

TEST_CASE("installed_mcp_server identifies the release sibling payload",
          "[cli][upgrade][mcp]") {
    std::vector<fs::path> installed{
        fs::path{"/tmp"} / ui::cpp_binary_name(),
        fs::path{"/tmp"} / ui::mcp_binary_name(),
    };
    REQUIRE(ui::installed_mcp_server(installed).filename() ==
            ui::mcp_binary_name());
    REQUIRE(ui::installed_mcp_server({}).empty());
}

TEST_CASE("Windows upgrade persists MCP path without command-shell expansion",
          "[cli][upgrade][mcp][windows]") {
    const auto source = read_file(
        fs::path{PULP_REPO_ROOT} / "tools" / "cli" / "cmd_upgrade.cpp");

    REQUIRE(source.find("RegCreateKeyExW(") != std::string::npos);
    REQUIRE(source.find("RegSetValueExW(") != std::string::npos);
    REQUIRE(source.find("REG_SZ") != std::string::npos);
    REQUIRE(source.find(R"(L"Software\\Pulp")") != std::string::npos);
    REQUIRE(source.find(R"(L"ManagedPulpMcpBinary")") !=
            std::string::npos);
    REQUIRE(source.find("setx PULP_MCP_BINARY") == std::string::npos);
}
