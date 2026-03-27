# CI/CD and GitHub Integration Plan

## Overview

This document specifies Pulp's continuous integration, continuous delivery, and GitHub integration strategy. The goal is a fully automated pipeline from code commit through signed, notarized release artifacts, driven by Claude Code commands and GitHub Actions.

---

## GitHub Actions Architecture

### Multi-Platform Build Matrix

Pulp targets three desktop platforms and must build and test on all of them in CI:

- **macOS** (ARM64, Apple Silicon) -- primary development platform
- **Windows** (x64, MSVC toolchain)
- **Linux** (x64, Clang and GCC)

All workflow templates are shipped as part of the Pulp repository under `.github/workflows/` and are generated or updated by the `/pulp:ci` Claude Code command. Developers can also trigger workflows manually via GitHub's workflow dispatch UI.

### Workflow Trigger Model

Workflows can be triggered by:

1. **Push to main or release branches** -- automatic build and test
2. **Pull request** -- automatic build, test, and format validation
3. **Manual dispatch** -- via GitHub Actions UI or `gh workflow run`
4. **Claude Code command** -- `/pulp:ci` triggers the appropriate workflow via the `gh` CLI
5. **Tag push** -- `v*` tags trigger the sign-and-release pipeline

---

## Workflows

### 1. build.yml -- Build and Test on All Platforms

**Purpose:** Compile all plugin formats and run the full test suite on every supported platform.

**Trigger:** Push to `main`, pull requests, manual dispatch, `/pulp:ci build`

**Matrix:**

| Runner | Architecture | Toolchain | Notes |
|--------|-------------|-----------|-------|
| `macos-14` (or later) | ARM64 | Xcode + CMake | Apple Silicon native |
| `windows-latest` | x64 | MSVC 2022 + CMake | Visual Studio generator |
| `ubuntu-latest` | x64 | Clang 17 + GCC 13 + CMake | Dual-compiler testing |

**Steps:**

1. **Checkout** -- `actions/checkout@v4` with submodules (recursive)
2. **Cache** -- CMake build directory, external dependencies, ccache/sccache
3. **Setup toolchains**
   - macOS: Select Xcode version via `xcodes`; install CMake via Homebrew if needed
   - Windows: Configure MSVC environment via `ilammy/msvc-dev-cmd`
   - Linux: Install required dev packages (`libasound2-dev`, `libx11-dev`, `libxrandr-dev`, etc.)
4. **CMake configure** -- `cmake -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON -DPULP_BUILD_ALL_FORMATS=ON`
5. **Build** -- `cmake --build build --config Release --parallel`
6. **Run tests** -- `ctest --test-dir build --output-on-failure`
7. **Artifact upload** -- Upload built plugin binaries per platform via `actions/upload-artifact@v4`

**Caching Strategy:**

- Use `ccache` (macOS/Linux) or `sccache` (Windows) for compiler output caching
- Cache key includes: OS, compiler version, CMakeLists.txt hash, dependency lock hash
- Restore keys allow partial cache hits for incremental speedups

**Expected Duration:** 5-15 minutes per platform (depending on cache state)

---

### 2. sign-and-release.yml -- Signing, Notarization, and GitHub Release

**Purpose:** Produce signed, notarized, distribution-ready artifacts and publish a GitHub Release.

**Trigger:** Tag push matching `v*`, manual dispatch, `/pulp:ci publish`

**Platform-Specific Signing:**

#### macOS: Code Signing and Notarization

1. **Import certificates** -- Decode `APPLE_DEVELOPER_CERT_P12_BASE64` and `APPLE_INSTALLER_CERT_P12_BASE64` into a temporary keychain
2. **Code sign** -- `codesign --deep --force --options runtime --sign "$TEAM_ID"` on all `.vst3`, `.component`, `.clap` bundles and the standalone `.app`
3. **Create installer** -- `pkgbuild` + `productbuild` to create a signed `.pkg` installer
4. **Sign installer** -- `productsign --sign "Developer ID Installer: ..."` on the `.pkg`
5. **Notarize** -- `xcrun notarytool submit --apple-id "$APPLE_ID" --password "$APP_SPECIFIC_PASSWORD" --team-id "$TEAM_ID" --wait`
6. **Staple** -- `xcrun stapler staple` on the `.pkg` and all individual bundles
7. **Verify** -- `spctl --assess --type install` on the `.pkg`

