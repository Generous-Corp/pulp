// cmd_ship.cpp — pulp ship command
// Handles: sign, notarize, package, appcast, check

#include "cli_common.hpp"
#include "xcode_developer_path.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#include <pulp/ship/android.hpp>
#include <pulp/ship/appcast.hpp>
#include <pulp/ship/codesign.hpp>
#include <pulp/ship/installer.hpp>

// ── Config fallback helper ──────────────────────────────────────────────────
// Resolve a ship config value: CLI flag → env var → global config.toml

static std::string ship_config(const std::string& cli_val,
                                const std::string& env_name,
                                const std::string& section,
                                const std::string& key) {
    if (!cli_val.empty()) return cli_val;
    if (!env_name.empty()) {
        if (auto env = pulp::runtime::get_env(env_name))
            if (!env->empty()) return *env;
    }
    return read_user_config_value(section, key);
}

static bool take_ship_value(const std::vector<std::string>& args,
                            size_t& i,
                            const std::string& subcommand,
                            const std::string& flag,
                            std::string& out) {
    if (i + 1 >= args.size() || args[i + 1].empty()) {
        std::cerr << "pulp ship " << subcommand << ": " << flag
                  << " requires a value\n";
        return false;
    }
    out = args[++i];
    return true;
}

static int unknown_ship_arg(const std::string& subcommand,
                            const std::string& arg) {
    std::cerr << "pulp ship " << subcommand << ": unknown argument: "
              << arg << "\n";
    return 2;
}

