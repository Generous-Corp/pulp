# Time-Stretch & Pitch-Shift

Pulp ships its own **MIT-licensed** time-stretch and pitch-shift engine in
`core/signal`. There is no external dependency (no Rubber Band / GPL) — the engine,
its character modes, the A/B measurement harness, and the tunable presets are all
in-tree and reusable from any plugin.

> Honest status: the engine is **R3-competitive** — strong on tonal/melodic and, with
> the material-adaptive window, sharp on percussion. It is not yet a blanket "beats
> Rubber Band everywhere"; that's the tracked Track-A roadmap
> (`planning/GOAL-offline-stretch-beat-r3.md`). Use the A/B harness below to judge for
> your material.

## The two engines

| Engine | Use | Header |
|--------|-----|--------|
| `pulp::signal::OfflineStretch` | Whole-buffer (offline) render: exact output length, best quality. The path PulpTempoSampler uses. | `pulp/signal/offline_stretch.hpp` |
| `pulp::signal::RealtimePitchTimeProcessor` | Streaming/realtime, hop-quantized. Laroche-Dolson phase propagation with identity phase locking. | `pulp/signal/realtime_pitch_time_processor.hpp` |

### Minimal offline render

```cpp
#include <pulp/signal/offline_stretch.hpp>
using namespace pulp::signal;

OfflineStretch eng;
OfflineStretchOptions sizing;            // sizing fixes the supported range
sizing.max_time_ratio = 4.0;             // [0.25x .. 4x]
eng.prepare(sample_rate, channels, sizing);

OfflineStretchOptions o;
o.time_ratio = 1.5;                      // 1.5x longer (slower)
o.pitch_semitones = 0.0;
const long out_frames = offline_stretch_output_frames(in_frames, o.time_ratio);
eng.process(in_ptrs, in_frames, out_ptrs, out_frames, o);
```

## Character modes — an "engine per job"

`OfflineStretchOptions::character` (`StretchCharacter`) picks the algorithm voicing:

| Mode | What | Status |
|------|------|--------|
| `clean` | Peak-locked phase vocoder + material-adaptive FFT. Natural; time ≠ pitch. Best for tonal/melodic/sustained. **Default.** | Live |
| `varispeed` | Pitch + time **linked** (pure resample) + speed-scaled tape-head EQ. Tape character, *no* stretch artifacts; pitch follows tempo. | Live |
| `phase_vocoder` | Reserved for clean + verbatim transient relocation. Renders as `clean` until seam handling passes the quality gate. | Reserved (→ clean) |
| `granular` | Reserved for grain/stutter texture. | Reserved (→ clean) |

`clean` and `varispeed` are the two production voicings today; `phase_vocoder` and
`granular` are honest scaffolds that fall back to `clean` so code written against them
keeps working as they land.

## Material-adaptive FFT window

`OfflineStretch::recommend_window(in, frames, channels, sample_rate)` analyzes the
input's transient density (crest) and low-band fraction and returns the STFT geometry
to set on the options before `prepare()`:

| Material | Window / hop | Why |
|----------|--------------|-----|
| Percussive (high crest) | `1024 / 128` | Time resolution — sharp, bright attacks (the ear-validated "drum_pl" reference) |
| Bass / low-fundamental | `8192 / 512` | Resolve closely-spaced low partials so the stretch doesn't wobble |
| Everything else | `0 / 0` (default `4096 / 512`) | Balanced |

PulpTempoSampler calls this per loop, so a drum break renders sharp and a bass line
renders stable — instead of one fixed window smearing one or the other.

```cpp
const auto w = OfflineStretch::recommend_window(in, frames, channels, sample_rate);
sizing.fft_size = w.fft_size;            // 0 keeps the default
sizing.analysis_hop = w.analysis_hop;
eng.prepare(sample_rate, channels, sizing);
```

## Tunable knobs (`OfflineStretchOptions`)

| Field | Default | Effect |
|-------|---------|--------|
| `character` | `clean` | Voicing (above) |
| `fft_size` / `analysis_hop` | `0/0` | STFT window; `0` = default, or set from `recommend_window` |
| `transient_sensitivity` | `0` (engine default) | Higher = more aggressive transient preservation |
| `formant_mode` | `preserve_original` | `follow_pitch` / `preserve_original` / `shift_independently` |
| `formant_semitones` | `0` | Used with `shift_independently` |
| `repitch_linked` | `false` | `true` = pure resample (vinyl), pitch tied to time |
| `route_noise_stn` | `false` | Route noise/residual through the STN `NoiseMorpher` (experimental; off because it dulls transients — see the roadmap) |
| `relocate_transients` | `false` | Verbatim transient graft (reserved; no-op until seam-clean) |
| `quality` | `2` | `0` draft preview … `2` best |

## Presets — ship your own tweaks

`pulp/signal/stretch_preset.hpp` defines a **flat, human-editable** `StretchPreset`
(the tunable subset above) plus text (de)serialization, so a developer can save,
share, and ship a custom voicing without recompiling.

```cpp
#include <pulp/signal/stretch_preset.hpp>
using namespace pulp::signal;

// Author a preset
StretchPreset p;
p.name = "Punchy Drums";
p.character = StretchCharacter::clean;
p.fft_size = 1024; p.analysis_hop = 128;     // force the sharp percussive window
p.transient_sensitivity = 1.5f;
std::string text = preset_to_text(p);        // save this to disk / ship it

// Load a preset and apply it to options
StretchPreset loaded;
preset_from_text(text, loaded);              // tolerant: skips blanks/# comments
OfflineStretchOptions o;
apply_preset(o, loaded);                     // sets only the preset-managed fields
o.time_ratio = 1.5;                          // ratio/pitch stay the caller's

// Capture the current options back into a preset (round-trip)
StretchPreset snapshot = capture_preset(o, "My Render");
```

The text format is comment-tolerant:

```
# Pulp stretch preset
name = Punchy Drums
character = clean              # clean | varispeed | phase_vocoder | granular
fft_size = 1024
analysis_hop = 128
transient_sensitivity = 1.5
```

## A/B testing & debugging tools

The offline-stretch example (`examples/offline-stretch/`) is the measurement bench:

```bash
# Render with the material-adaptive window (what the sampler does)
stretchcli in.wav out.wav --ratio 1.5 --auto-window --quality 2

# Tempo-match by detected BPM, or just analyze
stretchcli in.wav out.wav --bpm-to 120 --auto-window
stretchcli in.wav --analyze          # JSON: detected BPM + onsets

# Pitch / formant / vinyl
stretchcli in.wav out.wav --pitch 3 --formant preserve
stretchcli in.wav out.wav --ratio 0.8 --repitch     # varispeed-style
```

The Python harness (`examples/offline-stretch/tools/`) renders a corpus and scores
it with **established** quality libraries (pyloudnorm for loudness match, librosa for
onset strength / HPSS balance, essentia for inharmonicity) rather than hand-rolled
probes — so an A/B is loudness-matched and reference-free. To fine-tune: render the
same loop with two presets / windows, score both, and keep the winner.

Quick objective check used in development — **transient crest** (peak/RMS) at 2×
stretch on a drum loop: the fixed default window scores ~11, the adaptive `1024/128`
window ~19 (sharper, more present attacks).

## See also

- `planning/GOAL-offline-stretch-beat-r3.md` — the quality goal + Track-A roadmap
- [Packages: Signalsmith Stretch](packages/signalsmith-stretch.md) — the MIT alternative and when to reach for it
