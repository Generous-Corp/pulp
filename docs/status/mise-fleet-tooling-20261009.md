# mise fleet-tooling status

Status: **task-only local profile implemented; host-tool locking and CI/release
adoption are gated on registry/replay and platform-canary evidence**.

The first slice addresses a proven developer failure: the configured Homebrew
Python 3.14 interpreter lacked `numpy`, `Pillow`, and `scikit-image`, causing
`visual-python-deps-present` to fail closed. The local mise task consumes the
same hash-pinned requirements lock already used by CI. It does not replace the
CI installer or make the network a required runtime dependency.

Replay evidence reviewed:

- The visual dependency absence was a real agent-session blocker. A local mise
  task would have selected a known Python and installed the existing lock, but
  only the lock-aware installer fixes the CI case.
- A PyPI egress `403` on an ephemeral VM was a real CI failure. mise cannot fix
  network reachability; the existing wheelhouse and retry path remain required.
- Android Studio JBR 25 failed against the Android build while JDK 17 worked.
  mise may select JDK 17 for a developer VM, but SDK/NDK, Gradle-wrapper, and
  Skia artifact identity stay outside mise.
- Existing Node runtime discovery already understands mise-managed Node roots.
- Stale build directories and governed-build failures are Pulp CLI/governor
  concerns and are not assigned to mise.

Next gates are a macOS arm64, Linux VM, and Windows canary using a generated
cross-platform lock, with a tool manifest and image digest. CI/release may consume mise only
after parity is demonstrated; those jobs must set `MISE_AUTO_INSTALL=false`,
avoid per-tool automatic updates, and install from the lock. Updates should be
weekly `mise outdated` reports followed by reviewed lockfile changes, with a
minimum release age and rollback to native provisioning on failure.

The live replay exposed an additional fail-closed condition when testing host
tool pins: `mise lock` could not resolve while `mise-versions.jdx.dev` timed out
and the Python backend could not complete its GitHub transport. The task-only
profile therefore does not pretend to provide locked host tools. A generated
`mise.lock` is mandatory before a fleet or CI rollout; a floating profile would
make bootstrap depend on the registry at task-start time.

The sampled GitHub runs are retained as classification evidence:

- [Build and Test run 38001060380](https://github.com/Generous-Corp/pulp/actions/runs/38001060380)
  had no visual dependency failure in the failed log excerpt.
- [Build and Test run 37986212901](https://github.com/Generous-Corp/pulp/actions/runs/37986212901)
  failed in a Windows test regression, after dependency provisioning passed.
- [Build and Test run 37953784905](https://github.com/Generous-Corp/pulp/actions/runs/37953784905)
  failed in affected-test assertions, not missing Python packages.

These runs show where mise would not have helped; the local visual-dependency
incident remains the positive replay case.