int cmd_ship(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    // Parse subcommand
    std::string sub = args.empty() ? "help" : args[0];

    // ── sign ────────────────────────────────────────────────────────────────
    if (sub == "sign") {
        std::string identity, target, keystore_path, key_alias, store_pass, key_pass;
        std::string entitlements = (root / "ship" / "templates" / "entitlements.plist").string();
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--identity") {
                if (!take_ship_value(args, i, sub, args[i], identity)) return 2;
            } else if (args[i] == "--entitlements") {
                if (!take_ship_value(args, i, sub, args[i], entitlements)) return 2;
            } else if (args[i] == "--target") {
                if (!take_ship_value(args, i, sub, args[i], target)) return 2;
            } else if (args[i] == "--keystore") {
                if (!take_ship_value(args, i, sub, args[i], keystore_path)) return 2;
            } else if (args[i] == "--key-alias") {
                if (!take_ship_value(args, i, sub, args[i], key_alias)) return 2;
            } else if (args[i] == "--store-pass") {
                if (!take_ship_value(args, i, sub, args[i], store_pass)) return 2;
            } else if (args[i] == "--key-pass") {
                if (!take_ship_value(args, i, sub, args[i], key_pass)) return 2;
            } else {
                return unknown_ship_arg(sub, args[i]);
            }
        }

        if (target == "android") {
            keystore_path = ship_config(keystore_path, "", "signing.android", "keystore");
            key_alias = ship_config(key_alias, "", "signing.android", "key_alias");
            store_pass = ship_config(store_pass, "ANDROID_STORE_PASS", "signing.android", "store_pass");
            key_pass = ship_config(key_pass, "ANDROID_KEY_PASS", "signing.android", "key_pass");

            if (keystore_path.empty()) {
                std::cerr << "Error: No Android keystore specified.\n\n";
                std::cerr << "  pulp ship sign --target android --keystore ~/keystores/release.jks --key-alias release\n\n";
                std::cerr << "  To create a release keystore:\n";
                std::cerr << "    keytool -genkey -v -keystore release.jks -keyalg RSA -keysize 2048 -validity 10000\n\n";
                std::cerr << "  To save for next time, add to ~/.pulp/config.toml:\n";
                std::cerr << "    [signing.android]\n";
                std::cerr << "    keystore  = \"~/keystores/release.jks\"\n";
                std::cerr << "    key_alias = \"release\"\n";
                std::cerr << "    store_pass = \"@env:ANDROID_STORE_PASS\"\n";
                return 1;
            }
            pulp::ship::AndroidKeystoreConfig ks;
            ks.keystore_path = keystore_path;
            ks.key_alias = key_alias.empty() ? "key0" : key_alias;
            ks.store_password = store_pass;
            ks.key_password = key_pass;

            auto artifacts = root / "artifacts";
            int signed_count = 0;
            if (fs::exists(artifacts)) {
                for (auto& entry : fs::directory_iterator(artifacts)) {
                    auto ext = entry.path().extension().string();
                    if (ext == ".apk") {
                        std::cout << "Signing " << entry.path().filename().string() << "...\n";
                        auto aligned = entry.path().parent_path()
                            / (entry.path().stem().string() + "-aligned.apk");
                        if (pulp::ship::zipalign_apk(entry.path(), aligned)) {
                            fs::rename(aligned, entry.path());
                            if (pulp::ship::sign_apk(entry.path(), ks)) ++signed_count;
                            else std::cerr << "  Sign FAILED\n";
                        } else {
                            std::cerr << "  zipalign FAILED\n";
                        }
                    } else if (ext == ".aab") {
                        std::cout << "Signing " << entry.path().filename().string() << "...\n";
                        if (pulp::ship::sign_aab(entry.path(), ks)) ++signed_count;
                        else std::cerr << "  Sign FAILED\n";
                    }
                }
            }
            std::cout << "Signed " << signed_count << " Android artifacts.\n";
            return signed_count > 0 ? 0 : 1;
        }

        // Desktop signing (macOS/Windows) — fall back to config
        identity = ship_config(identity, "PULP_SIGN_IDENTITY", "signing.apple", "identity");

        if (identity.empty()) {
            std::cerr << "Error: No signing identity specified.\n\n";
            std::cerr << "  pulp ship sign --identity \"Developer ID Application: Your Name (TEAMID)\"\n\n";
            std::cerr << "  To find your identity:\n";
#ifdef __APPLE__
            std::cerr << "    security find-identity -v -p codesigning\n";
            std::cerr << "    Or: Keychain Access → My Certificates\n\n";
#else
            std::cerr << "    Check your certificate store for a code signing certificate.\n\n";
#endif
            std::cerr << "  To save for next time, add to ~/.pulp/config.toml:\n";
            std::cerr << "    [signing.apple]\n";
            std::cerr << "    identity = \"Developer ID Application: Your Name (TEAMID)\"\n\n";
            std::cerr << "  Or run: pulp config set signing.apple.identity \"Developer ID Application: ...\"\n";
            return 1;
        }

        int signed_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << "Signing " << entry.path().filename().string() << "...\n";
                    std::string cmd = "codesign --force --sign \"" + identity
                        + "\" --timestamp --options runtime"
                        + " --entitlements \"" + entitlements + "\""
                        + " \"" + entry.path().string() + "\"";
                    if (run(cmd) == 0) ++signed_count;
                    else std::cerr << "  FAILED\n";
                }
            }
        }
        if (signed_count == 0)
            std::cerr << "No plugin bundles found to sign. Run `pulp build` first.\n";
        else
            std::cout << "Signed " << signed_count << " bundles.\n";
        return signed_count > 0 ? 0 : 1;
    }

    // ── package ─────────────────────────────────────────────────────────────
    if (sub == "package") {
        auto artifacts = root / "artifacts";
        fs::create_directories(artifacts);

        std::string version, target, keystore_path, key_alias, store_pass, key_pass, abi_arg;
        bool per_user = false, apk_only = false, aab_only = false;
        // Item 7.5: per-artifact packaging on macOS. When the user
        // passes neither flag we use the historical default — `.pkg`
        // for every plugin bundle, which is right for AU/VST3/CLAP
        // under `~/Library/Audio/Plug-Ins/`. `--dmg` flips standalone
        // apps to disk images. `--pkg` is explicit form of the default.
        bool want_pkg = false, want_dmg = false;

        // Read version from CMakeLists.txt project(VERSION), falling back to SDK version
        auto cmake_ver = read_project_cmake_version(root);
        version = cmake_ver.empty() ? std::string(PULP_SDK_VERSION) : cmake_ver;

        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version") {
                if (!take_ship_value(args, i, sub, args[i], version)) return 2;
            } else if (args[i] == "--target") {
                if (!take_ship_value(args, i, sub, args[i], target)) return 2;
            } else if (args[i] == "--keystore") {
                if (!take_ship_value(args, i, sub, args[i], keystore_path)) return 2;
            } else if (args[i] == "--key-alias") {
                if (!take_ship_value(args, i, sub, args[i], key_alias)) return 2;
            } else if (args[i] == "--store-pass") {
                if (!take_ship_value(args, i, sub, args[i], store_pass)) return 2;
            } else if (args[i] == "--key-pass") {
                if (!take_ship_value(args, i, sub, args[i], key_pass)) return 2;
            } else if (args[i] == "--abi") {
                if (!take_ship_value(args, i, sub, args[i], abi_arg)) return 2;
            }
            else if (args[i] == "--apk-only") apk_only = true;
            else if (args[i] == "--aab-only") aab_only = true;
            else if (args[i] == "--per-user") per_user = true;
            else if (args[i] == "--pkg") want_pkg = true;
            else if (args[i] == "--dmg") want_dmg = true;
            else return unknown_ship_arg(sub, args[i]);
        }

        if (apk_only && aab_only) {
            std::cerr << "Error: --apk-only and --aab-only are mutually exclusive.\n";
            return 1;
        }
        if (want_pkg && want_dmg) {
            std::cerr << "Error: --pkg and --dmg are mutually exclusive — pass one per "
                         "invocation (item 7.5 picks per artifact, not both at once).\n";
            return 1;
        }

        if (target == "android") {
            auto android_dir = root / "android";
            if (!fs::exists(android_dir / "build.gradle.kts")
                && !fs::exists(android_dir / "build.gradle")) {
                std::cerr << "No android/ project found. Run `pulp create --targets android` first.\n";
                return 1;
            }

            std::vector<std::string> abis;
            if (abi_arg == "all") abis = {"arm64-v8a", "x86_64", "armeabi-v7a"};
            else if (!abi_arg.empty()) abis = {abi_arg};
            else abis = {"arm64-v8a"};

            // Resolve keystore from CLI flags, env vars, or config.toml
            keystore_path = ship_config(keystore_path, "", "signing.android", "keystore");
            key_alias = ship_config(key_alias, "", "signing.android", "key_alias");
            store_pass = ship_config(store_pass, "ANDROID_STORE_PASS", "signing.android", "store_pass");
            key_pass = ship_config(key_pass, "ANDROID_KEY_PASS", "signing.android", "key_pass");

            std::unique_ptr<pulp::ship::AndroidKeystoreConfig> ks;
            if (!keystore_path.empty()) {
                ks = std::make_unique<pulp::ship::AndroidKeystoreConfig>();
                ks->keystore_path = keystore_path;
                ks->key_alias = key_alias.empty() ? "key0" : key_alias;
                ks->store_password = store_pass;
                ks->key_password = key_pass;
            }

            bool build_apk = !aab_only, build_aab = !apk_only;
            std::cout << "Building Android package...\n";
            auto result = pulp::ship::build_android_package(
                android_dir, abis, ks.get(), build_aab, build_apk);

            if (!result.success) {
                std::cerr << "Android build failed: " << result.error << "\n";
                return 1;
            }

            if (!result.apk_path.empty()) {
                auto dest = artifacts / result.apk_path.filename();
                fs::copy_file(result.apk_path, dest, fs::copy_options::overwrite_existing);
                std::cout << "  APK: " << dest.string() << "\n";
            }
            if (!result.aab_path.empty()) {
                auto dest = artifacts / result.aab_path.filename();
                fs::copy_file(result.aab_path, dest, fs::copy_options::overwrite_existing);
                std::cout << "  AAB: " << dest.string() << "\n";
            }
            std::cout << "Android package created in " << artifacts.string() << "\n";
            return 0;
        }

