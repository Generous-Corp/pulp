# Standalone author-host catalog checkpoint

Status: tested foundation, not ordinary Standalone capability parity and not
launch-ready.

This checkpoint lets an installed production broker select a DSP-only author
companion by a broker-owned, collision-resistant bundle-ID key. The companion
is installed with its exact capability sidecar using owner-private atomic
transactions; clients never provide an executable, arguments, working
directory, or tier. The installed-SDK proof launches the companion through the
production broker and verifies exact registration plus typed state read.

The checkpoint deliberately fails the companion build when a non-system
dynamic dependency remains. It does not copy an unpinned dylib into the trusted
launch path.

Launch blockers that remain required work:

1. Reconcile catalog install, update, and removal with an already-running
   broker so its broker-owned selection and pinned digests refresh safely.
2. Snapshot, sign, hash-pin, launch, update, and roll back the exact runtime
   dependency closure needed by ordinary UI-bearing author processors.
3. Remove the crash/concurrency visibility gap between retaining the old
   catalog entry and publishing its replacement.

Capability parity remains blocked beyond this catalog slice by typed writes,
trace, telemetry, UI inspection, Motion, and runtime-evaluation policy. None of
those outcomes are deprecated by this checkpoint.
