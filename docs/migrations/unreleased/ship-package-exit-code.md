---
breaking = false
summary = "pulp ship package on macOS now exits 1 when an installer fails or nothing was packaged"
---

## What changed

On macOS, `pulp ship package` used to exit 0 even when it built nothing.
It printed `FAILED to create combined installer` (or `FAILED` for a
per-format `.pkg` or `.dmg`) and `Created 0 .pkg and 0 .dmg artifacts`, and
returned success. It now exits 1 in two cases:

- any installer failed to build. Each failure still prints its `FAILED`
  line, followed by `Error: N installer(s) failed to build`.
- nothing was packaged, because no plugin bundles or standalone apps were
  found under `build/`. The message is `Error: nothing to package`.

A run that builds every installer it attempts still exits 0. The Linux
packager already exited 1 when it found no plugins; macOS now matches it.

`pulp ship release` stops at its package stage in these cases instead of
going on to notarize. Before, it reached notarization with nothing to submit.

## Who needs to act

Only scripts that call `pulp ship package` and expect exit 0 from a run
that produced no installer. Those scripts were treating a failure as
success. Check that `build/` holds the bundles you mean to ship, or that
the installer identity exists in the signing keychain (`pulp ship doctor`),
before calling `package`.

`pulp ship` also now works in an SDK consumer project (`pulp.toml`, no
`core/`). It used to stop there with `not in a Pulp project directory`. No
action is needed for that.