#ifdef _WIN32
        // Windows: use NSIS installer
        if (std::system("where makensis >nul 2>&1") != 0) {
            std::cerr << "Error: makensis not found on PATH\n";
            std::cerr << "  Install NSIS from https://nsis.sourceforge.io/\n";
            std::cerr << "  Then add its directory to PATH\n";
            return 1;
        }

        std::string product_name;
        pulp::ship::InstallerConfig config;
        config.version = version;
        config.per_user_install = per_user;

        for (auto dir_name : {"VST3", "CLAP"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            std::string format_lower = dir_name;
            for (auto& c : format_lower) c = static_cast<char>(std::tolower(c));

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap") {
                    if (product_name.empty())
                        product_name = entry.path().stem().string();
                    config.plugins.push_back({
                        entry.path().string(), "", format_lower
                    });
                }
            }
        }

        if (config.plugins.empty()) {
            std::cerr << "Error: no plugins found in " << build_dir.string() << "\n";
            return 1;
        }

        config.product_name = product_name;
        config.publisher = "Pulp";
        config.output_path = (artifacts / (product_name + "-" + version + "-setup.exe")).string();

        auto license = root / "LICENSE.md";
        if (fs::exists(license)) config.license_path = license.string();

        std::cout << "Creating NSIS installer for " << product_name << "...\n";
        if (pulp::ship::create_nsis_installer(config)) {
            std::cout << "  Created " << config.output_path << "\n";
        } else {
            std::cerr << "  FAILED to create installer\n";
            return 1;
        }
        return 0;
#else
        // macOS / Linux. On macOS we honour the item 7.5 per-artifact
        // selection: `.pkg` for AU/VST3/CLAP plugin bundles, `.dmg`
        // for `.app` standalones. `--pkg` and `--dmg` flags override
        // the per-artifact default for the entire run (e.g. `--dmg`
        // wraps even plugin bundles in a disk image when that's
        // explicitly requested, useful for the "drag to Plug-Ins"
        // distribution pattern).
        int pkg_count = 0, dmg_count = 0;

#ifdef __APPLE__
        // Standalone `.app` bundles → `.dmg` by default (or when --dmg).
        auto standalone_dir = build_dir / "Standalone";
        if (fs::exists(standalone_dir)) {
            for (auto& entry : fs::directory_iterator(standalone_dir)) {
                if (entry.path().extension().string() != ".app") continue;
                auto name = entry.path().stem().string();
                auto dmg_name = name + "-" + version + ".dmg";
                auto dmg_path = artifacts / dmg_name;
                std::cout << "Packaging " << name << " (Standalone .app → .dmg)...\n";
                if (pulp::ship::create_dmg(entry.path().string(),
                                           dmg_path.string(), name)) {
                    ++dmg_count;
                } else {
                    std::cerr << "  FAILED to create .dmg for " << name << "\n";
                }
            }
        }
#endif

        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            std::string format_lower = dir_name;
            for (auto& c : format_lower) c = static_cast<char>(std::tolower(c));

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext != ".vst3" && ext != ".clap" && ext != ".component") continue;

                auto name = entry.path().stem().string();

#ifdef __APPLE__
                // Plugin bundle → .dmg ONLY when the user explicitly
                // asked. Default + --pkg both produce .pkg.
                if (want_dmg) {
                    auto dmg_name = name + "-" + dir_name + "-" + version + ".dmg";
                    auto dmg_path = artifacts / dmg_name;
                    std::cout << "Packaging " << name << " (" << dir_name
                              << " → .dmg)...\n";
                    if (pulp::ship::create_dmg(entry.path().string(),
                                               dmg_path.string(),
                                               name + " " + dir_name)) {
                        ++dmg_count;
                    } else {
                        std::cerr << "  FAILED to create .dmg\n";
                    }
                    continue;
                }
