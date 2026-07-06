---
name: ship
description: Sign, notarize, package, and distribute Pulp plugins and apps across macOS, Windows, and Android
triggers:
  - ship
  - sign
  - notarize
  - package
  - appcast
  - keystore
  - code signing
  - Play Store
  - release
  - distribute
  - installer
---

# Ship Skill — Signing, Packaging, and Distribution

## Overview

The `pulp ship` command handles the full distribution pipeline: code signing, Apple notarization, platform-specific packaging, update feed generation, and Android APK/AAB builds.

For the end-to-end release pipeline that turns a merged PR into a published GitHub Release (auto-release.yml → sign-and-release.yml → release-cli.yml, the 11 published assets, and the failure-mode triage by which asset is missing), see [`docs/guides/release-pipeline.md`](../../../docs/guides/release-pipeline.md). Cross-reference comments at the top of `release-cli.yml` and `sign-and-release.yml` point back to that doc — keep them in sync when either workflow changes ownership of appcast generation, draft creation, or the matrix-tarball upload.

## Pre-flight: plugin ↔ CLI skew check

Before running `pulp ship ...`, source the shared skew-check helper so
a user on an outdated CLI sees a one-line hint (stderr, once per
session) when the installed CLI is older than the plugin's declared
`min_cli_version`:

```bash
source "$(git rev-parse --show-toplevel)/tools/scripts/cli_version_check.sh"
pulp_cli_version_check
```

Advisory only — never blocks. Full contract + override knobs live in the
`upgrade` skill.

## Subcommands

| Command | Platform | What It Does |
|---------|----------|-------------|
| `sign` | macOS, Windows, Android | Sign plugin bundles or APK/AAB; `--path <file>` signs one explicit desktop artifact (macOS `.app`/`.dmg`/bundle, Windows `.exe`/bundle; not `.pkg`) |
| `notarize` | macOS only | Submit to Apple notarization, poll, staple; `--path <file>` notarizes one explicit `.dmg`/`.pkg`/`.zip` (repeatable), not a raw `.app` |
| `package` | All | Create .pkg/.dmg (macOS), NSIS (Windows), APK+AAB (Android) |
| `release` | macOS only | One command: sign → package → **notarize + staple the .pkg/.dmg it builds** → verify |
| `share` | macOS only | One-off: sign → wrap `.app` in DMG → notarize → staple → Gatekeeper-verify a single artifact for sharing |
| `auv3-xcodeproj` | macOS only | Generate an Xcode project for an AUv3 target |
| `appcast` | All | Generate Sparkle-compatible XML update feed |
| `check` | All | Verify signing status of built artifacts |
| `doctor` | macOS | Make signing+notarization non-interactive: self-heal the dedicated signing keychain and validate the `.p8` notary key. No build dir required. |
| `swap-pack` | All (keychain macOS-only) | Sign a hot-reload UX bundle into a swap pack. **No build dir required.** Capabilities are inferred from the bundle's JS (`--autocaps`, no hand-maintained list); the Ed25519 signing key is reused from the macOS keychain (`pulp.reload.signing.<plugin_id>`, created + stored + screamed on first use, **never silently regenerated**, corrupt entry refused) or a `--sign-key <file>`. `--backup-github [--repo owner/name]` publishes the key as a GitHub secret in the **plugin** repo (refuses core Pulp). The key blob is piped to `security`/`gh` via a 0600 temp, never on argv. Thin assembly over the header-only reload building blocks (`pack_build`/`pack_signing_ui`/`swap_pack` serialize/`key_store`); the signing key is a distinct concern from the Developer-ID/notary identity the other subcommands use. See [`docs/guides/reload-trust.md`](../../../docs/guides/reload-trust.md). |

## Non-interactive signing (no keychain / 1Password prompt)

On a workstation, `codesign` and `notarytool` can pop a macOS **keychain "allow
access"** dialog or a **1Password** prompt — which silently wedges any headless,
SSH, or CI sign. Two root causes, two durable fixes, both codified in
`pulp ship doctor` (script: `tools/scripts/ensure_signing_ready.sh`):

1. **Signing key in the *login* keychain** → `codesign`/1Password prompt. Fix:
   sign from a **dedicated keychain** whose key is authorized for `codesign` via
   `security set-key-partition-list`. The doctor creates/unlocks it, imports the
   `.p12`, runs the partition-list step, and adds it to the search list — all
   idempotent.
2. **`notarytool` driven by a keychain *profile*** re-prompts when the login
   keychain locks (and the profile periodically vanishes on churny hosts). Fix:
   notarize from a **file-based App Store Connect `.p8` API key** — no keychain
   at all. The doctor validates it; `--check-online` also re-mints the optional
   `pulp-notary` convenience profile from the same `.p8`.

```bash
pulp ship doctor                 # heal + report; exit 0 = ready (offline, deterministic)
pulp ship doctor --check-online  # also prove the .p8 against Apple (read-only) + refresh profile
pulp ship doctor --print-env     # emit resolved identity/keychain/keypath handles (no secrets) for eval
```

`pulp ship sign` runs this doctor as a **best-effort, quiet preflight** so the
hardened path is automatic. The doctor itself NEVER prints secret values.

**Secrets live OUTSIDE the repo** (never committed), in
`~/.config/pulp/secrets/` (override dir with `$PULP_SECRETS_DIR`):
- `keychain.env` — `PULP_SIGN_KEYCHAIN`, `PULP_SIGN_KEYCHAIN_PW`, `PULP_SIGN_P12`,
  `PULP_SIGN_P12_PW`, `PULP_SIGN_IDENTITY_HASH` (+ optional `…_INSTALLER_HASH`)
- `notary.env` — `PULP_NOTARY_KEY_PATH` (`.p8`), `PULP_NOTARY_KEY_ID`,
  `PULP_NOTARY_ISSUER_ID` (the same trio `notary_env.cpp` resolves for `notarize`)

Each value may also come from the same-named environment variable; **env wins
over the file**. If no dedicated keychain is configured, the doctor falls back to
the login keychain and warns loudly that signing **may** prompt.

## Configuration

Settings are resolved in order: **CLI flag > environment variable > ~/.pulp/config.toml**

### Setup

```bash
pulp config init                    # Create config from template
pulp config set signing.apple.identity "Developer ID Application: Name (TEAMID)"
pulp config set signing.apple.team_id "ABCDE12345"
pulp config set signing.apple.apple_id "you@example.com"
pulp config set signing.android.keystore "~/keystores/release.jks"
```

