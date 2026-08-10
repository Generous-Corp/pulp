# Standalone author-host catalog status

Status: author-host catalog launch blockers closed; ordinary Standalone
capability parity remains incomplete.

The catalog lets an installed production broker select an ordinary author
companion by a broker-owned, collision-resistant bundle-ID key. The companion
links the author's processor and Standalone/UI closure and is installed with
its exact capability sidecar and runtime dylibs using owner-private immutable
versions plus one atomic `active` selection. Clients never provide an
executable, arguments, working directory, or tier.

The production broker continuously reconciles install, removal, and update
selection changes. Before launch it derives and pins the executable, sidecar,
each runtime dependency digest, and each static signing identity, then copies
that entire closure into its immutable trusted-host snapshot. Catalog version
names are the SHA-256 of the closure manifest and are checked before selection.
An interrupted update leaves the prior `active` selection visible; removal is
an atomic marker rename and retains immutable versions for safe rollback.

Focused release proof covers:

- successful and synthetically interrupted install/update plus atomic removal;
- installed author product and `DESTDIR` layouts, including the staged GPU
  runtime dylib and closure manifest;
- production broker launch, registration, catalog removal/re-add reconciliation,
  and relaunch without accepting a client path; and
- typed author-host state read through the installed SDK.

Capability parity remains blocked beyond this catalog slice by typed writes,
trace, telemetry, UI inspection, Motion, and runtime-evaluation policy. None of
those outcomes are deprecated by this work, and this status does not claim
parity.
