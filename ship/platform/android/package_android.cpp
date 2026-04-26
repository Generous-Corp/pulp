// Android signing and packaging for Pulp
// Host-side tool — runs on macOS/Windows/Linux dev machine, not on Android device.
// Shells out to Android SDK build-tools (apksigner, zipalign, bundletool)
// and Gradle for APK/AAB generation.

#include <pulp/ship/android.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

namespace pulp::ship {
namespace fs = std::filesystem;

static std::optional<std::string> get_env(const std::string& name) {
    auto val = std::getenv(name.c_str());
    if (val) return std::string(val);
    return std::nullopt;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::string exec_cmd(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, timeout_ms);
    auto result = r.stdout_output;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

static int exec_status(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, timeout_ms);
    return r.exit_code;
}

#ifdef _WIN32
static std::string wrap_for_cmd_c(const std::string& cmd);

static std::string exec_cmd_win(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("cmd.exe", {"/c", wrap_for_cmd_c(cmd)}, timeout_ms);
    auto result = r.stdout_output;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}
static int exec_status_win(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("cmd.exe", {"/c", wrap_for_cmd_c(cmd)}, timeout_ms);
    return r.exit_code;
}
#endif

// Shell-quote a string to protect spaces and metacharacters
static std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    // Windows: wrap in double quotes, escape internal double quotes
    std::string result = "\"";
    for (char c : s) {
        if (c == '"') result += "\\\"";
        else result += c;
    }
    result += "\"";
    return result;
#else
    // POSIX: wrap in single quotes, escape internal single quotes
    std::string result = "'";
    for (char c : s) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += "'";
    return result;
#endif
}

