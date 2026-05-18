#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#include "../tools/cli/cmd_project.cpp"

namespace pulp::cli::migration {
const MigrationEntry* const kMigrationIndex = nullptr;
const std::size_t kMigrationIndexSize = 0;
}

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path();
        path = base / fs::path("pulp-project-command-test-" +
                               std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                               std::to_string(std::chrono::steady_clock::now()
                                                  .time_since_epoch().count()));
        fs::create_directories(path);
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedEnv {
    std::string name;
    bool had_old = false;
    std::string old_value;

    ScopedEnv(std::string env_name, const std::string& value) : name(std::move(env_name)) {
        if (auto old = std::getenv(name.c_str())) {
            had_old = true;
            old_value = old;
        }
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedEnv() {
#if defined(_WIN32)
        _putenv_s(name.c_str(), had_old ? old_value.c_str() : "");
#else
        if (had_old) {
            setenv(name.c_str(), old_value.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }
};

struct ScopedCwd {
    fs::path old;

    explicit ScopedCwd(const fs::path& path) : old(fs::current_path()) {
        fs::current_path(path);
    }

    ~ScopedCwd() {
        fs::current_path(old);
    }
};

struct CapturedStreams {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = nullptr;
    std::streambuf* old_err = nullptr;

    CapturedStreams() {
        old_out = std::cout.rdbuf(out.rdbuf());
        old_err = std::cerr.rdbuf(err.rdbuf());
    }

    ~CapturedStreams() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
    }
};

struct ScopedStdin {
    std::istringstream input;
    std::streambuf* old_in = nullptr;

    explicit ScopedStdin(std::string text) : input(std::move(text)) {
        old_in = std::cin.rdbuf(input.rdbuf());
    }

    ~ScopedStdin() {
        std::cin.rdbuf(old_in);
    }
};

struct ScopedColorDisabled {
    bool old_color = g_color_enabled;

    ScopedColorDisabled() {
        g_color_enabled = false;
    }

    ~ScopedColorDisabled() {
        g_color_enabled = old_color;
    }
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.is_open());
    f << body;
    REQUIRE(f.good());
}

std::string read_file_text(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

std::string normalize_newlines(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

std::string quote(const fs::path& path) {
    return shell_quote(path.string());
}

void require_run_ok(const std::string& cmd) {
    INFO(cmd);
    REQUIRE(run(cmd) == 0);
}

void configure_git_identity(const fs::path& repo) {
    require_run_ok("git -C " + quote(repo) + " config user.name \"Pulp Test\"");
    require_run_ok("git -C " + quote(repo) + " config user.email \"pulp-test@example.com\"");
}

void make_standalone_project(const fs::path& root,
                             const std::string& sdk_version,
                             const std::string& sdk_path = {},
                             const std::string& sdk_checkout = {}) {
    fs::create_directories(root);
    write_file(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\n"
               "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
               "find_package(Pulp " + sdk_version + " REQUIRED)\n");

    std::ostringstream toml;
    toml << "[pulp]\n";
    toml << "sdk_version = \"" << sdk_version << "\"\n";
    if (!sdk_path.empty()) toml << "sdk_path = \"" << sdk_path << "\"\n";
    if (!sdk_checkout.empty()) toml << "sdk_checkout = \"" << sdk_checkout << "\"\n";
    write_file(root / "pulp.toml", toml.str());
}

void make_fetchcontent_project(const fs::path& root, const std::string& sdk_version) {
    fs::create_directories(root);
    write_file(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\n"
               "include(FetchContent)\n"
               "FetchContent_Declare(pulp\n"
               "    GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n"
               "    GIT_TAG v" + sdk_version + ")\n"
               "FetchContent_MakeAvailable(pulp)\n"
               "pulp_add_project(Clock VERSION 1.0.0)\n");
}

void make_fake_sdk(const fs::path& sdk_root,
                   const std::string& version,
                   bool write_version_file = true) {
    auto cmake_dir = sdk_root / "lib" / "cmake" / "Pulp";
    fs::create_directories(cmake_dir);
    write_file(cmake_dir / "PulpConfig.cmake",
               "set(Pulp_FOUND TRUE)\n");
    write_file(cmake_dir / "PulpConfigVersion.cmake",
               "set(PACKAGE_VERSION \"" + version + "\")\n"
               "if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)\n"
               "  set(PACKAGE_VERSION_COMPATIBLE FALSE)\n"
               "else()\n"
               "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
               "  if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)\n"
               "    set(PACKAGE_VERSION_EXACT TRUE)\n"
               "  endif()\n"
               "endif()\n");
    if (write_version_file) {
        write_file(sdk_root / "version.txt", version + "\n");
    }
}

void init_clean_git_repo(const fs::path& repo) {
    require_run_ok("git init -q " + quote(repo));
    configure_git_identity(repo);
    require_run_ok("git -C " + quote(repo) + " add CMakeLists.txt pulp.toml");
    require_run_ok("git -C " + quote(repo) + " commit -q -m \"initial\"");
}

}  // namespace

TEST_CASE("cmd_project reports help and unknown subcommands deterministically",
          "[cli][project-command][issue-643]") {
    {
        CapturedStreams capture;
        REQUIRE(cmd_project({}) == 1);
        REQUIRE(capture.out.str().find("pulp project") != std::string::npos);
        REQUIRE(capture.out.str().find("Usage:") != std::string::npos);
    }

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"help"}) == 0);
        REQUIRE(capture.out.str().find("pulp project") != std::string::npos);
    }

    {
        // Pulp #2087: `bump` is now a deprecated alias for `pin`. The
        // help body keeps both names visible — the title is "pulp
        // project pin" with "(alias: `pulp project bump`)" — so the
        // bump-help substring still appears, and the flag list still
        // includes --verify-builds.
        CapturedStreams capture;
        REQUIRE(cmd_project({"bump", "--help"}) == 0);
        REQUIRE(capture.out.str().find("pulp project bump") != std::string::npos);
        REQUIRE(capture.out.str().find("--verify-builds") != std::string::npos);
    }

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"pin", "--help"}) == 0);
        REQUIRE(capture.out.str().find("pulp project pin") != std::string::npos);
        REQUIRE(capture.out.str().find("--verify-builds") != std::string::npos);
    }

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"unpin", "--help"}) == 0);
        REQUIRE(capture.out.str().find("pulp project unpin") != std::string::npos);
        REQUIRE(capture.out.str().find("floating") != std::string::npos);
    }

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"undo", "--help"}) == 0);
        REQUIRE(capture.out.str().find("pulp project undo") != std::string::npos);
    }

    {
        // The unknown-subcommand redirect now points at the renamed
        // primary `pin` help, not the deprecated `bump` alias.
        CapturedStreams capture;
        REQUIRE(cmd_project({"not-a-command"}) == 2);
        REQUIRE(capture.err.str().find("unknown subcommand 'not-a-command'")
                != std::string::npos);
        REQUIRE(capture.out.str().find("Run `pulp project pin --help`")
                != std::string::npos);
    }
}

