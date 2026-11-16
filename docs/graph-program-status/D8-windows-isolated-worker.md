# D8: Windows isolated CLAP worker

Status: the slot and worker port is built; the owner-run Windows receipt below
was taken on the Windows Server VM. Windows realtime pacing is not qualified.

## What is built

- Section-value ownership: `SharedSection::adopt` maps into a local first,
  never closes a value that does not map, and closes the duplicated value right
  after a successful map (the view keeps the section alive), so the value is
  closed exactly once. The parent never closes it remotely; a failed hello is
  followed by terminating the job.
- Shared hello checks: `hello_header_ok` and `hello_identity_matches` sit beside
  `hello_reply_matches` in `isolated_clap_protocol.hpp`; both workers use them.
- Windows x64 launch in `isolated_plugin_slot.cpp`: section created before
  launch, retained job-object custody with the private duplex channel,
  `parent_hello` binding generation, duplicated value, hashes, build and worker
  process id, then the exact load report. Lifecycle, supervisor and cleanup are
  the shared session code. Blocking channel work and cleanup stay on control
  threads; the callback runs the shared `CallbackEndpoint` only.
- Windows worker transport in `clap_worker_main.cpp`: section adoption, module
  self-hash, `PeekNamedPipe` polling (a closed pipe ends the worker), and a
  high-resolution waitable timer between blocks. The load, lifecycle and block
  loop are shared with POSIX, which keeps its checks and exit codes.
- `pulp-test-isolated-clap-worker-windows`, the fixture as a plain `.clap`
  module, and Windows import-table callback traps with their own negative
  control.

## Receipt (Windows Server VM)

Host: Windows Server 2022 (NT 10.0.20348), 4 logical processors, MSVC 19.51,
Ninja, Release, examples/GPU/WebView off. The VM tree was compared with the
branch tree blob by blob (18054 tracked files identical, planning excluded).

| Check | Result |
|---|---|
| `[retained-custody]` | 11 cases, 137 assertions passed |
| `pulp-test-isolated-clap-protocol` (8 `[windows]` cases) | 13 cases, 1288 assertions passed |
| `pulp-test-isolated-clap-worker-windows` | 10 cases, 10529 assertions passed |
| Real fixture processed | worker image matches the production worker; descriptor `dev.pulp.g9.lifecycle.healthy`; worker and plugin SHA-256 equal the files' hashes; 33 process witnesses, all from the worker pid |
| One-block latency | reported 64, measured 64 by impulse, gain 0.5 |
| Sibling continuity | 2048 sibling samples bit-exact through a worker kill; isolated path silent after the kill; worst post-kill callback 18.5 us |
| Callback traps | 0 operator-new allocations and 0 trapped heap, lock, wait, pipe I/O, handle or process calls across every probed callback; the trap negative control sees each kind |
| Stalled worker | worst callback 1.3 us against a 1333 us block period |
| Malformed and partial completions | partial error, NaN, never-return and process abort latch silence with their reasons |
| Identity | plugin hash, worker hash, missing worker, unknown descriptor and mismatched build all refuse; the mismatched build loads no vendor code |
| Timeouts | load hang, prepare hang (lifecycle_timeout) and destroy hang bounded under 8 s |
| Failed state save | oversize, ignored error, abort and hang keep no checkpoint; a saving plugin recovers into a new generation |
| Descendant cleanup | cmd child and ping grandchild gone after cleanup, with a hanging and with an exited leader |
| Paced run (`[.paced]`, hidden) | 374 and 742 of 4000 blocks before the first deadline miss in two runs; not a gate |

Break-and-restore controls on the VM (each rebuilt the edited source, named in
the build log, failed when broken and passed when restored with the original
SHA-256): adoption closing an unmapped value, adoption keeping the mapped value,
the slot skipping the load report, and the worker skipping its identity check.
On macOS, `confirm_failure.sh` confirmed the shared header check and the worker
identity check.

## Remainder

- Narrow the inheritable window of the duplex child end in
  `child_process_win.cpp` (create it non-inheritable and mark it inheritable only
  for `CreateProcess`), or state the limit in `docs/reference/runtime.md`.
- Windows realtime pacing: the worker keeps pace for hundreds of blocks on the
  4-vCPU VM, not thousands. Qualifying it needs a quiet Windows host and probably
  worker thread priority or MMCSS registration; it is a host-time item.
- A scheduled Windows lane for these suites (an owner decision; see
  `final-review-2-followups.md`).
- The graph-node recovery and recovery-race cases, the finite registry cases,
  the installed SDK consumer and `run_mutations.py` run on macOS only.
