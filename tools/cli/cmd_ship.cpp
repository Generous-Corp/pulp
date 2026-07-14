// cmd_ship.cpp — pulp ship command
// Handles: sign, notarize, package, appcast, check

#include "cli_common.hpp"
#include "notary_env.hpp"
#include "ship_tracing_guard.hpp"
#include "xcode_developer_path.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>

#include <pulp/ship/android.hpp>
#include <pulp/ship/appcast.hpp>
#include <pulp/ship/codesign.hpp>
#include <pulp/ship/installer.hpp>

#include <pulp/format/reload/key_store.hpp>
#include <pulp/format/reload/pack_build.hpp>
#include <pulp/format/reload/pack_signing_ui.hpp>
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>

// MSVC spells the POSIX pipe helpers with a leading underscore. cmd_ship.cpp
// uses popen/pclose to shell out (notarytool, makensis); map them so the CLI
// compiles on Windows. Sibling CLI translation units carry the same guard.
#ifdef _WIN32
#  define popen _popen
#  define pclose _pclose
#endif

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

static int print_ship_help() {
    std::cout << "pulp ship — signing, notarization, packaging, and update feeds\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  sign       Sign plugin bundles or Android artifacts\n";
    std::cout << "             --identity \"Developer ID Application: ...\"  (macOS/Windows)\n";
    std::cout << "             --path <artifact>  "
                 "(sign one explicit desktop artifact; .pkg is signed by package)\n";
    std::cout << "             --target android --keystore key.jks  (Android)\n";
    std::cout << "  notarize   Submit signed bundles for Apple notarization (macOS)\n";
    std::cout << "             --api-key <p8> --api-key-id <id> --api-issuer <uuid>   (preferred)\n";
    std::cout << "             --apple-id you@example.com --team-id ABCDE12345        (legacy)\n";
    std::cout << "             --path <dmg|pkg|zip>  (notarize+staple an explicit artifact; repeatable; not a raw .app)\n";
    std::cout << "             --env-file <path>  (override ~/.config/pulp/secrets/notary.env)\n";
    std::cout << "             --staple   (staple only, skip submission)\n";
    std::cout << "             --dry-run  (print resolved notarytool argv, no submission)\n";
    std::cout << "  package    Create installers for the target platform\n";
    std::cout << "             --version 1.0.0\n";
    std::cout << "             --pkg | --dmg  (item 7.5: per-artifact macOS packaging)\n";
    std::cout << "             --allow-tracing  (override the guard that refuses a PULP_TRACING=ON build)\n";
    std::cout << "             --installer-identity \"Developer ID Installer: ...\"  (sign the .pkg)\n";
    std::cout << "             --target android --keystore key.jks --abi arm64-v8a|x86_64|all\n";
    std::cout << "  release    macOS: sign → package → notarize → staple in one command\n";
    std::cout << "             --target macos --identity \"...\" --apple-id ... --team-id ...\n";
    std::cout << "             --installer-identity \"Developer ID Installer: ...\"  (.pkg signing)\n";
    std::cout << "             --pkg | --dmg   (notarizes the signed .pkg/.dmg it builds) (item 7.5)\n";
    std::cout << "             --skip-sign | --skip-package | --skip-notarize      (CI flags)\n";
    std::cout << "  share      One-shot: sign → (wrap .app in DMG) → notarize → staple → verify\n";
    std::cout << "             <app|dmg|pkg> --identity \"...\" [--version X.Y.Z] [--output <dir>]\n";
    std::cout << "             [--entitlements <plist>] [creds]\n";
    std::cout << "             --dry-run  (print the plan without doing anything)\n";
    std::cout << "  appcast    Generate Sparkle-compatible update feed\n";
    std::cout << "             --url <artifact-or-url> [--download-url https://...] --version 1.0.0\n";
    std::cout << "  check      Check signing status of built desktop plugins or Android APK/AAB artifacts\n";
    std::cout << "             --target android  (check APK/AAB in artifacts/)\n";
    std::cout << "  doctor     Make signing+notarization non-interactive (no keychain/1Password prompt)\n";
    std::cout << "             --check-online  (validate the .p8 against Apple, refresh pulp-notary profile)\n";
    std::cout << "             --print-env     (emit resolved identity/keychain handles; no secrets)\n";
    std::cout << "  auv3-xcodeproj  Generate an Xcode project for an AUv3 target\n";
    std::cout << "             <target> [--sdk iphonesimulator|iphoneos|macosx]\n";
    std::cout << "             [--output <dir>] [--open] [--dry-run]\n";
    std::cout << "  swap-pack  Sign a hot-reload UX bundle into a swap pack\n";
    std::cout << "             --bundle <dir> --plugin-id <id> [--pack-version N] [--channel c]\n";
    std::cout << "             [--sign-key <file>] [--out <manifest.json>] [--yes]\n";
    std::cout << "             [--backup-github [--repo owner/name]]  (publish the signing\n";
    std::cout << "              key as a GitHub secret in the PLUGIN repo — never core Pulp)\n";
    std::cout << "             (capabilities are inferred from the bundle's JS; the key is\n";
    std::cout << "              reused from the macOS keychain or the --sign-key file)\n";
    return 0;
}

// Path to the non-interactive-signing doctor script (tools/scripts).
static fs::path signing_doctor_script(const fs::path& root) {
    return root / "tools" / "scripts" / "ensure_signing_ready.sh";
}

// Best-effort, quiet, idempotent preflight: materialize the dedicated signing
// keychain + validate the .p8 notary key so codesign/notarytool never pop a
// keychain/1Password prompt. Never fails the caller — if the doctor reports
// not-ready, the subsequent sign/notarize surfaces the real error itself.
static void run_signing_preflight(const fs::path& root) {
    auto script = signing_doctor_script(root);
    if (fs::exists(script))
        run("/bin/bash \"" + script.string() + "\" --quiet >/dev/null 2>&1 || true");
}

// Ship-time PULP_TRACING guard. Refuses to sign/package/release an artifact
// built with Perfetto tracing compiled in — a dev-only build that must never
// reach a customer (an ~80 MB ring + a `.pftrace` written inside their DAW).
// Scans the given roots for the runtime sentinel (pulp::cli::kTracingShipSentinel).
// Returns 0 when clean or when `allow_tracing` overrides; non-zero (and prints a
// loud, actionable error naming the offending file) when a traced artifact is
// found. A missing path is skipped, not an error.
static int enforce_no_tracing(const std::vector<fs::path>& roots,
                              bool allow_tracing) {
    for (const auto& r : roots) {
        std::error_code ec;
        if (r.empty() || !fs::exists(r, ec) || ec) continue;
        auto offender = pulp::cli::artifact_tracing_offender(r);
        if (offender.empty()) continue;
        if (allow_tracing) {
            std::cerr << "⚠︎  pulp ship: artifact was built with PULP_TRACING=ON ("
                      << offender.string()
                      << ") — proceeding because --allow-tracing was passed.\n";
            return 0;
        }
        std::cerr
            << "Error: refusing to ship a binary built with PULP_TRACING=ON.\n"
            << "  Offending artifact: " << offender.string() << "\n"
            << "  Perfetto tracing is a DEV-ONLY build (an ~80 MB in-memory ring\n"
            << "  and a .pftrace written inside the host). Rebuild with tracing off\n"
            << "  (the default: `pulp build` / `-DPULP_TRACING=OFF`) and re-run,\n"
            << "  or pass --allow-tracing to override deliberately.\n";
        return 1;
    }
    return 0;
}