TEST_CASE("cmd_project bump option parser accepts flags and rejects unknown arguments",
          "[cli][project-command][issue-643]") {
    bool help = false;
    CapturedStreams capture;
    auto opts = parse_bump_options({
        "--all",
        "--dry-run",
        "--force-dirty",
        "--allow-downgrade",
        "--allow-cli-skew",
        "--allow-redundant",
        "--verify-builds",
        "--to=1.2.3",
        "legacy-positional",
        "--surprise",
    }, help);

    REQUIRE_FALSE(help);
    REQUIRE(opts.all);
    REQUIRE(opts.dry_run);
    REQUIRE(opts.force_dirty);
    REQUIRE(opts.allow_downgrade);
    REQUIRE(opts.allow_cli_skew);
    REQUIRE(opts.allow_redundant);
    REQUIRE(opts.verify_builds);
    REQUIRE(opts.to_version == "1.2.3");
    REQUIRE(opts.positional == std::vector<std::string>{"legacy-positional"});
    REQUIRE(opts.error.find("unknown argument '--surprise'")
            != std::string::npos);
    REQUIRE(capture.err.str().empty());
}

TEST_CASE("cmd_project bump --all reports empty registries without touching projects",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    ScopedEnv pulp_home("PULP_HOME", (tmp.path / "home").string());
    CapturedStreams capture;

    REQUIRE(cmd_project({"bump", "--all", "--to", "0.2.0"}) == 1);
    REQUIRE(capture.err.str().find("registry is empty") != std::string::npos);
    REQUIRE_FALSE(fs::exists(tmp.path / "home" / "projects.json"));
}

TEST_CASE("cmd_project bump --all dry-run uses registry names and avoids undo files",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto standalone = tmp.path / "Clock";
    auto fetch = tmp.path / "FetchClock";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    make_standalone_project(standalone, "0.1.0");
    make_fetchcontent_project(fetch, "0.1.0");
    REQUIRE(prjreg::write_registry(prjreg::registry_path(), {
        prjreg::Project{standalone, "Clock Display", "2026-04-26T00:00:00Z"},
        prjreg::Project{fetch, "", "2026-04-26T00:00:01Z"},
    }));

    CapturedStreams capture;
    REQUIRE(cmd_project({
        "bump",
        "--all",
        "--to=0.2.0",
        "--dry-run",
        "--allow-cli-skew",
        "--allow-redundant",
    }) == 0);

    auto out = capture.out.str();
    REQUIRE(out.find("pulp project bump (dry run) target=0.2.0") != std::string::npos);
    REQUIRE(out.find("would bump") != std::string::npos);
    REQUIRE(out.find("Clock Display") != std::string::npos);
    REQUIRE(out.find("FetchClock") != std::string::npos);
    REQUIRE(out.find("Summary: 0 bumped, 2 would-bump, 0 skipped, 0 failed")
            != std::string::npos);
    REQUIRE(out.find("No migration notes apply for 0.1.0 -> 0.2.0")
            != std::string::npos);

    REQUIRE(read_file_text(standalone / "pulp.toml").find("sdk_version = \"0.1.0\"")
            != std::string::npos);
    REQUIRE(read_file_text(fetch / "CMakeLists.txt").find("GIT_TAG v0.1.0")
            != std::string::npos);
    REQUIRE(pb::list_undo_batches(home).empty());
}

TEST_CASE("cmd_project bump writes an undo batch and undo reverts the newest batch",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto project = tmp.path / "Clock";
    ScopedEnv pulp_home("PULP_HOME", home.string());
    make_standalone_project(project, "0.1.0");

    {
        ScopedCwd cwd(project);
        CapturedStreams capture;
        REQUIRE(cmd_project({"bump", "0.2.0", "--allow-redundant"}) == 0);
        REQUIRE(capture.out.str().find("Summary: 1 bumped, 0 would-bump, 0 skipped, 0 failed")
                != std::string::npos);
        REQUIRE(capture.out.str().find("Undo file:") != std::string::npos);
    }

    auto batches = pb::list_undo_batches(home);
    REQUIRE(batches.size() == 1);
    REQUIRE(read_file_text(project / "pulp.toml").find("sdk_version = \"0.2.0\"")
            != std::string::npos);
    REQUIRE(read_file_text(project / "CMakeLists.txt").find("find_package(Pulp 0.2.0 REQUIRED)")
            != std::string::npos);

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"undo"}) == 0);
        REQUIRE(capture.out.str().find("Reverting bump batch") != std::string::npos);
        REQUIRE(capture.out.str().find("Summary: 1 reverted, 0 skipped, 0 failed")
                != std::string::npos);
        REQUIRE(capture.out.str().find("Removed undo file") != std::string::npos);
    }

    REQUIRE(read_file_text(project / "pulp.toml").find("sdk_version = \"0.1.0\"")
            != std::string::npos);
    REQUIRE(read_file_text(project / "CMakeLists.txt").find("find_package(Pulp 0.1.0 REQUIRED)")
            != std::string::npos);
    REQUIRE(pb::list_undo_batches(home).empty());
}

TEST_CASE("cmd_project undo reports missing and malformed batches",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"undo"}) == 1);
        REQUIRE(capture.err.str().find("no bump batches on disk") != std::string::npos);
    }

    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"undo", "missing"}) == 1);
        REQUIRE(capture.err.str().find("no batch at") != std::string::npos);
    }

    write_file(pb::undo_batch_path(home, "bad"), "{not json\n");
    {
        CapturedStreams capture;
        REQUIRE(cmd_project({"undo", "bad"}) == 1);
        REQUIRE(capture.err.str().find("could not parse") != std::string::npos);
    }
}

