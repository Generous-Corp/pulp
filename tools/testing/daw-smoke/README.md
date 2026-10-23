# DAW smoke (REAPER) — functional reload/editor verification

A real-DAW functional smoke for Pulp **reload / editor / format-adapter** changes.
It loads a Pulp plugin in REAPER, drives a hot-swap, and asserts the reload was
**accepted + applied** by scraping the plugin's reload log.

**This complements — never replaces — `auval` / `pluginval` / `clap-validator`.**
Those prove a plugin scans + loads. This proves a *functional* behavior actually
happens inside a host. Some bugs only appear in a host because the format adapter
injects state no headless path has (e.g. a synthesized bypass parameter that made
the reload contract gate reject every in-DAW reload — fixed 2026-07-04). See
[`docs/guides/daw-smoke.md`](../../../docs/guides/daw-smoke.md) for the full rules.

## When to run it (opt-in, NOT every build)
Default OFF. Enable per-machine in `~/.config/pulp/daw-smoke.toml`. Run it for
initial functional validation of reload/editor behavior (before asking a human to
test) and for risky reload/editor/format-adapter changes. Not trivial edits.

## Modes

### `reload` (default) — hot-swap a watched DSP artifact
```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --mode reload \
  --plugin-name "Pulp Hot-Reload Morph" \
  --format vst3 \
  --plugin-path "build/VST3/Pulp Hot-Reload Morph.vst3" \
  --watched-logic ~/.pulp/hot-reload-morph/logic.dylib \
  --initial-logic build/examples/hot-reload-morph/logic.dylib \
  --swap-logic    build/examples/hot-reload-morph/logic-harsh.dylib
```

### `live-plugin-swap` — swap a hosted plugin INSTANCE in a SignalGraph
Loads a Pulp host that hosts another plugin in a `SignalGraph`, then writes a
swap request to the path the host watches and asserts the swap **committed with
no dropout** by scraping the host's `[live-swap] committed` marker (a refusal
logs `live plugin swap refused` → FAIL).
```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --mode live-plugin-swap \
  --plugin-name "Pulp Graph Host" \
  --format vst3 \
  --plugin-path "build/VST3/Pulp Graph Host.vst3" \
  --watched-swap-request ~/.pulp/graph-host/swap-request.txt \
  --swap-target "com.example.replacement"
```
The CI-runnable, DAW-free mirror of this mode is the headless continuity test
`test/test_signal_graph_live_swap_continuity.cpp`, which drives the same
stage + `prepare_swap` commit through `process()` and asserts sample continuity
across the swap block for every hosted format (VST3/AU/CLAP/LV2). This REAPER
mode is the local-only in-host proof.