#### Windows: Authenticode or Azure Trusted Signing

**Option A: Traditional PFX Signing**

1. **Decode certificate** -- Decode `WINDOWS_CERT_PFX_BASE64` to a temporary `.pfx` file
2. **Sign** -- `signtool sign /f cert.pfx /p "$WINDOWS_CERT_PASSWORD" /tr http://timestamp.digicert.com /td sha256 /fd sha256` on all `.dll` and `.exe` files
3. **Verify** -- `signtool verify /pa` on signed files

**Option B: Azure Trusted Signing**

1. **Install Azure CLI** and the Trusted Signing extension
2. **Authenticate** via service principal (`AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`)
3. **Sign** via `az trustedsigning sign` with the configured account and profile
4. **Verify** -- `signtool verify /pa` on signed files

The workflow detects which secrets are available and selects the appropriate signing method.

#### Linux

No code signing required for Linux. Package as `.tar.gz` with appropriate directory structure.

**Release Assembly:**

1. **EdDSA sign** all final artifacts using the project's EdDSA private key (for auto-update verification)
2. **Generate checksums** -- SHA256 for all artifacts
3. **Create GitHub Release** -- Draft release with all artifacts attached, auto-generated changelog from commits
4. **Update gh-pages** -- Push updated download links and appcast XML files to the `gh-pages` branch
5. **Publish** -- Release transitions from draft to published after developer review

---

### 3. validate.yml -- Plugin Format Validation

**Purpose:** Run official and community plugin validators against all built formats.

**Trigger:** Push to `main`, pull requests, manual dispatch, `/pulp:ci validate`

**Validation Matrix:**

| Format | Validator | Platform | Description |
|--------|-----------|----------|-------------|
| AU | `auval` | macOS | Apple's official Audio Unit validation tool. Tests parameter handling, state, rendering, and lifecycle. |
| AUv3 | `auval` | macOS | Audio Unit v3 extension validation. |
| VST3 | `pluginval` | All | Community validator; tests parameter enumeration, state save/restore, audio processing, and edge cases. |
| VST3 | VST3 SDK `validator` | All | Steinberg's official validation tool from the VST3 SDK. |
| CLAP | `clap-validator` | All | Official CLAP community validation tool. Tests all CLAP extensions and host interactions. |
| AAX | PACE validator | macOS/Win | Avid's official AAX validation (requires AAX SDK). |
| LV2 | `lv2lint` | Linux | Community LV2 specification compliance checker. |

**Audio Golden-File Comparison Tests:**

1. Feed known input signals (sine waves, impulses, noise) through each plugin format
2. Compare output against reference golden files
3. Tolerance: bit-exact for integer formats, configurable epsilon for floating-point (default: 1e-6)
4. Tests run at multiple sample rates (44100, 48000, 96000) and buffer sizes (64, 256, 1024)
5. Any deviation beyond tolerance fails the workflow

**Steps:**

1. **Download artifacts** from the `build.yml` run (via `actions/download-artifact@v4`)
2. **Install validators** -- Download or build `pluginval`, `clap-validator`, `lv2lint` as needed
3. **Run AU validation** (macOS only) -- `auval -a` to list, then `auval -v aufx <code> <mfr>` for each AU
4. **Run VST3 validation** -- `pluginval --validate <path>.vst3 --strictness-level 10`
5. **Run CLAP validation** -- `clap-validator validate <path>.clap`
6. **Run golden-file tests** -- Custom test harness that loads plugin headlessly and compares output
7. **Report results** -- Summary comment on PR, status check pass/fail

---

## Secrets Management

### Required Secrets

All secrets are stored as GitHub Actions encrypted secrets at the repository level. They are never logged, printed, or exposed in process argument lists.