### Config file location

`~/.pulp/config.toml` (override with `$PULP_HOME`)

See `config.example.toml` in the repo root for all options with documentation.

## Interactive safety contract

Before executing any signing, notarization, or packaging action, the `/ship` command uses AskUserQuestion to:
1. Show resolved config values (identity, keystore, credentials) and their source (CLI/env/config.toml)
2. Let the user review, edit, or cancel before proceeding
3. Offer to save new values with `pulp config set`

When invoked via skill trigger (not slash command), apply the same pattern: always show what will happen and confirm before executing.

## Workflows

### One-off share (macOS): hand a build to a friend, properly signed

When a developer just wants to give someone a working `.app`/`.dmg`/`.pkg` —
NOT cut a versioned GitHub release — reach for `share`. It is the opinionated
one-command path and is fully separate from the repo release pipeline.

```bash
# .app → signs, wraps in a DMG, signs the DMG, notarizes, staples, then runs
# the exact `spctl -a -t open --context context:primary-signature` Gatekeeper
# check. Green = the recipient will NOT see "Unnotarized Developer ID".
pulp ship share MyApp.app --identity "Developer ID Application: Name (TEAMID)"
pulp ship share MyApp.app --identity "..." --output dist --entitlements entitlements.plist

pulp ship share MyApp.dmg            # already a DMG → skip the wrap step
pulp ship share Installer.pkg        # pkg assumed installer-signed → notarize only
pulp ship share MyApp.app --dry-run  # print the plan; sign/notarize nothing
```

Why this exists: a Developer-ID-signed-but-**unnotarized** DMG is rejected by
Gatekeeper (`source=Unnotarized Developer ID`). The cert is fine; the gap is
notarization. `share` closes that gap in one step. Credentials resolve through
the same chain as `pulp ship notarize` (App Store Connect API key preferred);
without creds, the notarize step fails loudly rather than shipping unnotarized.
For `.app` inputs, `--output <dir>` chooses where the generated DMG lands and
`--entitlements <plist>` overrides the default app-signing entitlements.

**Signed + notarized is NOT the same as portable.** A perfectly signed, notarized
`.app` still degrades on another machine if it reads an asset (SVG / JSON / image)
from an absolute **build-tree** path at runtime — that path exists only on the
build box. TRIAZ hit this (2026-06-18): `create_view()` did
`std::ifstream(TRIAZ_FRAME_SVG)` where the macro was
`"${CMAKE_CURRENT_SOURCE_DIR}/import/frames/mixer.frame.svg"`; the shared `.app`
found nothing and fell back to the generic auto-Parameters panel on every Mac but
the build box. It built, signed, notarized, and ran fine locally — nothing flagged
it. Two defenses now exist:
- **`pulp_assert_portable_bundle` (PulpPortable.cmake)** runs automatically on every
  standalone `.app` build and WARNs (or fails, with `-DPULP_STRICT_PORTABLE=ON`) if
  the binary bakes the source/build dir as a string. Set `PULP_STRICT_PORTABLE=ON`
  for release/ship builds so a non-portable binary can't ship.
- **The fix for a finding:** EMBED the asset — `pulp_embed_files(target FILES …)` →
  `pulp::EmbeddedAsset::get("name")`, or `pulp_add_binary_data(...)` — so it's
  compiled in. Never read an absolute build-tree path at runtime. (The faithful
  design-import codegen already embeds; only hand-rolled `create_view` asset loads
  bypass it.)

The composable primitives underneath:
- `pulp ship sign --path <artifact>` — sign exactly one desktop artifact (`.pkg` installers are signed by `package`).
- `pulp ship notarize --path <dmg|pkg|zip>` — notarize + staple one artifact (repeatable). Use `share` for a raw `.app`.

**Do not confuse with the production release.** `share`/`release`/`sign`/
`notarize` are local developer commands. The versioned GitHub Release (all
example plugins, appcast, downloads) runs through CI
(`.github/workflows/sign-and-release.yml`), which does its own
`codesign` + `pkgbuild`/`productbuild` + `xcrun notarytool submit` +
`stapler staple` and never calls these CLI subcommands. Changing the CLI
ship subcommands cannot affect a regular release.

### Combined installer is component-SELECTABLE by default

`create_combined_pkg` (`ship/platform/mac/codesign_mac.mm`) builds one
component `.pkg` per format and combines them with `productbuild
--distribution`, emitting a distribution document with one user-toggleable
`<choice>` per `InstallComponent` (all `start_selected`, `customize="allow"`).
So a multi-format installer always offers a **Customize** pane to install only
AU / VST3 / CLAP as desired — do NOT drop back to a flat
`productbuild --package ...` archive (that installs everything with no choice).
Set `InstallComponent::title` for the choice label, or leave it empty to derive
from the install location ("Components" → "Audio Unit (AU)", etc.). Verified by
`test_codesign.cpp` ("component-selectable with a choice per format").

### macOS one-command pipeline: `pulp ship release`

```bash
pulp ship release --dmg --identity "Developer ID Application: ..."   # standalone app
pulp ship release --pkg --identity "..."                            # plugin installers
```

Runs sign → package → notarize → staple as one command. Unlike calling the
stages by hand, the notarize stage targets the **distributable `.pkg`/`.dmg`
that packaging produced** (selected by build time), so `release --dmg` leaves a
Gatekeeper-ready disk image in `artifacts/`, not a signed-but-unnotarized one.
`--skip-sign` / `--skip-package` / `--skip-notarize` gate stages for CI dry-runs.

**Signing identities matter per artifact.** A `.pkg` must be signed with a
**Developer ID Installer** identity (distinct from the Developer ID
*Application* identity that signs bundles/apps/dmgs) or notarization rejects it.
Pass it via `--installer-identity "Developer ID Installer: …"` (or
`signing.apple.installer_identity` in config); `release` also signs standalone
`.app`s and the produced `.dmg`. As a safety net, the notarize stage verifies
each artifact's signature and **skips (does not submit) any unsigned `.pkg`/
`.dmg`** — so a missing installer identity yields a clear "skipping unsigned
artifact" warning instead of a failed notarytool submission. `notarize --path`
likewise rejects a raw `.app` (notarytool needs a `.dmg`/`.pkg`/`.zip`
container — use `share` for an app).

### AUv3 Xcode handoff: `pulp ship auv3-xcodeproj`