// Load (or, on first use, create + store) the plugin's Ed25519 signing key from the
// macOS keychain. The key material is kept as a single-line base64 blob under a
// stable, discoverable service name so it is easy to find / rotate / revoke. A freshly
// generated key screams (provenance banner) and is never silently regenerated.
// Returns nullopt off macOS (caller falls back to --sign-key).
static std::optional<pulp::format::reload::KeyMaterial>
reload_signing_key_from_keychain(const std::string& plugin_id) {
#if defined(__APPLE__)
    namespace reload = pulp::format::reload;
    const std::string service = reload::reload_keychain_service(reload::KeyRole::Signing, plugin_id);
    const std::string account = "gen1";
    auto sh_quote = [](const std::string& s) {
        std::string q = "'";
        for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
        return q + "'";
    };

    // Read an existing key.
    std::string find_cmd = "security find-generic-password -s " + sh_quote(service) +
                           " -a " + sh_quote(account) + " -w 2>/dev/null";
    std::string stored;
    if (FILE* pipe = popen(find_cmd.c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) stored += buf;
        pclose(pipe);
    }
    while (!stored.empty() && (stored.back() == '\n' || stored.back() == '\r')) stored.pop_back();
    if (!stored.empty()) {
        if (auto raw = pulp::runtime::base64_decode(stored)) {
            std::string blob(raw->begin(), raw->end());
            if (auto km = reload::parse_key_blob(blob); km && km->valid()) return km;
        }
        std::cerr << "pulp ship swap-pack: keychain entry " << service
                  << " is unreadable; refusing to overwrite it. Fix or remove it.\n";
        return std::nullopt;  // never silently mint a second identity over a corrupt entry
    }

    // First use: generate, store, scream.
    auto kp = pulp::runtime::ed25519_keypair_generate();
    if (!kp) return std::nullopt;
    reload::KeyMaterial km{kp->public_key, kp->private_key};
    const std::string blob = reload::serialize_key_blob(km);
    const std::string b64 = pulp::runtime::base64_encode(
        reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
    std::string add_cmd = "security add-generic-password -U -s " + sh_quote(service) +
                          " -a " + sh_quote(account) + " -w " + sh_quote(b64);
    if (run(add_cmd) != 0) {
        std::cerr << "pulp ship swap-pack: failed to store the signing key in the keychain\n";
        return std::nullopt;
    }
    std::cout << reload::key_generation_banner(reload::KeyRole::Signing, service, plugin_id);
    return km;
#else
    (void)plugin_id;
    return std::nullopt;
#endif
}

int cmd_ship(const std::vector<std::string>& args) {
    // Parse subcommand
    std::string sub = args.empty() ? "help" : args[0];
    if (sub == "help" || sub == "--help" || sub == "-h")
        return print_ship_help();

    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    // ── doctor: ensure non-interactive signing + notarization readiness ───────
    // Runs the self-healing keychain/.p8 doctor. Independent of a build dir, so
    // it is handled before the build-dir check below. Flags (--check-online,
    // --print-env, --quiet) pass straight through.
    if (sub == "doctor") {
        auto script = signing_doctor_script(root);
        if (!fs::exists(script)) {
            std::cerr << "pulp ship doctor: missing " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "/bin/bash \"" + script.string() + "\"";
        for (size_t i = 1; i < args.size(); ++i)
            cmd += " \"" + args[i] + "\"";
        return run(cmd);
    }

    auto build_dir = root / "build";
    // swap-pack signs a UX bundle passed explicitly; it needs no configured build.
    if (sub != "swap-pack" && !fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    // ── sign ────────────────────────────────────────────────────────────────
    if (sub == "sign") {
        run_signing_preflight(root);  // self-heal the dedicated keychain (no prompt)
        std::string identity, target, keystore_path, key_alias, store_pass, key_pass;
        std::string sign_path;  // --path: sign one explicit desktop artifact (not .pkg)
        std::string entitlements = (root / "ship" / "templates" / "entitlements.plist").string();
        bool allow_tracing = false;  // override the PULP_TRACING ship guard
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--identity") {
                if (!take_ship_value(args, i, sub, args[i], identity)) return 2;
            } else if (args[i] == "--allow-tracing") {
                allow_tracing = true;
            } else if (args[i] == "--path") {
                if (!take_ship_value(args, i, sub, args[i], sign_path)) return 2;
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

        // Refuse to sign a PULP_TRACING=ON build unless explicitly allowed —
        // signing is the first step toward distributing an artifact.
        if (int rc = enforce_no_tracing({build_dir, fs::path(sign_path)},
                                        allow_tracing);
            rc != 0) {
            return rc;
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

        // --path: sign exactly one artifact instead of auto-scanning the
        // build dirs. This is the composable primitive behind the one-off
        // "I built an app/DMG and want to hand it to a friend" flow — point
        // it at a standalone `.app`, `.dmg`, `.exe`, or a single plugin bundle.
        // `.pkg` installers are signed at creation time (`productsign` via
        // `create_pkg`'s signing_identity), not here, so reject them with a
        // pointer rather than producing a broken signature.
        if (!sign_path.empty()) {
            if (!fs::exists(sign_path)) {
                std::cerr << "pulp ship sign: --path not found: " << sign_path << "\n";
                return 1;
            }
            auto ext = fs::path(sign_path).extension().string();
            if (ext == ".pkg") {
                std::cerr << "pulp ship sign: .pkg installers are signed when created"
                             " — pass a Developer ID Installer identity to\n"
                             "  `pulp ship package` / `create_pkg`, not `sign --path`.\n";
                return 1;
            }
            // Hardened runtime + secure timestamp are mandatory for
            // notarization. Entitlements only apply to executables, so we
            // only attach them for app/plugin bundles, never a `.dmg`.
            std::cout << "Signing " << fs::path(sign_path).filename().string() << "...\n";
            const bool is_disk_image = (ext == ".dmg");
            bool ok = pulp::ship::codesign(
                sign_path, identity, is_disk_image ? "" : entitlements);
            if (!ok) {
                std::cerr << "  FAILED to sign " << sign_path << "\n";
                return 1;
            }
            std::cout << "Signed " << sign_path << "\n";
            return 0;
        }

        int signed_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << "Signing " << entry.path().filename().string() << "...\n";
                    if (pulp::ship::codesign(entry.path().string(), identity,
                                             entitlements)) {
                        ++signed_count;
                    } else {
                        std::cerr << "  FAILED\n";
                    }
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
        std::string installer_identity;  // Developer ID Installer (macOS .pkg signing)
        std::string format;        // Linux: "appimage" selects the AppImage packager
        std::string binary_path;   // Linux AppImage: path to the standalone executable
        std::string icon_path;     // Linux AppImage: optional .png icon
        bool per_user = false, apk_only = false, aab_only = false;
        bool allow_tracing = false;  // override the PULP_TRACING ship guard
        // macOS packaging. DEFAULT: one component-selectable installer — a single
        // signed `.pkg` whose "Customize" pane lets the user install only the
        // formats they want (AU/VST3/CLAP/Standalone, all pre-checked). This is
        // how pro audio installers behave; users should never be forced to
        // install every format. Overrides: `--separate` emits a per-format `.pkg`
        // each (legacy), `--dmg` wraps each bundle as a disk image. `--pkg` is the
        // explicit form of the default.
        bool want_pkg = false, want_dmg = false, want_separate = false;
        // Scope discovery to one product's bundles. A build tree holds every
        // example's AU/VST3/CLAP; without this the installer bundles them all (one
        // choice per bundle, dozens of entries). `--product SuperConvolver` keeps
        // only bundles whose name matches, and names the installer after it.
        std::string product_filter;

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
            } else if (args[i] == "--installer-identity") {
                if (!take_ship_value(args, i, sub, args[i], installer_identity)) return 2;
            } else if (args[i] == "--format") {
                if (!take_ship_value(args, i, sub, args[i], format)) return 2;
            } else if (args[i] == "--binary") {
                if (!take_ship_value(args, i, sub, args[i], binary_path)) return 2;
            } else if (args[i] == "--icon") {
                if (!take_ship_value(args, i, sub, args[i], icon_path)) return 2;
            } else if (args[i] == "--product") {
                if (!take_ship_value(args, i, sub, args[i], product_filter)) return 2;
            }
            else if (args[i] == "--apk-only") apk_only = true;
            else if (args[i] == "--aab-only") aab_only = true;
            else if (args[i] == "--per-user") per_user = true;
            else if (args[i] == "--pkg") want_pkg = true;
            else if (args[i] == "--dmg") want_dmg = true;
            else if (args[i] == "--separate") want_separate = true;
            else if (args[i] == "--allow-tracing") allow_tracing = true;
            else return unknown_ship_arg(sub, args[i]);
        }

        // Refuse to package a PULP_TRACING=ON build unless explicitly allowed.
        if (int rc = enforce_no_tracing({build_dir, fs::path(binary_path)},
                                        allow_tracing);
            rc != 0) {
            return rc;
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
        // macOS / Linux. On macOS we honor the per-artifact selection:
        // `.pkg` for AU/VST3/CLAP plugin bundles, `.dmg` for `.app`
        // standalones. `--pkg` and `--dmg` flags override the per-artifact
        // default for the entire run (e.g. `--dmg` wraps even plugin bundles
        // in a disk image when that's explicitly requested, useful for the
        // "drag to Plug-Ins" distribution pattern).
#if defined(__linux__)
        // Linux: produce a real `.deb` from the plugin bundles via the
        // first-party helper in ship/platform/linux/package_linux.cpp. The
        // macOS pkgbuild/dmg path below is Apple-only (`pkgbuild`/`hdiutil`
        // don't exist on Linux), so falling through to it previously yielded
        // nothing usable. `.tar.gz` is the fallback when `dpkg-deb` is absent.
        {
            // AppImage path (`--format appimage`): wraps a standalone
            // executable, not the plugin bundles. The caller points at the
            // built binary with `--binary` (the plugin-centric build_dir has no
            // standardized standalone location to auto-discover).
            if (format == "appimage") {
                if (binary_path.empty() || !fs::exists(binary_path)) {
                    std::cerr << "Error: --format appimage needs --binary <path-to-standalone-executable>"
                              << (binary_path.empty() ? "" : " (not found: " + binary_path + ")") << "\n";
                    return 1;
                }
                std::string app_name = fs::path(binary_path).filename().string();
                auto out = artifacts / (app_name + "-" + version + "-" +
                                        pulp::ship::debian_architecture() + ".AppImage");
                std::cout << "Packaging " << app_name << " (Linux → AppImage)...\n";
                if (pulp::ship::create_appimage(app_name, version, binary_path,
                                                out.string(), icon_path)) {
                    std::cout << "  Created " << out.string() << "\n";
                    return 0;
                }
                std::cerr << "  AppImage creation failed (is appimagetool installed?)\n";
                return 1;
            }

            std::string product_name;
            bool found_linux_plugin = false;
            for (auto dir_name : {"VST3", "CLAP", "LV2"}) {
                auto dir = build_dir / dir_name;
                if (!fs::exists(dir)) continue;
                for (auto& entry : fs::directory_iterator(dir)) {
                    const auto ext = entry.path().extension().string();
                    const bool is_plugin =
                        (std::string_view(dir_name) == "VST3" && ext == ".vst3") ||
                        (std::string_view(dir_name) == "CLAP" && ext == ".clap") ||
                        (std::string_view(dir_name) == "LV2" && ext == ".lv2");
                    if (!is_plugin) continue;
                    found_linux_plugin = true;
                    product_name = entry.path().stem().string();
                    break;
                }
                if (!product_name.empty()) break;
            }
            if (!found_linux_plugin) {
                std::cerr << "Error: no VST3/CLAP/LV2 plugins found in "
                          << build_dir.string() << "\n";
                return 1;
            }

            auto deb_path = artifacts / (product_name + "-" + version + ".deb");
            std::cout << "Packaging " << product_name << " (Linux → .deb)...\n";
            if (pulp::ship::create_deb(product_name, version, build_dir.string(),
                                       deb_path.string(), "Pulp")) {
                std::cout << "  Created " << deb_path.string() << "\n";
                return 0;
            }

            std::cerr << "  .deb creation failed (is dpkg-deb installed?) — "
                         "writing .tar.gz fallback\n";
            auto tar_path = artifacts / (product_name + "-" + version + ".tar.gz");
            if (pulp::ship::create_tar_gz(product_name, build_dir.string(),
                                          tar_path.string())) {
                std::cout << "  Created " << tar_path.string() << "\n";
                return 0;
            }
            std::cerr << "  FAILED to create Linux package\n";
            return 1;
        }
#endif  // __linux__

#if defined(__APPLE__)
        int pkg_count = 0, dmg_count = 0;

        // Resolve the Developer ID Installer identity for flat-package
        // signing. Apple's installer-distribution flow requires the `.pkg`
        // itself to be signed (separate from the Developer ID *Application*
        // identity that signs the bundles), or notarization rejects it. When
        // none is configured we build an unsigned `.pkg` as before; `release`
        // then declines to notarize it rather than submitting garbage.
        installer_identity = ship_config(installer_identity, "PULP_INSTALLER_IDENTITY",
                                         "signing.apple", "installer_identity");

#ifdef __APPLE__
        // Components collected for the DEFAULT single, component-selectable
        // installer (a "Customize" pane lets the user pick formats).
        std::vector<pulp::ship::InstallComponent> combined;
        std::string product_name;

        // Standalone `.app`: a choice in the combined installer (→ /Applications),
        // or its own `.dmg` when --dmg. Discover it from the conventional
        // build/Standalone dir AND (there is no enforced convention, and examples
        // build the standalone under their own dir) from build/examples/*/. Dedup
        // by path so an app present in both isn't packaged twice.
        std::vector<fs::path> standalone_apps;
        auto add_apps_in = [&](const fs::path& dir) {
            if (!fs::exists(dir)) return;
            for (auto& entry : fs::directory_iterator(dir)) {
                if (entry.path().extension().string() != ".app") continue;
                if (!product_filter.empty() &&
                    entry.path().stem().string() != product_filter) continue;
                standalone_apps.push_back(entry.path());
            }
        };
        add_apps_in(build_dir / "Standalone");
        if (fs::exists(build_dir / "examples"))
            for (auto& ex : fs::directory_iterator(build_dir / "examples"))
                if (ex.is_directory()) add_apps_in(ex.path());
        std::sort(standalone_apps.begin(), standalone_apps.end());
        standalone_apps.erase(std::unique(standalone_apps.begin(), standalone_apps.end()),
                              standalone_apps.end());
        for (auto& app : standalone_apps) {
            auto name = app.stem().string();
            product_name = name;
            if (want_dmg) {
                auto dmg_path = artifacts / (name + "-" + version + ".dmg");
                std::cout << "Packaging " << name << " (Standalone .app → .dmg)...\n";
                if (pulp::ship::create_dmg(app.string(), dmg_path.string(), name))
                    ++dmg_count;
                else std::cerr << "  FAILED to create .dmg for " << name << "\n";
            } else {
                combined.push_back({app.string(), "/Applications", "Standalone App"});
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
                if (!product_filter.empty() && name != product_filter) continue;

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

                product_name = name;
                std::string install_loc = "/Library/Audio/Plug-Ins/";
                std::string title = dir_name;
                if (ext == ".vst3") { install_loc += "VST3"; title = "VST3"; }
                else if (ext == ".clap") { install_loc += "CLAP"; title = "CLAP"; }
                else { install_loc += "Components"; title = "Audio Unit (AU)"; }

                if (!want_separate) {
                    // DEFAULT: collect as one toggleable choice in the combined,
                    // component-selectable installer built after this loop.
                    combined.push_back({entry.path().string(), install_loc, title});
                    continue;
                }
                // --separate (legacy): one signed `.pkg` per format.
                auto pkg_path = artifacts / (name + "-" + dir_name + "-" + version + ".pkg");
                std::cout << "Packaging " << name << " (" << dir_name << " → .pkg"
                          << (installer_identity.empty() ? "" : ", signed") << ")...\n";
                std::string cmd = "pkgbuild --component \"" + entry.path().string() + "\""
                    + " --identifier \"com.pulp." + name + "." + format_lower + "\""
                    + " --version \"" + version + "\""
                    + " --install-location \"" + install_loc + "\"";
                if (!installer_identity.empty())
                    cmd += " --sign \"" + installer_identity + "\"";
                cmd += " \"" + pkg_path.string() + "\" 2>/dev/null";
                if (run(cmd) == 0) ++pkg_count;
                else std::cerr << "  FAILED\n";
            }
        }

        // DEFAULT: one component-selectable installer (Customize pane) with every
        // built format as a pre-checked, user-toggleable choice.
        if (!combined.empty() && !product_name.empty()) {
            auto pkg_path = artifacts / (product_name + "-" + version + ".pkg");
            std::cout << "Packaging " << product_name << " (component-selectable installer, "
                      << combined.size() << " formats"
                      << (installer_identity.empty() ? "" : ", signed") << ")...\n";
            if (pulp::ship::create_combined_pkg(combined, pkg_path.string(),
                    "com.pulp." + product_name, version, installer_identity))
                ++pkg_count;
            else std::cerr << "  FAILED to create combined installer\n";
        }
        std::cout << "Created " << pkg_count << " .pkg and " << dmg_count
                  << " .dmg artifacts in " << artifacts.string() << "\n";
        return 0;
#else
        std::cerr << "Error: `pulp ship package` has no packager for this "
                     "platform (supported: macOS, Windows, Linux)\n";
        return 1;
#endif  // __APPLE__
#endif  // _WIN32 else
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
                    auto info = pulp::ship::check_codesign(entry.path().string());
                    std::cout << (info.is_valid ? "signed" : "unsigned") << "\n";
                }
            }
        }
        return 0;
    }

    // ── notarize ────────────────────────────────────────────────────────────
    //
    // Two credential flows are supported and resolved in this order:
    //
    //   1. App Store Connect API key (preferred): a `.p8` private key
    //      file plus Key ID + Issuer UUID. `xcrun notarytool` reads
    //      these via `--key/--key-id/--issuer`; rcodesign on Linux
    //      accepts the same trio via `--api-key-path`. Apple recommends
    //      this flow because the key can be rotated and scoped.
    //
    //   2. Legacy Apple-ID + Team-ID + app-specific password (typically
    //      `@keychain:AC_PASSWORD`). Kept working as a fallback so we
    //      don't break existing users mid-flight.
    //
    // Resolution precedence per credential surface (highest wins):
    //   - CLI flags (`--api-key`, `--api-key-id`, `--api-issuer`,
    //     `--apple-id`, `--team-id`, `--password`)
    //   - Environment variables (PULP_NOTARY_KEY_PATH / _ID / _ISSUER_ID,
    //     PULP_APPLE_ID, PULP_TEAM_ID)
    //   - `~/.config/pulp/secrets/notary.env` (overridable via
    //     PULP_NOTARY_ENV for tests)
    //   - `~/.pulp/config.toml` (`[signing.apple]`, legacy fields only)
    //
    // `--dry-run` short-circuits before invoking notarytool and prints
    // the resolved command line so the user (or our shell-out test) can
    // verify the flags without touching Apple's servers.
    if (sub == "notarize") {
#ifndef __APPLE__
        std::cerr << "Notarization is only available on macOS.\n";
        return 1;
#else
        std::string apple_id, team_id, password;
        std::string api_key, api_key_id, api_issuer;
        std::string env_file_override;
        std::vector<std::string> explicit_paths;  // --path (repeatable)
        bool staple_only = false;
        bool dry_run = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--apple-id") {
                if (!take_ship_value(args, i, sub, args[i], apple_id)) return 2;
            } else if (args[i] == "--path") {
                std::string p;
                if (!take_ship_value(args, i, sub, args[i], p)) return 2;
                explicit_paths.push_back(p);
            } else if (args[i] == "--team-id") {
                if (!take_ship_value(args, i, sub, args[i], team_id)) return 2;
            } else if (args[i] == "--password") {
                if (!take_ship_value(args, i, sub, args[i], password)) return 2;
            } else if (args[i] == "--api-key") {
                if (!take_ship_value(args, i, sub, args[i], api_key)) return 2;
            } else if (args[i] == "--api-key-id") {
                if (!take_ship_value(args, i, sub, args[i], api_key_id)) return 2;
            } else if (args[i] == "--api-issuer") {
                if (!take_ship_value(args, i, sub, args[i], api_issuer)) return 2;
            } else if (args[i] == "--env-file") {
                if (!take_ship_value(args, i, sub, args[i], env_file_override)) return 2;
            }
            else if (args[i] == "--staple") staple_only = true;
            else if (args[i] == "--dry-run") dry_run = true;
            else return unknown_ship_arg(sub, args[i]);
        }

        // When --path is given, notarize exactly those artifacts (a
        // `.dmg`, `.pkg`, `.app`, or zipped bundle) instead of auto-
        // scanning the plugin build dirs. This is what lets `release`
        // notarize the distributable it just packaged, and what the
        // `share` one-shot points at.
        std::vector<std::string> bundles;
        if (!explicit_paths.empty()) {
            // notarytool only accepts upload containers — UDIF disk images
            // (.dmg), signed flat packages (.pkg), and .zip archives. A raw
            // .app bundle cannot be submitted directly, so reject it with a
            // pointer to `share` (which wraps it in a DMG) rather than letting
            // notarytool fail mid-submission.
            for (const auto& p : explicit_paths) {
                if (fs::path(p).extension() == ".app") {
                    std::cerr << "pulp ship notarize: notarytool cannot submit a raw"
                                 " .app bundle:\n  " << p << "\n"
                                 "  Use `pulp ship share " << p << "` (wraps it in a"
                                 " DMG, signs, notarizes, staples),\n"
                                 "  or zip it first:"
                                 " ditto -c -k --keepParent \"" << p << "\" out.zip\n";
                    return 2;
                }
            }
            bundles = explicit_paths;
        } else {
            for (auto dir_name : {"VST3", "CLAP", "AU"}) {
                auto dir = build_dir / dir_name;
                if (!fs::exists(dir)) continue;
                for (auto& entry : fs::directory_iterator(dir)) {
                    auto ext = entry.path().extension().string();
                    if (ext == ".vst3" || ext == ".clap" || ext == ".component")
                        bundles.push_back(entry.path().string());
                }
            }
        }

        if (bundles.empty() && !dry_run) {
            std::cerr << (explicit_paths.empty()
                              ? "No plugin bundles found.\n"
                              : "No --path artifacts to notarize.\n");
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

        // ── ASC API key resolution (preferred) ──────────────────────
        // Look at the env file first so we can layer env+CLI over it.
        auto env_path = env_file_override.empty()
            ? pulp::cli::notary::default_env_path()
            : std::filesystem::path(env_file_override);
        std::string home_str;
        if (auto h = pulp::runtime::get_env("HOME")) home_str = *h;
        pulp::cli::notary::NotaryEnvFile env_file;
        bool env_file_loaded = false;
        if (!env_path.empty() && fs::exists(env_path)) {
            env_file = pulp::cli::notary::parse_env_file(env_path, home_str);
            env_file_loaded = true;
        }

        auto getenv_fn = [](const std::string& name)
            -> std::optional<std::string> {
            return pulp::runtime::get_env(name);
        };
        auto asc = pulp::cli::notary::resolve_creds(
            api_key, api_key_id, api_issuer, env_file, getenv_fn);

        // Layer legacy creds over the same surface so the failure
        // message can tell the user which lane they fell back to.
        if (apple_id.empty()) {
            if (auto e = pulp::runtime::get_env("PULP_APPLE_ID"); e && !e->empty()) apple_id = *e;
        }
        if (team_id.empty()) {
            if (auto e = pulp::runtime::get_env("PULP_TEAM_ID"); e && !e->empty()) team_id = *e;
            else if (!env_file.team_id.empty()) team_id = env_file.team_id;
        }
        if (apple_id.empty())
            apple_id = read_user_config_value("signing.apple", "apple_id");
        if (team_id.empty())
            team_id = read_user_config_value("signing.apple", "team_id");
        if (password.empty())
            password = read_user_config_value("signing.apple", "password");

        const bool use_asc = asc.complete();
        const bool use_legacy = !use_asc && !apple_id.empty() && !team_id.empty();

        if (env_file_loaded) {
            std::cout << "Loaded notary credentials from "
                      << pulp::cli::notary::redact_path(env_path.string()) << "\n";
        }
        if (use_asc) {
            std::cout << "Notary flow: App Store Connect API key (notarytool --key ...).\n"
                      << "  key      : " << pulp::cli::notary::redact_path(asc.key_path)
                      << " (from " << asc.key_path_source << ")\n"
                      << "  key-id   : " << asc.key_id
                      << " (from " << asc.key_id_source << ")\n"
                      << "  issuer   : " << asc.issuer_id
                      << " (from " << asc.issuer_id_source << ")\n";
        } else if (use_legacy) {
            if (password.empty()) password = "@keychain:AC_PASSWORD";
            std::cout << "Notary flow: legacy Apple ID + Team ID (notarytool --apple-id ...).\n"
                      << "  apple-id : " << apple_id << "\n"
                      << "  team-id  : " << team_id << "\n";
        } else {
            std::cerr << "Error: no notary credentials resolved.\n\n";
            std::cerr << "  Preferred (App Store Connect API key):\n";
            std::cerr << "    pulp ship notarize --api-key /path/AuthKey_X.p8 \\\n";
            std::cerr << "                       --api-key-id X --api-issuer <uuid>\n";
            std::cerr << "    Or store in ~/.config/pulp/secrets/notary.env:\n";
            std::cerr << "      PULP_NOTARY_KEY_PATH=\"$HOME/.config/pulp/secrets/AuthKey_X.p8\"\n";
            std::cerr << "      PULP_NOTARY_KEY_ID=\"X\"\n";
            std::cerr << "      PULP_NOTARY_ISSUER_ID=\"<uuid>\"\n\n";
            std::cerr << "  Legacy (Apple ID + app-specific password):\n";
            std::cerr << "    pulp ship notarize --apple-id you@example.com --team-id ABCDE12345\n";
            std::cerr << "    Stash password in Keychain:\n";
            std::cerr << "      security add-generic-password -s AC_PASSWORD -a you@example.com -w\n";
            return 1;
        }

        if (dry_run) {
            // Build the same notarytool argv the real path would
            // shell out, so callers (and our shell-out test) can
            // verify the resolution without involving Apple. We use
            // a placeholder bundle when no bundles are present.
            std::string sample_bundle = bundles.empty()
                ? std::string("<bundle>") : bundles.front();
            std::string cmd = "xcrun notarytool submit \"" + sample_bundle + "\"";
            if (use_asc) {
                cmd += " --key \"" + asc.key_path + "\""
                    +  " --key-id \"" + asc.key_id + "\""
                    +  " --issuer \"" + asc.issuer_id + "\"";
            } else {
                cmd += " --apple-id \"" + apple_id + "\""
                    +  " --team-id \"" + team_id + "\""
                    +  " --password \"" + password + "\"";
            }
            cmd += " --wait";
            std::cout << "(--dry-run) would invoke:\n  " << cmd << "\n";
            return 0;
        }

        int success_count = 0;
        for (auto& path : bundles) {
            auto name = fs::path(path).filename().string();
            std::cout << "Submitting " << name << " for notarization...\n";
            auto uuid = use_asc
                ? pulp::ship::notarize_submit_asc(path, asc.key_path,
                                                  asc.key_id, asc.issuer_id)
                : pulp::ship::notarize_submit(path, apple_id, team_id, password);
            if (!uuid) { std::cerr << "  Submission FAILED\n"; continue; }
            std::cout << "  Request UUID: " << *uuid << "\n";
            auto status = use_asc
                ? pulp::ship::notarize_check_asc(*uuid, asc.key_path,
                                                 asc.key_id, asc.issuer_id)
                : pulp::ship::notarize_check(*uuid, apple_id, team_id, password);
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
        std::string target = "macos";
        std::string identity, apple_id, team_id, password, version;
        std::string api_key, api_key_id, api_issuer, installer_identity;
        bool want_pkg = false, want_dmg = false;
        bool skip_sign = false, skip_package = false, skip_notarize = false;

        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--target") {
                if (!take_ship_value(args, i, sub, args[i], target)) return 2;
            } else if (args[i] == "--identity") {
                if (!take_ship_value(args, i, sub, args[i], identity)) return 2;
            } else if (args[i] == "--installer-identity") {
                if (!take_ship_value(args, i, sub, args[i], installer_identity)) return 2;
            } else if (args[i] == "--apple-id") {
                if (!take_ship_value(args, i, sub, args[i], apple_id)) return 2;
            } else if (args[i] == "--team-id") {
                if (!take_ship_value(args, i, sub, args[i], team_id)) return 2;
            } else if (args[i] == "--password") {
                if (!take_ship_value(args, i, sub, args[i], password)) return 2;
            } else if (args[i] == "--api-key") {
                if (!take_ship_value(args, i, sub, args[i], api_key)) return 2;
            } else if (args[i] == "--api-key-id") {
                if (!take_ship_value(args, i, sub, args[i], api_key_id)) return 2;
            } else if (args[i] == "--api-issuer") {
                if (!take_ship_value(args, i, sub, args[i], api_issuer)) return 2;
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

#ifndef __APPLE__
        std::cerr << "pulp ship release: macOS-only (notarization + .pkg/.dmg).\n";
        return 1;
#else
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
            // The bundle pass above only walks VST3/CLAP/AU. Standalone
            // `.app` bundles (which the package stage wraps into a DMG) must
            // be signed too, or notarizing the resulting disk image fails on
            // an unsigned app. Sign each one explicitly via `sign --path`.
            auto standalone = build_dir / "Standalone";
            if (fs::exists(standalone)) {
                for (auto& e : fs::directory_iterator(standalone)) {
                    if (e.path().extension() != ".app") continue;
                    std::vector<std::string> app_args =
                        {"sign", "--path", e.path().string()};
                    if (!identity.empty()) {
                        app_args.push_back("--identity");
                        app_args.push_back(identity);
                    }
                    if (cmd_ship(app_args) != 0) {
                        std::cerr << "pulp ship release: failed to sign standalone app "
                                  << e.path().filename().string() << "\n";
                        return 1;
                    }
                }
            }
        } else {
            std::cout << "── Stage 1/4: sign (SKIPPED) ──────────────────────\n";
        }

        // Stage 2: package. The pkg vs dmg decision lives in cmd_ship
        // 'package' itself.
        //
        // We notarize the *distributable* (the `.dmg`/`.pkg` this stage
        // produces), not the loose plugin bundles — otherwise `release
        // --dmg` would emit a signed-but-unnotarized disk image and the
        // command name would be a lie (Gatekeeper rejects it with
        // "Unnotarized Developer ID"). Stamp the time before packaging so
        // we can tell freshly-built artifacts from stale ones left in
        // artifacts/ by an earlier run.
        std::vector<std::string> release_artifacts;
        auto pkg_t0 = fs::file_time_type::clock::now();
        if (!skip_package) {
            std::cout << "── Stage 2/4: package ─────────────────────────────\n";
            std::vector<std::string> pkg_args = {"package"};
            if (!version.empty()) {
                pkg_args.push_back("--version");
                pkg_args.push_back(version);
            }
            if (want_pkg) pkg_args.push_back("--pkg");
            if (want_dmg) pkg_args.push_back("--dmg");
            // Plumb the Developer ID Installer identity so `package` signs the
            // flat package it builds — an unsigned .pkg cannot be notarized.
            if (!installer_identity.empty()) {
                pkg_args.push_back("--installer-identity");
                pkg_args.push_back(installer_identity);
            }
            int pkg_rc = cmd_ship(pkg_args);
            if (pkg_rc != 0) {
                std::cerr << "pulp ship release: package stage failed; "
                             "aborting before notarize.\n";
                return pkg_rc;
            }
            auto artifacts = root / "artifacts";
            if (fs::exists(artifacts)) {
                for (auto& entry : fs::directory_iterator(artifacts)) {
                    auto ext = entry.path().extension().string();
                    if (ext != ".dmg" && ext != ".pkg") continue;
                    std::error_code ec;
                    auto mtime = fs::last_write_time(entry.path(), ec);
                    if (!ec && mtime >= pkg_t0)
                        release_artifacts.push_back(entry.path().string());
                }
            }
            // `package` builds the DMG but does not sign the disk image
            // itself. Sign each freshly-built .dmg (unless signing was
            // skipped) so it passes the signature guard below and Gatekeeper
            // accepts the downloaded image.
            if (!skip_sign) {
                for (auto& a : release_artifacts) {
                    if (fs::path(a).extension() != ".dmg") continue;
                    std::vector<std::string> dmg_args = {"sign", "--path", a};
                    if (!identity.empty()) {
                        dmg_args.push_back("--identity");
                        dmg_args.push_back(identity);
                    }
                    if (cmd_ship(dmg_args) != 0)
                        std::cerr << "pulp ship release: warning — failed to sign "
                                  << fs::path(a).filename().string() << "\n";
                }
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
        if (!api_key.empty())     { nz_args.push_back("--api-key");     nz_args.push_back(api_key); }
        if (!api_key_id.empty())  { nz_args.push_back("--api-key-id");  nz_args.push_back(api_key_id); }
        if (!api_issuer.empty())  { nz_args.push_back("--api-issuer");  nz_args.push_back(api_issuer); }
        if (!apple_id.empty()) { nz_args.push_back("--apple-id"); nz_args.push_back(apple_id); }
        if (!team_id.empty())  { nz_args.push_back("--team-id");  nz_args.push_back(team_id); }
        if (!password.empty()) { nz_args.push_back("--password"); nz_args.push_back(password); }
        // Only submit artifacts that are actually signed. notarytool rejects
        // an unsigned .pkg/.dmg, and an unsigned installer would never pass
        // Gatekeeper, so verify each and skip (loudly) the unsigned ones
        // instead of submitting garbage. `.pkg` needs a Developer ID
        // Installer signature; `.dmg` a Developer ID Application one.
        std::vector<std::string> signed_artifacts;
        for (auto& a : release_artifacts) {
            auto ext = fs::path(a).extension().string();
            std::string verify = (ext == ".pkg")
                ? "pkgutil --check-signature \"" + a + "\" >/dev/null 2>&1"
                : "codesign --verify \"" + a + "\" >/dev/null 2>&1";
            if (run(verify) == 0) {
                signed_artifacts.push_back(a);
            } else {
                std::cerr << "  skipping unsigned artifact "
                          << fs::path(a).filename().string()
                          << (ext == ".pkg"
                                  ? " — pass --installer-identity \"Developer ID Installer: ...\"\n"
                                  : " — needs a Developer ID Application signature\n");
            }
        }
        // When packaging produced artifacts but none are signed, refuse to
        // fall through to the bundle scan (that would notarize loose bundles
        // the user didn't ask to distribute). Only the genuine
        // `--skip-package` plugin flow keeps the bundle-scan fallback.
        if (!release_artifacts.empty() && signed_artifacts.empty()) {
            std::cerr << "pulp ship release: no signed distributable to notarize"
                         " — configure the signing identities and re-run.\n";
            return 1;
        }
        for (auto& a : signed_artifacts) {
            nz_args.push_back("--path");
            nz_args.push_back(a);
            std::cout << "  artifact: " << fs::path(a).filename().string() << "\n";
        }
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

    // ── auv3-xcodeproj (one-click Xcode flow) ───────────────────────────────
    //
    // Thin wrapper around `cmake -G Xcode` that generates a separate Xcode
    // build tree ready to build the requested `<target>_AUv3` target for an
    // iOS Simulator, connected iOS device, or macOS. Picks the right SDK +
    // the right entitlements template from
    // `tools/templates/auv3/iOS-{Simulator,Device}-Entitlements.plist.template`.
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
    // generation requires Xcode and the matching iOS SDK to be installed.
    // `--dry-run` still exits 0 before those checks so CI / sandboxed
    // environments can exercise the wrapper without a real Xcode install.
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
                "build for the iOS Simulator, a connected device, or macOS.\n";
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
            " -G Xcode";
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

        // Configure is target-agnostic: CMake generates the project, and the
        // build hint selects the conventional AUv3 target that CMake creates.
        const std::string build_hint =
            "cmake --build " + shell_quote(output_dir) +
            " --target " + shell_quote(target_name + "_AUv3");

        if (dry_run) {
            std::cout << "  cmake  = " << configure_cmd << "\n";
            std::cout << "  build  = " << build_hint << "\n";
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
        // check blocked all of them. We still reject the command-line-tools-only
        // selection (`/Library/Developer/CommandLineTools`) by requiring both
        // `.app/` AND `/Contents/Developer` in the path.
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
        std::cout << "  build from CLI: " << build_hint << "\n";

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
        std::string version, notes, url, download_url, output_path, title, sign_key, min_os;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version") {
                if (!take_ship_value(args, i, sub, args[i], version)) return 2;
            } else if (args[i] == "--notes") {
                if (!take_ship_value(args, i, sub, args[i], notes)) return 2;
            } else if (args[i] == "--url") {
                if (!take_ship_value(args, i, sub, args[i], url)) return 2;
            } else if (args[i] == "--download-url") {
                if (!take_ship_value(args, i, sub, args[i], download_url)) return 2;
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
            std::cerr << "Usage: pulp ship appcast --url <artifact-or-url> "
                         "[--download-url https://example.com/Plugin-1.0.pkg] "
                         "--version 1.0.0\n";
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
        item.download_url = download_url.empty() ? url : download_url;
        if (!min_os.empty()) item.minimum_os = min_os;

        auto url_as_path = fs::path(url);
        if (fs::exists(url_as_path)) {
            item.file_size = fs::file_size(url_as_path);
            if (!sign_key.empty()) {
                // Hard-fail on missing/failed signing. Writing
                // an empty edSignature into the appcast looked like a
                // successful sign but produced unsigned XML that Sparkle
                // rejects silently — worse than no signing at all.
                // Ed25519 is wired through pulp::runtime::ed25519_sign.
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
            std::cerr << "  the local path as --url and set --download-url to the public enclosure URL.\n";
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

    // ── share ─────────────────────────────────────────────────────────────
    //
    // Opinionated one-shot for the "I built something cool and want to hand
    // it to a friend / put it on a TestFlight-style page" case — point it at
    // a standalone `.app`, a `.dmg`, or a `.pkg` and it produces a signed,
    // notarized, stapled, Gatekeeper-accepted distributable without the full
    // repo release pipeline. Built on the `sign --path` / `notarize --path`
    // primitives:
    //
    //   .app  → codesign (hardened runtime) → wrap in DMG → codesign DMG →
    //           notarize+staple DMG → spctl verify
    //   .dmg  → codesign DMG → notarize+staple → spctl verify
    //   .pkg  → notarize+staple (already productsigned) → spctl verify
    //
    // The final `spctl` is the exact check a downloader's Gatekeeper runs, so
    // a green result here means the friend will not see "Unnotarized
    // Developer ID". `--dry-run` prints the plan without touching anything.
    if (sub == "share") {
#ifndef __APPLE__
        std::cerr << "pulp ship share: macOS-only (codesign + notarization + DMG).\n";
        return 1;
#else
        std::string input, identity, version, output_dir, entitlements;
        std::string api_key, api_key_id, api_issuer, apple_id, team_id, password;
        bool dry_run = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--identity") {
                if (!take_ship_value(args, i, sub, args[i], identity)) return 2;
            } else if (args[i] == "--version") {
                if (!take_ship_value(args, i, sub, args[i], version)) return 2;
            } else if (args[i] == "--output") {
                if (!take_ship_value(args, i, sub, args[i], output_dir)) return 2;
            } else if (args[i] == "--entitlements") {
                if (!take_ship_value(args, i, sub, args[i], entitlements)) return 2;
            } else if (args[i] == "--api-key") {
                if (!take_ship_value(args, i, sub, args[i], api_key)) return 2;
            } else if (args[i] == "--api-key-id") {
                if (!take_ship_value(args, i, sub, args[i], api_key_id)) return 2;
            } else if (args[i] == "--api-issuer") {
                if (!take_ship_value(args, i, sub, args[i], api_issuer)) return 2;
            } else if (args[i] == "--apple-id") {
                if (!take_ship_value(args, i, sub, args[i], apple_id)) return 2;
            } else if (args[i] == "--team-id") {
                if (!take_ship_value(args, i, sub, args[i], team_id)) return 2;
            } else if (args[i] == "--password") {
                if (!take_ship_value(args, i, sub, args[i], password)) return 2;
            } else if (args[i] == "--dry-run") {
                dry_run = true;
            } else if (!args[i].empty() && args[i][0] != '-' && input.empty()) {
                input = args[i];
            } else {
                return unknown_ship_arg(sub, args[i]);
            }
        }

        if (input.empty()) {
            std::cerr <<
                "Usage: pulp ship share <app-or-dmg-or-pkg> [--identity \"...\"]\n"
                "                       [--version X.Y.Z] [--output <dir>]\n"
                "                       [--entitlements <plist>] [notarization creds]\n"
                "                       [--dry-run]\n"
                "Signs, notarizes, staples, and Gatekeeper-verifies a single\n"
                "artifact for sharing. .app inputs are wrapped in a DMG first.\n";
            return 2;
        }
        if (!fs::exists(input)) {
            std::cerr << "pulp ship share: not found: " << input << "\n";
            return 1;
        }
        auto ext = fs::path(input).extension().string();
        if (ext != ".app" && ext != ".dmg" && ext != ".pkg") {
            std::cerr << "pulp ship share: unsupported input '" << ext
                      << "' — expected .app, .dmg, or .pkg.\n";
            return 2;
        }

        if (version.empty()) version = read_project_cmake_version(root);
        if (version.empty()) version = PULP_SDK_VERSION;
        identity = ship_config(identity, "PULP_SIGN_IDENTITY",
                               "signing.apple", "identity");
        if (entitlements.empty()) {
            auto def = root / "ship" / "templates" / "entitlements.plist";
            if (fs::exists(def)) entitlements = def.string();
        }

        auto artifacts = output_dir.empty() ? (root / "artifacts")
                                            : fs::path(output_dir);
        auto stem = fs::path(input).stem().string();
        auto dmg_target = (artifacts / (stem + "-" + version + ".dmg")).string();

        if (dry_run) {
            std::cout << "pulp ship share plan for " << input << ":\n";
            if (ext == ".app") {
                std::cout << "  1. codesign (hardened runtime) " << input << "\n"
                          << "  2. create DMG → " << dmg_target << "\n"
                          << "  3. codesign DMG\n"
                          << "  4. notarize --path <DMG> + staple\n"
                          << "  5. spctl -a -t open --context context:primary-signature -vv <DMG>\n";
            } else if (ext == ".dmg") {
                std::cout << "  1. codesign (hardened runtime) " << input << "\n"
                          << "  2. notarize --path " << input << " + staple\n"
                          << "  3. spctl -a -t open --context context:primary-signature -vv " << input << "\n";
            } else {
                std::cout << "  1. notarize --path " << input
                          << " + staple  (.pkg already productsigned)\n"
                          << "  2. spctl --assess --type install -vv " << input << "\n";
            }
            std::cout << "  identity: "
                      << (identity.empty() ? "(unset — required for .app/.dmg)"
                                           : identity)
                      << "\n  version : " << version << "\n";
            std::cout << "(--dry-run: nothing signed, packaged, or notarized)\n";
            return 0;
        }

        if (ext != ".pkg" && identity.empty()) {
            std::cerr << "pulp ship share: a Developer ID Application identity is "
                         "required to sign a " << ext << ".\n";
            std::cerr << "  pulp ship share " << input
                      << " --identity \"Developer ID Application: Name (TEAMID)\"\n";
            std::cerr << "  Or: pulp config set signing.apple.identity \"...\"\n";
            return 1;
        }

        fs::create_directories(artifacts);
        std::string artifact;  // the distributable we notarize + verify
        if (ext == ".app") {
            std::cout << "Signing app " << fs::path(input).filename().string()
                      << "...\n";
            if (!pulp::ship::codesign(input, identity, entitlements)) {
                std::cerr << "  FAILED to codesign " << input << "\n";
                return 1;
            }
            std::cout << "Wrapping into DMG → "
                      << fs::path(dmg_target).filename().string() << "...\n";
            if (!pulp::ship::create_dmg(input, dmg_target, stem)) {
                std::cerr << "  FAILED to create DMG\n";
                return 1;
            }
            std::cout << "Signing DMG...\n";
            if (!pulp::ship::codesign(dmg_target, identity, "")) {
                std::cerr << "  FAILED to codesign DMG\n";
                return 1;
            }
            artifact = dmg_target;
        } else if (ext == ".dmg") {
            std::cout << "Signing DMG " << fs::path(input).filename().string()
                      << "...\n";
            if (!pulp::ship::codesign(input, identity, "")) {
                std::cerr << "  FAILED to codesign " << input << "\n";
                return 1;
            }
            artifact = input;
        } else {  // .pkg — installer cert signing happens at build time
            std::cout << "Using pre-signed .pkg as-is (installer cert).\n";
            artifact = input;
        }

        std::vector<std::string> nz_args = {"notarize", "--path", artifact};
        if (!api_key.empty())    { nz_args.push_back("--api-key");    nz_args.push_back(api_key); }
        if (!api_key_id.empty()) { nz_args.push_back("--api-key-id"); nz_args.push_back(api_key_id); }
        if (!api_issuer.empty()) { nz_args.push_back("--api-issuer"); nz_args.push_back(api_issuer); }
        if (!apple_id.empty())   { nz_args.push_back("--apple-id");   nz_args.push_back(apple_id); }
        if (!team_id.empty())    { nz_args.push_back("--team-id");    nz_args.push_back(team_id); }
        if (!password.empty())   { nz_args.push_back("--password");   nz_args.push_back(password); }
        int nz_rc = cmd_ship(nz_args);
        if (nz_rc != 0) {
            std::cerr << "pulp ship share: notarization failed.\n";
            return nz_rc;
        }

        std::cout << "Verifying Gatekeeper acceptance...\n";
        std::string verify_cmd = (ext == ".pkg")
            ? "spctl --assess --type install -vv " + shell_quote(artifact)
            : "spctl -a -t open --context context:primary-signature -vv "
              + shell_quote(artifact);
        if (run(verify_cmd) == 0) {
            std::cout << "\n✓ " << fs::path(artifact).filename().string()
                      << " is signed, notarized, stapled, and Gatekeeper-accepted.\n"
                      << "  Ready to share: " << artifact << "\n";
            return 0;
        }
        std::cerr << "\n✗ Gatekeeper did not accept " << artifact
                  << " — inspect the spctl output above.\n";
        return 1;
#endif
    }

    // ── swap-pack: sign a hot-reload UX bundle ────────────────────────────────
    if (sub == "swap-pack") {
        namespace reload = pulp::format::reload;
        std::string bundle, plugin_id, channel, out, sign_key, repo;
        std::uint64_t pack_version = 1;
        bool yes = false;
        bool backup_github = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--bundle") {
                if (!take_ship_value(args, i, sub, args[i], bundle)) return 2;
            } else if (args[i] == "--plugin-id") {
                if (!take_ship_value(args, i, sub, args[i], plugin_id)) return 2;
            } else if (args[i] == "--channel") {
                if (!take_ship_value(args, i, sub, args[i], channel)) return 2;
            } else if (args[i] == "--out") {
                if (!take_ship_value(args, i, sub, args[i], out)) return 2;
            } else if (args[i] == "--sign-key") {
                if (!take_ship_value(args, i, sub, args[i], sign_key)) return 2;
            } else if (args[i] == "--repo") {
                if (!take_ship_value(args, i, sub, args[i], repo)) return 2;
            } else if (args[i] == "--backup-github") {
                backup_github = true;
            } else if (args[i] == "--pack-version") {
                std::string v;
                if (!take_ship_value(args, i, sub, args[i], v)) return 2;
                pack_version = std::strtoull(v.c_str(), nullptr, 10);
            } else if (args[i] == "--yes") {
                yes = true;
            } else {
                return unknown_ship_arg(sub, args[i]);
            }
        }
        if (bundle.empty() || plugin_id.empty()) {
            std::cerr << "pulp ship swap-pack: --bundle <dir> and --plugin-id <id> are required\n";
            return 2;
        }

        auto built = reload::build_signable_manifest(bundle, plugin_id, pack_version, channel);
        if (!built.ok) {
            std::cerr << "pulp ship swap-pack: " << built.error << "\n";
            return 1;
        }
        std::cout << reload::swap_pack_signing_summary(built.manifest);

        if (!yes) {
            std::cout << "Sign this pack? [y/N] " << std::flush;
            std::string line;
            std::getline(std::cin, line);
            if (line != "y" && line != "Y" && line != "yes") {
                std::cerr << "aborted (no confirmation)\n";
                return 1;
            }
        }

        reload::KeyMaterial key;
        if (!sign_key.empty()) {
            bool generated = false;
            auto k = reload::load_or_generate_key_file(sign_key, generated);
            if (!k || !k->valid()) {
                std::cerr << "pulp ship swap-pack: could not load/generate signing key at "
                          << sign_key << "\n";
                return 1;
            }
            if (generated)
                std::cout << reload::key_generation_banner(reload::KeyRole::Signing, sign_key,
                                                           plugin_id);
            key = *k;
        } else if (auto k = reload_signing_key_from_keychain(plugin_id)) {
            key = *k;
        } else {
            std::cerr << "pulp ship swap-pack: no signing key. On macOS a key is created + "
                         "stored in your keychain automatically; elsewhere pass --sign-key <file>.\n";
            return 2;
        }

        built.manifest.signer_public_key = key.public_key;
        const auto msg = reload::swap_pack_signed_message(built.manifest);
        auto sig = pulp::runtime::ed25519_sign(key.private_key.data(), key.private_key.size(),
                                               msg.data(), msg.size());
        if (!sig) {
            std::cerr << "pulp ship swap-pack: signing failed\n";
            return 1;
        }
        built.manifest.signature = *sig;

        fs::path out_path = out.empty() ? (fs::path(bundle) / "swap-pack.manifest.json")
                                        : fs::path(out);
        std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cerr << "pulp ship swap-pack: cannot write " << out_path.string() << "\n";
            return 1;
        }
        ofs << reload::serialize_swap_pack_manifest(built.manifest);
        std::cout << "Signed swap pack written → " << out_path.string() << "\n";

        if (backup_github) {
            std::string target = repo;
            if (target.empty()) {
                // Default to the bundle's own git origin.
                std::string cmd = "git -C '" + bundle + "' remote get-url origin 2>/dev/null";
                if (FILE* p = popen(cmd.c_str(), "r")) {
                    char buf[512];
                    while (fgets(buf, sizeof(buf), p)) target += buf;
                    pclose(p);
                }
                while (!target.empty() && (target.back() == '\n' || target.back() == '\r'))
                    target.pop_back();
            }
            const std::string norm = reload::normalize_github_repo(target);
            if (!reload::github_backup_repo_allowed(target)) {
                std::cerr << "pulp ship swap-pack: refusing to back up the signing key to '"
                          << target << "'. A key may only be published to a plugin's OWN "
                          << "repository (never the core Pulp repo). Pass --repo owner/name.\n";
                return 2;
            }
            if (!yes) {
                std::cout << "Publish this plugin's signing key as a GitHub secret in " << norm
                          << "? Anyone with write access there can sign as you. [y/N] " << std::flush;
                std::string line;
                std::getline(std::cin, line);
                if (line != "y" && line != "Y" && line != "yes") {
                    std::cerr << "backup aborted (no confirmation)\n";
                    return 1;
                }
            }
            const std::string secret =
                reload::reload_github_secret_name(reload::KeyRole::Signing, plugin_id, 1);
            const std::string blob = reload::serialize_key_blob(key);
            // Pipe the key via a 0600 temp file — never on the argv (it is a secret).
            fs::path tmp = fs::temp_directory_path() / (secret + ".secret");
            {
                std::ofstream t(tmp, std::ios::binary | std::ios::trunc);
                t << blob;
            }
            std::error_code pec;
            fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                            fs::perm_options::replace, pec);
            std::string gh = "gh secret set '" + secret + "' --repo '" + norm + "' < '" +
                             tmp.string() + "'";
            int rc = run(gh);
            fs::remove(tmp, pec);
            if (rc != 0) {
                std::cerr << "pulp ship swap-pack: `gh secret set` failed — is the GitHub CLI "
                             "installed and authenticated (`gh auth status`)?\n";
                return 1;
            }
            std::cout << "Signing key backed up as GitHub secret " << secret << " in " << norm
                      << "\n";
        }
        return 0;
    }

    std::cerr << "pulp ship: unknown subcommand: " << sub << "\n";
    return 2;
}