| Secret | Platform | Purpose |
|--------|----------|---------|
| `APPLE_ID` | macOS | Apple ID email for notarization submissions |
| `APP_SPECIFIC_PASSWORD` | macOS | App-specific password for notarization (generated at appleid.apple.com) |
| `TEAM_ID` | macOS | Apple Developer Team ID for code signing identity |
| `APPLE_DEVELOPER_CERT_P12_BASE64` | macOS | Base64-encoded Developer ID Application certificate (.p12) for app/plugin signing |
| `APPLE_DEVELOPER_CERT_PASSWORD` | macOS | Password for the Developer ID Application .p12 file |
| `APPLE_INSTALLER_CERT_P12_BASE64` | macOS | Base64-encoded Developer ID Installer certificate (.p12) for .pkg signing |
| `APPLE_INSTALLER_CERT_PASSWORD` | macOS | Password for the Developer ID Installer .p12 file |
| `WINDOWS_CERT_PFX_BASE64` | Windows | Base64-encoded Authenticode signing certificate (.pfx) |
| `WINDOWS_CERT_PASSWORD` | Windows | Password for the Authenticode .pfx file |
| `AZURE_TENANT_ID` | Windows | Azure Active Directory tenant ID for Trusted Signing |
| `AZURE_CLIENT_ID` | Windows | Azure service principal client/application ID |
| `AZURE_CLIENT_SECRET` | Windows | Azure service principal client secret |
| `AZURE_SIGNING_ACCOUNT` | Windows | Azure Trusted Signing account name |
| `AZURE_SIGNING_PROFILE` | Windows | Azure Trusted Signing certificate profile name |
| `AZURE_SIGNING_ENDPOINT` | Windows | Azure Trusted Signing endpoint URI |

### Optional Secrets

| Secret | Platform | Purpose |
|--------|----------|---------|
| `EDDSA_PRIVATE_KEY` | All | EdDSA (Ed25519) private key for signing auto-update artifacts |
| `GH_PAGES_DEPLOY_KEY` | All | SSH deploy key for pushing to gh-pages branch |
| `AAX_SDK_URL` | All | URL to download AAX SDK (if hosted privately) |

---

## Secrets Sync

The `/pulp:ci secrets` command synchronizes secrets from a local `.env` file to GitHub Actions repository secrets.

### Mechanism

```bash
# For each KEY=VALUE pair in .env:
echo -n "$VALUE" | gh secret set "$KEY"
```

**Critical:** The value is piped via `echo -n` to `gh secret set`, which reads from stdin. The `--body` flag is explicitly NOT used because it would expose the secret value in the process argument list (visible via `ps` on shared CI runners or multi-user machines).

### Usage

```
/pulp:ci secrets              # Sync all secrets from .env
/pulp:ci secrets --list       # List which secrets are set (not their values)
/pulp:ci secrets --verify     # Verify all required secrets are present
/pulp:ci secrets --platform macos  # Sync only macOS-related secrets
```

### .env File Format

```env
# macOS Signing
APPLE_ID=developer@example.com
APP_SPECIFIC_PASSWORD=xxxx-xxxx-xxxx-xxxx
TEAM_ID=XXXXXXXXXX
APPLE_DEVELOPER_CERT_P12_BASE64=<base64-encoded-cert>
APPLE_DEVELOPER_CERT_PASSWORD=<password>
APPLE_INSTALLER_CERT_P12_BASE64=<base64-encoded-cert>
APPLE_INSTALLER_CERT_PASSWORD=<password>

# Windows Signing (Option A: PFX)
WINDOWS_CERT_PFX_BASE64=<base64-encoded-pfx>
WINDOWS_CERT_PASSWORD=<password>

# Windows Signing (Option B: Azure Trusted Signing)
AZURE_TENANT_ID=<uuid>
AZURE_CLIENT_ID=<uuid>
AZURE_CLIENT_SECRET=<secret>
AZURE_SIGNING_ACCOUNT=<account-name>
AZURE_SIGNING_PROFILE=<profile-name>
AZURE_SIGNING_ENDPOINT=https://<region>.codesigning.azure.net

# Auto-Updates
EDDSA_PRIVATE_KEY=<ed25519-private-key>
```

The `.env` file MUST be listed in `.gitignore` and MUST NOT be committed to the repository.