```bash
pulp ship auv3-xcodeproj MyPlugin --sdk iphonesimulator --dry-run
pulp ship auv3-xcodeproj MyPlugin --sdk iphoneos --open
```

Generates a separate CMake Xcode build directory for a project containing an
AUv3 target. `--sdk` accepts `iphonesimulator`, `iphoneos`, or `macosx`; the
default output is `build/xcode/<target>-<sdk>`. The generated build hint targets
`<target>_AUv3`. Use `--dry-run` to print the CMake invocation and build hint
without requiring Xcode. For macOS AUv3 work, the generated project also
contains the runnable containing-app target `<target>_AUv3Host`.

### macOS: Build → Sign → Package → Notarize → Appcast

```bash
pulp build                                              # Must build first
pulp ship sign                                          # Uses identity from config
pulp ship package --version 1.0.0                       # Creates .pkg in artifacts/
pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg  # Submit the packaged artifact
pulp ship appcast --url https://example.com/Plugin.pkg --version 1.0.0
pulp ship appcast --url artifacts/Plugin.pkg --download-url https://example.com/Plugin.pkg --sign-key <base64-key>
```

#### Notarization credentials (`pulp ship notarize`)

**FIRST, on a set-up dev machine: check `~/.config/pulp/secrets/`.** It holds
`notary.env` (the ASC API-key trio), the `AuthKey_*.p8`, `keychain.env`, and
`pulp-signing.p12` — everything needed to sign AND notarize. Source it and go:
`set -a; . ~/.config/pulp/secrets/notary.env; set +a` → `xcrun notarytool
submit … --wait` → `xcrun stapler staple`. Do NOT conclude "no credentials"
without looking there — these are machine-local and absent from fresh
checkouts, but present on a configured machine. (Mirrored in the CLAUDE.md
"Local macOS signing & notarization credentials" note.)

Two lanes, resolved in this precedence (CLI > env > file > config.toml):

1. **App Store Connect API key** — preferred. `xcrun notarytool submit
   --key <p8> --key-id <id> --issuer <uuid>`. Maps to
   `rcodesign --api-key-path` on Linux too.

   ```bash
   # One-shot CLI form
   pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg \
                      --api-key ~/.config/pulp/secrets/AuthKey_XXX.p8 \
                      --api-key-id XXX --api-issuer <uuid>

   # Persisted form (recommended) — store once, reuse forever:
   # ~/.config/pulp/secrets/notary.env  (chmod 600)
   #   PULP_NOTARY_KEY_PATH="$HOME/.config/pulp/secrets/AuthKey_XXX.p8"
   #   PULP_NOTARY_KEY_ID="XXX"
   #   PULP_NOTARY_ISSUER_ID="<issuer-uuid>"
   pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg
   pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg --dry-run
   ```

   For the full reusable dev-signing stub (schema template + sourceable
   helper + per-plug-in setup), see
   [`docs/guides/ios-dev-signing.md`](../../../docs/guides/ios-dev-signing.md).

2. **Legacy Apple-ID + app-specific password** — kept working as a fallback.

   ```bash
   pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg \
                      --apple-id you@example.com --team-id ABCDE12345
   # password defaults to @keychain:AC_PASSWORD — store via
   #   security add-generic-password -s AC_PASSWORD -a you@example.com -w
   ```

Override the env-file path with `PULP_NOTARY_ENV=/some/path` or
`pulp ship notarize --env-file /some/path` (handy in CI sandboxes that don't
share `$HOME`). `--dry-run` is the safest way to verify credential
resolution — it prints which surface each value came from
(`(from cli)` / `(from env)` / `(from file)`) and never contacts Apple.

When changing `ship/include/pulp/ship/codesign.hpp`, keep every platform
implementation in lockstep. Linux intentionally returns no-op / false /
`std::nullopt` for signing and notarization entry points, but it still must
define every public overload, including App Store Connect
`notarize_submit_asc(...)`; otherwise Linux-only package tests are the first
place the API parity break appears. Keep this covered in
`test/test_linux_packaging.cpp` alongside the macOS parser tests in
`test/test_codesign.cpp`.

### macOS manual sign + notarize from a worktree (no `pulp` CLI built)

Feature worktrees usually build only the example targets (`build-gpu/...`), not
the `pulp` CLI — so `pulp ship sign/notarize` isn't available. Sign and notarize
with the raw Apple tools. This is the exact flow `pulp ship` automates; reach for
it only when the CLI binary is absent.

**One-time keychain authorization (THE blocker).** A fresh login keychain does
not let `codesign` use the Developer ID private key non-interactively, even when
the key is present and the keychain is unlocked. `codesign` fails with
`errSecInternalComponent` (and a half-written `*.cstemp` is left behind). The fix
is to authorize the partition list once — it needs the login password, so the
user runs it (suggest the `!` prefix, or their own Terminal):

```bash
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
  -k "<login-password>" ~/Library/Keychains/login.keychain-db
```

Symptoms map cleanly: `errSecInternalComponent` → partition list not set;
`User interaction is not allowed` → keychain locked (`security unlock-keychain`);
`no identity found` → wrong/absent cert (`security find-identity -v -p codesigning`).

