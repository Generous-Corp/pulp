---
name: create
description: Scaffold a new Pulp plugin or app project
---

Scaffold a new Pulp plugin project. $ARGUMENTS should be the plugin name.

Run: `./build/tools/cli/pulp create $ARGUMENTS`

If no arguments provided, ask the user for:
1. **Name** — the plugin name (e.g., "My Synth")
2. **Type** — effect or instrument (default: effect)
3. **Formats** — which formats to target (default: vst3,au,clap)
4. **MPE** — for instruments, offer `--mpe` to opt into MPE (per-note expression).
   When set, the generated descriptor declares `supports_mpe = true` and the
   header includes `<pulp/midi/mpe_buffer.hpp>`. See `docs/guides/mpe.md`.

Then run: `./build/tools/cli/pulp create "<name>" --type <type> --formats <formats> [--mpe]`

After scaffolding, build the new project and run its tests to verify everything works.