#endif

                auto pkg_name = name + "-" + dir_name + "-" + version + ".pkg";
                auto pkg_path = artifacts / pkg_name;

                std::string install_loc = "/Library/Audio/Plug-Ins/";
                if (ext == ".vst3") install_loc += "VST3/";
                else if (ext == ".clap") install_loc += "CLAP/";
                else install_loc = pulp::runtime::get_env("HOME").value_or("~")
                                 + "/Library/Audio/Plug-Ins/Components/";

                std::cout << "Packaging " << name << " (" << dir_name << " → .pkg)...\n";
                std::string cmd = "pkgbuild --component \"" + entry.path().string() + "\""
                    + " --identifier \"com.pulp." + name + "." + format_lower + "\""
                    + " --version \"" + version + "\""
                    + " --install-location \"" + install_loc + "\""
                    + " \"" + pkg_path.string() + "\" 2>/dev/null";
                if (run(cmd) == 0) ++pkg_count;
                else std::cerr << "  FAILED\n";
            }
        }
        std::cout << "Created " << pkg_count << " .pkg and " << dmg_count
                  << " .dmg artifacts in " << artifacts.string() << "\n";
        return 0;
#endif
    }

    // ── check ───────────────────────────────────────────────────────────────
    if (sub == "check") {
        std::string target;
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] == "--target") {
                if (!take_ship_value(args, i, sub, args[i], target)) return 2;
            } else {
                return unknown_ship_arg(sub, args[i]);
            }

        if (target == "android") {
            auto art_dir = root / "artifacts";
            if (!fs::exists(art_dir)) {
                std::cerr << "No artifacts/ directory. Run `pulp ship package --target android` first.\n";
                return 1;
            }
            for (auto& entry : fs::directory_iterator(art_dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".apk" || ext == ".aab") {
                    std::cout << entry.path().filename().string() << ": ";
                    auto info = pulp::ship::check_android_signing(entry.path());
                    if (info.is_signed) {
                        std::cout << "signed";
                        if (ext == ".apk") {
                            if (info.v2_signed) std::cout << " v2";
                            if (info.v3_signed) std::cout << " v3";
                        }
                        if (!info.signer_cn.empty()) std::cout << " (" << info.signer_cn << ")";
                        std::cout << "\n";
                    } else {
                        std::cout << "unsigned\n";
                    }
                }
            }
            return 0;
        }

        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << entry.path().filename().string() << ": ";
                    int rc = run("codesign --verify --deep --strict \""
                                 + entry.path().string() + "\" 2>/dev/null");
                    std::cout << (rc == 0 ? "signed" : "unsigned") << "\n";
                }
            }
        }
        return 0;
    }

    // ── notarize ────────────────────────────────────────────────────────────
    if (sub == "notarize") {
#ifndef __APPLE__
        std::cerr << "Notarization is only available on macOS.\n";
        return 1;
#else
        std::string apple_id, team_id, password;
        bool staple_only = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--apple-id") {
                if (!take_ship_value(args, i, sub, args[i], apple_id)) return 2;
            } else if (args[i] == "--team-id") {
                if (!take_ship_value(args, i, sub, args[i], team_id)) return 2;
            } else if (args[i] == "--password") {
                if (!take_ship_value(args, i, sub, args[i], password)) return 2;
            }
            else if (args[i] == "--staple") staple_only = true;
            else return unknown_ship_arg(sub, args[i]);
        }

        std::vector<std::string> bundles;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component")
                    bundles.push_back(entry.path().string());
            }
        }

        if (bundles.empty()) {
            std::cerr << "No plugin bundles found.\n";
            return 1;
        }

        if (staple_only) {
            int stapled = 0;
            for (auto& path : bundles) {
                std::cout << "Stapling " << fs::path(path).filename().string() << "...\n";
                if (pulp::ship::notarize_staple(path)) ++stapled;
                else std::cerr << "  FAILED\n";
            }
            std::cout << "Stapled " << stapled << "/" << bundles.size() << " bundles.\n";
            return stapled == static_cast<int>(bundles.size()) ? 0 : 1;
        }

        // Fall back to config
        apple_id = ship_config(apple_id, "PULP_APPLE_ID", "signing.apple", "apple_id");
        team_id = ship_config(team_id, "PULP_TEAM_ID", "signing.apple", "team_id");
        password = ship_config(password, "", "signing.apple", "password");

        if (apple_id.empty() || team_id.empty()) {
            std::cerr << "Error: Apple ID and Team ID required for notarization.\n\n";
            std::cerr << "  pulp ship notarize --apple-id you@example.com --team-id ABCDE12345\n\n";
            std::cerr << "  Where to find these:\n";
            std::cerr << "    Apple ID:  Your Apple Developer account email\n";
            std::cerr << "    Team ID:   https://developer.apple.com/account → Membership → Team ID\n";
            std::cerr << "    Password:  https://appleid.apple.com → Sign-In and Security → App-Specific Passwords\n";
            std::cerr << "               Then store in Keychain:\n";
            std::cerr << "               security add-generic-password -s AC_PASSWORD -a you@example.com -w\n\n";
            std::cerr << "  To save for next time, add to ~/.pulp/config.toml:\n";
            std::cerr << "    [signing.apple]\n";
            std::cerr << "    apple_id = \"you@example.com\"\n";
            std::cerr << "    team_id  = \"ABCDE12345\"\n";
            std::cerr << "    password = \"@keychain:AC_PASSWORD\"\n";
            return 1;
        }
        if (password.empty()) password = "@keychain:AC_PASSWORD";

        int success_count = 0;
        for (auto& path : bundles) {
            auto name = fs::path(path).filename().string();
            std::cout << "Submitting " << name << " for notarization...\n";
            auto uuid = pulp::ship::notarize_submit(path, apple_id, team_id, password);
            if (!uuid) { std::cerr << "  Submission FAILED\n"; continue; }
            std::cout << "  Request UUID: " << *uuid << "\n";
            auto status = pulp::ship::notarize_check(*uuid);
            if (status.success) {
                std::cout << "  Notarization succeeded. Stapling...\n";
                if (pulp::ship::notarize_staple(path)) std::cout << "  Stapled " << name << "\n";
                else std::cerr << "  Staple FAILED\n";
                ++success_count;
            } else {
                std::cerr << "  Notarization FAILED: " << status.message << "\n";
            }
        }
        std::cout << "Notarized " << success_count << "/" << bundles.size() << " bundles.\n";
        return success_count == static_cast<int>(bundles.size()) ? 0 : 1;