**Signing over SSH (e.g. an agent on a remote Mac like `macstudio`).** An SSH
session is NOT the GUI Aqua session, so the login keychain is **locked** there
even when it's unlocked on the console — `security show-keychain-info
login.keychain-db` prints `User interaction is not allowed`, and codesign fails
with `errSecInternalComponent` regardless of the partition list. The GitHub
self-hosted runners sign fine only because they run inside the logged-in GUI
session. For SSH signing you must unlock the keychain in-session first:

```bash
security unlock-keychain -p "<login-password>" ~/Library/Keychains/login.keychain-db
# then partition-list (once) + codesign as above
```

Notarization over SSH is unaffected — `notarytool` authenticates with the App
Store Connect API key in `~/.config/pulp/secrets/notary.env`, never the keychain.
For unattended/turnkey remote signing, prefer a **dedicated build keychain**
(import a `.p12` of the Developer ID cert+key, give it its own password stored in
`~/.config/pulp/secrets/`, `set-key-partition-list` on it, and
`unlock-keychain` it per session) so signing never depends on the interactive
login password — the same pattern `apple-actions/import-codesign-certs` uses in
CI.

**Dedicated-keychain recipe that actually works when the login keychain ALSO has
the cert** (the macstudio case — the GUI host has the same Developer ID identity):

1. Export the identity to a `.p12` once on a Mac where it works — the private-key
   export needs a GUI "Allow" click, so the *user* runs it (`security export -k
   login.keychain-db -t identities -f pkcs12 -P <p12pw> -o key.p12`); it can't be
   done headlessly.
2. On the remote Mac: `security create-keychain -p <kcpw> pulp-signing.keychain-db`;
   `unlock-keychain`; `security import key.p12 -k <kc> -P <p12pw> -T /usr/bin/codesign
   -T /usr/bin/productbuild -T /usr/bin/pkgbuild`; `set-key-partition-list -S
   apple-tool:,apple:,codesign: -s -k <kcpw> <kc>`. Store `<kcpw>` + the cert SHA-1
   hashes in `~/.config/pulp/secrets/keychain.env`.
3. **Sign by SHA-1 hash, not by name.** The identity now exists in BOTH the
   dedicated keychain and login keychain, so signing by name → `ambiguous (matches
   ... in two keychains)`. Signing by the 40-char hash disambiguates.
4. **The dedicated keychain MUST be in the search list** for codesign to find the
   identity — `--keychain <kc>` alone does NOT restrict lookup (codesign still uses
   the search list and hits the *locked* login cert → `errSecInternalComponent`).
   Put it FRONT of the search list for the signing call, then restore:
   `security list-keychains -d user -s <kc> <login>` → sign → `... -s <login>`.
   Safe on a CI-runner host because the Mac Studio runner only builds+tests; the
   signing/release lanes run on GitHub-hosted (it never signs, so a transient
   search-list change can't break the required `macos` gate).

`~/.config/pulp/pulp-sign.sh` wraps steps 3–4 (inner-out, by hash, restore) and
falls back to login-keychain-by-name when no `keychain.env` exists. Deployed on
both Daniel's Macs; notary + signing creds live only in `~/.config/pulp/secrets/`
(chmod 600, never in the repo).

**Sign inner-out.** Pulp GPU bundles embed `libwgpu_native.dylib` in
`Contents/MacOS/`. Sign every embedded dylib BEFORE the bundle, else you get
`invalid or unsupported format for signature` / `code has no resources but
signature indicates they must be present`. Remove any stale `_CodeSignature`
first so the re-seal is clean. `--deep` is deprecated and misses nested Mach-Os —
do the explicit inner-out walk:

```bash
ID="Developer ID Application: NAME (TEAMID)"
sign() { codesign --force --timestamp --options runtime --sign "$ID" "$@"; }
for b in build-gpu/AU/X.component build-gpu/VST3/X.vst3 build-gpu/CLAP/X.clap \
         build-gpu/examples/X/X.app; do
  rm -rf "$b/Contents/_CodeSignature"
  find "$b/Contents/MacOS" -name '*.dylib' -exec false {} + ; \
    while IFS= read -r d; do sign "$d"; done < <(find "$b/Contents/MacOS" -name '*.dylib')
  sign "$b"
  codesign --verify --deep --strict --verbose=2 "$b"   # expect "satisfies its Designated Requirement"
done
```

**Build the signed installer:**

```bash
pkgbuild --root <stage-dir> --identifier com.pulp.<name> --version X.Y.Z \
  --install-location / --sign "Developer ID Installer: NAME (TEAMID)" out.pkg
pkgutil --check-signature out.pkg   # "signed by a developer certificate ... for distribution"
```

**Signed ≠ notarized.** `spctl --assess --type execute MyApp.app` on a signed but
un-notarized bundle prints `rejected / source=Unnotarized Developer ID`. That is
expected and fine for the build machine; recipients on other Macs need
notarization. Submit the installer (or app/dmg) with notarytool — it authenticates
to Apple's cloud, so it needs App Store Connect API-key creds (the same
`~/.config/pulp/secrets/notary.env` trio that `pulp ship notarize` reads):

```bash
set -a; . ~/.config/pulp/secrets/notary.env; set +a   # PULP_NOTARY_KEY_PATH/_KEY_ID/_ISSUER_ID
xcrun notarytool submit out.pkg --key "$PULP_NOTARY_KEY_PATH" \
  --key-id "$PULP_NOTARY_KEY_ID" --issuer "$PULP_NOTARY_ISSUER_ID" --wait
xcrun stapler staple out.pkg                  # embed the ticket so it works offline
spctl --assess --type install -vv out.pkg     # now: accepted / source=Notarized Developer ID
```

If notarization is `Invalid`, fetch the per-file reasons:
`xcrun notarytool log <submission-id> --key ... --key-id ... --issuer ...`.

### Linux: Build → Package (.deb / .tar.gz, no signing)

```bash
pulp build                                              # Must build first
pulp ship package --version 1.0.0                       # Creates a .deb (or .tar.gz)
# Standalone app → single-file AppImage (point --binary at the built executable):
pulp ship package --version 1.0.0 --format appimage \
    --binary build/examples/myapp/MyApp_Standalone [--icon myapp.png]
```

`pulp ship package` on Linux builds a Debian package from the plugin
bundles in `build/{VST3,CLAP,LV2}` via `pulp::ship::create_deb`
(`ship/platform/linux/package_linux.cpp`), installing them under
`/usr/lib/{vst3,clap,lv2}`. When `dpkg-deb` is not on `PATH` it falls back
to a `.tar.gz`. Linux has no signing requirement.

**AppImage (standalone apps):** `--format appimage` routes to
`pulp::ship::create_appimage`, which wraps a single standalone executable
(plugins ship as `.deb`/`.tar.gz`, not AppImage). It synthesizes an AppDir
(`AppRun` launcher, `<app>.desktop`, an icon — caller `--icon` or a built-in
1×1 PNG placeholder, `.DirIcon`) and invokes `appimagetool` with
`ARCH=<appimage_arch()>` (note: `aarch64`/`x86_64`/…, distinct from the
Debian arch names). It honest-fails (returns false, no stray AppDir) when
`appimagetool` is absent or the executable is missing — `appimagetool` is
**not vendored**. The standalone binary must be passed explicitly via
`--binary`; the plugin-centric `build/` layout has no standardized
standalone location to auto-discover. To run the produced AppImage at the
end-user's side, FUSE (libfuse2) or `APPIMAGE_EXTRACT_AND_RUN=1` is needed,
same as any AppImage.

**Routing invariant:** the `pulp ship package` branch in
`tools/cli/cmd_ship.cpp` keeps three mutually exclusive platform arms —
`#if defined(__linux__)` (→ `.deb` via `create_deb`, `.tar.gz` fallback),
`#if defined(__APPLE__)` (→ `.pkg`/`.dmg`), and an honest-fail `#else` for
other Unixes. `pkgbuild`/`hdiutil` exist only on macOS, so the Linux arm
must never fall through into them. Inside the Linux arm, `--format appimage`
is an early sub-branch (standalone executable) that returns before the
plugin-bundle `.deb`/`.tar.gz` path. If you touch that block, preserve the
mutual exclusion and re-run `test_cli_ship_shellout.cpp` (the Linux routing
regression guard) plus `test_linux_packaging.cpp` (the
`create_deb`/`create_tar_gz`/`create_appimage` helper coverage; the AppImage
real-build case runs only where `appimagetool` is installed — e.g. the
tartci VM with libfuse2 — and verifies honest-fail otherwise).

