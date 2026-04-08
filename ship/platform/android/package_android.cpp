// Android signing and packaging for Pulp
// Host-side tool — runs on macOS/Windows/Linux dev machine, not on Android device.
// Shells out to Android SDK build-tools (apksigner, zipalign, bundletool)
// and Gradle for APK/AAB generation.

#include <pulp/ship/android.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>

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
static std::string exec_cmd_win(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("cmd.exe", {"/c", cmd}, timeout_ms);
    auto result = r.stdout_output;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}
static int exec_status_win(const std::string& cmd, int timeout_ms = 120000) {
    auto r = pulp::platform::exec("cmd.exe", {"/c", cmd}, timeout_ms);
    return r.exit_code;
}
#endif

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

static fs::path find_sdk_root() {
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

    // Sort by version string (lexicographic works for semver with same digit count)
    std::sort(versions.begin(), versions.end());
    return versions.back();
}

static fs::path find_build_tool(const std::string& name) {
    auto sdk = find_sdk_root();
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
    auto tool = find_build_tool("zipalign");
    if (tool.empty()) return false;

    std::string cmd = "\"" + tool.string() + "\" -f 4 \""
        + input_apk.string() + "\" \"" + output_apk.string() + "\"";
#ifdef _WIN32
    return exec_status_win(cmd) == 0;
#else
    return exec_status(cmd) == 0;
#endif
}

bool sign_apk(const fs::path& apk_path, const AndroidKeystoreConfig& keystore) {
    auto tool = find_build_tool("apksigner");
    if (tool.empty()) return false;

    auto store_pass = resolve_password(keystore.store_password);
    auto key_pass = resolve_password(
        keystore.key_password.empty() ? keystore.store_password : keystore.key_password);

    std::string cmd = "\"" + tool.string() + "\" sign"
        " --ks \"" + keystore.keystore_path.string() + "\""
        " --ks-key-alias \"" + keystore.key_alias + "\""
        " --ks-pass pass:" + store_pass +
        " --key-pass pass:" + key_pass +
        " --v2-signing-enabled true"
        " --v3-signing-enabled true"
        " \"" + apk_path.string() + "\"";
#ifdef _WIN32
    return exec_status_win(cmd) == 0;
#else
    return exec_status(cmd) == 0;
#endif
}

bool sign_aab(const fs::path& aab_path, const AndroidKeystoreConfig& keystore) {
    auto store_pass = resolve_password(keystore.store_password);
    auto key_pass = resolve_password(
        keystore.key_password.empty() ? keystore.store_password : keystore.key_password);

    // AABs use jarsigner (Google re-signs with Play App Signing)
    std::string cmd = "jarsigner"
        " -keystore \"" + keystore.keystore_path.string() + "\""
        " -storepass " + store_pass +
        " -keypass " + key_pass +
        " \"" + aab_path.string() + "\""
        " \"" + keystore.key_alias + "\"";
#ifdef _WIN32
    return exec_status_win(cmd) == 0;
#else
    return exec_status(cmd) == 0;
#endif
}

AndroidSigningInfo check_android_signing(const fs::path& path) {
    AndroidSigningInfo info;

    auto ext = path.extension().string();
    if (ext == ".aab") {
        // AABs: use jarsigner -verify
        auto output = exec_cmd("jarsigner -verify \"" + path.string() + "\" 2>&1");
        info.is_signed = output.find("jar verified") != std::string::npos;
        if (!info.is_signed)
            info.error = output;
        return info;
    }

    // APKs: use apksigner verify
    auto tool = find_build_tool("apksigner");
    if (tool.empty()) {
        info.error = "apksigner not found — install Android SDK Build Tools";
        return info;
    }

    auto output = exec_cmd("\"" + tool.string() + "\" verify --print-certs --verbose \""
                           + path.string() + "\" 2>&1");

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
    std::string cmd = "cd \"" + gradle_project_dir.string() + "\" && \""
        + gradlew.string() + "\"" + tasks;

    if (!abi_filter.empty())
        cmd += " -Pandroid.injected.build.abi=" + abi_filter;

    // Pass signing config via Gradle properties if keystore provided
    if (keystore) {
        auto store_pass = resolve_password(keystore->store_password);
        auto key_pass = resolve_password(
            keystore->key_password.empty() ? keystore->store_password : keystore->key_password);
        cmd += " -Pandroid.injected.signing.store.file=\"" + keystore->keystore_path.string() + "\"";
        cmd += " -Pandroid.injected.signing.store.password=" + store_pass;
        cmd += " -Pandroid.injected.signing.key.alias=" + keystore->key_alias;
        cmd += " -Pandroid.injected.signing.key.password=" + key_pass;
    }

    // Run Gradle (can take minutes for a full build)
#ifdef _WIN32
    int rc = exec_status_win(cmd, 600000);
#else
    int rc = exec_status(cmd, 600000);
#endif
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

    std::string cmd = bundletool + " build-apks"
        " --bundle=\"" + aab_path.string() + "\""
        " --output=\"" + output_apks.string() + "\""
        " --overwrite";

    if (keystore) {
        auto store_pass = resolve_password(keystore->store_password);
        auto key_pass = resolve_password(
            keystore->key_password.empty() ? keystore->store_password : keystore->key_password);
        cmd += " --ks=\"" + keystore->keystore_path.string() + "\"";
        cmd += " --ks-key-alias=" + keystore->key_alias;
        cmd += " --ks-pass=pass:" + store_pass;
        cmd += " --key-pass=pass:" + key_pass;
    }

#ifdef _WIN32
    return exec_status_win(cmd) == 0;
#else
    return exec_status(cmd) == 0;
#endif
}

} // namespace pulp::ship
