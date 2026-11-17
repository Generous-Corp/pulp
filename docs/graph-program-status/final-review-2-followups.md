# Final review 2: deferred findings (2026-11-16)

Scope: the adversarial and code-health re-reviews of the merged graph program
on `origin/main` (MIDI mailbox counters, Windows retained custody, the reset
contract and seek reset, the paced-timing selftest control, and saved identity).
Each finding below was re-read against the code and left open, either because it
needs an owner decision or because the fix is larger than a review repair.
Findings fixed in the same change are not listed. Saved-identity findings in
`graph_serializer.*`, `spectral_graph_persistence.*` and the `cmd_graph.cpp`
save path belong to the open saved-identity change and are not repeated here.

## Needs an owner decision

### The Windows transport has no CI lane
- Where: the `_WIN32` cases in `test/test_runtime_utils.cpp`
  (`[retained-custody]`), `test/test_isolated_clap_protocol.cpp`
  (`[windows]`) and the whole of `test/test_isolated_clap_worker_windows.cpp`.
- Why deferred: the PR-head `windows` check is an alias, and the Windows leg of
  `build.yml` runs only on an explicit dispatch with an opted-in runner. No
  scheduled Windows lane exists to register the cases on. The decision is
  whether to add a scheduled Windows lane (local VM runner or hosted).
- Done meanwhile: `docs/guides/isolated-clap-worker.md` states that these cases
  are owner-run on the Windows Server VM and gives the exact commands.

## Larger than a review repair

### Mailbox counter structs are copied field for field
- Where: `MidiMailboxCounters` and `ParameterMailboxCounters` in
  `signal_graph_runtime.hpp`, and the matching private atomics in the two input
  mailboxes. The shared templates in `signal_graph_mailbox.cpp` work only because
  the member names match.
- Why deferred: the fix changes a public header (one `MailboxCounters` with two
  aliases and an embedded telemetry member), which needs a capability
  fingerprint refresh and a consumer review of its own.

### The private channel is inheritable only for the launch call
- Where: `duplex.child` in `duplex_channel_win.hpp`. It is created
  non-inheritable and `make_child_inheritable()` runs immediately before the
  `CreateProcess` in `child_process_win.cpp`; `close_child()` follows the call.
- Remaining exposure: a `CreateProcess` with `bInheritHandles=TRUE` made outside
  `ChildProcess` on another thread during that one call still inherits it. The
  handle-list contract requires the handle to be inheritable then, so this
  cannot be removed, only kept to the call. No deterministic test can race a
  foreign launch; the flag states are pinned by `[duplex-inheritance]`.

### `atomic_signal_fence` is described as a compiler barrier
- Where: `isolated_clap_endpoint.hpp`. It works with current Clang, but the
  standard does not promise the private copy is materialised.
- Next step: copy through a `volatile` or an explicit `std::memcpy` into a local
  and say why.

### Worker exit codes and the Windows cleanup deadline are literals
- Where: exit codes 64 to 86 in `tools/clap-worker/clap_worker_main.cpp` (its
  control deadline is now the named `kControlDeadline`), and the `seconds(2)`
  cleanup deadline in the Windows `start_impl`.

### Test names keep phase prefixes
- Where: the "G5"/"G6" prefixes in `test_voice_bank_binding.cpp` and
  `test_g5_host_event_contract.cpp`, `[p4]` tags in the GPU audio trace tests,
  and `[g12]` in `g5_graph_event_rt_cases.inc`.
- Next step: one rename sweep with the test-name ledger updated in the same
  change.

### Repeated close sequence in the Windows launch path
- Where: `start_impl` in `child_process_win.cpp` repeats the error-path close of
  the standard handles three times. A scope guard would hold it once.
