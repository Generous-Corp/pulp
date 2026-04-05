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

Then run: `./build/tools/cli/pulp create "<name>" --type <type> --formats <formats>`

After scaffolding, build the new project and run its tests to verify everything works.
