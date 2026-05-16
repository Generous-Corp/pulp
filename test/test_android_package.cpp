#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/ship/android.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <process.h>
#include <stdlib.h>
#endif

using namespace pulp::ship;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::atomic_uint64_t temp_dir_counter{0};

int set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    return _putenv_s(name, value.c_str());
#else
    return ::setenv(name, value.c_str(), 1);
#endif
}

int unset_env_var(const char* name) {
#ifdef _WIN32
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

std::optional<std::string> read_env_var(const char* name) {
    if (const char* value = std::getenv(name))
        return std::string(value);
    return std::nullopt;
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, std::string value)
        : name_(name), previous_(read_env_var(name)) {
        REQUIRE(set_env_var(name_.c_str(), value) == 0);
    }

    ~ScopedEnvVar() {
        if (previous_)
            set_env_var(name_.c_str(), *previous_);
        else
            unset_env_var(name_.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedUnsetEnvVar {
public:
    explicit ScopedUnsetEnvVar(const char* name)
        : name_(name), previous_(read_env_var(name)) {
        REQUIRE(unset_env_var(name_.c_str()) == 0);
    }

    ~ScopedUnsetEnvVar() {
        if (previous_)
            set_env_var(name_.c_str(), *previous_);
    }

    ScopedUnsetEnvVar(const ScopedUnsetEnvVar&) = delete;
    ScopedUnsetEnvVar& operator=(const ScopedUnsetEnvVar&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

struct TempDir {
    fs::path path;

    TempDir() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto counter = temp_dir_counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() / (
            "pulp-android-package-test-" + std::to_string(process_id()) +
            "-" + std::to_string(stamp) + "-" + std::to_string(counter));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#ifndef _WIN32
void make_executable(const fs::path& path) {
    REQUIRE(::chmod(path.c_str(), 0755) == 0);
}

void write_tool(const fs::path& path, const std::string& body) {
    std::ofstream out(path);
    out << "#!/bin/sh\n";
    out << body;
    out.close();
    make_executable(path);
}
#else
void write_tool(const fs::path& path, const std::string& body) {
    std::ofstream out(path);
    out << "@echo off\r\n";
    out << body;
}
#endif

fs::path tool_path(const fs::path& dir, const std::string& name) {
#ifdef _WIN32
    return dir / (name + ".bat");
#else
    return dir / name;
#endif
}

fs::path make_fake_sdk(const fs::path& root) {
    auto sdk = root / "sdk";
    fs::create_directories(sdk / "build-tools" / "30.0.0");
    fs::create_directories(sdk / "build-tools" / "35.0.1");
    fs::create_directories(sdk / "ndk" / "26.1.10909125" / "toolchains");
    fs::create_directories(sdk / "ndk" / "27.0.12077973" / "toolchains");
    return sdk;
}

void write_fake_apksigner(const fs::path& build_tools, const fs::path& log_path) {
#ifdef _WIN32
    write_tool(tool_path(build_tools, "apksigner"),
        "echo %*>>\"" + log_path.string() + "\"\r\n"
        "if \"%1\"==\"verify\" (\r\n"
        "  echo Verified using v2 scheme ^(APK Signature Scheme v2^): true\r\n"
        "  echo Verified using v3 scheme ^(APK Signature Scheme v3^): true\r\n"
        "  echo Signer #1 certificate DN: CN=Pulp Android Test,O=Pulp\r\n"
        ")\r\n"
        "exit /b 0\r\n");
#else
    write_tool(build_tools / "apksigner",
        "printf '%s\\n' \"$*\" >> '" + log_path.string() + "'\n"
        "if [ \"$1\" = verify ]; then\n"
        "  echo 'Verified using v2 scheme (APK Signature Scheme v2): true'\n"
        "  echo 'Verified using v3 scheme (APK Signature Scheme v3): true'\n"
        "  echo 'Signer #1 certificate DN: CN=Pulp Android Test,O=Pulp'\n"
        "fi\n"
        "exit 0\n");
#endif
}

void write_fake_zipalign(const fs::path& build_tools, const fs::path& log_path) {
#ifdef _WIN32
    write_tool(tool_path(build_tools, "zipalign"),
        "echo %*>>\"" + log_path.string() + "\"\r\n"
        "copy /Y \"%~3\" \"%~4\" >NUL\r\n"
        "exit /b %ERRORLEVEL%\r\n");
#else
    write_tool(build_tools / "zipalign",
        "printf '%s\\n' \"$*\" >> '" + log_path.string() + "'\n"
        "cp \"$3\" \"$4\"\n");
#endif
}

void write_fake_jarsigner(const fs::path& bin_dir, const fs::path& log_path) {
    fs::create_directories(bin_dir);
#ifdef _WIN32
    write_tool(tool_path(bin_dir, "jarsigner"),
        "echo %*>>\"" + log_path.string() + "\"\r\n"
        "if \"%1\"==\"-verify\" echo jar verified.\r\n"
        "exit /b 0\r\n");
#else
    write_tool(bin_dir / "jarsigner",
        "printf '%s\\n' \"$*\" >> '" + log_path.string() + "'\n"
        "if [ \"$1\" = -verify ]; then echo 'jar verified.'; fi\n"
        "exit 0\n");
#endif
}

void write_fake_gradlew(const fs::path& project_dir, const fs::path& log_path) {
#ifdef _WIN32
    write_tool(project_dir / "gradlew.bat",
        "echo %* >\"" + log_path.string() + "\"\r\n"
        "mkdir app\\build\\outputs\\apk\\release 2>NUL\r\n"
        "mkdir app\\build\\outputs\\bundle\\release 2>NUL\r\n"
        "echo apk>app\\build\\outputs\\apk\\release\\app-release.apk\r\n"
        "echo aab>app\\build\\outputs\\bundle\\release\\app-release.aab\r\n"
        "exit /b 0\r\n");
#else
    write_tool(project_dir / "gradlew",
        "printf '%s\\n' \"$*\" > '" + log_path.string() + "'\n"
        "mkdir -p app/build/outputs/apk/release app/build/outputs/bundle/release\n"
        "printf apk > app/build/outputs/apk/release/app-release.apk\n"
        "printf aab > app/build/outputs/bundle/release/app-release.aab\n");
#endif
}

void write_failing_gradlew(const fs::path& project_dir, int exit_code) {
#ifdef _WIN32
    write_tool(project_dir / "gradlew.bat",
        "exit /b " + std::to_string(exit_code) + "\r\n");
#else
    write_tool(project_dir / "gradlew",
        "exit " + std::to_string(exit_code) + "\n");
#endif
}

void write_fake_bundletool(const fs::path& path, const fs::path& log_path) {
#ifdef _WIN32
    write_tool(path,
        "echo %* >\"" + log_path.string() + "\"\r\n"
        "set out=\r\n"
        ":loop\r\n"
        "if \"%~1\"==\"\" goto done\r\n"
        "set arg=%~1\r\n"
        "if \"%arg:~0,9%\"==\"--output=\" set out=%arg:~9%\r\n"
        "shift\r\n"
        "goto loop\r\n"
        ":done\r\n"
        "echo apks>\"%out%\"\r\n"
        "exit /b 0\r\n");
#else
    write_tool(path,
        "printf '%s\\n' \"$*\" > '" + log_path.string() + "'\n"
        "out=''\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in --output=*) out=${arg#--output=} ;; esac\n"
        "done\n"
        "printf apks > \"$out\"\n");
#endif
}

} // namespace

TEST_CASE("Android SDK discovery honors environment roots and latest tools", "[ship][android]") {
    TempDir temp;
    auto sdk = make_fake_sdk(temp.path);
    ScopedEnvVar android_home("ANDROID_HOME", sdk.string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");
    ScopedUnsetEnvVar android_ndk_home("ANDROID_NDK_HOME");

    auto build_tools = sdk / "build-tools" / "35.0.1";
    write_fake_apksigner(build_tools, temp.path / "apksigner.log");
    write_fake_zipalign(build_tools, temp.path / "zipalign.log");

    REQUIRE(detect_android_sdk() == sdk);
    REQUIRE(android_build_tools_version() == "35.0.1");
    REQUIRE(find_android_ndk() == sdk / "ndk" / "27.0.12077973");
    REQUIRE(find_android_build_tool("apksigner") == tool_path(build_tools, "apksigner"));
    REQUIRE(find_android_build_tool("zipalign") == tool_path(build_tools, "zipalign"));
    REQUIRE(find_android_build_tool("missing-tool").empty());
}

TEST_CASE("Android SDK discovery compares tool revisions numerically", "[ship][android]") {
    TempDir temp;
    auto sdk = temp.path / "sdk";
    fs::create_directories(sdk / "build-tools" / "9.0.0");
    fs::create_directories(sdk / "build-tools" / "10.0.0");
    fs::create_directories(sdk / "ndk" / "9.0.0" / "toolchains");
    fs::create_directories(sdk / "ndk" / "10.0.0" / "toolchains");

    ScopedEnvVar android_home("ANDROID_HOME", sdk.string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");
    ScopedUnsetEnvVar android_ndk_home("ANDROID_NDK_HOME");

    REQUIRE(android_build_tools_version() == "10.0.0");
    REQUIRE(find_android_ndk() == sdk / "ndk" / "10.0.0");
}

TEST_CASE("Android SDK discovery falls back to SDK root and dedicated NDK home", "[ship][android]") {
    TempDir temp;
    auto sdk = make_fake_sdk(temp.path);
    auto standalone_ndk = temp.path / "standalone-ndk";
    fs::create_directories(standalone_ndk / "toolchains");

    ScopedUnsetEnvVar android_home("ANDROID_HOME");
    ScopedEnvVar android_sdk_root("ANDROID_SDK_ROOT", sdk.string());
    ScopedEnvVar android_ndk_home("ANDROID_NDK_HOME", standalone_ndk.string());

    REQUIRE(detect_android_sdk() == sdk);
    REQUIRE(find_android_ndk() == standalone_ndk);
}

TEST_CASE("Android SDK discovery skips invalid primary env roots",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto sdk = make_fake_sdk(temp.path);
    auto invalid_sdk = temp.path / "invalid-sdk";
    auto invalid_ndk = temp.path / "invalid-ndk";
    fs::create_directories(invalid_sdk);
    fs::create_directories(invalid_ndk);

    ScopedEnvVar android_home("ANDROID_HOME", invalid_sdk.string());
    ScopedEnvVar android_sdk_root("ANDROID_SDK_ROOT", sdk.string());
    ScopedEnvVar android_ndk_home("ANDROID_NDK_HOME", invalid_ndk.string());

    REQUIRE(detect_android_sdk() == sdk);
    REQUIRE(find_android_ndk() == sdk / "ndk" / "27.0.12077973");
}

TEST_CASE("Android SDK discovery returns empty for invalid env roots",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto invalid_home = temp.path / "not-an-sdk";
    fs::create_directories(invalid_home);

    ScopedEnvVar android_home("ANDROID_HOME", invalid_home.string());
    ScopedEnvVar android_sdk_root("ANDROID_SDK_ROOT", invalid_home.string());
    ScopedEnvVar android_ndk_home("ANDROID_NDK_HOME", (temp.path / "not-an-ndk").string());
    ScopedEnvVar home("HOME", (temp.path / "home").string());
    ScopedUnsetEnvVar userprofile("USERPROFILE");

    REQUIRE(detect_android_sdk().empty());
    REQUIRE(find_android_ndk().empty());
    REQUIRE(android_build_tools_version().empty());
    REQUIRE(find_android_build_tool("apksigner").empty());
}

TEST_CASE("Android SDK discovery ignores empty tool directories",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto sdk = temp.path / "sdk";
    fs::create_directories(sdk / "build-tools");
    fs::create_directories(sdk / "ndk");

    ScopedEnvVar android_home("ANDROID_HOME", sdk.string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");
    ScopedUnsetEnvVar android_ndk_home("ANDROID_NDK_HOME");

    REQUIRE(detect_android_sdk() == sdk);
    REQUIRE(android_build_tools_version().empty());
    REQUIRE(find_android_ndk().empty());
}

TEST_CASE("Android SDK discovery treats malformed revisions as zero",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto sdk = temp.path / "sdk";
    fs::create_directories(sdk / "build-tools" / "preview");
    fs::create_directories(sdk / "build-tools" / "1.preview");
    fs::create_directories(sdk / "build-tools" / "1.0.1");
    fs::create_directories(sdk / "ndk" / "beta" / "toolchains");
    fs::create_directories(sdk / "ndk" / "1.0.1" / "toolchains");

    ScopedEnvVar android_home("ANDROID_HOME", sdk.string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");
    ScopedUnsetEnvVar android_ndk_home("ANDROID_NDK_HOME");

    REQUIRE(android_build_tools_version() == "1.0.1");
    REQUIRE(find_android_ndk() == sdk / "ndk" / "1.0.1");
}

TEST_CASE("Android SDK discovery handles very large revision components",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto sdk = temp.path / "sdk";
    const std::string huge = "999999999999999999999999999999";
    fs::create_directories(sdk / "build-tools" / "100.0.0");
    fs::create_directories(sdk / "build-tools" / huge);
    fs::create_directories(sdk / "ndk" / "100.0.0" / "toolchains");
    fs::create_directories(sdk / "ndk" / huge / "toolchains");

    ScopedEnvVar android_home("ANDROID_HOME", sdk.string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");
    ScopedUnsetEnvVar android_ndk_home("ANDROID_NDK_HOME");

    REQUIRE(android_build_tools_version() == huge);
    REQUIRE(find_android_ndk() == sdk / "ndk" / huge);
}

TEST_CASE("Android Java version detection parses quoted java output",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto bin_dir = temp.path / "bin";
    fs::create_directories(bin_dir);

#ifdef _WIN32
    write_tool(tool_path(bin_dir, "java"),
        "echo openjdk version \"21.0.2\" 2026-01-16\r\n"
        "exit /b 0\r\n");
#else
    write_tool(bin_dir / "java",
        "echo 'openjdk version \"21.0.2\" 2026-01-16' >&2\n");
#endif

    auto old_path = read_env_var("PATH").value_or("");
#ifdef _WIN32
    ScopedEnvVar path_env("PATH", bin_dir.string() + ";" + old_path);
#else
    ScopedEnvVar path_env("PATH", bin_dir.string() + ":" + old_path);
#endif

    REQUIRE(detect_java_version() == "21.0.2");
}

TEST_CASE("Android Java version detection returns empty for unquoted output",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto bin_dir = temp.path / "bin";
    fs::create_directories(bin_dir);

#ifdef _WIN32
    write_tool(tool_path(bin_dir, "java"),
        "echo openjdk version 21.0.2\r\n"
        "exit /b 0\r\n");
#else
    write_tool(bin_dir / "java",
        "echo 'openjdk version 21.0.2' >&2\n");
#endif

    auto old_path = read_env_var("PATH").value_or("");
#ifdef _WIN32
    ScopedEnvVar path_env("PATH", bin_dir.string() + ";" + old_path);
#else
    ScopedEnvVar path_env("PATH", bin_dir.string() + ":" + old_path);
#endif

    REQUIRE(detect_java_version().empty());
}

TEST_CASE("Android signing helpers use SDK tools and parse APK verification output", "[ship][android]") {
    TempDir temp;
    auto sdk = make_fake_sdk(temp.path);
    auto build_tools = sdk / "build-tools" / "35.0.1";
    auto apksigner_log = temp.path / "apksigner.log";
    auto zipalign_log = temp.path / "zipalign.log";
    write_fake_apksigner(build_tools, apksigner_log);
    write_fake_zipalign(build_tools, zipalign_log);

    ScopedEnvVar android_home("ANDROID_HOME", sdk.string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");

    auto input_apk = temp.path / "input.apk";
    auto output_apk = temp.path / "aligned.apk";
    std::ofstream(input_apk) << "apk";
    REQUIRE(zipalign_apk(input_apk, output_apk));
    REQUIRE(fs::exists(output_apk));
    REQUIRE_THAT(read_text(zipalign_log), ContainsSubstring("-f 4"));

    AndroidKeystoreConfig keystore;
    keystore.keystore_path = temp.path / "release.jks";
    keystore.store_password = "store-pass";
    keystore.key_alias = "release";
    keystore.key_password = "key-pass";

    REQUIRE(sign_apk(output_apk, keystore));

    auto info = check_android_signing(output_apk);
    REQUIRE(info.is_signed);
    REQUIRE(info.v2_signed);
    REQUIRE(info.v3_signed);
    REQUIRE(info.signer_cn == "Pulp Android Test");
    REQUIRE(info.error.empty());

    auto log = read_text(apksigner_log);
    REQUIRE_THAT(log, ContainsSubstring("sign"));
    REQUIRE_THAT(log, ContainsSubstring("--ks-key-alias"));
    REQUIRE_THAT(log, ContainsSubstring("verify --print-certs --verbose"));
}

TEST_CASE("Android signing helpers fail cleanly when SDK tools are unavailable",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    ScopedEnvVar android_home("ANDROID_HOME", (temp.path / "missing-sdk").string());
    ScopedUnsetEnvVar android_sdk_root("ANDROID_SDK_ROOT");
    ScopedEnvVar home("HOME", (temp.path / "home").string());
    ScopedUnsetEnvVar userprofile("USERPROFILE");

    auto apk = temp.path / "app.apk";
    std::ofstream(apk) << "apk";

    AndroidKeystoreConfig keystore;
    keystore.keystore_path = temp.path / "release.jks";
    keystore.store_password = "store-pass";
    keystore.key_alias = "release";

    REQUIRE_FALSE(zipalign_apk(apk, temp.path / "aligned.apk"));
    REQUIRE_FALSE(sign_apk(apk, keystore));

    auto info = check_android_signing(apk);
    REQUIRE_FALSE(info.is_signed);
    REQUIRE_THAT(info.error, ContainsSubstring("apksigner not found"));
}

TEST_CASE("Android AAB signing and verification use jarsigner on PATH", "[ship][android]") {
    TempDir temp;
    auto bin_dir = temp.path / "bin";
    auto jarsigner_log = temp.path / "jarsigner.log";
    write_fake_jarsigner(bin_dir, jarsigner_log);

    auto old_path = read_env_var("PATH").value_or("");
#ifdef _WIN32
    ScopedEnvVar path_env("PATH", bin_dir.string() + ";" + old_path);
#else
    ScopedEnvVar path_env("PATH", bin_dir.string() + ":" + old_path);
#endif

    auto aab = temp.path / "app-release.aab";
    std::ofstream(aab) << "aab";

    AndroidKeystoreConfig keystore;
    keystore.keystore_path = temp.path / "release.jks";
    keystore.store_password = "store-pass";
    keystore.key_alias = "release";

    REQUIRE(sign_aab(aab, keystore));

    auto info = check_android_signing(aab);
    REQUIRE(info.is_signed);
    REQUIRE(info.error.empty());

    auto log = read_text(jarsigner_log);
    REQUIRE_THAT(log, ContainsSubstring("-keystore"));
    REQUIRE_THAT(log, ContainsSubstring("-verify"));
}

TEST_CASE("Android Gradle packaging collects APK and AAB artifacts", "[ship][android]") {
    TempDir temp;
    auto project = temp.path / "android";
    fs::create_directories(project);
    auto gradle_log = temp.path / "gradle.args";
    write_fake_gradlew(project, gradle_log);

    ScopedEnvVar signing_pass("PULP_ANDROID_TEST_STORE_PASS", "env-store-pass");

    AndroidKeystoreConfig keystore;
    keystore.keystore_path = temp.path / "release.jks";
    keystore.store_password = "@env:PULP_ANDROID_TEST_STORE_PASS";
    keystore.key_alias = "release";
    keystore.key_password = "";

    auto result = build_android_package(
        project,
        {"arm64-v8a", "x86_64"},
        &keystore,
        true,
        true);

    INFO(result.error);
    INFO("gradle args: " << read_text(gradle_log));
    REQUIRE(result.success);
    REQUIRE(result.error.empty());
    REQUIRE(result.apk_path.filename() == "app-release.apk");
    REQUIRE(result.aab_path.filename() == "app-release.aab");

    auto args = read_text(gradle_log);
    REQUIRE_THAT(args, ContainsSubstring("assembleRelease"));
    REQUIRE_THAT(args, ContainsSubstring("bundleRelease"));
    REQUIRE_THAT(args, ContainsSubstring("-Pandroid.injected.build.abi=arm64-v8a,x86_64"));
    REQUIRE_THAT(args, ContainsSubstring("-Pandroid.injected.signing.store.file="));
    REQUIRE_THAT(args, ContainsSubstring("-Pandroid.injected.signing.key.alias=release"));
}

TEST_CASE("Android Gradle packaging supports APK-only and AAB-only builds",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto project = temp.path / "android";
    fs::create_directories(project);
    auto gradle_log = temp.path / "gradle.args";
    write_fake_gradlew(project, gradle_log);

    auto apk_only = build_android_package(project, {}, nullptr, false, true);
    INFO(apk_only.error);
    REQUIRE(apk_only.success);
    REQUIRE(apk_only.apk_path.filename() == "app-release.apk");
    REQUIRE(apk_only.aab_path.empty());
    REQUIRE_THAT(read_text(gradle_log), ContainsSubstring("assembleRelease"));
    REQUIRE(read_text(gradle_log).find("bundleRelease") == std::string::npos);

    fs::remove_all(project / "app" / "build" / "outputs");
    auto aab_only = build_android_package(project, {}, nullptr, true, false);
    INFO(aab_only.error);
    REQUIRE(aab_only.success);
    REQUIRE(aab_only.apk_path.empty());
    REQUIRE(aab_only.aab_path.filename() == "app-release.aab");
    REQUIRE_THAT(read_text(gradle_log), ContainsSubstring("bundleRelease"));
    REQUIRE(read_text(gradle_log).find("assembleRelease") == std::string::npos);
}

TEST_CASE("Android Gradle packaging reports command failures",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto project = temp.path / "android";
    fs::create_directories(project);
    write_failing_gradlew(project, 7);

    auto result = build_android_package(project, {"arm64-v8a"}, nullptr, true, true);

    REQUIRE_FALSE(result.success);
    REQUIRE(result.apk_path.empty());
    REQUIRE(result.aab_path.empty());
    REQUIRE_THAT(result.error, ContainsSubstring("Gradle build failed (exit code 7)"));
}

TEST_CASE("Android Gradle packaging reports missing wrapper and missing artifacts", "[ship][android]") {
    TempDir temp;
    auto missing_wrapper = build_android_package(temp.path / "missing", {}, nullptr, true, true);
    REQUIRE_FALSE(missing_wrapper.success);
    REQUIRE_THAT(missing_wrapper.error, ContainsSubstring("gradlew not found"));

    auto project = temp.path / "android";
    fs::create_directories(project);
#ifdef _WIN32
    write_tool(project / "gradlew.bat", "exit /b 0\r\n");
#else
    write_tool(project / "gradlew", "exit 0\n");
#endif

    auto missing_artifacts = build_android_package(project, {}, nullptr, true, true);
    REQUIRE_FALSE(missing_artifacts.success);
    REQUIRE_THAT(missing_artifacts.error, ContainsSubstring("expected artifacts not found"));
}

TEST_CASE("Android bundletool conversion passes optional signing config", "[ship][android]") {
    TempDir temp;
    auto bundletool = tool_path(temp.path, "bundletool");
    auto bundletool_log = temp.path / "bundletool.args";
    write_fake_bundletool(bundletool, bundletool_log);
    ScopedEnvVar bundletool_env("BUNDLETOOL", bundletool.string());

    ScopedEnvVar signing_pass("PULP_ANDROID_TEST_KEY_PASS", "env-key-pass");

    AndroidKeystoreConfig keystore;
    keystore.keystore_path = temp.path / "release.jks";
    keystore.store_password = "store-pass";
    keystore.key_alias = "release";
    keystore.key_password = "@env:PULP_ANDROID_TEST_KEY_PASS";

    auto aab = temp.path / "app-release.aab";
    auto apks = temp.path / "app-release.apks";
    std::ofstream(aab) << "aab";

    auto converted = aab_to_apks(aab, apks, &keystore);
    INFO("bundletool log: " << read_text(bundletool_log));
    REQUIRE(converted);
    REQUIRE(fs::exists(apks));

    auto args = read_text(bundletool_log);
    REQUIRE_THAT(args, ContainsSubstring("build-apks"));
    REQUIRE_THAT(args, ContainsSubstring("--bundle=" + aab.string()));
    REQUIRE_THAT(args, ContainsSubstring("--output=" + apks.string()));
    REQUIRE_THAT(args, ContainsSubstring("--ks-key-alias=release"));
}

TEST_CASE("Android bundletool conversion supports unsigned output",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    auto bundletool = tool_path(temp.path, "bundletool");
    auto bundletool_log = temp.path / "bundletool-unsigned.args";
    write_fake_bundletool(bundletool, bundletool_log);
    ScopedEnvVar bundletool_env("BUNDLETOOL", bundletool.string());

    auto aab = temp.path / "app-release.aab";
    auto apks = temp.path / "app-release.apks";
    std::ofstream(aab) << "aab";

    REQUIRE(aab_to_apks(aab, apks, nullptr));
    REQUIRE(fs::exists(apks));

    auto args = read_text(bundletool_log);
    REQUIRE_THAT(args, ContainsSubstring("build-apks"));
    REQUIRE_THAT(args, ContainsSubstring("--overwrite"));
    REQUIRE(args.find("--ks=") == std::string::npos);
}

TEST_CASE("Android bundletool conversion reports missing command",
          "[ship][android][coverage][issue-644]") {
    TempDir temp;
    ScopedEnvVar bundletool_env("BUNDLETOOL", (temp.path / "missing-bundletool").string());

    auto aab = temp.path / "app-release.aab";
    auto apks = temp.path / "app-release.apks";
    std::ofstream(aab) << "aab";

    REQUIRE_FALSE(aab_to_apks(aab, apks, nullptr));
    REQUIRE_FALSE(fs::exists(apks));
}
