// cmd_ship.cpp — pulp ship command
// Handles: sign, notarize, package, appcast, check

#include "cli_common.hpp"

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
            else return unknown_ship_arg(sub, args[i]);
        }

        if (apk_only && aab_only) {
            std::cerr << "Error: --apk-only and --aab-only are mutually exclusive.\n";
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
        // macOS/Linux: use pkgbuild (macOS) or deb (Linux)
        int pkg_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            std::string format_lower = dir_name;
            for (auto& c : format_lower) c = static_cast<char>(std::tolower(c));

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    auto name = entry.path().stem().string();
                    auto pkg_name = name + "-" + dir_name + "-" + version + ".pkg";
                    auto pkg_path = artifacts / pkg_name;

                    std::string install_loc = "/Library/Audio/Plug-Ins/";
                    if (ext == ".vst3") install_loc += "VST3/";
                    else if (ext == ".clap") install_loc += "CLAP/";
                    else install_loc = pulp::runtime::get_env("HOME").value_or("~")
                                     + "/Library/Audio/Plug-Ins/Components/";

                    std::cout << "Packaging " << name << " (" << dir_name << ")...\n";
                    std::string cmd = "pkgbuild --component \"" + entry.path().string() + "\""
                        + " --identifier \"com.pulp." + name + "." + format_lower + "\""
                        + " --version \"" + version + "\""
                        + " --install-location \"" + install_loc + "\""
                        + " \"" + pkg_path.string() + "\" 2>/dev/null";
                    if (run(cmd) == 0) ++pkg_count;
                    else std::cerr << "  FAILED\n";
                }
            }
        }
        std::cout << "Created " << pkg_count << " packages in " << artifacts.string() << "\n";
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
                auto sig = pulp::ship::sign_file_ed25519(url_as_path.string(), sign_key);
                if (!sig || sig->empty()) {
                    std::cerr << "Error: --sign-key requested but Ed25519 signing is not "
                                 "available in this build. Refusing to emit an empty "
                                 "signature into the appcast.\n";
                    std::cerr << "  Follow-up tracked at: "
                                 "https://github.com/danielraffel/pulp/issues/295\n";
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
        fs::create_directories(fs::path(output_path).parent_path());
        std::ofstream out(output_path);
        if (!out) { std::cerr << "Error: cannot write to " << output_path << "\n"; return 1; }
        out << feed.to_xml();
        std::cout << "Appcast written to " << output_path << " (" << feed.items.size() << " items)\n";
        return 0;
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
    std::cout << "             --target android --keystore key.jks --abi arm64-v8a|x86_64|all\n";
    std::cout << "  appcast    Generate Sparkle-compatible update feed\n";
    std::cout << "             --url https://... --version 1.0.0 --notes \"...\"\n";
    std::cout << "  check      Check signing status of built plugins\n";
    std::cout << "             --target android  (check APK/AAB in artifacts/)\n";
    return 0;
}