TEST_CASE("cmd_project undo reports stale, missing, and non-bumped batch entries",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto stale = tmp.path / "Stale";
    ScopedEnv pulp_home("PULP_HOME", home.string());
    ScopedColorDisabled color;

    make_fetchcontent_project(stale, "0.3.0");

    pb::UndoBatch batch;
    batch.timestamp = "undo-edge";
    batch.target_version = "0.2.0";

    pb::UndoEntry stale_entry;
    stale_entry.project_path = stale;
    stale_entry.project_name = "Stale";
    stale_entry.old_pin = "v0.1.0";
    stale_entry.old_pin_style_has_v = true;
    stale_entry.pin_kind = pb::PinKind::FetchContentGitTag;
    stale_entry.status = "bumped";
    stale_entry.edits.push_back(pb::UndoEdit{
        stale / "CMakeLists.txt",
        pb::PinKind::FetchContentGitTag,
        "v0.1.0",
        "v0.2.0",
        true,
    });
    batch.entries.push_back(stale_entry);

    auto missing = tmp.path / "Missing";
    pb::UndoEntry missing_entry;
    missing_entry.project_path = missing;
    missing_entry.project_name = "Missing";
    missing_entry.old_pin = "v0.1.0";
    missing_entry.old_pin_style_has_v = true;
    missing_entry.pin_kind = pb::PinKind::FetchContentGitTag;
    missing_entry.status = "bumped";
    missing_entry.edits.push_back(pb::UndoEdit{
        missing / "CMakeLists.txt",
        pb::PinKind::FetchContentGitTag,
        "v0.1.0",
        "v0.2.0",
        true,
    });
    batch.entries.push_back(missing_entry);

    pb::UndoEntry skipped_entry;
    skipped_entry.project_path = tmp.path / "Skipped";
    skipped_entry.project_name = "Skipped";
    skipped_entry.status = "skipped";
    batch.entries.push_back(skipped_entry);

    auto undo_path = pb::undo_batch_path(home, batch.timestamp);
    REQUIRE(pb::write_undo_batch(undo_path, batch));

    CapturedStreams capture;
    REQUIRE(cmd_project({"undo", batch.timestamp}) == 1);

    REQUIRE(capture.err.str().find("Stale  (current value no longer matches bumped value)")
            != std::string::npos);
    REQUIRE(capture.err.str().find("Missing  (missing ") != std::string::npos);
    REQUIRE(capture.out.str().find("Summary: 0 reverted, 2 skipped, 1 failed")
            != std::string::npos);
    REQUIRE(capture.out.str().find("Undo file retained") != std::string::npos);
    REQUIRE(fs::exists(undo_path));
    REQUIRE(read_file_text(stale / "CMakeLists.txt").find("GIT_TAG v0.3.0")
            != std::string::npos);
}

TEST_CASE("cmd_project bump rejects source checkout and non-project directories",
          "[project-command][issue-244]") {
    CapturedStreams source_capture;
    {
        ScopedCwd cwd(fs::path(PULP_SOURCE_DIR));
        REQUIRE(cmd_project({"bump"}) == 1);
    }
    REQUIRE(source_capture.err.str().find("refusing to run inside the Pulp source checkout")
            != std::string::npos);

    TempDir tmp;
    CapturedStreams empty_capture;
    {
        ScopedCwd cwd(tmp.path);
        REQUIRE(cmd_project({"bump"}) == 1);
    }
    REQUIRE(empty_capture.err.str().find("not inside a bumpable Pulp project")
            != std::string::npos);
}

TEST_CASE("cmd_project bump rejects targets newer than the installed CLI",
          "[project-command][issue-244]") {
    TempDir tmp;
    CapturedStreams capture;
    {
        ScopedCwd cwd(tmp.path);
        REQUIRE(cmd_project({"bump", "99.0.0"}) == 1);
    }
    REQUIRE(capture.err.str().find("is newer than this pulp CLI") != std::string::npos);
    REQUIRE(capture.err.str().find("pulp upgrade 99.0.0") != std::string::npos);
}

TEST_CASE("cmd_project bump rejects invalid target versions before project resolution",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    CapturedStreams capture;
    {
        ScopedCwd cwd(tmp.path);
        REQUIRE(cmd_project({"bump", "--to=main"}) == 1);
    }
    REQUIRE(capture.err.str().find("invalid target version 'main'") != std::string::npos);
    REQUIRE(capture.err.str().find("not inside a bumpable Pulp project") == std::string::npos);

    CapturedStreams positional_capture;
    {
        ScopedCwd cwd(tmp.path);
        REQUIRE(cmd_project({"bump", "0.2"}) == 1);
    }
    REQUIRE(positional_capture.err.str().find("invalid target version '0.2'")
            != std::string::npos);
}

TEST_CASE("project command shell redirection helpers use platform null devices",
          "[project-command][issue-244]") {
#if defined(_WIN32)
    REQUIRE(std::string(stderr_to_null()) == " 2>NUL");
    REQUIRE(std::string(output_to_null()) == " >NUL 2>&1");
#else
    REQUIRE(std::string(stderr_to_null()) == " 2>/dev/null");
    REQUIRE(std::string(output_to_null()) == " >/dev/null 2>&1");
#endif
}

