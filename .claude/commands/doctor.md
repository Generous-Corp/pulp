---
name: doctor
description: Check environment and diagnose build issues
---

Check the development environment for missing dependencies and configuration issues.

Prepend the output with an **Environment** section that includes the canonical version line (see `.claude/commands/version.md` for the parsing recipe):

```
Environment
  Claude plugin <plugin_version> · Pulp SDK/CLI <sdk_version>
```

Then run the full doctor:

```bash
pulp doctor
```

Options:
- `pulp doctor --fix` — auto-fix issues where possible
- `pulp doctor --ci` — CI mode, exit codes only
- `pulp doctor --dry-run` — show what `--fix` would do
- `pulp doctor android` / `pulp doctor ios` — run mobile development environment checks
- `pulp doctor --versions [--scan-parents] [--json]` — diagnose CLI / SDK / plugin version skew
- `pulp doctor --validators [--fix] [--dry-run]` — verify auval / pluginval / clap-validator install and code-signature health
- `pulp doctor --caches [--fix] [--dry-run] [--json]` — audit and heal the FetchContent shared-source cache
- `pulp doctor --host-quirks` / `pulp doctor quirks` — show the runtime DAW host-quirks policy and enforced accommodations
- `pulp doctor --au-cache --dry-run` — preview macOS AudioComponentRegistrar refresh after AU metadata changes
- `pulp doctor list` / `pulp doctor --only "<name>"` — enumerate checks or run one targeted probe
- `pulp doctor --only WidgetBridge` — check generated WidgetBridge `.d.ts`, mock function lists, and JS bridge docs for source-tree input-fingerprint freshness

Default checks include C++20 compiler availability, CMake, git/git-lfs, LFS-backed Skia binaries when present, generated WidgetBridge API artifacts in source-tree mode, VST3/AudioUnit SDKs where relevant, optional AAX setup, package/platform alignment, Cmajor when used, build configuration, and pulp-mcp availability. Mobile subcommands add Android SDK/NDK/adb/emulator and iOS/Xcode/Simulator checks.

Run this first when builds fail unexpectedly or on a new machine. Run `pulp doctor --validators` if `pulp validate` aborts with "broken code signature", `pulp doctor --caches` if build/test reports FetchContent cache drift, and `pulp doctor --host-quirks` when DAW accommodations or host-specific runtime behavior look suspicious.
