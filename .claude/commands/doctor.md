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
- `pulp doctor --dry-run` — show what --fix would do
- `pulp doctor --validators` — verify auval / pluginval / clap-validator are installed and have intact code signatures (#743)
- `pulp doctor --validators --fix` — also remove broken user-owned validator copies; root-owned breakage prints a sudo one-liner

Checks include: CMake version, C++ compiler, git-lfs, Skia binaries, VST3 SDK, AudioUnit SDK, platform-specific dependencies (ALSA on Linux, Xcode CLI tools on macOS).

Run this first when builds fail unexpectedly or on a new machine. Run `pulp doctor --validators` if `pulp validate` aborts with "broken code signature" — the doctor will surface the exact path and remediation.