The plugin-bundle Linux path must also fail when no actual `.vst3`, `.clap`,
or `.lv2` bundles exist under the build directory. Do not reuse the macOS
"Created 0 .pkg and 0 .dmg" empty-artifact summary on Linux; report missing
plugins and return a non-zero exit instead. The regression guard is
`test_cli_ship_shellout.cpp`'s "pulp ship package on Linux with no plugin
bundles reports missing plugins" case.

**Architecture field:** `create_deb` stamps the `.deb` `Architecture:` from
the compile-time `debian_architecture()` helper in `installer.hpp`, so a
native build labels the package for its own arch (arm64/amd64/…) rather
than a fixed value. Keep that helper and the package field in sync.

### Android: Build → Package (includes Gradle build + optional signing) → Verify

```bash
pulp build --target android                             # Build native libs
pulp ship package --target android --keystore ~/key.jks # Gradle build + sign APK/AAB
pulp ship check --target android                        # Verify APK/AAB signatures
```

Note: `pulp ship package --target android` invokes Gradle which builds AND signs in one step when a keystore is provided. Use `pulp ship sign --target android` only to re-sign existing artifacts in `artifacts/`.

### Windows: Build → Sign → Package

```bash
pulp build                                              # Must build first
pulp ship sign --identity "Your Company"                # Uses signtool
pulp ship package --version 1.0.0                       # Creates NSIS .exe installer
```

Windows does not notarize, but `ship/platform/win/codesign_win.cpp` must
still define every public notarization symbol from
`ship/include/pulp/ship/codesign.hpp`. Keep ASC-key
`notarize_submit_asc(...)` as a fail-closed `std::nullopt` stub so
`pulp-test-codesign` links on Windows and the cross-platform API surface
stays in lockstep with macOS/Linux.

**signtool failure contract.** `codesign()` on Windows
must never report success for an unusable signature: it rejects an empty
identity/path up front (no `signtool sign /n ""`), and after signing it runs
`signtool verify /pa` and returns false if verification fails — so a sign that
exits 0 but leaves the artifact unverifiable still fails. The empty-input
reject is covered by the `[windows]`-guarded case in `test_codesign.cpp`
(runs without signtool present); the verify-after-sign path is compile-verified
on the Windows lane (a real round-trip needs a cert).

**NSIS installer (W7).** `generate_nsis_script()` is a pure function — assert
its output in `test_nsis_installer.cpp` (cross-platform, real verification, no
NSIS/Windows needed). It places VST3/CLAP under `$COMMONFILES\{VST3,CLAP}`
(or `$LOCALAPPDATA\Programs\Common\...` for `--per-user`), and **standalone
apps get a Start-menu shortcut** under `$SMPROGRAMS\<publisher>` (removed on
uninstall); plugins get none. When you touch the generator, add/extend the
pure-output assertions.

**Auto-update decision (W7): DEFERRED.** No WinSparkle/auto-update is wired on
Windows. Rationale: Pulp targets developers shipping their own apps/plugins,
who pick their own update channel (DAW-scanned plugins don't self-update at
all); pulling WinSparkle in would add a dependency for a story the SDK doesn't
own. macOS Sparkle appcast generation stays available for those who want it.
Revisit if a first-party standalone-app updater becomes a requirement.

## Android-Specific

### ABI Selection

```bash
--abi arm64-v8a     # Default — ARM64 phones + tablets
--abi x86_64        # Emulator / Chromebook
--abi all           # arm64-v8a + x86_64 + armeabi-v7a
```

### Tablet Support

No special flags needed. ARM64 covers phones and tablets. AAB with split APKs handles screen density automatically.

### Keystore Creation

```bash
keytool -genkey -v -keystore release.jks -keyalg RSA -keysize 2048 -validity 10000
```

### Password Security

Never store passwords in plaintext config. Use environment variable references:
```toml
store_pass = "@env:ANDROID_STORE_PASS"
```

## Common Issues

### "No signing identity specified"

Run `security find-identity -v -p codesigning` (macOS) to find your identity, then:
```bash
pulp config set signing.apple.identity "Developer ID Application: ..."
```

### `codesign` fails with `errSecInternalComponent`

The identity is present and the keychain is unlocked, but `codesign` cannot use
the private key non-interactively until the partition list is authorized once.
The user runs (it needs the login password):
```bash
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
  -k "<login-password>" ~/Library/Keychains/login.keychain-db
```
Full local sign+notarize recipe (inner-out dylib signing, the `*.cstemp`
leftover, `pkgbuild`, notarytool, stapler) in *macOS manual sign + notarize from
a worktree* above.

### "Android SDK not found"

Install Android Studio or set `ANDROID_HOME`:
```bash
export ANDROID_HOME=~/Library/Android/sdk  # macOS
```

### "Notarization failed" / "no notary credentials resolved"

- For the ASC-key flow: verify the `.p8` is readable, the Key ID matches
  the filename suffix (`AuthKey_<id>.p8`), and the issuer UUID is from the
  same App Store Connect tenant. Use `pulp ship notarize --dry-run` to
  see which surface each value resolved from and confirm no fields are blank.
- For the legacy flow: regenerate the app-specific password at
  https://appleid.apple.com → Sign-In and Security.
- Ensure the bundle is properly signed with a Developer ID certificate
  (not just a development cert).
- Check the notarization log: `xcrun notarytool log <UUID>`.

### `check_notarization` and Gatekeeper-disabled CI environments