---

## Release Pipeline

The full release pipeline is orchestrated by `/pulp:ship` or `/pulp:ci publish`. Here is the complete sequence:

### Step-by-Step Release Flow

1. **Developer initiates release**
   - Runs `/pulp:ship` or `/pulp:ci publish`
   - Claude Code prompts for version number (semver) and release notes
   - Creates a git tag `v<version>` and pushes it

2. **CI triggers on tag push**
   - `build.yml` runs: builds all plugin formats on all platforms
   - All tests and validations execute
   - Build artifacts are uploaded

3. **macOS signing and notarization**
   - Import Developer ID Application and Installer certificates into temporary keychain
   - Code sign all plugin bundles (`.vst3`, `.component`, `.clap`) and standalone `.app`
   - Build `.pkg` installer with `pkgbuild` + `productbuild`
   - Sign `.pkg` with Developer ID Installer certificate
   - Submit `.pkg` to Apple notarization service via `notarytool`
   - Wait for notarization approval (typically 2-10 minutes)
   - Staple notarization ticket to `.pkg` and all bundles

4. **Windows signing**
   - Sign all `.dll` (plugin) and `.exe` (standalone/installer) files
   - Use Authenticode PFX or Azure Trusted Signing (auto-detected based on available secrets)
   - Include RFC 3161 timestamp for long-term validity
   - Create installer (NSIS or WiX-based)
   - Sign the installer itself

5. **Linux packaging**
   - Create `.tar.gz` archive with standard directory layout
   - Include installation script (`install.sh`) that copies to standard plugin paths
   - Optionally create `.deb` package for Debian/Ubuntu

6. **EdDSA artifact signing**
   - Generate Ed25519 signatures for all distribution artifacts
   - Signature files stored alongside artifacts (`.sig` extension)
   - Public key embedded in appcast and auto-updater client

7. **GitHub Release creation**
   - Create draft GitHub Release with auto-generated changelog
   - Attach all signed artifacts: macOS `.pkg`, Windows installer, Linux `.tar.gz`
   - Attach EdDSA signature files and SHA256 checksums
   - Developer reviews draft, edits release notes if needed, then publishes

8. **gh-pages update**
   - Update download page with links to latest release artifacts
   - Update platform-specific download buttons (auto-detect visitor OS)
   - Update version badge

9. **Appcast XML update**
   - Update Sparkle appcast XML (macOS auto-updates)
   - Update WinSparkle appcast XML (Windows auto-updates)
   - Include EdDSA signatures in appcast entries
   - Include minimum OS version requirements
   - Push updated appcast files to gh-pages or configured CDN

### Rollback Procedure

If issues are discovered after release:

1. Unpublish the GitHub Release (revert to draft)
2. Remove download links from gh-pages
3. Revert appcast to previous version
4. Tag a patch release with fixes and re-run the pipeline

### Version Strategy

- Use semantic versioning: `MAJOR.MINOR.PATCH`
- Pre-release versions: `v1.0.0-beta.1`, `v1.0.0-rc.1`
- Build metadata: `v1.0.0+build.123`
- Version embedded in: plugin binary metadata, about dialog, appcast, download page

---

## Claude Code Integration Commands

### /pulp:ci

Primary CI interaction command.

| Subcommand | Description |
|------------|-------------|
| `/pulp:ci build` | Trigger build.yml on current branch |
| `/pulp:ci validate` | Trigger validate.yml on current branch |
| `/pulp:ci publish` | Tag and trigger full release pipeline |
| `/pulp:ci secrets` | Sync .env secrets to GitHub |
| `/pulp:ci secrets --list` | List configured secrets |
| `/pulp:ci secrets --verify` | Verify all required secrets are set |
| `/pulp:ci status` | Show status of recent workflow runs |
| `/pulp:ci logs` | Fetch and display logs from last failed run |

### /pulp:ship

High-level release command that orchestrates the full pipeline:

1. Verify all secrets are configured
2. Run local build and test
3. Prompt for version and release notes
4. Create git tag and push
5. Monitor CI pipeline
6. Report success or failure with links to artifacts