### `sequence-loop-seek` — loop + seek the transport under an embedded sequence
Loads a Pulp plugin that embeds a sequence, sets a loop region on the REAPER
timeline, starts playback, and performs a scripted series of seeks (into/out of
the loop and across the loop wrap). The plugin emits a per-block transport marker
carrying BOTH the host playhead and its own sequence read position; the harness
asserts the sequence **followed the host** across every wrap/seek with no
dropout. A free-running counter that ignores the host jump is caught as position
**drift** → FAIL.
```bash
python3 tools/testing/daw-smoke/reaper_smoke.py \
  --mode sequence-loop-seek \
  --plugin-name "Pulp Sequence" \
  --format vst3 \
  --plugin-path "build/VST3/Pulp Sequence.vst3" \
  --loop-start 1.0 --loop-end 3.0
```
The plugin must emit these markers to stdout (the scraper's contract):
```
[seq-loop] loaded events=<int> len_qn=<float>
[seq-loop] play
[seq-loop] blk host_qn=<float> seq_qn=<float> active=<int> jump=<0|1> dropout=<0|1>
```
`host_qn` is the host playhead and `seq_qn` the plugin's sequence read position,
both in quarter notes; `jump=1` flags a block where the host playhead was NOT a
smooth continuation (a wrap or a seek). PASS requires `|host_qn - seq_qn| <=
--pos-tolerance-qn` on **every** block (wrap/seek blocks included), at least one
loop wrap AND one seek, observed note activity, and no dropout.

> This is the **harness half** of Phase-2 DoD Proof #2. Completing the proof also
> needs the embedded-sequence plugin (Gate-5/6 work) and a real REAPER run; until
> both exist this mode degrades to SKIP (REAPER absent) or INCONCLUSIVE (no
> plugin markers). The scraper/dispatch logic is unit-tested (no REAPER) in
> `test_reaper_smoke.py`, including a negative drift/dropout case.

Add `--check-config` to honor the opt-in toggle (SKIP unless enabled) — CI/gate use.
Use `--reaper-bin /Applications/REAPER.app/Contents/MacOS/REAPER` when a receipt
must bind one exact host executable; an invalid or symlinked path fails closed.

### `gpu-fault` — audio continuity through one injected GPU fault (WP-h1)
Loads the GPU fault-continuity test plug-in (`examples/gpu-fault-continuity`,
built only with `-DPULP_GPU_AUDIO_FAULT_HOOKS=ON`) on its shared-I/O GPU path
and floats its editor. It then renders the track **online** (REAPER's
realtime render, set through the project file the harness writes), and the
plug-in fires one fault when the playhead crosses `--fault-at-sample`:

- `force_loss_before|force_loss_between|force_loss_after|delay_completion|worker_stall|missing_completion`
  hit the running **audio** provider. The provider's own fault hooks are
  construction-time options, and WP-A2's `gpu_audio_fault_inject` control op
  cannot reach a plug-in inside REAPER. So the test plug-in interposes Dawn's
  proc table (test-only, guarded by `#error` unless `PULP_GPU_FAULT_TEST_ONLY`)
  and applies the fault at the provider's own call sites, on this instance's
  provider device only:
  - ForceLoss before `Submit`, between `Submit` and `OnSubmittedWorkDone`, or
    after it;
  - a stalled `ProcessEvents` (`delay_completion`, 100 ms; `worker_stall`,
    3 s, formerly `hang`; `--fault-delay-ms` to change);
  - a swallowed completion callback (`missing_completion`).

  An audio fault counts only once the plug-in reports it applied it, and to
  which device.
- `render_force_loss` loses the **editor's render** device through the
  surface's existing device accessor. Audio owns a separate Dawn device, so the
  expectation is "audio continues"; a REAPER crash is recorded as FAIL.
- `positive_control` fires at the sample and faults nothing.

A PASS needs three nulls against an independent CPU reference, each covering
the whole fault window:
1. the plug-in's own capture of every callback (its recorded input convolved
   with the IR, delayed by the PDC each prepare reported);
2. **the host's realtime render of the fault pass** (`realtime.wav`, the
   primary host-side evidence);
3. an offline bounce of the reopened project. The bounce comes from a fresh,
   fault-free instance, so it only cross-checks the offline path and PDC.

The capture (v2) also records the transport's delivery counters for every
block. From them a PASS further requires:
- the GPU carried at least 90% of the 96 quanta before the fault (50–90% is
  reported as a degraded host; below 50% the run is void);
- no silence block anywhere;
- the fallback took over within `lead_blocks + 1` quanta of the applied fault;
- after a loss, the GPU delivered nothing.

Live receipts are judged by two further rules:
- **The quiet-host rule.** A live run whose host preflight
  (`gpu_audio_p2_host_preflight.py`, re-derived from `preflight.json`) did not
  admit a quiet host is INCONCLUSIVE (`host_not_quiet`) and never upgraded to
  PASS.
- **Realtime provenance.** The realtime render needs a bound
  `realtime_render.json`: the harness's wall-clock span around the online
  render. The render must take at least 90% of the audio's duration and differ
  from the bounce byte for byte, or the run is INCONCLUSIVE.

Controls: `--planted no_fallback|fallback_misaligned|fallback_stale` are planted
negatives that keep audio flowing but wrong (or absent), and must FAIL.
`--gpu-off` (the staged path) is the path negative and must be INCONCLUSIVE.
`--stimulus internal` (the env var `PULP_GPU_FAULT_INPUT=internal`, read before
the first callback) has the plug-in make its own input and never read the host's.
Over the whole run, the verifier requires the plug-in's captured input to equal
the expected stimulus at every played sample, and exact silence everywhere else:
the bound `stimulus.wav` normally, or the internal stimulus. Any other audio, such as a
live input or a microphone, makes the receipt invalid, so no host input can hide
in `capture.bin`. The capture-producing source files (`gpu_fault_continuity.cpp`,
`gpu_fault_view.cpp` and `gpu_fault_interposer.cpp`) refuse to compile without
`PULP_GPU_FAULT_TEST_ONLY`, which only this plug-in's targets define. The run directory holds every artifact plus a
`pulp.gpu-audio.daw-continuity.v1` receipt
([schema](../../../docs/contracts/gpu-audio-daw-continuity-v1.schema.json)).

