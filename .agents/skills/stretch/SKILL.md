---
name: stretch
description: Offline time-stretch / pitch / varispeed — character modes, fine-tune presets, A/B toolkit, and the honest quality state, so an agent can pick a mode, dial it in, and ship a plugin with it.
requires:
  - build/examples/offline-stretch/stretchcli
---

# Stretch (offline time-stretch / pitch / varispeed)

`pulp::signal::OfflineStretch` is the non-realtime, max-quality stretch/pitch
engine. This skill is the agent-facing guide: which **character mode** to use,
how to **fine-tune and share a preset**, how to **A/B measure**, and what's
genuinely good vs. still cooking. Headers:
`core/signal/include/pulp/signal/{offline_stretch,stretch_preset}.hpp`. Dev CLI:
`examples/offline-stretch/stretchcli`. Eval toolkit: `examples/offline-stretch/eval/`.

## Build + run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
cmake --build build --target stretchcli -j
./build/examples/offline-stretch/stretchcli in.wav out.wav --ratio 2.0 --quality 2
```
`--ratio` = out_dur/in_dur (2.0 = half speed). Output length is exactly
`round(in_frames*ratio)` — loops stay bar-locked. `--pitch S` shifts S semitones
(duration preserved). `--bpm-to T` picks the ratio from detected BPM.

## The four character modes (`--character`, or `StretchCharacter`)

| Mode | What | Use for | State |
|------|------|---------|-------|
| `clean` (default) | peak-lock phase vocoder + **material-adaptive FFT**; time ≠ pitch | tonal / melodic / sustained (bass, vocals, pads, mixes) | **good, shipping** |
| `varispeed` | pitch+time **LINKED** (sinc resample) + speed-scaled tape head EQ + end-fade | tape slow/speed character; sidesteps the drum weakness | **good, shipping** |
| `phase_vocoder` | reserved; renders as clean (the `clean` engine + `--relocate` is the punch path) | percussion punch | **scaffold → renders clean**; use `--relocate` for punch |
| `granular` | grain/stutter texture | texture | **scaffold → renders clean** |

`varispeed` ≠ time-stretch: it changes pitch AND tempo together (like tape/vinyl
speed), so it has **no phase-vocoder artifacts**. Slowing down warms + dulls
(head-gap HF loss + low-mid bump, scaled by `log2(ratio)`); speeding up brightens.
Exact identity at ratio 1. Demo: `--character varispeed` vs `--repitch` (plain
resample, no EQ) vs `--character clean` (time-stretch).

## Honest quality state (don't oversell)

- **Bass**: excellent — pitch-exact (adaptive 8192 FFT resolves close low partials).
- **Tonal (vocals/pads/mixes)**: very good — matches/beats Rubber Band R3 to the ear.
- **Drums/percussion (clean mode)**: a phase vocoder smears percussion ATTACKS
  (keeps only ~70-75% of each transient peak) — the "compressed/less dynamic"
  artifact. **Verbatim transient relocation** (below) now restores the attack peaks
  to ~97%+ of source; enable it for percussion. The PV still smears the decay TAIL
  (a paradigm limit), so for the most tape-like character `varispeed` is still an
  option; full R3-beating drums would need a true transient-separated path.

## Engine internals worth knowing (validated, do not regress)

- **Peak-lock (Laroche-Dolson) is the universal phase mode.** Vertical phase
  coherence was tried and **RETIRED** — it shifted partials ~5-6 Hz (audible
  "howl" + pitch drift). Do not reintroduce it.
- **Adaptive FFT** (`recommend_window`): percussive→1024/128, bass/low→8192/512,
  else 4096/512. Override with `--fft/--hop`.
- **STN noise-morphing is OFF by default** — it dulled every material ~400 centroid
  points (muddy + "wind"). `--stn` to opt in for noisy textures (its HF loss is a
  known bug to fix before re-enabling by default).
- **Mandatory end-fade on varispeed** (tape doesn't hard-cut; a bare resample of a
  ringing tail ticks).
- `--transient-sens X` raises the Röbel reset sensitivity (sharper attacks); off by
  default — a fine-tune knob. (Measured to REGRESS at 2× on its own — a graft is
  better; see relocation below.)
- **Transient-reset refractory gate** (`TransientPhasePolicy::Config::refractory_frames`,
  default 3): the detector fires a phase reset per high-flux frame, so a drum hit's
  DECAY/ring re-fires it on many consecutive frames — and each full-spectrum reset
  discards the vocoder's accumulated synthesis-phase lead. Sustained re-firing
  degenerates the PV toward raw OLA at the synthesis hop, which (a) pitches partials
  DOWN by the stretch factor and (b) breaks phase coherence — audible as a
  "blown-out / wobbly" sound on the harder, DEEPER hits (kicks). The gate fires once
  at the onset then suppresses re-fires for N frames (≈8 ms at hop 128), keeping the
  attack sharp while killing the over-fire. This is NOT detectable by peak/clip
  metrics (the output never exceeds ~0.8 full-scale) — it's a perceptual transient
  distortion; trust ears + a controlled FULL/HALF/OFF reset A/B over a metric here.
- **Verbatim transient relocation** (`StretchTransientMode::verbatim_relocate`, or
  `--relocate`): grafts each ORIGINAL attack back onto the PV output, PEAK-ALIGNED,
  restoring the punch the PV smears (transient peak ~73% → ~97%+ of source across
  0.25–4×; tonal/sine = perfect no-op; identity at ratio 1). On the tempo-only
  spectral path. **The three gotchas that make-or-break it** (each cost a debugging
  cycle): (1) the output transient is **NOT at `oi*ratio`** — `tempo_stretch` leaves
  a ratio-dependent group-delay offset (~205 samples at 2×), so search `|out|` for
  the real peak near the nominal position, don't graft at `oi*ratio`; (2) the
  energy-window onset detector returns the window START, which **LEADS** the true
  peak, so the input-peak search must go **FORWARD** from the onset (a back-search
  grafts silence → makes it WORSE); (3) the output-peak search width **scales with
  the stretched onset spacing** so it can't lock onto a neighbour at high
  compression. Offline-only (allocates; runs on the render worker, never the audio
  thread). Enabled by default in the PulpTempoSampler render path. Crest factor is
  the WRONG success metric (restoring all peaks moves peak AND RMS together) — use
  per-transient peak-vs-source + attack slope.

## Fine-tune + share a preset (`StretchPreset`)

Presets are a layer ON TOP of the core engine (they don't fork it). Tunable knobs:
character, fft/hop, transient sensitivity, STN, relocation. Tiny human-editable
`key = value` text spec.

```bash
# dial in, then save
stretchcli in.wav out.wav --character varispeed --transient-sens 1.5 --save-preset my.preset
# others load it (later flags still override)
stretchcli in.wav out.wav --ratio 1.5 --preset my.preset
```
Share `my.preset` (a few lines of text). API: `apply_preset()` / `capture_preset()`
in `stretch_preset.hpp`. Ratio/pitch are the caller's, never the preset's.

## A/B measure (the fine-tune loop) — `examples/offline-stretch/eval/`

```bash
pip install -r examples/offline-stretch/eval/requirements.txt   # numpy + soundfile
python examples/offline-stretch/eval/ab_compare.py drum.wav \
  --cli build/examples/offline-stretch/stretchcli --ratios 0.75,1.5,2.0 \
  --configs "clean:--character clean" "tape:--character varispeed"
```
Metrics: centroid (brightness), onset (punch), peak_hz vs source (pitch fidelity),
wobble (pitch stability), spectral-L1 + band balance (EQ match). **Metrics are
necessary, not sufficient — they repeatedly mislead on subtle artifacts; confirm
by ear.** To compare against Rubber Band (GPL, NOT bundled), render with your own
`rubberband` and pass `--reference <file>`.

## Gotchas

- Don't trust harmonic-clarity (peak/valley) or autocorr-f0-on-drums — both misled
  during tuning. Use centroid + peak-Hz-vs-source + wobble + the ear.
- A faithful time-stretch must keep `peak_hz` IDENTICAL to the source at every
  ratio. If it drifts, something's wrong (that's how the vertical-coherence and the
  bass-FFT bugs were caught).
- The test corpus sources hard-cut at full energy, so clean/time-stretch outputs
  ending abruptly is FAITHFUL, not a bug — don't add fades to the spectral path.
  (Varispeed is the exception: it fades, because tape doesn't hard-cut.)
