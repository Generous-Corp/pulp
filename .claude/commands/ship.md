---
name: ship
description: Sign, package, and distribute a Pulp plugin
---

Ship a Pulp plugin — sign, notarize, and package for distribution.

Available subcommands:
- `pulp ship sign --identity "Developer ID Application: ..."` — code sign the plugin
- `pulp ship package --version 1.0.0` — create installer (DMG/PKG on macOS, NSIS on Windows)
- `pulp ship check` — show current signing status

Run the appropriate subcommand based on $ARGUMENTS. If no arguments, show signing status first with `pulp ship check`.

For full CI-driven shipping (PR + validate + merge + release), use the `ci` skill instead: say "ship this" or use `/ci ship`.