Build it with the shared-I/O convolver compiled in; without these flags
`SharedRequired` refuses and every run is INCONCLUSIVE:

```bash
cmake -S . -B build -DPULP_BUILD_EXAMPLES=ON -DPULP_GPU_AUDIO_FAULT_HOOKS=ON \
  -DPULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON \
  -DPULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER=ON
cmake --build build --target PulpGpuFaultContinuity_CLAP PulpGpuFaultContinuity_VST3
```

```bash
python3 tools/testing/daw-smoke/reaper_smoke.py --mode gpu-fault \
  --gpu-fault force_loss_after --format clap \
  --plugin-name "Pulp GPU Fault Continuity" \
  --plugin-path "build/CLAP/Pulp GPU Fault Continuity.clap" \
  --out-dir /tmp/gpu-fault-run --reaper-bin /Applications/REAPER.app/Contents/MacOS/REAPER
python3 tools/validation/verify_gpu_audio_daw_continuity_receipt.py /tmp/gpu-fault-run/receipt.json
```

The verifier reads only digest-listed artifacts. From them it re-derives every
null, the counter expectations, the verdict, and the preflight summary (from
`preflight.json`). It checks that the plug-in's events carry this run's nonce
and REAPER's pid, and that every fault event names the fault and sample the
receipt claims. It also checks that the post-fault window is the one the
plug-in was asked to observe, at least 1 s long for a live run (100 ms for a dry
run), that the render length comes from the bound stimulus, that the run
directory's claim names the process every event came from, and that the
applied fault hit this plug-in's provider device at the call site its kind
names. Edited numbers, unlisted or swapped files, and events from
another run are rejected (exit 2). What it cannot prove is that the bound files
came from a real REAPER rather than a careful forger: the receipt is
self-contained, with no external signer. `--require-quiet-host` makes host
quietness binding. Every live receipt today says `in_process_file`, which proves
the audio claim but not a control operation, so `--require-control-op` refuses
it. A failed null, or evidence that does not match the run, is a FAIL. A clean
run is INCONCLUSIVE, never PASS, when the plug-in is not on the shared-I/O path,
refuses or never applies the fault, drops a capture record, fires the fault on
an offline block, or the GPU delivered nothing before the fault.

The harness turns off REAPER's anticipative FX processing for the test track.
With it on, REAPER runs the plug-in ahead of playback in back-to-back bursts,
the GPU never meets its lead (which is counted in callbacks), and every block is
CPU fallback; the receipt then says the GPU delivered nothing before the fault
and the run is INCONCLUSIVE. `--anticipative-fx` keeps it on to show exactly
that. The plug-in's transport quantum is the host's whole prepared block for the
same reason: smaller quanta run back to back inside one callback.

`--dry-run` runs the whole evidence chain on synthetic captures with no REAPER;
`--dry-run --planted <shape>` and `--dry-run --gpu-off` synthesize the same
negatives the plug-in plants, which must come back FAIL and INCONCLUSIVE
respectively.

A REAPER run can be read in v2 terms (the same receipt as the other hosts, so
a planted negative reads `negative_proved`) without running anything again:

```bash
python3 tools/testing/daw-smoke/real_host_smoke.py wrap-reaper \
  --reaper-run /tmp/gpu-fault-run --out /tmp/gpu-fault-run-v2 --code-commit <sha>
```

It refuses a run the v1 verifier does not accept live. REAPER's harness does
not record its launch time, so the v2 host block bounds it below by the
preflight's `host_vitals` sample, which is taken before REAPER starts.

