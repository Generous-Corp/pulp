# Reusable Apple Dev-Signing Stub

A repeatable, agent-friendly way to stand up an iOS or macOS signing
path for **test builds** without copy-pasting recipes or hard-coding
team IDs into committed code. Pairs with
[`templates/secrets/notary.env.example`](../../templates/secrets/notary.env.example)
and [`tools/scripts/source_dev_creds.sh`](../../tools/scripts/source_dev_creds.sh).

## What this is for

- **Test builds** for plug-ins and host apps.
- **Development on real iOS devices** (iPad / iPhone) for AUv3 walkthroughs.
- **Pre-AppStore validation** — codesign + notarize a macOS build before
  the real submission flow.

What this is **not** for: App Store release builds. Those need
explicit per-app App IDs, real entitlements, an App Store Connect
distribution profile, and the full submission flow described under
[What MUST change for App Store release](#what-must-change-for-app-store-release).

## One-time Apple setup

These steps run once per Apple Developer account, not once per project.

1. **Register a WILDCARD App ID** at
   <https://developer.apple.com/account/resources/identifiers/list>.
   - Description: `Pulp Dev Wildcard`
   - Bundle ID: `Wildcard`, e.g. `com.<your-handle>.pulpdev.*`
   - All capabilities OFF. Wildcard App IDs cannot use App Groups,
     Push, Sign in with Apple, etc — and **none** of those are needed
     for a local test plug-in. Skip them.

2. **Generate an App Store Connect API key** at
   <https://appstoreconnect.apple.com/access/integrations/api>.
   - Role: `Developer` is sufficient for notarization. (`Admin` if
     you also use the same key for ASC uploads.)
   - Download the `.p8` once (it cannot be re-downloaded) and save it
     to `~/.config/pulp/secrets/AuthKey_<KEY_ID>.p8`.
   - Record the Key ID (10 chars) and Issuer ID (UUID at the top of
     the Keys page) — you'll paste them into `notary.env` below.
   - This single API key works for both macOS `xcrun notarytool` and
     Linux `rcodesign`.

3. **Skip explicit per-plug-in App IDs entirely** for test builds.
   The wildcard above covers every plug-in you ever scaffold under
   `com.<your-handle>.pulpdev.*`.

## One-time machine setup

```bash
mkdir -p ~/.config/pulp/secrets
cp templates/secrets/notary.env.example ~/.config/pulp/secrets/notary.env
chmod 600 ~/.config/pulp/secrets/notary.env
$EDITOR ~/.config/pulp/secrets/notary.env   # fill in your real values
```

Then verify the helper picks them up cleanly:

```bash
. tools/scripts/source_dev_creds.sh   # exit 0 silently on success
echo "$PULP_TEAM_ID"                   # should print your 10-char team ID
```

If a required key is missing or still contains a `<...>` placeholder,
the helper exits non-zero with a clear list of what to fix.

## Per-plug-in setup

Bundle IDs in your `CMakeLists.txt` must live under your wildcard
prefix. The Apple **parent-child rule** for app extensions
(`pkd` enforces it) requires the extension's bundle ID to be a
strict child of the host app's bundle ID — no exceptions.

For a hypothetical `MyKnob` plug-in:

| Target            | Bundle ID                                        |
|-------------------|--------------------------------------------------|
| HostApp           | `com.<your-handle>.pulpdev.myknob.host`          |
| AUv3 .appex       | `com.<your-handle>.pulpdev.myknob.host.MyKnob`   |

Both fall under the `com.<your-handle>.pulpdev.*` wildcard, so a
single App ID registration covers every plug-in you build going
forward. No App Store Connect roundtrip per plug-in.

## Build invocations

### Simulator (no signing)

```bash
cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_TESTS=OFF
cmake --build build-ios-sim --target MyKnob_HostApp --config Release -- -sdk iphonesimulator
```

### Physical iOS device (signed, automatic profile)

```bash
. tools/scripts/source_dev_creds.sh
cmake -S . -B build-ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${PULP_TEAM_ID}" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE="Automatic"
cmake --build build-ios-device --target MyKnob_HostApp --config Release -- -sdk iphoneos
```

### Notarized macOS build (Developer ID, distributable)

```bash
. tools/scripts/source_dev_creds.sh
pulp build
pulp ship sign --identity "${PULP_SIGN_IDENTITY}"
pulp ship notarize                # picks ASC key fields up from sourced env
pulp ship package --version 0.1.0
```

`pulp ship notarize` reads `PULP_NOTARY_KEY_PATH`, `PULP_NOTARY_KEY_ID`,
and `PULP_NOTARY_ISSUER_ID` from the environment, so sourcing the
helper first is all the wiring needed. See the [`ship` skill](../../.agents/skills/ship/SKILL.md)
for the full notarization precedence and the legacy Apple-ID lane.

## What MUST change for App Store release

The stub above is intentionally *not* an App Store release flow. For
an actual ASC submission:

1. **Explicit App ID per app** — required if you want App Groups, Push,
   Sign in with Apple, or any capability the wildcard can't host.
   Register at developer.apple.com under `Identifiers → App IDs →
   App` (not Wildcard).
2. **Apple Distribution cert + App Store provisioning profile** —
   not the `Apple Development` cert. Generate via Xcode → Settings →
   Accounts → Manage Certificates, or fastlane match.
3. **Real entitlements file** matching the explicit App ID's
   capabilities. `Entitlements.plist` in `templates/ios-auv3/` is the
   minimal shape; capabilities you enabled on the App ID must appear
   here.
4. **Versioning + ASC submission** via `xcrun altool --upload-app`
   or `xcrun notarytool submit --wait` followed by the App Store
   Connect web flow for store metadata.

Full release flow: [`docs/guides/shipping.md`](shipping.md).

## Why we use this stub pattern

- **Avoids registering N App IDs for N test plug-ins.** One wildcard
  covers every plug-in under your namespace.
- **Avoids hard-coding personal Team IDs into committed code or docs.**
  All identifying data lives in your per-user `notary.env`.
- **Lets multiple developers share one Pulp checkout** — each
  developer sources their own `notary.env`; the same repo builds
  signed plug-ins for any team.
- **Standard layout** means any agent or contributor can build a
  signed test plug-in by reading this guide once, without
  reverse-engineering a custom recipe.

## Related

- [`.agents/skills/ship/SKILL.md`](../../.agents/skills/ship/SKILL.md) —
  full sign / notarize / package reference.
- [`.agents/skills/ios/SKILL.md`](../../.agents/skills/ios/SKILL.md) —
  iOS platform and AUv3 host-app build details.
- [`.agents/skills/auv3/SKILL.md`](../../.agents/skills/auv3/SKILL.md) —
  AUv3 adapter and macOS notarization recipe.
- [`templates/secrets/notary.env.example`](../../templates/secrets/notary.env.example) —
  schema-only template (committed). Copy + fill in.
- [`tools/scripts/source_dev_creds.sh`](../../tools/scripts/source_dev_creds.sh) —
  sourceable helper that loads + validates the creds file.