#endif
    }

    // ── release (sign → package → notarize → staple, one command) ──────────
    //
    // Item 7.4 (macos-plugin-authoring-plan): one-command pipeline that
    // walks the entire macOS distribution surface for a Pulp plugin.
    // Equivalent to (under the hood):
    //
    //   pulp ship sign     --identity "..."
    //   pulp ship package  --version "..."  [--pkg | --dmg]
    //   pulp ship notarize --apple-id ... --team-id ...    (macos only)
    //   pulp ship notarize --staple                        (final step)
    //
    // Each stage short-circuits on failure so a broken sign doesn't try
    // to notarize garbage. Stages are skippable so CI lanes that only
    // care about, say, the package step can `--skip-sign --skip-notarize`.
    //
    // Notarization requires real Apple credentials — when `--skip-notarize`
    // is passed (or when neither `--apple-id`/`--team-id` nor
    // `signing.apple.*` config is set), the pipeline runs sign+package
    // only and exits cleanly so CI can still exercise the orchestration
    // without notarytool round-trips.
    if (sub == "release") {
#ifndef __APPLE__
        std::cerr << "pulp ship release: macOS-only (notarization + .pkg/.dmg).\n";
        return 1;
#else
        std::string target = "macos";
        std::string identity, apple_id, team_id, password, version;
        bool want_pkg = false, want_dmg = false;
        bool skip_sign = false, skip_package = false, skip_notarize = false;

        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--target") {
                if (!take_ship_value(args, i, sub, args[i], target)) return 2;
            } else if (args[i] == "--identity") {
                if (!take_ship_value(args, i, sub, args[i], identity)) return 2;
            } else if (args[i] == "--apple-id") {
                if (!take_ship_value(args, i, sub, args[i], apple_id)) return 2;
            } else if (args[i] == "--team-id") {
                if (!take_ship_value(args, i, sub, args[i], team_id)) return 2;
            } else if (args[i] == "--password") {
                if (!take_ship_value(args, i, sub, args[i], password)) return 2;
            } else if (args[i] == "--version") {
                if (!take_ship_value(args, i, sub, args[i], version)) return 2;
            } else if (args[i] == "--pkg") {
                want_pkg = true;
            } else if (args[i] == "--dmg") {
                want_dmg = true;
            } else if (args[i] == "--skip-sign") {
                skip_sign = true;
            } else if (args[i] == "--skip-package") {
                skip_package = true;
            } else if (args[i] == "--skip-notarize") {
                skip_notarize = true;
            } else {
                return unknown_ship_arg(sub, args[i]);
            }
        }

        if (target != "macos") {
            std::cerr << "pulp ship release: --target " << target
                      << " is not implemented yet (only macos).\n";
            return 1;
        }
        if (want_pkg && want_dmg) {
            std::cerr << "pulp ship release: --pkg and --dmg are mutually "
                         "exclusive (item 7.5 picks one per artifact).\n";
            return 2;
        }

        // Stage 1: sign. Skippable for CI dry-runs.
        if (!skip_sign) {
            std::cout << "── Stage 1/4: sign ────────────────────────────────\n";
            std::vector<std::string> sign_args = {"sign"};
            if (!identity.empty()) {
                sign_args.push_back("--identity");
                sign_args.push_back(identity);
            }
            int sign_rc = cmd_ship(sign_args);
            if (sign_rc != 0) {
                std::cerr << "pulp ship release: sign stage failed; aborting "
                             "before package/notarize.\n";
                return sign_rc;
            }
        } else {
            std::cout << "── Stage 1/4: sign (SKIPPED) ──────────────────────\n";
        }

        // Stage 2: package. The pkg vs dmg decision lives in cmd_ship
        // 'package' itself (item 7.5 wiring below).
        if (!skip_package) {
            std::cout << "── Stage 2/4: package ─────────────────────────────\n";
            std::vector<std::string> pkg_args = {"package"};
            if (!version.empty()) {
                pkg_args.push_back("--version");
                pkg_args.push_back(version);
            }
            if (want_pkg) pkg_args.push_back("--pkg");
            if (want_dmg) pkg_args.push_back("--dmg");
            int pkg_rc = cmd_ship(pkg_args);
            if (pkg_rc != 0) {
                std::cerr << "pulp ship release: package stage failed; "
                             "aborting before notarize.\n";
                return pkg_rc;
            }
        } else {
            std::cout << "── Stage 2/4: package (SKIPPED) ───────────────────\n";
        }

        // Stage 3 + 4: notarize + staple. Submission requires Apple
        // credentials — when they aren't available we run the
        // orchestration up to here and exit cleanly. CI can pass
        // `--skip-notarize` explicitly to make that decision visible.
        if (skip_notarize) {
            std::cout << "── Stage 3/4: notarize (SKIPPED) ──────────────────\n";
            std::cout << "── Stage 4/4: staple   (SKIPPED) ──────────────────\n";
            std::cout << "\npulp ship release: sign+package complete; "
                         "notarization skipped by request.\n";
            return 0;
        }

        // Defer credential resolution to the notarize handler — it
        // already has the apple_id/team_id/password CLI→env→config
        // fallback chain and the helpful "where to find these" hint.
        std::cout << "── Stage 3/4: notarize ────────────────────────────\n";
        std::vector<std::string> nz_args = {"notarize"};
        if (!apple_id.empty()) { nz_args.push_back("--apple-id"); nz_args.push_back(apple_id); }
        if (!team_id.empty())  { nz_args.push_back("--team-id");  nz_args.push_back(team_id); }
        if (!password.empty()) { nz_args.push_back("--password"); nz_args.push_back(password); }
        int nz_rc = cmd_ship(nz_args);
        if (nz_rc != 0) {
            std::cerr << "pulp ship release: notarize stage failed.\n";
            return nz_rc;
        }

        // The notarize handler already staples on success, so the
        // Stage 4 line is a status echo, not a re-run.
        std::cout << "── Stage 4/4: staple (handled by notarize stage) ──\n";
        std::cout << "\npulp ship release: macOS pipeline complete.\n";
        return 0;