TEST_CASE("cli common parses numeric arguments and normalizes strings",
          "[cli][common][issue-643]") {
    CapturedStreams capture;

    std::size_t size = 0;
    REQUIRE(parse_size_arg("42", "--top", size));
    REQUIRE(size == 42);
    REQUIRE(parse_size_arg("0", "--top", size));
    REQUIRE(size == 0);
    REQUIRE(parse_size_arg("+7", "--top", size));
    REQUIRE(size == 7);
    REQUIRE_FALSE(parse_size_arg("", "--top", size));
    REQUIRE_FALSE(parse_size_arg("12x", "--top", size));
    REQUIRE_FALSE(parse_size_arg("-1", "--top", size));
    REQUIRE_FALSE(parse_size_arg("184467440737095516160", "--top", size));

    double value = 0.0;
    REQUIRE(parse_double_arg("0.125", "--min-score", value));
    REQUIRE(value == 0.125);
    REQUIRE(parse_double_arg("-12.5", "--min-score", value));
    REQUIRE(value == -12.5);
    REQUIRE(parse_double_arg("+12.5", "--min-score", value));
    REQUIRE(value == 12.5);
    REQUIRE(parse_double_arg("1e-3", "--min-score", value));
    REQUIRE(value == 0.001);
    REQUIRE_FALSE(parse_double_arg("", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("nan", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("inf", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("1e309", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("3.5x", "--min-score", value));

    REQUIRE(trim(" \t hello \n") == "hello");
    REQUIRE(trim("\r\n\t ") == "");
    REQUIRE(strip_quotes("\"quoted\"") == "quoted");
    REQUIRE(strip_quotes("'quoted'") == "quoted");
    REQUIRE(strip_quotes("\"mismatched'") == "\"mismatched'");
    REQUIRE(strip_quotes("unquoted") == "unquoted");
    REQUIRE(replace_all_str("one two one", "one", "1") == "1 two 1");
    TempDir tmp;
    write_file(tmp.path / "contents.txt", "file body");
    REQUIRE(read_file_contents(tmp.path / "contents.txt") == "file body");
    REQUIRE(icontains("Signalsmith DSP", "smith"));
    REQUIRE(icontains("Signalsmith DSP", "SIGNAL"));
    REQUIRE(icontains("Signalsmith DSP", ""));
    REQUIRE_FALSE(icontains("Signalsmith DSP", "delay"));
    REQUIRE(yaml_value("  target: plugin  # comment", "target") == "plugin  # comment");
    REQUIRE(yaml_value("prefix_target: wrong", "target").empty());
    REQUIRE(yaml_value("target:", "target").empty());
    REQUIRE(yaml_value("kind: guide", "target").empty());
    REQUIRE(sanitize_process_output(std::string("a\0b\0", 4)) == "ab");
    REQUIRE(truncate_message("abcdef", 4) == "abcd...");
    REQUIRE(truncate_message("abcdef", 0) == "...");
    REQUIRE(truncate_message("abcdef", 6) == "abcdef");
    REQUIRE(truncate_message("abc", 4) == "abc");
}

TEST_CASE("cli common path helpers find roots and constrain containment",
          "[cli][common][issue-643]") {
    TempDir tmp;
    auto repo = tmp.path / "repo";
    auto child = repo / "examples" / "demo";
    fs::create_directories(child);
    fs::create_directories(repo / "core");
    write_file(repo / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    auto cmake_file = repo / "CMakeLists.txt";

    auto standalone = tmp.path / "standalone" / "nested";
    fs::create_directories(standalone);
    write_file(tmp.path / "standalone" / "pulp.toml", "[pulp]\nsdk_version = \"1.2.3\"\n");

    REQUIRE(find_project_root_from(child) == repo);
    REQUIRE(find_project_root_from(cmake_file) == repo);
    REQUIRE(find_project_root_from(tmp.path / "missing") == fs::path{});
    {
        ScopedCwd cwd(child);
        REQUIRE(find_project_root_from({}) == repo);
    }
    REQUIRE(path_is_within(child, repo));
    REQUIRE(path_is_within(repo, repo));
    REQUIRE_FALSE(path_is_within(tmp.path, repo));
    REQUIRE_FALSE(path_is_within(tmp.path / "repo-sibling", repo));

    bool is_standalone = false;
    {
        ScopedCwd cwd(standalone);
        REQUIRE(find_standalone_root() == tmp.path / "standalone");
        REQUIRE(resolve_active_project_root(&is_standalone) == tmp.path / "standalone");
        REQUIRE(is_standalone);
    }
}

TEST_CASE("cli common config helpers honor PULP_HOME and project-dir overrides",
          "[cli][common][issue-643]") {
    TempDir tmp;
    ScopedEnv pulp_home_env("PULP_HOME", (tmp.path / "home").string());
    ScopedEnv projects_dir("PULP_PROJECTS_DIR", (tmp.path / "projects").string());

    REQUIRE(pulp_home() == tmp.path / "home");
    REQUIRE(read_user_config_value("create", "projects_dir").empty());
    REQUIRE(write_user_config_value("create", "projects_dir", "~/Pulp Projects"));
    REQUIRE(read_user_config_value("create", "projects_dir") == "~/Pulp Projects");
    REQUIRE(write_user_config_value("updates", "mode", "manual"));
    REQUIRE(read_user_config_value("updates", "mode") == "manual");
    REQUIRE(read_user_config_value("create", "projects_dir") == "~/Pulp Projects");

    auto repo = tmp.path / "repo";
    fs::create_directories(repo);
    REQUIRE(resolve_create_projects_base_dir(repo) == tmp.path / "projects");
}

TEST_CASE("cli common config parser skips comments malformed lines and other sections",
          "[cli][common][issue-643]") {
    TempDir tmp;
    ScopedEnv pulp_home_env("PULP_HOME", (tmp.path / "home").string());

    write_file(tmp.path / "home" / "config.toml",
               "  # leading comment\n"
               "\n"
               "[updates]\n"
               "projects_dir = \"ignored\"\n"
               "[create]\n"
               "malformed line without equals\n"
               "projects_dir = Bare Projects # trailing comment\n"
               "mode = 'manual'\n"
               "[other]\n"
               "mode = wrong\n");

    REQUIRE(read_user_config_value("create", "projects_dir") == "Bare Projects");
    REQUIRE(read_user_config_value("create", "mode") == "manual");
    REQUIRE(read_user_config_value("updates", "projects_dir") == "ignored");
    REQUIRE(read_user_config_value("missing", "projects_dir").empty());
    REQUIRE(read_user_config_value("create", "absent").empty());
}

TEST_CASE("cli common config fallback expands user and relative project directories",
          "[cli][common][issue-643]") {
    TempDir tmp;
    ScopedEnv home_env("HOME", (tmp.path / "home").string());
    ScopedEnv pulp_home_env("PULP_HOME", (tmp.path / "pulp-home").string());

    {
        ScopedEnv projects_dir("PULP_PROJECTS_DIR", "~/Pulp Projects");
        REQUIRE(resolve_create_projects_base_dir(tmp.path / "repo")
                == tmp.path / "home" / "Pulp Projects");
    }

    {
        ScopedCwd cwd(tmp.path);
        ScopedEnv projects_dir("PULP_PROJECTS_DIR", "relative-projects");
        REQUIRE(resolve_create_projects_base_dir(tmp.path / "repo")
                == fs::absolute("relative-projects"));
    }

    {
        ScopedEnv projects_dir("PULP_PROJECTS_DIR", "\"~\"");
        REQUIRE(resolve_create_projects_base_dir(tmp.path / "repo") == tmp.path / "home");
    }

    ScopedEnv no_projects_dir("PULP_PROJECTS_DIR", "");
    REQUIRE(write_user_config_value("create", "projects_dir", "~/Configured Projects"));
    REQUIRE(resolve_create_projects_base_dir(tmp.path / "repo")
            == tmp.path / "home" / "Configured Projects");

    REQUIRE(write_user_config_value("create", "projects_dir", "~"));
    REQUIRE(resolve_create_projects_base_dir(tmp.path / "repo") == tmp.path / "home");

    TempDir empty_config;
    ScopedEnv empty_home("PULP_HOME", (empty_config.path / "empty-home").string());
    ScopedCwd cwd(tmp.path);
    REQUIRE(resolve_create_projects_base_dir({}) == tmp.path);
}

TEST_CASE("cli common format, AAX, quoting, and fuzzy helpers stay deterministic",
          "[cli][common][issue-643]") {
    // shell_quote is platform-divergent (#776): POSIX escapes both `\`
    // and `"`; Windows uses MSVCRT-correct escaping (backslashes are
    // literal unless they precede `"`). Input chars: `a b"c\d`.
#ifdef _WIN32
    // Windows: `\` is literal, only `"` is escaped (with one preceding `\`).
    REQUIRE(shell_quote(std::string("a b\"c\\d")) == "\"a b\\\"c\\d\"");
#else
    // POSIX: both `\` and `"` are backslash-escaped.
    REQUIRE(shell_quote(std::string("a b\"c\\d")) == "\"a b\\\"c\\\\d\"");
#endif
    REQUIRE(platform_executable("tool").filename().string().find("tool") == 0);

    REQUIRE(default_create_formats({}, "app") == "Standalone");
    REQUIRE(default_create_formats({}, "bare") == "Standalone");
    auto plugin_formats = default_create_formats({}, "plugin");
    REQUIRE(plugin_formats.find("CLAP") != std::string::npos);
    REQUIRE(plugin_formats.find("Standalone") != std::string::npos);

    REQUIRE(aax_download_url().find("avid.com") != std::string::npos);
    REQUIRE(aax_sdk_download_label().find("AAX") != std::string::npos);
    REQUIRE(aax_validator_download_label().find("Validator") != std::string::npos);

    REQUIRE(fuzzy_score("package registry", "pgr") > 0);
    REQUIRE(fuzzy_score("package", "package") > fuzzy_score("package", "pkg"));
    REQUIRE(fuzzy_score("abc", "") == 1);
    REQUIRE(fuzzy_score("", "abc") == 0);
    REQUIRE(fuzzy_score("package", "zzz") == 0);
}

TEST_CASE("cli common project metadata and compatibility helpers cover edge paths",
          "[cli][common][issue-643]") {
    TempDir tmp;
    auto project = tmp.path / "Clock";

    REQUIRE(read_pulp_toml_value(project, "sdk_version").empty());
    REQUIRE(read_sdk_version(project) == PULP_SDK_VERSION);
    REQUIRE(read_sdk_path_hint(project).empty());
    REQUIRE(read_sdk_checkout_hint(project).empty());
    REQUIRE(read_project_cmake_version(project).empty());

    write_file(project / "pulp.toml",
               "[pulp]\n"
               "# sdk_version = \"0.1.0\"\n"
               "sdk_version = \"0.2.0\"\n"
               "sdk_path = \"/opt/pulp-sdk\"\n"
               "sdk_checkout = \"/src/pulp\"\n"
               "legacy_sdk_version_note = \"0.0.1\"\n");
    REQUIRE(read_pulp_toml_value(project, "sdk_version") == "0.2.0");
    REQUIRE(read_sdk_version(project) == "0.2.0");
    REQUIRE(read_sdk_path_hint(project) == fs::path("/opt/pulp-sdk"));
    REQUIRE(read_sdk_checkout_hint(project) == fs::path("/src/pulp"));
    REQUIRE(read_pulp_toml_value(project, "missing").empty());

    write_file(project / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\n"
               "project(Clock VERSION 1.2.3 LANGUAGES CXX)\n");
    REQUIRE(read_project_cmake_version(project) == "1.2.3");

    write_file(project / "pulp.toml", "[pulp]\nsdk_version = \"99.0.0\"\n");
    REQUIRE(enforce_project_cli_compatibility(project, "pulp build", true));
    CapturedStreams capture;
    REQUIRE_FALSE(enforce_project_cli_compatibility(project, "pulp build", false));
    REQUIRE(capture.err.str().find("requires a newer Pulp CLI") != std::string::npos);
    REQUIRE(capture.err.str().find("pulp upgrade") != std::string::npos);

    ScopedEnv skip_cache("PULP_SKIP_CACHE_PREFLIGHT", "1");
    REQUIRE(cache_preflight_check(project, "pulp build"));
    REQUIRE(cache_preflight_check({}, "pulp build"));
}

TEST_CASE("cli common standalone SDK resolution reports missing hints without materializing",
          "[cli][common][issue-643]") {
    TempDir tmp;
    ScopedEnv pulp_home("PULP_HOME", (tmp.path / "home").string());

    auto missing_path_project = tmp.path / "MissingPath";
    make_standalone_project(missing_path_project, "0.4.0", (tmp.path / "missing-sdk").generic_string());
    auto missing_path = resolve_standalone_sdk(missing_path_project, false);
    REQUIRE(missing_path.sdk_path_hint == tmp.path / "missing-sdk");
    REQUIRE_FALSE(missing_path.sdk_path_config_ready);
    REQUIRE(missing_path.resolved_sdk_dir.empty());

    auto checkout_project = tmp.path / "CheckoutOnly";
    auto checkout = tmp.path / "checkout";
    fs::create_directories(checkout);
    make_standalone_project(checkout_project, "0.5.0", {}, checkout.generic_string());
    auto checkout_only = resolve_standalone_sdk(checkout_project, false);
    REQUIRE(checkout_only.sdk_checkout_hint == checkout);
    REQUIRE(checkout_only.resolved_sdk_dir.empty());
}

TEST_CASE("cli common cached SDK and build helpers return without shelling out",
          "[cli][common][issue-643]") {
    TempDir tmp;
    ScopedEnv pulp_home("PULP_HOME", (tmp.path / "home").string());

    make_fake_sdk(sdk_cache_path("0.8.0"), "0.8.0");
    REQUIRE(ensure_sdk("0.8.0") == sdk_cache_path("0.8.0"));

    auto repo = tmp.path / "repo";
    fs::create_directories(repo / "core");
    write_file(repo / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\n"
               "project(PulpFixture VERSION 0.8.0 LANGUAGES CXX)\n");

    make_fake_sdk(local_sdk_cache_path("0.9.0"), "0.9.0");
    REQUIRE(ensure_checkout_sdk(repo, "0.9.0") == local_sdk_cache_path("0.9.0"));

    auto build = tmp.path / "build";
    fs::create_directories(build);
    write_file(build / "CMakeCache.txt",
               "CMAKE_HOME_DIRECTORY:INTERNAL=" + repo.string() + "\n");

    REQUIRE(cmake_home_directory(build) == repo);
    REQUIRE(ensure_repo_build_configured(repo, build) == 0);
}

TEST_CASE("cli common AAX helpers classify SDKs, bundles, and validator output",
          "[cli][common][issue-643]") {
    TempDir tmp;
    REQUIRE_FALSE(looks_like_aax_sdk_root({}));

    auto sdk = tmp.path / "sdk";
    write_file(sdk / "Interfaces" / "AAX.h", "// fake\n");
    write_file(sdk / "Interfaces" / "AAX_Exports.cpp", "// fake\n");
    REQUIRE(looks_like_aax_sdk_root(sdk));
    {
        ScopedEnv aax_sdk("PULP_AAX_SDK_DIR", "\"" + sdk.string() + "\"");
        REQUIRE(find_aax_sdk_root() == fs::absolute(sdk));
    }

    REQUIRE_FALSE(bundle_contains_payload({}));
    auto loose = tmp.path / "Loose.aaxplugin";
    write_file(loose, "binary");
    REQUIRE(bundle_contains_payload(loose));

    auto bundle = tmp.path / "Clock.aaxplugin";
    write_file(bundle / "Contents" / "MacOS" / "Clock", "binary");
    REQUIRE(bundle_contains_payload(bundle));

    auto empty_bundle = tmp.path / "Empty.aaxplugin";
    write_file(empty_bundle / "Contents" / "MacOS" / "Other", "binary");
    REQUIRE_FALSE(bundle_contains_payload(empty_bundle));

    auto validator = tmp.path / "validator";
    auto commandline = validator / "CommandLineTools";
    write_file(platform_executable(commandline / "dsh"), "");
#ifdef __APPLE__
    write_file(commandline / "Dishes" / "aaxval.dish" / "Contents" / "MacOS" / "aaxval", "");
#else
    fs::create_directories(commandline / "Dishes" / "aaxval.dish");
#endif
    {
        ScopedEnv aax_validator("PULP_AAX_VALIDATOR_DIR", validator.string());
        REQUIRE(find_aax_validator_root() == fs::absolute(validator));
    }

    {
        CapturedStreams capture;
        print_aax_setup_guidance(true, false);
        auto out = capture.out.str();
        REQUIRE(out.find(aax_sdk_download_label()) != std::string::npos);
        REQUIRE(out.find(aax_validator_download_label()) == std::string::npos);
        REQUIRE(out.find("PULP_AAX_SDK_DIR") != std::string::npos);
        REQUIRE(out.find("PULP_AAX_VALIDATOR_DIR") == std::string::npos);
    }

    {
        CapturedStreams capture;
        print_aax_setup_guidance(false, true);
        auto out = capture.out.str();
        REQUIRE(out.find(aax_sdk_download_label()) == std::string::npos);
        REQUIRE(out.find(aax_validator_download_label()) != std::string::npos);
        REQUIRE(out.find("PULP_AAX_SDK_DIR") == std::string::npos);
        REQUIRE(out.find("PULP_AAX_VALIDATOR_DIR") != std::string::npos);
    }

    auto temp_note = write_temp_text_file("pulp-cli-common-test", "temporary note\n");
    REQUIRE(temp_note.filename().string().find("pulp-cli-common-test-") == 0);
    REQUIRE(temp_note.extension() == ".txt");
    REQUIRE(normalize_newlines(read_file_text(temp_note)) == "temporary note\n");
    std::error_code remove_ec;
    fs::remove(temp_note, remove_ec);

    REQUIRE(run_aax_validator_command({}, tmp.path / "Plugin.aaxplugin", false).empty());
    REQUIRE(aax_validator_passed("result_status: E_COMPLETED_PASS"));
    REQUIRE(aax_validator_passed("12 passed, 0 failed, 3 warnings, 0 cancelled"));
    REQUIRE_FALSE(aax_validator_passed("12 passed, 1 failed, 0 warnings, 0 cancelled"));
    REQUIRE_FALSE(aax_validator_passed("result_status: E_COMPLETED_FAIL"));
    REQUIRE_FALSE(aax_validator_passed("failed to complete"));
}

TEST_CASE("cli common delegates fail cleanly before shelling out",
          "[cli][common][issue-643]") {
    TempDir tmp;

    {
        ScopedCwd cwd(tmp.path);
        CapturedStreams capture;
        REQUIRE(delegate_to_python_script("tools/missing.py", {}) == 1);
        REQUIRE(capture.err.str().find("not in a Pulp project directory") != std::string::npos);
    }

    auto repo = tmp.path / "repo";
    fs::create_directories(repo / "core");
    write_file(repo / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

    {
        ScopedCwd cwd(repo);
        CapturedStreams capture;
        REQUIRE(delegate_to_python_script("tools/missing.py", {"--flag"}) == 1);
        REQUIRE(capture.err.str().find("script not found") != std::string::npos);
    }

    {
        ScopedCwd cwd(repo);
        CapturedStreams capture;
        REQUIRE(delegate_to_build_binary("tools/missing-tool", {"--flag"}, "--prepend") == 1);
        // pulp #-friction-1+#-friction-2 — error message switched from
        // "not built" to "helper not found" + a cmake --build hint when
        // the delegate began resolving from argv[0] before the project
        // root. Match against the stable "helper not found" prefix.
        REQUIRE(capture.err.str().find("helper not found") != std::string::npos);
    }
}

TEST_CASE("cli common interactive prompts accept defaults and parsed answers",
          "[cli][common][issue-643]") {
    ScopedColorDisabled color;
    CapturedStreams capture;

    {
        ScopedStdin input("\n");
        REQUIRE(cli::confirm("Continue?", true));
    }
    {
        ScopedStdin input("\n");
        REQUIRE_FALSE(cli::confirm("Continue?", false));
    }
    {
        ScopedStdin input("yes\n");
        REQUIRE(cli::confirm("Continue?", false));
    }
    {
        ScopedStdin input("n\n");
        REQUIRE_FALSE(cli::confirm("Continue?", true));
    }
    {
        ScopedStdin input("2\n");
        REQUIRE(cli::choose("Pick", {"one", "two", "three"}) == 1);
    }
    {
        ScopedStdin input("bad\n");
        REQUIRE(cli::choose("Pick", {"one", "two"}) == 0);
    }
    {
        ScopedStdin input("\n");
        REQUIRE(cli::choose("Pick", {"one", "two"}) == 0);
    }
    REQUIRE(cli::choose("Pick", {}) == -1);
    {
        ScopedStdin input("\n");
        REQUIRE(cli::input("Name", "Clock") == "Clock");
    }
    {
        ScopedStdin input("Delay\n");
        REQUIRE(cli::input("Name", "Clock") == "Delay");
    }
}

TEST_CASE("bump_one enforces the dirty gate and rewrites managed standalone sdk_path",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto project = tmp.path / "Clock";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    auto current_sdk = local_sdk_cache_path("0.1.0");
    make_standalone_project(project, "0.1.0", current_sdk.generic_string());
    init_clean_git_repo(project);

    std::ofstream dirty(project / "CMakeLists.txt", std::ios::app);
    dirty << "# dirty\n";
    dirty.close();

    BumpOptions opts;
    auto skipped = bump_one(project, "0.2.0", opts, "Clock");
    REQUIRE(skipped.status == "skipped");
    REQUIRE(skipped.failure_reason.find("uncommitted changes") != std::string::npos);

    opts.force_dirty = true;
    auto bumped = bump_one(project, "0.2.0", opts, "Clock");
    REQUIRE(bumped.status == "bumped");
    REQUIRE(bumped.edits.size() == 3);

    auto cmake = read_file_text(project / "CMakeLists.txt");
    auto toml = read_file_text(project / "pulp.toml");
    REQUIRE(cmake.find("find_package(Pulp 0.2.0 REQUIRED)") != std::string::npos);
    REQUIRE(toml.find("sdk_version = \"0.2.0\"") != std::string::npos);
    REQUIRE(toml.find(local_sdk_cache_path("0.2.0").generic_string()) != std::string::npos);
}

TEST_CASE("bump_one leaves custom standalone sdk_path unchanged and reports the note",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    auto project = tmp.path / "Clock";
    auto custom_sdk = tmp.path / "custom-sdk";

    make_standalone_project(project, "0.1.0", custom_sdk.generic_string());

    BumpOptions opts;
    auto bumped = bump_one(project, "0.2.0", opts, "Clock");

    REQUIRE(bumped.status == "bumped");
    REQUIRE(bumped.edits.size() == 2);
    REQUIRE(bumped.notes.size() == 1);
    REQUIRE(bumped.notes.front().find("custom sdk_path left unchanged")
            != std::string::npos);

    auto cmake = read_file_text(project / "CMakeLists.txt");
    auto toml = read_file_text(project / "pulp.toml");
    REQUIRE(cmake.find("find_package(Pulp 0.2.0 REQUIRED)") != std::string::npos);
    REQUIRE(toml.find("sdk_version = \"0.2.0\"") != std::string::npos);
    REQUIRE(toml.find("sdk_path = \"" + custom_sdk.generic_string() + "\"")
            != std::string::npos);
}

TEST_CASE("bump_one reports malformed standalone and dynamic CMake pins",
          "[cli][project-command][issue-643]") {
    TempDir tmp;

    auto standalone = tmp.path / "MalformedClock";
    make_standalone_project(standalone, "main");

    BumpOptions opts;
    auto malformed = bump_one(standalone, "0.2.0", opts, "MalformedClock");
    REQUIRE(malformed.status == "skipped");
    REQUIRE(malformed.pin_kind == pb::PinKind::PulpTomlSdkVersion);
    REQUIRE(malformed.old_pin == "main");
    REQUIRE(malformed.failure_reason == "pulp.toml sdk_version doesn't parse as semver");
    REQUIRE(malformed.edits.empty());

    auto dynamic = tmp.path / "DynamicFetch";
    fs::create_directories(dynamic);
    write_file(dynamic / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\n"
               "include(FetchContent)\n"
               "FetchContent_Declare(pulp\n"
               "    GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n"
               "    GIT_TAG main)\n"
               "FetchContent_MakeAvailable(pulp)\n");

    auto skipped = bump_one(dynamic, "0.2.0", opts, "DynamicFetch");
    REQUIRE(skipped.status == "skipped");
    REQUIRE(skipped.pin_kind == pb::PinKind::FetchContentGitTag);
    REQUIRE(skipped.old_pin == "main");
    REQUIRE(skipped.failure_reason.find("dynamic pin") != std::string::npos);
    REQUIRE(skipped.edits.empty());
}

TEST_CASE("bump_one distinguishes downgrades from explicit downgrade bumps",
          "[cli][project-command][issue-643]") {
    TempDir tmp;
    auto project = tmp.path / "Clock";
    make_fetchcontent_project(project, "0.3.0");

    BumpOptions opts;
    auto skipped = bump_one(project, "0.2.0", opts, "Clock");
    REQUIRE(skipped.status == "skipped");
    REQUIRE(skipped.pin_kind == pb::PinKind::FetchContentGitTag);
    REQUIRE(skipped.failure_reason ==
            "target version older than current pin (use --allow-downgrade to override)");
    REQUIRE(read_file_text(project / "CMakeLists.txt").find("GIT_TAG v0.3.0")
            != std::string::npos);

    opts.allow_downgrade = true;
    auto bumped = bump_one(project, "0.2.0", opts, "Clock");
    REQUIRE(bumped.status == "bumped");
    REQUIRE(bumped.edits.size() == 1);
    REQUIRE(read_file_text(project / "CMakeLists.txt").find("GIT_TAG v0.2.0")
            != std::string::npos);
}

TEST_CASE("bump_one rewrites non-standalone FetchContent pins",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto project = tmp.path / "FetchClock";
    make_fetchcontent_project(project, "0.1.0");

    BumpOptions opts;
    auto bumped = bump_one(project, "0.2.0", opts, "FetchClock");
    REQUIRE(bumped.status == "bumped");
    REQUIRE(bumped.pin_kind == pb::PinKind::FetchContentGitTag);
    REQUIRE(read_file_text(project / "CMakeLists.txt").find("GIT_TAG v0.2.0")
            != std::string::npos);
}

TEST_CASE("bump_one refuses redundant FetchContent bumps when origin main already pins the target",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto origin = tmp.path / "origin.git";
    auto seed = tmp.path / "seed";
    auto feature = tmp.path / "feature";

    require_run_ok("git init --bare -q " + quote(origin));

    fs::create_directories(seed);
    require_run_ok("git init -q " + quote(seed));
    configure_git_identity(seed);
    require_run_ok("git -C " + quote(seed) + " checkout -q -b main");
    make_fetchcontent_project(seed, "0.2.0");
    require_run_ok("git -C " + quote(seed) + " add CMakeLists.txt");
    require_run_ok("git -C " + quote(seed) + " commit -q -m \"main pin\"");
    require_run_ok("git -C " + quote(seed) + " remote add origin " + quote(origin));
    require_run_ok("git -C " + quote(seed) + " push -q -u origin main");

    require_run_ok("git clone -q " + quote(origin) + " " + quote(feature));
    configure_git_identity(feature);
    require_run_ok("git -C " + quote(feature) + " checkout -q -b feature");
    make_fetchcontent_project(feature, "0.1.0");
    require_run_ok("git -C " + quote(feature) + " add CMakeLists.txt");
    require_run_ok("git -C " + quote(feature) + " commit -q -m \"older local pin\"");

    BumpOptions opts;
    auto skipped = bump_one(feature, "0.2.0", opts, "FetchClock");
    REQUIRE(skipped.status == "skipped");
    REQUIRE(skipped.failure_reason.find("origin/main already pins SDK 0.2.0 >= target 0.2.0")
            != std::string::npos);
}

TEST_CASE("bump_one refuses redundant standalone bumps when origin main already pins the target",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto origin = tmp.path / "origin.git";
    auto seed = tmp.path / "seed";
    auto feature = tmp.path / "feature";

    require_run_ok("git init --bare -q " + quote(origin));

    fs::create_directories(seed);
    require_run_ok("git init -q " + quote(seed));
    configure_git_identity(seed);
    require_run_ok("git -C " + quote(seed) + " checkout -q -b main");
    make_standalone_project(seed, "0.2.0");
    require_run_ok("git -C " + quote(seed) + " add CMakeLists.txt pulp.toml");
    require_run_ok("git -C " + quote(seed) + " commit -q -m \"main pin\"");
    require_run_ok("git -C " + quote(seed) + " remote add origin " + quote(origin));
    require_run_ok("git -C " + quote(seed) + " push -q -u origin main");

    require_run_ok("git clone -q " + quote(origin) + " " + quote(feature));
    configure_git_identity(feature);
    require_run_ok("git -C " + quote(feature) + " checkout -q -b feature");
    make_standalone_project(feature, "0.1.0");
    require_run_ok("git -C " + quote(feature) + " add CMakeLists.txt pulp.toml");
    require_run_ok("git -C " + quote(feature) + " commit -q -m \"older local pin\"");

    BumpOptions opts;
    auto skipped = bump_one(feature, "0.2.0", opts, "Clock");
    REQUIRE(skipped.status == "skipped");
    REQUIRE(skipped.failure_reason.find("origin/main already pins SDK 0.2.0 >= target 0.2.0")
            != std::string::npos);
}

TEST_CASE("bump_one verify-builds succeeds with a cached standalone SDK",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto project = tmp.path / "Clock";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    make_standalone_project(project, "0.1.0");
    make_fake_sdk(sdk_cache_path("0.2.0"), "0.2.0");

    BumpOptions opts;
    opts.verify_builds = true;
    auto bumped = bump_one(project, "0.2.0", opts, "Clock");

    REQUIRE(bumped.status == "bumped");
    REQUIRE(bumped.edits.size() == 2);
    REQUIRE_FALSE(fs::exists(project / "build-bump-verify"));
    REQUIRE(read_file_text(project / "CMakeLists.txt").find("find_package(Pulp 0.2.0 REQUIRED)")
            != std::string::npos);
    REQUIRE(read_file_text(project / "pulp.toml").find("sdk_version = \"0.2.0\"")
            != std::string::npos);
}

TEST_CASE("bump_one skips standalone no-op managed sdk_path rewrites at target",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto project = tmp.path / "Clock";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    auto current_sdk = local_sdk_cache_path("0.2.0");
    make_standalone_project(project, "0.2.0", current_sdk.generic_string());

    BumpOptions opts;
    auto skipped = bump_one(project, "0.2.0", opts, "Clock");
    REQUIRE(skipped.status == "skipped");
    REQUIRE(skipped.failure_reason == "already at target version");
    REQUIRE(skipped.edits.empty());

    auto toml = read_file_text(project / "pulp.toml");
    REQUIRE(toml.find("sdk_path = \"" + current_sdk.generic_string() + "\"")
            != std::string::npos);
}

TEST_CASE("bump_one repairs standalone lockstep drift even when origin main already pins target",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto origin = tmp.path / "origin.git";
    auto seed = tmp.path / "seed";
    auto feature = tmp.path / "feature";

    require_run_ok("git init --bare -q " + quote(origin));

    fs::create_directories(seed);
    require_run_ok("git init -q " + quote(seed));
    configure_git_identity(seed);
    require_run_ok("git -C " + quote(seed) + " checkout -q -b main");
    make_standalone_project(seed, "0.2.0");
    require_run_ok("git -C " + quote(seed) + " add CMakeLists.txt pulp.toml");
    require_run_ok("git -C " + quote(seed) + " commit -q -m \"main pin\"");
    require_run_ok("git -C " + quote(seed) + " remote add origin " + quote(origin));
    require_run_ok("git -C " + quote(seed) + " push -q -u origin main");

    require_run_ok("git clone -q " + quote(origin) + " " + quote(feature));
    configure_git_identity(feature);
    require_run_ok("git -C " + quote(feature) + " checkout -q -b feature");
    make_standalone_project(feature, "0.2.0");
    write_file(feature / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\n"
               "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
               "find_package(Pulp 0.1.0 REQUIRED)\n");
    require_run_ok("git -C " + quote(feature) + " add CMakeLists.txt pulp.toml");
    require_run_ok("git -C " + quote(feature) + " commit -q -m \"stale cmake pin\"");

    BumpOptions opts;
    auto bumped = bump_one(feature, "0.2.0", opts, "Clock");
    REQUIRE(bumped.status == "bumped");
    REQUIRE(bumped.edits.size() == 1);
    REQUIRE(bumped.edits.front().kind == pb::PinKind::CMakeFindPackagePulpVersion);
    REQUIRE(read_file_text(feature / "CMakeLists.txt")
                .find("find_package(Pulp 0.2.0 REQUIRED)")
            != std::string::npos);
}

TEST_CASE("resolve_standalone_sdk falls back from mismatched sdk_path to caches",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto project = tmp.path / "Clock";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    auto mismatched = tmp.path / "sdk-mismatch";
    make_fake_sdk(mismatched, "0.1.0");
    make_standalone_project(project, "0.2.0", mismatched.generic_string());

    auto downloaded = sdk_cache_path("0.2.0");
    make_fake_sdk(downloaded, "0.2.0");

    auto resolved = resolve_standalone_sdk(project, false);
    REQUIRE(resolved.sdk_path_version_known);
    REQUIRE_FALSE(resolved.sdk_path_version_matches);
    REQUIRE(resolved.warning.find("sdk_path points at SDK v0.1.0 but pulp.toml requests v0.2.0")
            != std::string::npos);
    REQUIRE(resolved.resolved_sdk_dir == downloaded);

    auto checks = run_doctor_checks(project, true);
    auto installed = std::find_if(checks.begin(), checks.end(), [](const DoctorCheck& check) {
        return check.name == "Installed SDK";
    });
    REQUIRE(installed != checks.end());
    REQUIRE_FALSE(installed->passed);
    REQUIRE(installed->detail.find("sdk_path points at SDK v0.1.0") != std::string::npos);
    REQUIRE(installed->fix == "pulp build");
}

TEST_CASE("resolve_standalone_sdk handles custom sdk_path and checkout local cache hints",
          "[project-command][issue-244]") {
    TempDir tmp;
    auto home = tmp.path / "home";
    ScopedEnv pulp_home("PULP_HOME", home.string());

    auto custom_project = tmp.path / "CustomClock";
    auto custom_sdk = tmp.path / "custom-sdk";
    make_fake_sdk(custom_sdk, "0.2.0", false);
    make_standalone_project(custom_project, "0.2.0", custom_sdk.generic_string());

    auto custom = resolve_standalone_sdk(custom_project, false);
    REQUIRE(custom.used_sdk_path_hint);
    REQUIRE(custom.sdk_path_custom_unverifiable);
    REQUIRE(custom.resolved_sdk_dir == custom_sdk);

    auto custom_checks = run_doctor_checks(custom_project, true);
    auto installed_custom = std::find_if(custom_checks.begin(), custom_checks.end(),
                                         [](const DoctorCheck& check) {
                                             return check.name == "Installed SDK";
                                         });
    REQUIRE(installed_custom != custom_checks.end());
    REQUIRE(installed_custom->passed);
    REQUIRE(installed_custom->detail.find("custom sdk_path; version unverifiable")
            != std::string::npos);

    auto checkout_project = tmp.path / "CheckoutClock";
    auto checkout = tmp.path / "pulp-checkout";
    write_file(checkout / "setup.sh", "#!/bin/sh\n");
    make_standalone_project(checkout_project, "0.3.0", {}, checkout.generic_string());
    auto local_cache = local_sdk_cache_path("0.3.0");
    make_fake_sdk(local_cache, "0.3.0");

    auto from_checkout = resolve_standalone_sdk(checkout_project, false);
    REQUIRE(from_checkout.resolved_sdk_dir == local_cache);

    auto checkout_checks = run_doctor_checks(checkout_project, true);
    auto installed_checkout = std::find_if(checkout_checks.begin(), checkout_checks.end(),
                                           [](const DoctorCheck& check) {
                                               return check.name == "Installed SDK";
                                           });
    REQUIRE(installed_checkout != checkout_checks.end());
    REQUIRE(installed_checkout->passed);
    REQUIRE(installed_checkout->detail.find("(local cache)") != std::string::npos);
}