`check_notarization(path)` runs `spctl --assess --type exec <path>`. On a stock
Mac, `spctl --assess` returns non-zero for an unsigned or **nonexistent** path.
But CI base images (notably the cirruslabs macOS bases used by the Tart VM lane)
ship with Gatekeeper assessment **disabled** (`spctl --master-disable`), in which
case `spctl --assess` returns **0 for any argument** — so it cannot distinguish a
notarized binary from a bogus path. The fix (already applied in
`ship/platform/mac/codesign_mac.mm`) is an explicit `fs::exists` short-circuit:
a path that doesn't exist returns false before `spctl` is consulted, which is
correct in both environments. If you add new spctl/codesign predicates, don't
rely on `spctl --assess` alone for negative cases — guard with an existence /
signature pre-check so they hold on Gatekeeper-disabled runners too.

### "Gradle build failed"

- Run `pulp doctor` to check Android SDK/NDK/Java versions
- Ensure `android/` project exists (`pulp create --targets android`)
- Check Gradle output in the terminal for specific errors

### Appcast parsing must tolerate malformed metadata

`pulp ship appcast` feeds can be generated by older tools or edited by hand.
When parsing existing Sparkle appcast XML, malformed optional enclosure
metadata must fail soft. In particular, a non-numeric or overflowing
`length="..."` attribute should leave the item's file size at `0` and keep
parsing the item instead of throwing out of `Appcast::from_xml`. Keep this
behavior covered in `test/test_appcast.cpp` when changing
`ship/src/appcast.cpp`.

### Appcast output paths

`pulp ship appcast --output appcast.xml` must work from a project root. When
creating the output directory, guard empty `parent_path()` values before calling
`std::filesystem::create_directories`; otherwise a bare filename can throw
instead of writing the feed. Keep this covered in
`test/test_cli_ship_shellout.cpp`.

When signing appcast entries, `--url` must be the local artifact path used for
file size and Ed25519 signing. Use `--download-url` to write the public
Sparkle enclosure URL into the feed. The hosted file must be byte-identical to
the local artifact passed as `--url`; otherwise Sparkle rejects the update
because the feed length and signature describe different bytes.

### Android package tests fail only on Windows

Pulp executes Android package helpers through `cmd.exe /c` on Windows. Android
SDK tools and Gradle wrappers may resolve to `.bat` files there, so command
strings that begin with a quoted batch path need an outer command quote so
`cmd.exe` does not strip the executable quote while preserving the remaining
quoted arguments. For Gradle and bundletool `name=value` parameters that
contain paths or passwords, quote the whole `name=value` token rather than only
the value (`"--output=C:\path\file.apks"`, not `--output="C:\path\file.apks"`),
because Windows batch `%~1`/`shift` parsing does not normalize embedded quotes
the same way POSIX shells do. Gradle wrapper invocations should use the
ChildProcess `working_directory` option instead of inlining `cd ... && gradlew`
into the shell string, so fake wrappers and real Gradle builds write artifacts
relative to the project root consistently. Keep this in mind when touching
`ship/platform/android/package_android.cpp`.

### Released CLI tarball crashes on user machines

Symptom: `dyld: Library not loaded: @rpath/libwgpu_native.dylib` or
`error while loading shared libraries: libwgpu_native.so` after the
user extracts `pulp-<platform>.tar.gz` from a GitHub Release.

Root cause: `release-cli.yml` historically uploaded the bare
`build/tools/cli/pulp` binary, which carries an `LC_RPATH` /
`DT_RUNPATH` pointing at the build runner's home directory (e.g.
`/Users/runner/Library/Caches/Pulp/...` on macOS, the
GitHub-hosted-runner workspace on Linux). That path doesn't exist on
user machines.

Fix (active since v0.20.x): `tools/scripts/package_cli.py` is invoked
by `release-cli.yml` to:
1. Copy `libwgpu_native.{dylib,so,dll}` next to the binary.
2. macOS: `install_name_tool -delete_rpath` every absolute LC_RPATH
   and add `@loader_path`.
3. Linux: `patchelf --set-rpath '$ORIGIN'`.
4. Windows: no rewrite needed — DLLs resolve from the binary's
   directory automatically.

The portable-binary smoke gate in `release-cli.yml` runs the produced
artifact on a *clean* runner that did not build it, catching the bug
class before tagging. If you change rpath logic, run the smoke job
locally first or it will fail in CI for everyone else.

### Phase 8 CLI release artifacts are dual-binary

After the Rust CLI flip, release artifacts must contain both `pulp`
and `pulp-cpp` in the same archive. `pulp` is the user-facing Rust CLI;
`pulp-cpp` is the C++ delegate used for fallthrough commands that still
link framework libraries. Do not ship `pulp-rs` as the public binary
name, and do not drop `pulp-cpp` from tarballs or zips.

Smoke both paths when touching `.github/workflows/release-cli.yml` or
`tools/scripts/package_cli.py`: run `pulp version --json` against the
Rust binary, then exercise a C++-owned command through
`PULP_RS_CPP_BINARY=/path/to/pulp-cpp pulp ...` or invoke `pulp-cpp`
directly. This preserves rollback/debug workflows such as
`PULP_USE_CPP=1 pulp <args>` for existing Pulp projects.

### CLI tarball also ships `pulp-mcp`

The release tarball is **three** binaries — `pulp`,
`pulp-cpp`, and `pulp-mcp`. `pulp-mcp` is the Claude Code plugin's MCP
server. The plugin ships no binaries; its `.mcp.json` invokes
`tools/mcp/pulp-mcp-launcher`, which `exec`s `pulp-mcp` from `$PATH`. If
the CLI tarball is missing `pulp-mcp`, every fresh
`claude plugin install pulp` produces a `/mcp` panel reporting `cannot
locate pulp-mcp binary` — even though the plugin itself installed
cleanly.

Contract that must hold across release lanes:

- `tools/scripts/package_cli.py` is called with `--mcp-binary
  build/tools/mcp/pulp-mcp` (Unix) or
  `build/tools/mcp/Release/pulp-mcp.exe` (Windows).
- `.github/workflows/release-cli.yml` strips `pulp-mcp` and adds it to
  the smoke matrix as `pulp-mcp --version` (its JSON-RPC stdin loop is
  not meaningfully testable from CI; the flag short-circuits before
  reading stdin).
- `tools/scripts/install.sh` / `install.ps1` log `pulp-mcp` landing in
  `~/.pulp/bin/` so users see the binary that the plugin will use.
