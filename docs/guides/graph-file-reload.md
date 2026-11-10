# Transactional graph-file reload

The delegated C++ `pulp dev` and `pulp loop` watch loops can apply a changed `.pat`
or `.pulpgraph` file to an enrolled standalone product without rebuilding it.
The product must expose its existing `SignalGraphControlAuthority` through
`ProcessorSignalGraphBinding` and keep its `GraphPatchCatalog` alive after
`binding.authority().set_graph_file_catalog(catalog)`. A default catalog supports
only its existing built-in graph types. Registered Processors need the product's
registry; the CLI cannot manufacture product implementations.

Obtain a develop grant covering file reload and session renewal, and acquire the
session control lease using the existing
`pulp control` consent flow. Supply the exact instance, grant and current graph
authority generation:

```sh
pulp dev --graph-file live.pat --graph-instance INSTANCE --graph-grant GRANT --graph-generation 0
pulp loop --graph-file live.pulpgraph --graph-instance INSTANCE --graph-grant GRANT --graph-generation 0
```

The graph authority starts at generation zero; successful modulation edits or
file replacements advance it. Use the generation in the previous successful
operation receipt. A stale generation refuses replacement and requires restarting
the watch with the current receipt's generation. `--run` alone does not enroll a
child or grant control permission. These options require the C++ delegate; the
Rust fallback does not implement graph-file reload.

Each 500 ms poll reads bounded bytes. Two consecutive identical reads coalesce a
save; content changes are detected even with unchanged size and modification
time. An unchanged document is attempted once. Correcting a rejected document
with different contents retries it. `--graph-watch-polls N` exits after 1..120
polls for bounded automation and terminates and joins its own `--run` child process
group before returning. Source changes continue through the normal governed
build/test/relaunch path; graph file changes use the control request path.
SIGINT and SIGTERM request normal watch shutdown: the watch joins its owned
child group and returns 128 plus the signal number. Signal handlers only set a
stop flag; process cleanup runs outside the handler.
Watch rebuild, test and validation shellouts use the existing `ChildProcess`
ownership and cancellation path, so an active shellout is joined before watch
shutdown finishes. Their output streams and environment remain available to the
invoked commands.

The operation `dev.pulp.graph/file.reload@1` uses the shared strict notation or
saved-graph parser, validates every original edge, checks the catalog's state
representation, builds an off-side candidate with named typed mutations, prepares
it, and publishes with `PreparedTopologyEdit::commit`. The next audio block uses
the new snapshot. Preparation and failure leave the previous live graph intact.
The broker retains its 512 KiB serialized request bound, including JSON escaping;
local file reads and the public transaction input are separately bounded at
1 MiB for both formats. Oversize public input is refused before a candidate edit
is constructed; oversize broker requests are refused before submission. Admission, grants, leases, deadlines and
main-thread fencing remain enforced by the existing control client and host.
The watch renews the existing controller lease every four polls using the supplied
grant; it never acquires a lease automatically. If initial building outlasts the
lease, reacquire it through the existing control flow. Refused renewal retains
the live graph and reports the missing authority.

Malformed ports/kinds, cycles, invalid parameter controls, unsupported contracts,
failed prepare, cancellation and stale candidates retain old state/audio.
Failures carry the same `GraphPatchDiagnostic` as the public API. The saved
legacy loader remains tolerant; file reload deliberately uses strict loading.

This is the existing bounded notation profile. Opaque plugin state, external
PluginSlots, unknown/custom state contracts, richer channel layouts and other
states which the catalog cannot represent are refused explicitly. Authored
parameters replace prior values; unsaved DSP history is not migrated into fresh
Processor instances. Pinned old execution snapshots retain their old Processor
lifetimes until control-thread retirement. Readding a baseline instance is refused
before creating a second lifecycle token, including instances in older retained
generations or another graph. Reservations last through completion of release.
Stateless Custom reload can reuse an existing registration when all callbacks are
empty or identical raw function pointers and the existing compatibility check
accepts its metadata. Same-key changed callbacks, shapes and opaque captured
callbacks refuse explicitly, including captured callbacks from the same catalog.
This profile does not qualify arbitrary
node/state contracts or constitute full G2 completion.

The public consumer under `examples/graph-file-reload-consumer` uses only installed
headers: a real file save stages a 0.25 gain, preserves old output 0.125 during
prepare, publishes 0.75 gain/output 0.375, and verifies malformed replacement keeps
the new state/audio. Focused controls also retain old snapshots, exercise failed
prepare and stale publication, and compare executor/public diagnostics.

The broker product test drives the actual standalone audio callback with a constant
0.5 input. A test-owned control-thread observer records the last rendered sample
and canonical graph state. It checks 0.125 before replacement, 0.375 after
publication, and unchanged audio/state through malformed/stale documents, expired
leases, restricted renewal and owner revocation during a running watch. The
observer adds no production control operation or audio-thread I/O.
