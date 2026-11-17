# Final review slice 3: deferred findings (2026-11-15)

Scope: probes, the isolated CLAP worker, Windows custody, checkpoint recovery,
graph-file reload and watch, CLI codes and `docs/reference/cli.md`. Base:
`origin/main` `6b072721d25a`. The findings below were re-read against that base
and left open, either because they need an owner decision or because the fix is
larger than a review repair. Findings fixed in the same change are not listed.

## Needs an owner decision

### Graph CLI exit status differs by subcommand for the same code
- Where: `tools/cli/cmd_graph.cpp`, `probe_graph`'s `fail` lambda returns 2 for
  every failure; `render`, `report`, `validate` and `inspect` return 1 for
  `prepare_failed`, `unresolved_node`, `InvalidDocument` and `render_failed`.
- Why deferred: changing either side changes a documented CLI exit contract that
  scripts may key on. The decision is which status a precondition failure gets.
- Proposed fix: one code-to-exit table shared by every graph subcommand, then
  move `probe` and `render` into their own translation units (`cmd_graph.cpp`
  is about 1,000 lines and is not under `hotspot_size_guard`).

### Three spellings for graph failure codes
- `GraphPatchDiagnostic` codes are PascalCase (`InvalidDocument`), CLI-only and
  save codes are snake_case (`missing_input`, `serialization_refused`), and the
  reload receipt and watch use kebab-case (`invalid-document`, `stale-base`,
  `too-large`, `unreadable`).
- Why deferred: renaming any of them breaks a published receipt schema or CLI
  JSON contract. The decision is one convention, or an explicit mapping table
  in `docs/reference/cli.md`.
- Done meanwhile: a test now requires the `dev.pulp.graph/file.reload@1`
  receipt schema enum to equal `graph_file_reload_code_name` over every
  `GraphFileReloadCode`. The CLI classifier still compares the literal
  `"stale-base"` (`tools/cli/cli_graph_reload_receipt.cpp`); calling the name
  function there needs the receipt test target to link `pulp-host`.

### Isolated recovery refusals return `SwapResult::NeedsEagerPrepare`
- Where: `SignalGraph::publish_isolated_recovery`
  (`core/host/src/signal_graph_live_swap.cpp`), documented in
  `signal_graph_runtime.hpp`.
- Why deferred: the value names an action the caller cannot take (eager prepare
  is refused for isolated graphs), but adding `SwapResult::Refused` changes a
  public enum that existing switches cover. Callers read
  `last_swap_diagnostics()` for the real reason today.

### `GraphFileWatch::acknowledge()` takes no token
- Where: `core/host/src/graph_file_reload.cpp`.
- Risk: a consumer that polls again before acknowledging an earlier change marks
  newer, unapplied contents as observed. The CLI watch polls and acknowledges on
  one thread and is not affected.
- Proposed fix: `acknowledge(const GraphFileChange&)` that records only the
  offered contents it names. This is a public API change.

## Larger than a review repair

### Worker request polling and wake latency
- Where: `tools/clap-worker/clap_worker_main.cpp` sleeps 50 microseconds between
  polls of the request ring on an ordinary-priority thread.
- Risk: the host callback needs block N-1 complete at the entry of block N
  (1.33 ms at 48 kHz and 64 frames). One late wake latches `deadline_missed`,
  which is terminal. The recovery tests pace callbacks 10 ms apart and cannot
  show this.
- Next step: measure the miss rate at the real block period under host load,
  then either give the worker thread realtime scheduling and a blocking wake
  (a `WakeSignal`-style primitive), or record the measured rate before any
  realtime qualification claim. The guide already states realtime is not
  qualified.

### Supervisor and stop paths poll at 1 kHz
- Where: `core/host/src/isolated_plugin_slot.cpp` `supervise`, `stop` and
  `cleanup` each sleep 1 ms in a loop.
- Done meanwhile: the lifecycle exchange, default cleanup and stop deadlines are
  named constants, and the stop deadline is derived from the other two with a
  `static_assert`, so raising either cannot silently make `stop()` return while
  cleanup is pending.
- Next step: block on process exit (kqueue `EVFILT_PROC`/`NOTE_EXIT`) and on a
  condition variable for cleanup state.

### Bounded file readers are duplicated
- Readers in `graph_patch.cpp`, `graph_serializer.cpp`, `graph_file_reload.cpp`
  and `cmd_graph.cpp` (probe and diff) each bound and classify file reads.
- Done meanwhile: `load_graph_patch_file` sizes its buffer from the file instead
  of zero-filling 64 MiB per call, and reports a read failure as
  `FileUnreadable`; every graph subcommand except `probe` reports an oversize
  file as `ResourceLimit`, and a missing probe file is `FileUnreadable`.
  `probe` keeps `graph_file_limit` for its 1 MiB bound.
- Still open: one `read_bounded_file(path, limit)` helper with a typed result,
  and a typed code on `GraphSerializer::LoadResult` so the CLI stops
  classifying `from_file` failures by message prefix. The 1 MiB notation bound
  is named `max_graph_notation_bytes` in `graph_patch.hpp`, and the CLI probe
  now reads it there and sizes its buffer from the file. The reload and
  notation refusal messages still spell "1 MiB".

## Accepted as is

- Isolation checks (`is_isolated_plugin_slot`) live in generic `SignalGraph`
  code, and the recovery test hook is a production global. Revisit with a
  `PluginSlot` execution-constraints query when a second constrained slot type
  appears.
- The probe attach precondition block is repeated for the audio, control and
  event probes.
- Windows custody repeats its release block in two destructors and two move
  assignments, and caps any cleanup deadline at two seconds.
- The worker and parent hash plugin and worker files by path, which does not
  bind the mapped bytes. This is crash containment, which is all the guide
  claims.