- `pulp upgrade --install` extracts the whole tarball, so refreshing
  the CLI also refreshes `pulp-mcp`.

**Document and enforce install order in user-facing docs**:
`curl install.sh | sh` BEFORE `claude plugin install pulp`. The
reverse order leaves `/mcp` broken until the user upgrades the CLI.

**Do not lock-step plugin and `pulp-mcp` versions.** The plugin and
`pulp-mcp` are installed via separate channels and a project may pin an
older SDK. `pulp-mcp` advertises its real `PROJECT_VERSION` in
`serverInfo.version`, but the launcher must remain version-tolerant —
backwards compat is the MCP server's responsibility (per-tool feature
detection), not the launcher's. `pulp doctor` surfaces drift advisory-
only.

**Per-tool `min_sdk_version` floors live in
`tools/mcp/pulp_mcp.cpp::TOOL_MIN_SDK_TABLE`.** When adding a new MCP
tool that depends on a specific SDK API, add a row to that table with
the SDK version that API landed in. The tools/call dispatcher reads the
active project's pinned SDK (from `pulp.toml` `sdk_version` first, then
`CMakeLists.txt` `project(... VERSION ...)`) on every call and returns
an `isError:true` content payload with actionable upgrade guidance when
the project SDK is too old. The `pulp_compat` introspection tool
exposes the full matrix (`pulp_mcp_version`, `mcp_protocol_version`,
`project_sdk`, `tool_min_sdk`) so plugin authors can pre-filter their
visible tool list at startup. Leaving a tool out of the table = no
floor = runs on any project (matches pre-feature-detection behavior).
Never replace this with a launcher-side hard gate.

### Released SDK is missing WebView symbols

Symptom: a consumer links against a downloaded `pulp-sdk-<platform>`
release artifact and fails to resolve `pulp::view::WebViewPanel::create`
or `pulp::view::make_webview_embedded_resource_fetcher`.

Root cause: `core/view/CMakeLists.txt` defaults `PULP_BUILD_WEBVIEW=OFF`,
so any release SDK build that forgets to opt in will ship a
`libpulp-view-core` / `pulp-view-core.lib` without the native WebView objects.

Keep the release SDK path aligned with the GitHub release workflow:
1. Configure release SDK builds with `-DPULP_BUILD_WEBVIEW=ON`.
2. On Linux, install `libgtk-3-dev` and `libwebkit2gtk-4.1-dev` before
   configuring.
3. Before packaging, verify the staged SDK view-core archive still contains
   `WebViewPanel` and `make_webview_embedded_resource_fetcher`.

This applies to both `.github/workflows/release-cli.yml` and the local
helper `tools/scripts/release-cli-local.sh`. If one changes without the
other, GitHub releases and local release drills diverge.

Release/SDK builds also pass `-DPULP_ENABLE_AUDIO_PROBES=OFF` so SDK and
standalone artifacts do not ship the dev audio-probe surface. Keep
`.github/workflows/release-cli.yml`, `.github/workflows/sign-and-release.yml`,
and `tools/scripts/release-cli-local.sh` in sync when changing release
configure flags.

### Shipyard pin drift between local tooling and release workflows

Pulp's release automation depends on the pinned Shipyard CLI in two places:

- `tools/shipyard.toml` is the source-of-truth pin for local installs and
  `shipyard pr`.
- `.github/workflows/release-cli.yml` and `.github/workflows/post-tag-sync.yml`
  carry a `SHIPYARD_VERSION` env used by the release-side workflows.

If you bump the Shipyard pin, update both workflows in the same PR and keep the
existing string format in each file (`v0.56.2` in `tools/shipyard.toml`,
`0.56.2` in the workflow envs — the `v` prefix is intentional only on the
toml). Otherwise local shipping and tag-time release jobs quietly diverge
onto different Shipyard versions, which is how release-only behavior
changes get missed. Shipyard v0.55.0+ also ships `shipyard update`; use
`shipyard update --check --json` to report local drift and
`shipyard update --to v0.56.2` (or newer) before cutting or debugging release
jobs.

### VST3 SDK tag drift in `sign-and-release.yml`

Pulp's notarized macOS release workflow clones the Steinberg VST3 SDK directly
inside `.github/workflows/sign-and-release.yml`. Keep that workflow pinned to
the same upstream tag used everywhere else in the repo: `v3.7.12_build_20`.
The shortened `v3.7.12` ref does not exist on Steinberg's repo and causes the
tag-triggered macOS release job to fail immediately at `Clone VST3 SDK`, before
configure, build, or signing begin.

### Never run `validation` ctest tests in `sign-and-release.yml`

The `Test` step in `.github/workflows/sign-and-release.yml` MUST pass
`-LE validation` to ctest. Without that flag, the suite includes
`auval-Pulp*` tests that copy a freshly-built `.component` to
`$HOME/Library/Audio/Plug-Ins/Components/` and immediately invoke
`auval -v aufx <code> Pulp`. On hosted GitHub macOS runners the
`AudioComponentRegistrar` does not pick up the new bundle reliably, so
auval emits:

```
ERROR: Cannot get Component's Name strings
ERROR: Error from retrieving Component Version: -50
FATAL ERROR: didn't find the component
```

The Test step then exits non-zero and the entire sign / notarize /
publish pipeline silently fails. This failure mode can block many
consecutive tag-triggered release runs before anyone notices.

The validation gates already run in `.github/workflows/validate.yml`
on PR with the documented codesigning caveat. Re-running them in the
release workflow on a runner that cannot satisfy the prereqs adds zero
protection and only adds a silent-failure surface.

`tools/scripts/test_release_workflow_test_step.py` is the regression
test that asserts `-LE validation` stays in the workflow; it is wired
into `.github/workflows/workflow-lint.yml` so any future PR touching
`.github/workflows/**` runs it automatically.

### `sign-and-release.yml` must declare `contents: write`

Every release workflow that uses `softprops/action-gh-release@v2` with
`generate_release_notes: true` — or that otherwise PATCHes the release
entry — needs an explicit job-level `permissions: contents: write`
block. Without it the job inherits a read-only token on `push: tags`
events in many repo configurations, and the final `Create GitHub
Release` step fails with:

```
Skip retry — your GitHub token/PAT does not have the required
permission to create a release
##[error]Resource not accessible by integration
```