static std::string shell_quote_path(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

static std::uint64_t parse_android_revision_part(std::string_view part) {
    if (part.empty()) return 0;

    std::uint64_t value = 0;
    for (unsigned char c : part) {
        if (!std::isdigit(c)) return 0;
        auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return std::numeric_limits<std::uint64_t>::max();
        value = value * 10 + digit;
    }
    return value;
}

static std::vector<std::uint64_t> android_revision_key(const fs::path& path) {
    auto name = path.filename().string();
    std::vector<std::uint64_t> key;

    std::size_t start = 0;
    while (start <= name.size()) {
        auto end = name.find('.', start);
        auto view = std::string_view(name);
        auto part = end == std::string::npos
            ? view.substr(start)
            : view.substr(start, end - start);
        key.push_back(parse_android_revision_part(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }

    return key;
}

static bool android_revision_less(const fs::path& lhs, const fs::path& rhs) {
    auto left = android_revision_key(lhs);
    auto right = android_revision_key(rhs);
    auto count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        auto a = i < left.size() ? left[i] : 0;
        auto b = i < right.size() ? right[i] : 0;
        if (a != b) return a < b;
    }
    return lhs.filename().string() < rhs.filename().string();
}

static std::string shell_quote_arg(const std::string& arg) {
    return shell_quote(arg);
}

static std::string shell_quote_path_arg(const std::string& prefix, const fs::path& path) {
    return shell_quote(prefix + path.string());
}

#ifdef _WIN32
static std::string wrap_for_cmd_c(const std::string& cmd) {
    // `cmd.exe /c` strips the first and last quote when the command begins
    // with a quoted executable. Add an outer pair so the executable's quotes
    // survive and the remaining quoted arguments are parsed normally.
    return (!cmd.empty() && cmd.front() == '"') ? "\"" + cmd + "\"" : cmd;
}
#endif

static std::string shell_invoke_path(const fs::path& path) {
#ifdef _WIN32
    return shell_quote_path(path);
#else
    return shell_quote(path.string());
#endif
}

static std::string shell_invoke_command(const std::string& command) {
#ifdef _WIN32
    return command.find_first_of(" \t\\/") != std::string::npos ? shell_quote(command) : command;
#else
    return shell_quote(command);
#endif
}

// Platform-aware command execution
static std::string run_cmd(const std::string& cmd, int timeout_ms = 120000) {
#ifdef _WIN32
    return exec_cmd_win(cmd, timeout_ms);
#else
    return exec_cmd(cmd, timeout_ms);
#endif
}

static int run_status(const std::string& cmd, int timeout_ms = 120000) {
#ifdef _WIN32
    return exec_status_win(cmd, timeout_ms);
#else
    return exec_status(cmd, timeout_ms);
#endif
}

static int run_status_in_directory(const fs::path& directory,
                                   const std::string& cmd,
                                   int timeout_ms = 120000) {
    pulp::platform::ProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.working_directory = directory.string();
#ifdef _WIN32
    auto r = pulp::platform::ChildProcess::run("cmd.exe", {"/c", wrap_for_cmd_c(cmd)}, options);
#else
    auto r = pulp::platform::ChildProcess::run(
        "/bin/sh",
        {"-c", cmd},
        options);
#endif
    return r.exit_code;
}

static std::string resolve_password(const std::string& password) {
    // Support @env:VAR_NAME syntax
    if (password.size() > 5 && password.substr(0, 5) == "@env:") {
        auto var = password.substr(5);
        if (auto val = get_env(var))
            return *val;
        return {};  // env var not set
    }
    return password;
}

// ── Public SDK discovery API ────────────────────────────────────────────────

fs::path detect_android_sdk() {
    // Check environment variables first
    if (auto env = get_env("ANDROID_HOME")) {
        auto p = fs::path(*env);
        if (fs::exists(p / "build-tools")) return p;
    }
    if (auto env = get_env("ANDROID_SDK_ROOT")) {
        auto p = fs::path(*env);
        if (fs::exists(p / "build-tools")) return p;
    }

    // Platform defaults
    auto home_opt = get_env("HOME");
    if (!home_opt) home_opt = get_env("USERPROFILE");
    if (!home_opt) return {};
    auto home = fs::path(*home_opt);

#ifdef __APPLE__
    auto candidate = home / "Library" / "Android" / "sdk";
#elif defined(_WIN32)
    auto localappdata = get_env("LOCALAPPDATA");
    auto candidate = localappdata ? fs::path(*localappdata) / "Android" / "Sdk"
                                  : home / "AppData" / "Local" / "Android" / "Sdk";
#else
    auto candidate = home / "Android" / "Sdk";
#endif
    if (fs::exists(candidate / "build-tools")) return candidate;
    return {};
}

static fs::path find_latest_build_tools(const fs::path& sdk) {
    auto bt_dir = sdk / "build-tools";
    if (!fs::exists(bt_dir)) return {};

    std::vector<fs::path> versions;
    for (auto& entry : fs::directory_iterator(bt_dir)) {
        if (entry.is_directory())
            versions.push_back(entry.path());
    }
    if (versions.empty()) return {};

    std::sort(versions.begin(), versions.end(), android_revision_less);
    return versions.back();
}

fs::path find_android_ndk() {
    // Check dedicated env var first
    if (auto env = get_env("ANDROID_NDK_HOME")) {
        auto p = fs::path(*env);
        if (fs::exists(p / "ndk-build") || fs::exists(p / "toolchains"))
            return p;
    }

    auto sdk = detect_android_sdk();
    if (sdk.empty()) return {};

    auto ndk_dir = sdk / "ndk";
    if (!fs::exists(ndk_dir)) return {};

    std::vector<fs::path> versions;
    for (auto& entry : fs::directory_iterator(ndk_dir)) {
        if (entry.is_directory())
            versions.push_back(entry.path());
    }
    if (versions.empty()) return {};
    std::sort(versions.begin(), versions.end(), android_revision_less);
    return versions.back();
}

fs::path find_android_build_tool(const std::string& name) {
    auto sdk = detect_android_sdk();
    if (sdk.empty()) return {};
    auto bt = find_latest_build_tools(sdk);
    if (bt.empty()) return {};

    auto tool = bt / name;
#ifdef _WIN32
    auto tool_bat = bt / (name + ".bat");
    if (fs::exists(tool_bat)) return tool_bat;
    auto tool_exe = bt / (name + ".exe");
    if (fs::exists(tool_exe)) return tool_exe;
#endif
    if (fs::exists(tool)) return tool;
    return {};
}

std::string android_build_tools_version() {
    auto sdk = detect_android_sdk();
    if (sdk.empty()) return {};
    auto bt = find_latest_build_tools(sdk);
    if (bt.empty()) return {};
    return bt.filename().string();
}

std::string detect_java_version() {
    auto output = run_cmd("java -version 2>&1 | head -1");
    // Parse: openjdk version "17.0.2" or java version "21.0.2"
    auto q1 = output.find('"');
    if (q1 == std::string::npos) return {};
    auto q2 = output.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return output.substr(q1 + 1, q2 - q1 - 1);
}

static fs::path find_gradlew(const fs::path& project_dir) {
#ifdef _WIN32
    auto gradlew = project_dir / "gradlew.bat";
#else
    auto gradlew = project_dir / "gradlew";
#endif
    if (fs::exists(gradlew)) return gradlew;
    return {};
}

// ── Core operations ─────────────────────────────────────────────────────────

bool zipalign_apk(const fs::path& input_apk, const fs::path& output_apk) {
    auto tool = find_android_build_tool("zipalign");
    if (tool.empty()) return false;

    std::string cmd = shell_invoke_path(tool) + " -f 4 "
        + shell_quote_path(input_apk) + " " + shell_quote_path(output_apk);
    return run_status(cmd) == 0;
}

bool sign_apk(const fs::path& apk_path, const AndroidKeystoreConfig& keystore) {
    auto tool = find_android_build_tool("apksigner");
    if (tool.empty()) return false;

    auto store_pass = resolve_password(keystore.store_password);
    auto key_pass = resolve_password(
        keystore.key_password.empty() ? keystore.store_password : keystore.key_password);

    std::string cmd = shell_invoke_path(tool) + " sign"
        " --ks " + shell_quote_path(keystore.keystore_path) +
        " --ks-key-alias " + shell_quote(keystore.key_alias) +
        " --ks-pass " + shell_quote_arg("pass:" + store_pass) +
        " --key-pass " + shell_quote_arg("pass:" + key_pass) +
        " --v2-signing-enabled true"
        " --v3-signing-enabled true"
        " " + shell_quote_path(apk_path);
    return run_status(cmd) == 0;
}

bool sign_aab(const fs::path& aab_path, const AndroidKeystoreConfig& keystore) {
    auto store_pass = resolve_password(keystore.store_password);
    auto key_pass = resolve_password(
        keystore.key_password.empty() ? keystore.store_password : keystore.key_password);

    // AABs use jarsigner (Google re-signs with Play App Signing)
    std::string cmd = "jarsigner"
        " -keystore \"" + keystore.keystore_path.string() + "\""
        " -storepass " + shell_quote(store_pass) +
        " -keypass " + shell_quote(key_pass) +
        " \"" + aab_path.string() + "\""
        " \"" + keystore.key_alias + "\"";
    return run_status(cmd) == 0;
}

AndroidSigningInfo check_android_signing(const fs::path& path) {
    AndroidSigningInfo info;

    auto ext = path.extension().string();
    if (ext == ".aab") {
        // AABs: use jarsigner -verify (platform-aware)
        auto output = run_cmd("jarsigner -verify \"" + path.string() + "\" 2>&1");
        info.is_signed = output.find("jar verified") != std::string::npos;
        if (!info.is_signed)
            info.error = output;
        return info;
    }

    // APKs: use apksigner verify
    auto tool = find_android_build_tool("apksigner");
    if (tool.empty()) {
        info.error = "apksigner not found — install Android SDK Build Tools";
        return info;
    }

    auto output = run_cmd(shell_invoke_path(tool)
                          + " verify --print-certs --verbose "
                          + shell_quote_path(path) + " 2>&1");

    info.is_signed = output.find("Verified using") != std::string::npos;
    info.v2_signed = output.find("Verified using v2 scheme") != std::string::npos
                  || output.find("v2 scheme (APK Signature Scheme v2): true") != std::string::npos;
    info.v3_signed = output.find("Verified using v3 scheme") != std::string::npos
                  || output.find("v3 scheme (APK Signature Scheme v3): true") != std::string::npos;

    // Parse signer CN
    std::regex cn_re("CN=([^,\\n]+)");
    std::smatch match;
    if (std::regex_search(output, match, cn_re))
        info.signer_cn = match[1].str();

    if (!info.is_signed)
        info.error = output;

    return info;
}

// ── High-level packaging ────────────────────────────────────────────────────

AndroidPackageResult build_android_package(
    const fs::path& gradle_project_dir,
    const std::vector<std::string>& abis,
    const AndroidKeystoreConfig* keystore,
    bool build_aab,
    bool build_apk) {

    AndroidPackageResult result;

    auto gradlew = find_gradlew(gradle_project_dir);
    if (gradlew.empty()) {
        result.error = "gradlew not found in " + gradle_project_dir.string();
        return result;
    }

    // Build Gradle tasks
    std::string tasks;
    if (build_apk) tasks += " assembleRelease";
    if (build_aab) tasks += " bundleRelease";

    // Set ABI filter via Gradle property
    std::string abi_filter;
    if (!abis.empty()) {
        for (size_t i = 0; i < abis.size(); ++i) {
            if (i > 0) abi_filter += ",";
            abi_filter += abis[i];
        }
    }

    // Build command
    std::string cmd = shell_invoke_path(gradlew) + tasks;

    if (!abi_filter.empty())
        cmd += " -Pandroid.injected.build.abi=" + abi_filter;

    // Pass signing config via Gradle properties if keystore provided
    if (keystore) {
        auto store_pass = resolve_password(keystore->store_password);
        auto key_pass = resolve_password(
            keystore->key_password.empty() ? keystore->store_password : keystore->key_password);
        cmd += " " + shell_quote_path_arg("-Pandroid.injected.signing.store.file=",
                                           keystore->keystore_path);
        cmd += " " + shell_quote_arg("-Pandroid.injected.signing.store.password=" + store_pass);
        cmd += " " + shell_quote_arg("-Pandroid.injected.signing.key.alias=" + keystore->key_alias);
        cmd += " " + shell_quote_arg("-Pandroid.injected.signing.key.password=" + key_pass);
    }

    // Run Gradle (can take minutes for a full build)
    int rc = run_status_in_directory(gradle_project_dir, cmd, 600000);
    if (rc != 0) {
        result.error = "Gradle build failed (exit code " + std::to_string(rc) + ")";
        return result;
    }

    // Collect artifacts
    auto outputs = gradle_project_dir / "app" / "build" / "outputs";

    if (build_apk) {
        auto apk_dir = outputs / "apk" / "release";
        if (fs::exists(apk_dir)) {
            for (auto& entry : fs::directory_iterator(apk_dir)) {
                if (entry.path().extension() == ".apk"
                    && entry.path().filename().string().find("unsigned") == std::string::npos) {
                    result.apk_path = entry.path();
                    break;
                }
            }
        }
    }

    if (build_aab) {
        auto aab_dir = outputs / "bundle" / "release";
        if (fs::exists(aab_dir)) {
            for (auto& entry : fs::directory_iterator(aab_dir)) {
                if (entry.path().extension() == ".aab") {
                    result.aab_path = entry.path();
                    break;
                }
            }
        }
    }

    result.success = (!build_apk || !result.apk_path.empty())
                  && (!build_aab || !result.aab_path.empty());

    if (!result.success && result.error.empty())
        result.error = "Build succeeded but expected artifacts not found in " + outputs.string();

    return result;
}

bool aab_to_apks(const fs::path& aab_path,
                 const fs::path& output_apks,
                 const AndroidKeystoreConfig* keystore) {
    // bundletool must be on PATH or at BUNDLETOOL env var
    std::string bundletool = "bundletool";
    if (auto env = get_env("BUNDLETOOL"))
        bundletool = *env;

    std::string cmd = shell_invoke_command(bundletool) + " build-apks"
        " " + shell_quote_path_arg("--bundle=", aab_path) +
        " " + shell_quote_path_arg("--output=", output_apks) +
        " --overwrite";

    if (keystore) {
        auto store_pass = resolve_password(keystore->store_password);
        auto key_pass = resolve_password(
            keystore->key_password.empty() ? keystore->store_password : keystore->key_password);
        cmd += " " + shell_quote_path_arg("--ks=", keystore->keystore_path);
        cmd += " " + shell_quote_arg("--ks-key-alias=" + keystore->key_alias);
        cmd += " " + shell_quote_arg("--ks-pass=pass:" + store_pass);
        cmd += " " + shell_quote_arg("--key-pass=pass:" + key_pass);
    }

    return run_status(cmd) == 0;
}

} // namespace pulp::ship