Where both the plug-in capture and the host's realtime render deviate, the v2
verifier requires the render to sit exactly the PDC the plug-in declared in its
`prepared` event earlier, at both ends of the deviation. On the shared-I/O
convolver that is `(lead_blocks + 1) * quantum` (1536 at a 512 quantum); the
`latency_samples` in the health status (1024) is the GPU lead alone, not the
PDC the host compensates. Any other offset makes the row INCONCLUSIVE.

### Real hosts: Logic, GarageBand, Bitwig (WP-K)

`real_host_smoke.py` drives these one step at a time; the UI steps are done
through cmux-cua by accessibility element, with the host launched by
`open -g -n`. Allow about 15 minutes per host.

1. `preflight --host H --run DIR` refuses a running DAW, a locked console, an
   active rendezvous or an unreverted run.
2. For an AU host, `install-au`; then `snapshot-prefs`. Bitwig's prefs
   directory is copied; Logic's and GarageBand's preference domains
   (`com.apple.logic10`, `com.apple.garageband10`) are exported with
   `defaults`, and `launch` refuses Logic or GarageBand without that export,
   because the launch alone rewrites them (recent projects, the plug-in list,
   `NewTrackSettingsV1`). `revert` imports the export, deletes each key the run
   added (`defaults import` merges, so they would survive it), exports again
   and compares with the saved export byte for byte in a canonical form; a
   difference leaves the run unreverted and the next preflight refuses.
   GarageBand is sandboxed and had never run on the test machine, so its
   domain exports empty until it has.
3. Templates (`real_host_templates.json`) are authored once per host into the
   temp directory and fingerprinted with `template-manifest`. No kind is
   record-armed: Logic refuses to arm a No Input track ("No input source
   selected!"). A `live` template is an unarmed no-input track with no audio
   region, on which the plug-in renders its internal stimulus; the M6 input
   check proves no host input reached it. Save with the plan from
   `save-as-plan --host H --target /tmp/.../WPK-live.logicx`: Cmd-Shift-S and
   Cmd-Shift-G with only the host's own window briefly in front, because
   pressing File > Save As by accessibility did not open Logic's save sheet.
4. `stage`, `launch`, `await-claim`, then the realtime render between
   `realtime-begin` and `realtime-end`, and the offline bounce.
5. `collect --host-evidence AX.json`: the AX snapshot must carry `plugins` and a
   `track` object (`input`, `record_armed`, `input_monitoring`). `collect` and
   the v2 verifier refuse an armed track, monitoring on, or an input other than
   the host's no-input label (`No Input`, `none` for a GarageBand instrument
   track, `No input` in Bitwig).
6. Quit without saving, then `revert` (it refuses while the host is alive).

Expected side effects a revert does not undo: Logic's launch rewrites its
library databases under `~/Music/Audio Music Apps/Databases`, and the AU
component cache learns the test plug-in. Neither touches a user project.

## Scan-path note (2026-10-22)

The pre-warm launch now waits for a scanned plug-in entry in REAPER's cache, not
just the bundle's section header, which REAPER writes before scanning. Matching
the header killed the pre-warm too early and is the likely cause of the
long-standing "REAPER could not find/insert the FX" / "flaky launch"
INCONCLUSIVEs on CLAP and VST3.

## Audio etiquette
This launches REAPER with a Pulp plugin on a track and may press Play, so audio
can be active out of the default device during the run. The harness **announces**
the source + expected duration before launching, **caps** the run at `--timeout`
seconds, and **tears down** (kills REAPER) on every exit path.

Exit codes: `0` PASS · `1` FAIL (reload rejected / didn't apply) · `2` SKIP (REAPER
not installed) · `3` INCONCLUSIVE (flaky launch). **A SKIP is never a PASS.**

## Design guarantees
- **Zero pollution:** VST3/CLAP scan from a temp path via a fresh REAPER portable
  dir — never writes your REAPER config or `~/Library/Audio/Plug-Ins`. `--format au`
  installs to the real Components folder and always uninstalls on exit.
- **Cleanup always runs:** kills spawned REAPER, removes temp dirs, uninstalls any AU.
- **Headless-safe:** verifies by scraping the reload log, never `screencapture`
  (which returns black frames without a Screen-Recording grant). Use
  `render_to_png` for human-viewable visuals.

## Requirements
REAPER installed + licensed (local machines m1/m3/m5 already are). Absent → SKIP.