Everything up to that point — checkout, VST3 SDK clone, configure,
build, codesign, notarize, artifact upload — succeeds, and the
pipeline still exits non-zero. macOS-signed artifacts never land on
the release. Classic silent-release-failure pattern.

The fix is a four-line addition at the job header:

```yaml
jobs:
  build-and-sign-macos:
    runs-on: macos-14
    permissions:
      contents: write
```

Cross-reference: `release-cli.yml` already sets this on its
release-creating job (line ~360). If you add a new release-time
workflow, do the same. The regression test
`tools/scripts/test_release_workflow_test_step.py` now includes
`SignAndReleaseContentsWriteTest` to block reintroduction.

### Skia-builder zip layout drift breaks the release matrix

`release-cli.yml` fetches prebuilt Skia binaries via
`tools/scripts/fetch_skia_for_release.py` after `setup.sh --deps-only`.
The upstream skia-builder zip layout is **not** stable — older series
shipped libs flat under `build/<plat>-gpu/lib/Release/libskia.a`, but
the chrome/m144 series moved them one directory deeper under an arch
subdir: `build/mac-gpu/lib/Release/arm64/libskia.a`, similarly
`linux-gpu/lib/Release/x64/`. Every release after v0.94.0 (v0.95.0..
v0.97.0) failed silently on this for four days, with `release-guard.yml`
opening a per-tag tracking issue but no published GitHub Release.

The fetch script now flattens any single-arch subdir back up into
`Release/` after unpack, so `FindSkia.cmake`'s layout probe keeps
working unchanged. Regression coverage lives in
`tools/scripts/test_fetch_skia_for_release.py` (covers flat layout,
arch-subdir flatten for both mac-arm64 and linux-x64, missing-lib,
sha256 mismatch). Wired into `workflow-lint.yml`.

When `tools/deps/manifest.json` bumps `Skia` to a new skia-builder
release tag, eyeball the zip layout once:

```bash
curl -sL <new asset URL> -o /tmp/skia.zip
unzip -l /tmp/skia.zip | grep -oE '^.*/lib/Release/[^/]+/' | sort -u
```

If the lib path has a NEW level (not arch — e.g. a config subdir like
`Release/optimized/`), the flatten heuristic needs extending. The flat
+ single-arch-subdir layouts are the only two seen to date.

**Also eyeball exposed symbols.** Some Skia static lib builds re-expose fontconfig
symbols (`FcInitLoadConfigAndFonts`, `FcConfigGetSysRoot`,
`FcPatternGetString` et al.) that the previous release kept private.
`core/canvas/CMakeLists.txt` already has the
`pkg_check_modules(FONTCONFIG fontconfig)` block, but the runner
needs `libfontconfig1-dev` installed for `pkg_check_modules` to find
the library. Both `release-cli.yml` and `build.yml` Linux deps steps
now include it. When bumping Skia, run `nm -D` on the new `libskia.a`
and grep for `Fc[A-Z]\|Hb[a-z]\|FT_` — any new symbol class means
a matching system package needs to be added to the apt step.

**A new fork build lane must be wired into BOTH the matrix AND the
release upload — and Pulp must require GPU only where an asset exists.**
The chrome/m150 incident (v0.395.0 stuck as a draft for days): the
skia-builder fork's linux-arm64 build lane was added (634672f) ~20h
after m150 was already cut, and that commit wired the build *matrix*
but not the `create-release` `files:` list — so the slice was built
every run, uploaded as a workflow artifact, and silently dropped from
the published release. Meanwhile `release-cli.yml` set
`PULP_REQUIRE_GPU_FOR_SDK=ON` for linux-arm64 while
`tools/deps/manifest.json` had no linux-arm64 asset, so the release leg
FATALed with "Skia not found" and never promoted the draft. Two guards
now catch this class:
- skia-builder `tools/check_release_coverage.py` (+ `lint.yml`): every
  active build-matrix lane must appear in the `create-release` upload
  list. Catches "built but not released."
- Pulp `tools/scripts/test_release_cli_gpu_asset_coverage.py` (wired
  into `workflow-lint.yml`): every release-cli platform marked
  `PULP_REQUIRE_GPU_FOR_SDK=ON` must have a `release_assets` entry in
  manifest.json (via `fetch_skia_for_release.py`'s MATRIX_TO_MANIFEST).
  Catches "required but not provided" — the contradiction that
  `test_skia_linux_arm64_asset.py` deliberately tolerated.
When a fork release drops or adds a slice, update the manifest
`release_assets` + `tools/harness/visual/pins.py` in lockstep, and only
keep a platform in the `REQUIRE_GPU=ON` case blocks while its asset is
actually published.

### Backfilling a stuck release tag

`auto-release.yml` creates the tag immediately on merge, but
`release-cli.yml` only publishes after the matrix is green. If matrix
fails, the tag exists with no Release — and
`workflow_dispatch` against that tag re-runs the BROKEN workflow file
from the tag's source. Two safe options:

1. **Main workflow + blank source_ref:** Run the *fixed*
   workflow from main, pass the tag as `version`, and leave `source_ref`
   blank. That checks out the tag's source while enabling the backfill
   overlay step:

       gh workflow run release-cli.yml --ref main \
           -f version=v0.97.0

   The build-cli job overlays safe release-pipeline helper files from
   main automatically, so a backfill picks up post-tag fetch-script,
   packaging, manifest, and targeted CMake fixes even though the tag's
   tree predates them. Leave `make_latest` false for old-tag backfills;
   set it true only when backfilling the current newest tag after the
   automatic tag-triggered run failed.

2. **Cherry-pick fix + retag:** Only if the build itself needs to change.
   Pulp doesn't retag immutable releases — use option 1 unless the
   broken release artifacts would have been wrong even with a green
   build.

## Doctor Checks

`pulp doctor` validates Android toolchain:
- Android SDK location
- NDK version (r26+ required for C++20)
- Java version (17+ required for AGP 8+)
- Build-tools availability (apksigner, zipalign)

## Release publishing is coordinated across two workflows (immutable releases)

A full Pulp release = CLI binaries (`release-cli.yml`) + macOS sign/notarize &
Sparkle appcast (`sign-and-release.yml`), built in parallel on a tag. Because
GitHub published releases are now immutable, both legs create the release as a
DRAFT and `release-publish.yml` publishes it once BOTH succeed. So a release only
appears once the macOS sign/notarize leg is green too; a red sign-and-release leg
leaves the release a draft. See the `ci` skill's coordinator note for the full
mechanism and debugging steps.
