---
name: minos
description: Minimum-OS tooling — measure one binary's OS floor, or sweep every downstream consumer against an installed SDK
---

Minimum-OS tooling. The "minimum OS" a build needs is the **highest** minimum among everything linked into it — the SDK's own libraries (Skia, Dawn, the JS engine), the C++ runtime, and the plugin's own compiled code. These commands read that floor straight from the built artifact, so it's the honest number, not a guess from a manifest.

Full explainer: `docs/guides/minimum-os-support.md`.

## Measure one binary

Report the minimum OS a single built binary needs:

```bash
pulp minos measure <path-to-binary>
```

`<path-to-binary>` can be a `.dylib` / `.so` / `.dll`, a `.a` static archive, an executable, or a plugin bundle's inner binary (e.g. `MyPlugin.vst3/Contents/MacOS/MyPlugin`). Output is `<kind> <floor>`, e.g. `macho 13.3` (macOS 13.3), `elf 2.34` (glibc 2.34), `pe 10.0` (Windows 10).

If $ARGUMENTS names a binary, measure it:

```bash
pulp minos measure $ARGUMENTS
```

## Sweep every downstream consumer

Rebuild every downstream demo/example against one installed Pulp SDK and report each project's floor next to the SDK's floor — a `DRIFT` flag means a consumer's binaries ended up needing a higher OS than the SDK declares (usually a build host leaking a newer toolchain). Non-zero exit on any build failure or drift, so it can gate a release.

Dry-run first to see the plan (no clones, no builds):

```bash
pulp minos sweep --sdk-prefix /path/to/installed-sdk --dry-run
```

Then run it (optionally scope with `--only`, capture with `--json`):

```bash
pulp minos sweep --sdk-prefix /path/to/installed-sdk
pulp minos sweep --sdk-prefix /path/to/installed-sdk --only pulp-example-plugins,pulp-gpu-nam
pulp minos sweep --sdk-prefix /path/to/installed-sdk --json /tmp/sweep.json
```

The SDK prefix is an unpacked installed SDK (the directory containing `lib/cmake/Pulp`). Build a fresh one with `cmake --install <build> --prefix <prefix>`.

Notes:

- The registry of what to sweep is `planning/sdk-consumers/consumers.yaml` (private submodule); per-repo build knobs are in `tools/scripts/sdk_consumer_sweep_recipes.yaml`. README/PKG-only release mirrors are skipped automatically.
- `pulp minos sweep` needs PyYAML: `python3 -m pip install pyyaml`.
- The sweep clones and builds many repositories, so it's a CLI-only command (not exposed over MCP). The `pulp minos measure` op is available over MCP as `pulp_minos`.

## Batch-update consumers to a new SDK

When a new SDK ships, bump every downstream consumer's pinned SDK version in one pass. **Dry-run by default** — it prints the per-repo pin changes and writes nothing:

```bash
pulp minos update --to 0.640.0
```

Add `--open-prs` to actually clone each repo, apply the edit on a branch, commit, push, and open a PR:

```bash
pulp minos update --to 0.640.0                       # dry-run plan
pulp minos update --to 0.640.0 --only pulp-gpu-nam   # scope to one repo
pulp minos update --to 0.640.0 --open-prs            # open the PRs
```

It rewrites the common pin forms (`pulp.toml` `sdk_version`, `find_package(Pulp X.Y.Z)`, FetchContent `GIT_TAG`) and leaves a floating `sdk_version = "latest"` alone.

## Republish runbook

Print the exact rebuild + package + publish steps for the packaged demos (prints only — never builds, signs, or touches a release):

```bash
pulp minos publish-runbook --to 0.640.0
```

Full auto-publish is intentionally not automated: each packaged demo signs with its own identity and cuts its own release, and publishing mutates public releases — so the runbook is the reviewable hand-off to that per-repo step.