#endif
    }

    // ── auv3-xcodeproj (item 3.10 follow-up: one-click Xcode flow) ──────────
    //
    // Thin wrapper around `cmake -G Xcode -DPULP_AUV3_TARGET=<name>` that
    // generates an Xcode project ready to "Run" on an iOS Simulator or a
    // connected iOS device. Picks the right SDK + the right entitlements
    // template from `tools/templates/auv3/iOS-{Simulator,Device}-
    // Entitlements.plist.template` (shipped in PR #2938 alongside the
    // CMake template wiring).
    //
    // Usage:
    //   pulp ship auv3-xcodeproj <target>           # iphonesimulator (default)
    //   pulp ship auv3-xcodeproj <target> --sdk iphoneos
    //   pulp ship auv3-xcodeproj <target> --sdk macosx
    //   pulp ship auv3-xcodeproj <target> --output build-ios/MyPlugin.xcodeproj
    //   pulp ship auv3-xcodeproj <target> --open    # open in Xcode after gen
    //
    // The wrapper intentionally writes to a separate build dir (default
    // `build/xcode/<target>-<sdk>`) so it does not collide with the user's
    // normal `build/` Ninja/Makefile cache. The full Xcode-project
    // generation requires Xcode and the matching iOS SDK to be installed;
    // when they are missing we emit a clear scaffold message + exit 0 so
    // CI / sandboxed environments can still exercise the wrapper without a
    // real Xcode install.
    if (sub == "auv3-xcodeproj") {
#ifndef __APPLE__
        std::cerr << "pulp ship auv3-xcodeproj: macOS-only (requires Xcode + iOS SDKs).\n";
        return 1;
#else
        std::string target_name, sdk = "iphonesimulator", output_dir;
        bool open_after = false;
        bool dry_run = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--sdk") {
                if (!take_ship_value(args, i, sub, args[i], sdk)) return 2;
            } else if (args[i] == "--output") {
                if (!take_ship_value(args, i, sub, args[i], output_dir)) return 2;
            } else if (args[i] == "--open") {
                open_after = true;
            } else if (args[i] == "--dry-run") {
                // Print the cmake invocation that would run but do not
                // actually shell out. Lets the shell-out test exercise
                // the argument-parsing + SDK-selection logic without
                // requiring Xcode in CI.
                dry_run = true;
            } else if (!args[i].empty() && args[i][0] != '-' && target_name.empty()) {
                target_name = args[i];
            } else {
                return unknown_ship_arg(sub, args[i]);
            }
        }

        if (target_name.empty()) {
            std::cerr <<
                "Usage: pulp ship auv3-xcodeproj <target> "
                "[--sdk iphonesimulator|iphoneos|macosx] "
                "[--output <dir>] [--open] [--dry-run]\n"
                "Generates an Xcode project for an AUv3 target ready to "
                "Run on the iOS Simulator or a connected device.\n";
            return 2;
        }

        const bool sdk_ok =
            (sdk == "iphonesimulator" || sdk == "iphoneos" || sdk == "macosx");
        if (!sdk_ok) {
            std::cerr << "pulp ship auv3-xcodeproj: unknown --sdk value '" << sdk
                      << "'. Expected one of: iphonesimulator, iphoneos, macosx.\n";
            return 2;
        }

        if (output_dir.empty()) {
            output_dir = (root / "build" / "xcode" /
                          (target_name + "-" + sdk)).string();
        }
        fs::create_directories(output_dir);

        // Resolve the iOS toolchain only for iOS SDKs; macosx uses the
        // default native compiler and CMake's bundled Xcode generator
        // without an extra toolchain file.
        std::string toolchain;
        std::string ios_platform_arg;
        if (sdk == "iphonesimulator") {
            toolchain = (root / "tools" / "cmake" / "ios.toolchain.cmake").string();
            ios_platform_arg = "SIMULATOR64";
        } else if (sdk == "iphoneos") {
            toolchain = (root / "tools" / "cmake" / "ios.toolchain.cmake").string();
            ios_platform_arg = "OS";
        }

        std::string configure_cmd =
            "cmake -S " + shell_quote(root.string()) +
            " -B " + shell_quote(output_dir) +
            " -G Xcode" +
            " -DPULP_AUV3_TARGET=" + shell_quote(target_name);
        if (!toolchain.empty()) {
            // Only require the toolchain on disk when we're actually
            // going to invoke cmake. `--dry-run` is allowed to print
            // the resolved invocation against a fake project that
            // doesn't carry tools/cmake/ios.toolchain.cmake (e.g. the
            // shell-out tests). Real runs still hard-fail below.
            if (!dry_run && !fs::exists(toolchain)) {
                std::cerr << "pulp ship auv3-xcodeproj: iOS toolchain not "
                             "found at " << toolchain << "\n";
                return 1;
            }
            configure_cmd += " -DCMAKE_TOOLCHAIN_FILE=" + shell_quote(toolchain);
            configure_cmd += " -DIOS_PLATFORM=" + ios_platform_arg;
        } else {
            // macosx: explicit sysroot so users on machines with both
            // SDKs installed get a deterministic result.
            configure_cmd += " -DCMAKE_OSX_SYSROOT=macosx";
        }

        std::cout << "pulp ship auv3-xcodeproj: generating Xcode project\n"
                  << "  target = " << target_name << "\n"
                  << "  sdk    = " << sdk << "\n"
                  << "  output = " << output_dir << "\n";

        if (dry_run) {
            std::cout << "  cmake  = " << configure_cmd << "\n";
            std::cout << "(--dry-run: no cmake invocation)\n";
            return 0;
        }

        // Refuse to invoke cmake when the developer-tools shim is the
        // bare `/usr/bin/xcrun`-pointing stub that ships with macOS but
        // has no full Xcode behind it. Generating an Xcode project with
        // only the command-line tools succeeds in part but produces a
        // .xcodeproj that the actual Xcode.app refuses to open.
        //
        // Accept any full-Xcode developer path of the form
        // `<...>/Xcode*.app/Contents/Developer`. We must NOT hard-match
        // the literal substring `Xcode.app` because beta-channel users
        // run `Xcode-beta.app`, `Xcode_15.app`, etc., and the previous
        // check blocked all of them. Regression: #2969 / Codex comment
        // 3305628892. We still reject the command-line-tools-only
        // selection (`/Library/Developer/CommandLineTools`) by requiring
        // both `.app/` AND `/Contents/Developer` in the path.
        std::string xcselect = exec_output("xcode-select -p 2>/dev/null");
        // trim trailing newline
        while (!xcselect.empty() &&
               (xcselect.back() == '\n' || xcselect.back() == '\r'))
            xcselect.pop_back();
        if (xcselect.empty() ||
            !pulp::cli::looks_like_full_xcode_developer_dir(xcselect)) {
            std::cerr << "pulp ship auv3-xcodeproj: full Xcode.app not "
                         "selected (xcode-select -p reports '" << xcselect
                      << "').\n"
                         "Run `sudo xcode-select -s /Applications/Xcode.app/"
                         "Contents/Developer` (or `Xcode-beta.app/Contents/"
                         "Developer`) and retry, or pass --dry-run.\n";
            return 1;
        }

        int rc = run_with_spinner(configure_cmd, "Generating Xcode project");
        if (rc != 0) {
            std::cerr << "pulp ship auv3-xcodeproj: cmake generation failed "
                         "(exit " << rc << ").\n";
            return rc;
        }

        // Resolve the generated .xcodeproj for the open hint + the optional
        // `--open` shortcut. CMake names it <project>.xcodeproj based on
        // the top-level `project()` call, so we glob for the first one we
        // find in the build dir.
        fs::path xcodeproj;
        for (auto& entry : fs::directory_iterator(output_dir)) {
            if (entry.path().extension() == ".xcodeproj") {
                xcodeproj = entry.path();
                break;
            }
        }
        if (xcodeproj.empty()) {
            std::cerr << "pulp ship auv3-xcodeproj: no .xcodeproj produced "
                         "in " << output_dir << ". Check the cmake log.\n";
            return 1;
        }

        std::cout << "\nXcode project: " << xcodeproj.string() << "\n";
        std::cout << "  open in Xcode: open " << shell_quote(xcodeproj.string()) << "\n";
        std::cout << "  build from CLI: cmake --build " << shell_quote(output_dir)
                  << " --target " << target_name << "_AUv3\n";

        if (open_after) {
            std::string open_cmd = "open " + shell_quote(xcodeproj.string());
            int orc = run(open_cmd);
            if (orc != 0) {
                std::cerr << "pulp ship auv3-xcodeproj: `open` returned "
                          << orc << ".\n";
                return orc;
            }
        }
        return 0;
