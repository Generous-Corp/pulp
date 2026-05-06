---
name: validate
description: Run plugin format validators (auval, clap-validator, pluginval)
---

Run plugin format validation on built plugins.

Run: `pulp validate` when the CLI is on PATH, or `./build/pulp validate` from a source build.

This runs available validators:
- **auval** — Audio Unit validation (macOS, if installed)
- **clap-validator** — CLAP plugin validation (if installed)
- **pluginval** — VST3 validation (if installed)

Before launching any validator, `pulp validate` runs a discovery preflight (#743). If any validator has a broken code signature (e.g. a copy of `pluginval` ripped out of its `.app` bundle, where amfid will SIGKILL the process at launch with exit 137 and zero stderr), `pulp validate` aborts with the exact path + remediation instead of letting the run die mid-validation.

If no validators are installed, run: `pulp doctor --validators` to confirm and see install commands.
If `pulp validate` aborts with "broken code signature", run: `pulp doctor --validators --fix` to clean up user-owned copies (root-owned copies print a sudo one-liner instead of auto-elevating).
