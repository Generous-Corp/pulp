---
name: create
description: Scaffold a new Pulp plugin or app project
---

Scaffold a new Pulp plugin project. $ARGUMENTS should be the plugin name.

Run: `pulp create $ARGUMENTS` when the CLI is on PATH, or `./build/pulp create $ARGUMENTS` from a source build.

`--template <name-or-kit-dir>` can use a built-in template name or an explicit local template kit. Template kits are useful for reusable plugin starters, but they are not curated dependency packages: inspect unfamiliar kits first, and do not use `pulp add` for arbitrary local or external kit sources. Local template kits build only the format entry templates they export, so small starters can honestly target CLAP/Standalone without claiming every desktop format.

If no arguments provided, ask the user for:
1. **Name** — the plugin name (e.g., "My Synth")
2. **Type** — effect or instrument (default: effect)
3. **MPE** — for instruments, offer `--mpe` to opt into MPE (per-note expression).
   When set, the generated descriptor declares `supports_mpe = true` and the
   header includes `<pulp/midi/mpe_buffer.hpp>`. See `docs/guides/mpe.md`.
4. **Android scaffold** — if requested, add `--targets android`.

Then run: `pulp create "<name>" --type <type> [--mpe] [--targets android]` or `./build/pulp create ...` from a source build.

After scaffolding, build the new project and run its tests to verify everything works.