#endif
    }

    // ── appcast ─────────────────────────────────────────────────────────────
    if (sub == "appcast") {
        std::string version, notes, url, output_path, title, sign_key, min_os;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version") {
                if (!take_ship_value(args, i, sub, args[i], version)) return 2;
            } else if (args[i] == "--notes") {
                if (!take_ship_value(args, i, sub, args[i], notes)) return 2;
            } else if (args[i] == "--url") {
                if (!take_ship_value(args, i, sub, args[i], url)) return 2;
            } else if (args[i] == "--output") {
                if (!take_ship_value(args, i, sub, args[i], output_path)) return 2;
            } else if (args[i] == "--title") {
                if (!take_ship_value(args, i, sub, args[i], title)) return 2;
            } else if (args[i] == "--sign-key") {
                if (!take_ship_value(args, i, sub, args[i], sign_key)) return 2;
            } else if (args[i] == "--min-os") {
                if (!take_ship_value(args, i, sub, args[i], min_os)) return 2;
            } else {
                return unknown_ship_arg(sub, args[i]);
            }
        }

        if (version.empty()) version = "0.1.0";
        if (url.empty()) {
            std::cerr << "Usage: pulp ship appcast --url https://example.com/Plugin-1.0.pkg --version 1.0.0\n";
            return 1;
        }
        if (output_path.empty()) output_path = (root / "artifacts" / "appcast.xml").string();

        // Load existing appcast to append
        pulp::ship::Appcast feed;
        {
            std::ifstream existing(output_path);
            if (existing.good()) {
                std::string xml((std::istreambuf_iterator<char>(existing)),
                                std::istreambuf_iterator<char>());
                if (auto parsed = pulp::ship::Appcast::from_xml(xml)) feed = std::move(*parsed);
            }
        }

        if (title.empty() && feed.title.empty()) title = "Plugin Updates";
        if (!title.empty()) feed.title = title;

        pulp::ship::AppcastItem item;
        item.version = version;
        item.title = "Version " + version;
        item.description = notes.empty() ? "" : "<p>" + notes + "</p>";
        item.download_url = url;
        if (!min_os.empty()) item.minimum_os = min_os;

        auto url_as_path = fs::path(url);
        if (fs::exists(url_as_path)) {
            item.file_size = fs::file_size(url_as_path);
            if (!sign_key.empty()) {
                // Hard-fail on missing/failed signing (#295 P0). Writing
                // an empty edSignature into the appcast looked like a
                // successful sign but produced unsigned XML that Sparkle
                // rejects silently — worse than no signing at all.
                // Ed25519 is wired through pulp::runtime::ed25519_sign
                // since macOS plan item 7.3 (vendored TweetNaCl).
                auto sig = pulp::ship::sign_file_ed25519(url_as_path.string(), sign_key);
                if (!sig || sig->empty()) {
                    std::cerr << "Error: --sign-key Ed25519 signing failed. Refusing "
                                 "to emit an empty signature into the appcast.\n";
                    std::cerr << "  Check that the key is a base64-encoded 32-byte "
                                 "seed or 64-byte NaCl secret key,\n";
                    std::cerr << "  and that the artifact at " << url_as_path
                              << " is readable.\n";
                    return 1;
                }
                item.ed_signature = std::move(*sig);
            }
        } else if (!sign_key.empty()) {
            std::cerr << "Error: --sign-key requires a local file path to compute the signature.\n";
            std::cerr << "  The remote URL cannot be signed. Download the file first, then pass\n";
            std::cerr << "  the local path as --url and set --download-url for the enclosure URL.\n";
            return 1;
        }

        { auto now = std::time(nullptr); char buf[64];
          std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", std::localtime(&now));
          item.pub_date = buf; }

        feed.items.insert(feed.items.begin(), std::move(item));
        const auto parent = fs::path(output_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent);
        std::ofstream out(output_path);
        if (!out) { std::cerr << "Error: cannot write to " << output_path << "\n"; return 1; }
        out << feed.to_xml();
        std::cout << "Appcast written to " << output_path << " (" << feed.items.size() << " items)\n";
        return 0;
    }

    if (sub != "help" && sub != "--help" && sub != "-h") {
        std::cerr << "pulp ship: unknown subcommand: " << sub << "\n";
        return 2;
    }

    // ── help ────────────────────────────────────────────────────────────────
    std::cout << "pulp ship — signing, notarization, packaging, and update feeds\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  sign       Sign plugin bundles or Android artifacts\n";
    std::cout << "             --identity \"Developer ID Application: ...\"  (macOS/Windows)\n";
    std::cout << "             --target android --keystore key.jks  (Android)\n";
    std::cout << "  notarize   Submit signed bundles for Apple notarization (macOS)\n";
    std::cout << "             --apple-id you@example.com --team-id ABCDE12345\n";
    std::cout << "             --staple  (staple only, skip submission)\n";
    std::cout << "  package    Create installers for the target platform\n";
    std::cout << "             --version 1.0.0\n";
    std::cout << "             --pkg | --dmg  (item 7.5: per-artifact macOS packaging)\n";
    std::cout << "             --target android --keystore key.jks --abi arm64-v8a|x86_64|all\n";
    std::cout << "  release    macOS: sign → package → notarize → staple in one command\n";
    std::cout << "             --target macos --identity \"...\" --apple-id ... --team-id ...\n";
    std::cout << "             --pkg | --dmg                                       (item 7.5)\n";
    std::cout << "             --skip-sign | --skip-package | --skip-notarize      (CI flags)\n";
    std::cout << "  appcast    Generate Sparkle-compatible update feed\n";
    std::cout << "             --url https://... --version 1.0.0 --notes \"...\"\n";
    std::cout << "  check      Check signing status of built plugins\n";
    std::cout << "             --target android  (check APK/AAB in artifacts/)\n";
    std::cout << "  auv3-xcodeproj  Generate an Xcode project for an AUv3 target\n";
    std::cout << "             <target> [--sdk iphonesimulator|iphoneos|macosx]\n";
    std::cout << "             [--output <dir>] [--open] [--dry-run]\n";
    return 0;
}
