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
built only with `-DPULP_GPU_AUDIO_FAULT_HOOKS=ON`) on its shared-I/O GPU path,
floats its editor, and plays a known stimulus in realtime while the plug-in fires
one fault when the playhead crosses `--fault-at-sample`:

- `force_loss_before|force_loss_between|force_loss_after|delay_completion|hang`
  hit the **audio** device through WP-A2's `gpu_audio_fault_inject` seam;
- `render_force_loss` loses the **editor's render** device through the
  surface's existing device accessor. Audio owns a separate Dawn device, so the
  expectation is "audio continues"; a REAPER crash is recorded as FAIL.

The plug-in captures every callback's input and output from a host thread, and
both that capture and REAPER's bounce are nulled against an independent CPU
reference (stimulus convolved with the same IR, delayed by the reported PDC for
the capture, PDC-compensated for the bounce) over the whole run and in a window
around the fault sample. Transport health is snapshotted before, at, and after
the fault. The run directory holds every artifact plus a
`pulp.gpu-audio.daw-continuity.v1` receipt
([schema](../../../docs/contracts/gpu-audio-daw-continuity-v1.schema.json)):

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
  --gpu-fault force_loss_between --format clap \
  --plugin-name "Pulp GPU Fault Continuity" \
  --plugin-path "build/CLAP/Pulp GPU Fault Continuity.clap" \
  --out-dir /tmp/gpu-fault-run --reaper-bin /Applications/REAPER.app/Contents/MacOS/REAPER
python3 tools/validation/verify_gpu_audio_daw_continuity_receipt.py /tmp/gpu-fault-run/receipt.json
```

The verifier re-derives every null, counter expectation and the verdict from the
digest-bound artifacts; edited numbers or swapped files are rejected (exit 2).
The receipt labels host quietness from `gpu_audio_p2_host_preflight.py`;
`--require-quiet-host` and `--require-control-op` make those binding. A fault
that reached the plug-in through the request file (`in_process_file`) proves the
audio claim but not the control operation. A run is INCONCLUSIVE, never PASS,
when the plug-in is not on the shared-I/O path, refuses the fault, drops a
capture record, or fires the fault on an offline block. After playback the track
is bounced with an offline render, so the bounce null covers the post-fault
provider state through REAPER's own render path.

The harness turns off REAPER's anticipative FX processing for the test track.
With it on, REAPER runs the plug-in ahead of playback in back-to-back bursts,
the GPU never meets its lead (which is counted in callbacks), and every block is
CPU fallback; the receipt then says the GPU delivered nothing before the fault
and the run is INCONCLUSIVE. `--anticipative-fx` keeps it on to show exactly
that. The plug-in's transport quantum is the host's whole prepared block for the
same reason: smaller quanta run back to back inside one callback.

`--dry-run` runs the whole evidence chain on synthetic captures with no REAPER;
`--dry-run --dry-run-break-fallback` is the planted negative (silence after the
fault) and must come back FAIL at the fault sample.

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
