#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
    std::size_t size = 0;
    REQUIRE(parse_size_arg("42", "--top", size));
    REQUIRE(size == 42);
    REQUIRE_FALSE(parse_size_arg("", "--top", size));
    REQUIRE_FALSE(parse_size_arg("12x", "--top", size));

    double value = 0.0;
    REQUIRE(parse_double_arg("0.125", "--min-score", value));
    REQUIRE(value == 0.125);
    REQUIRE_FALSE(parse_double_arg("", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("nan", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("inf", "--min-score", value));
    REQUIRE_FALSE(parse_double_arg("3.5x", "--min-score", value));

    REQUIRE(trim(" \t hello \n") == "hello");
    REQUIRE(strip_quotes("\"quoted\"") == "quoted");
    REQUIRE(strip_quotes("'quoted'") == "quoted");
    REQUIRE(strip_quotes("unquoted") == "unquoted");
    REQUIRE(replace_all_str("one two one", "one", "1") == "1 two 1");
    REQUIRE(icontains("Signalsmith DSP", "smith"));
    REQUIRE(icontains("Signalsmith DSP", "SIGNAL"));
    REQUIRE_FALSE(icontains("Signalsmith DSP", "delay"));
    REQUIRE(yaml_value("  target: plugin  # comment", "target") == "plugin  # comment");
    REQUIRE(sanitize_process_output(std::string("a\0b", 3)) == "ab");
    REQUIRE(truncate_message("abcdef", 4) == "abcd...");
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

    auto standalone = tmp.path / "standalone" / "nested";
    fs::create_directories(standalone);
    write_file(tmp.path / "standalone" / "pulp.toml", "[pulp]\nsdk_version = \"1.2.3\"\n");

    REQUIRE(find_project_root_from(child) == repo);
    REQUIRE(find_project_root_from(tmp.path / "missing") == fs::path{});
    REQUIRE(path_is_within(child, repo));
    REQUIRE(path_is_within(repo, repo));
    REQUIRE_FALSE(path_is_within(tmp.path, repo));

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

TEST_CASE("cli common format, AAX, quoting, and fuzzy helpers stay deterministic",
          "[cli][common][issue-643]") {
    REQUIRE(shell_quote(std::string("a b\"c\\d")) == "\"a b\\\"c\\\\d\"");
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
    REQUIRE(fuzzy_score("package", "zzz") == 0);
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
