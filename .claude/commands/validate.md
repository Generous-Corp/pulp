---
name: validate
description: Run plugin format validators (auval, clap-validator, pluginval)
---

Run plugin format validation on built plugins.

Run: `./build/tools/cli/pulp validate`

This runs available validators:
- **auval** — Audio Unit validation (macOS, if installed)
- **clap-validator** — CLAP plugin validation (if installed)
- **pluginval** — VST3 validation (if installed)

If no validators are installed, suggest: `pulp doctor` to check the environment.
