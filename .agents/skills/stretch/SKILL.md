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

Most DSP helpers now expose `*64` aliases when the underlying class is templated,
including `OfflineStretch64` and `RealtimePitchTimeProcessor64`. The offline
stretch public config remains control-plane double regardless of sample type;
only the sample buffers and internal DSP storage widen for the f64 aliases.
Keep quality comparisons explicit about which sample type is under test.

## Build + run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
tools/ci/governed-build.sh cmake --build build --target stretchcli
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
  points (muddy + "wind"). `--stn` to opt in for noisy textures. Two correctness
  fixes landed on the OPT-IN path (default stays `route_noise_stn=false`, settled by
  the broader A/B): a CAUSAL StnConfig (the morph split applied the mask to the
  newest pushed frame, but the decomposer evaluated medians on the ring's CENTER
  frame — lagging ~(time_median-1)/2 frames, so a transient's broadband energy was
  misrouted into the random-phase noise path and decohered) and a √(8/3) Hann WOLA
  synthesis-gain (random-phase frames overlap-add incoherently while WOLA normalizes
  for coherent summation → ~4-5 dB loss). The morpher still dulls tonal/transient
  material, so it stays off by default; these only improve the opt-in path.
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
  per-transient peak-vs-source + attack slope. **(4) Graft only the HIGH band**
  (`kReloHpHz`, 300 Hz): the PV smears high-frequency attacks but reconstructs
  sustained LOW frequencies cleanly + continuously. A full-band graft re-injects
  low-frequency attack energy whose phase can't match the PV body across the short
  (~1 ms) seam — and a deep kick's period (~15 ms) is longer than the whole graft
  window — so the seam can't bridge it: a low-frequency discontinuity that "blows
  out" deep hits, ONLY at stretch (no PV body to mismatch at ratio 1). The graft
  high-passes both sides and swaps only the high band, leaving the kick body to the
  PV. Crossover tuned by ear on a real break (180 Hz still blew out; 300 Hz clean +
  punchy). This artifact is INVISIBLE to peak/clip/wobble metrics (output stays
  ~0.8 full-scale) — found only via a graft-on/off vs PV-only listening A/B.
- **Spectral output conditioning** (`match_spectral_rms`): a stretch reconstructs
  BROADBAND material ~3-4 dB QUIET (the WOLA is unity for COHERENT overlap — proven
  by the spectral-engine tests — but the incoherent broadband residual loses the
  `sqrt(8/3)` Hann figure; tonal/peak-locked energy stays at level). Do NOT add a
  constant at the normalization site (breaks the coherent-unity tests). Instead the
  spectral paths (tempo / pitch / R+S; NOT identity / repitch / varispeed, which are
  already level-correct) **make up the interior RMS to the input** (make-up only)
  then **soft-clip** (transparent below 0.9, tanh shoulder to 0.999). The soft-clip
  (not a whole-buffer peak-scale) is load-bearing: it also tames the verbatim graft's
  ADDITIVE overshoot (raw peak ~1.2, previously hidden by the sampler's master
  limiter) without attenuating the whole buffer — so the make-up survives and the
  engine never emits |x|>1. Test: broadband RMS within ~0.02 dB of source across
  0.5–2x, peak ≤ 1.0, a sine NOT inflated.
- **Onset soft-start** (`restore_onset_head`): the PV reconstructs a hard onset at
  sample 0 from an EMPTY analysis history (the Hann edge is ~0), so the attack ramps
  up over ~fft/2 samples (~10 ms percussive) — the first ~10 ms is too quiet. The
  length-lock trim aligns input[0]→output[0] but does NOT remove the ramp, and
  `detect_onsets` structurally misses a sample-0 attack (no flux RISE), so
  `relocate_transients` can't fix it. Graft the input's leading attack over the head
  (~10 ms, `kHeadMs`) with an equal-power crossfade (input[0] is the true sample — no
  peak search needed). No-op when the input head is silent (a real fade-in,
  `kHeadEps`) or the PV didn't lose the attack (`kHeadRatio`). Runs tempo_stretch →
  relocate → **restore_onset_head** → match_spectral_rms (the make-up's interior-RMS
  window excludes the head; its soft-clip bounds the grafted peak).
  - **The head length tracks the window, not a fixed 10 ms.** The PV ramp it has to
    cover is ~`fft/2` — ~10 ms only at the 1024 percussive window, but ~42 ms at the
    4096 default and ~85 ms at 8192 for sustained/tonal material. A fixed-10 ms head
    hands the crossfade back to a still-ramping PV and scoops the envelope between the
    head and the body, so `head = min(out, in, max(kHeadMs·sr, engine_.fft_size()/2))`.
    `match_spectral_rms` floors its interior-RMS edge guard at that same head span
    (`head_guard`), so the graft never skews the make-up gain on short (<~80 ms)
    outputs. Both read `engine_.fft_size()` (==0 ⇒ engine default 4096).

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

## Tuning methodology (when metrics lie)

Subtle transient/phase artifacts (smear, "wobble", blown-out deep hits) are often
INVISIBLE to peak/RMS/clip/AM metrics — output can sit at ~0.8 full-scale and read
"clean" while clearly wrong by ear. Tune by ear, with discipline:

1. **Level-match before listening.** Normalize every render to the SOURCE's RMS
   first. The engine can render a few dB quieter than a reference (a known energy
   leak), and loudness alone decides a blind A/B — match it or your ears lie.
2. **Bisect by ear: isolate ONE variable per render.** Don't compare "old vs new
   everything." Render variants that differ in a single knob and listen to the
   exact moment that sounds wrong — e.g. graft on / off / different crossover, or a
   parameter sweep (180/300/500/800). The first clean one is the answer. This is how
   the high-pass-graft crossover and the refractory window were found.
3. **Reproduce the real path.** The sampler renders with `--relocate` (verbatim
   graft); a no-`--relocate` A/B tests a DIFFERENT signal path and can read "great"
   while the app blows out. Match the flags the product uses.
4. **After the ear picks, guard the INVARIANT, not the sound.** Land a regression
   test for the property the fix establishes — "stretch preserves a tone's f0",
   "the graft doesn't alter the low band (<2% energy)", "a hit resets once not N
   times", "output never clips" — never a golden-audio compare (too brittle).

## Gotchas

- Don't trust harmonic-clarity (peak/valley) or autocorr-f0-on-drums — both misled
  during tuning. Use centroid + peak-Hz-vs-source + wobble + the ear.
- Keep integer types explicit around frame counts on Windows. MSVC's `long` is
  32-bit, while `std::llround` returns `long long`; passing that directly into an
  initializer-list such as `std::min<long>({ ... })` can fail the Windows
  release-path gate as narrowing. Cast or store the rounded value as `long` before
  it enters `std::min<long>` / frame-count lists.
- A faithful time-stretch must keep `peak_hz` IDENTICAL to the source at every
  ratio. If it drifts, something's wrong (that's how the vertical-coherence and the
  bass-FFT bugs were caught).
- The test corpus sources hard-cut at full energy, so clean/time-stretch outputs
  ending abruptly is FAITHFUL, not a bug — don't add fades to the spectral path.
  (Varispeed is the exception: it fades, because tape doesn't hard-cut.)
