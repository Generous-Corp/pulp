#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>

namespace pulp::ship {

// Keystore configuration for Android signing
struct AndroidKeystoreConfig {
    std::filesystem::path keystore_path;   // Path to .jks or .keystore file
    std::string store_password;            // Store password (or @env:VAR_NAME)
    std::string key_alias;                 // Key alias within the keystore
    std::string key_password;              // Key password (defaults to store_password)
};

// Result of checking an APK/AAB's signing status
struct AndroidSigningInfo {
    bool is_signed = false;
    bool v2_signed = false;       // APK Signature Scheme v2
    bool v3_signed = false;       // APK Signature Scheme v3
    std::string signer_cn;        // Certificate Common Name
    std::string error;
};

// Result of a build + package operation
struct AndroidPackageResult {
    bool success = false;
    std::filesystem::path apk_path;       // Release APK (if built)
    std::filesystem::path aab_path;       // Release AAB (if built)
    std::string error;
};

// ── Core signing operations ─────────────────────────────────────────────────

// Align an APK (must happen before signing)
bool zipalign_apk(const std::filesystem::path& input_apk,
                  const std::filesystem::path& output_apk);

// Sign an APK with apksigner (v2 + v3 schemes)
bool sign_apk(const std::filesystem::path& apk_path,
              const AndroidKeystoreConfig& keystore);

// Sign an AAB with jarsigner (Play Store re-signs with Play App Signing)
bool sign_aab(const std::filesystem::path& aab_path,
              const AndroidKeystoreConfig& keystore);

// Verify signing status of an APK or AAB
AndroidSigningInfo check_android_signing(const std::filesystem::path& path);

// ── High-level packaging ────────────────────────────────────────────────────

// Build APK and/or AAB via Gradle
// gradle_project_dir: the android/ subdir of the Pulp project
// keystore: null = debug signing
AndroidPackageResult build_android_package(
    const std::filesystem::path& gradle_project_dir,
    const std::vector<std::string>& abis,
    const AndroidKeystoreConfig* keystore = nullptr,
    bool build_aab = true,
    bool build_apk = true);

// Convert AAB to split APKs for local device testing via bundletool
bool aab_to_apks(const std::filesystem::path& aab_path,
                 const std::filesystem::path& output_apks,
                 const AndroidKeystoreConfig* keystore = nullptr);

// ── SDK discovery (host-side — used by doctor checks and ship commands) ──────

// Minimum versions for Android toolchain
constexpr int PULP_ANDROID_MIN_NDK = 26;
constexpr int PULP_ANDROID_MIN_JAVA = 17;
inline const char* PULP_ANDROID_MIN_BUILD_TOOLS = "30.0.0";

// Find Android SDK root (ANDROID_HOME → ANDROID_SDK_ROOT → platform defaults)
std::filesystem::path detect_android_sdk();

// Find Android NDK (ANDROID_NDK_HOME → <sdk>/ndk/<latest>/)
std::filesystem::path find_android_ndk();

// Find a build-tools binary (apksigner, zipalign) in the latest build-tools dir
std::filesystem::path find_android_build_tool(const std::string& name);

// Get the installed build-tools version string (e.g., "34.0.0")
std::string android_build_tools_version();

// Detect Java version from `java -version` output (e.g., "21.0.2")
std::string detect_java_version();

} // namespace pulp::ship
