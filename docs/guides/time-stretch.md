# Time-Stretch & Pitch-Shift

Pulp ships its own **MIT-licensed** time-stretch and pitch-shift engine in
`core/signal` — no GPL dependency. The engine, its character modes, the A/B
measurement harness, and the tunable presets are all in-tree and reusable from any
plugin.

The `clean` character is a peak-locked phase vocoder (Laroche-Dolson phase
propagation with identity phase locking) plus material-adaptive FFT windowing —
natural on tonal/melodic material and sharp on percussion. Use the A/B harness below
to dial it in for your own material.

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
| `phase_vocoder` | Reserved character mode. Renders through the `clean` spectral path today; use `relocate_transients` for opt-in tempo-only transient grafting. | Reserved (→ clean) |
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
| `route_noise_stn` | `false` | Route noise/residual through the STN `NoiseMorpher` (experimental; off by default because it can dull transients) |
| `relocate_transients` | `false` | Verbatim transient graft on the tempo-only spectral path (`pitch_semitones == 0`, `quality >= 1`); opt-in attack restoration |
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
# Render with the material-adaptive window (the default; what the sampler does)
stretchcli in.wav out.wav --ratio 1.5 --quality 2

# Tempo-match by detected BPM, or just analyze
stretchcli in.wav out.wav --bpm-to 120
stretchcli in.wav --analyze          # JSON: detected BPM + onsets

# Manual window, pitch / formant, and vinyl
stretchcli in.wav out.wav --ratio 1.5 --fft 1024 --hop 128
stretchcli in.wav out.wav --pitch 3 --formant preserve
stretchcli in.wav out.wav --ratio 0.8 --repitch     # varispeed-style
```

The Python A/B toolkit (`examples/offline-stretch/eval/`) renders candidate
configs through `stretchcli` and scores brightness, attack punch, dominant low
partial, low-frequency wobble, long-term spectral distance, and band balance
with numpy + soundfile. The older corpus helpers under
`examples/offline-stretch/tools/` remain for synthetic fixture generation and
baseline capture. To fine-tune: render the same loop with two presets / windows,
score both, listen, and keep the winner.

Quick objective check used in development — **transient crest** (peak/RMS) at 2×
stretch on a drum loop: the fixed default window scores ~11, the adaptive `1024/128`
window ~19 (sharper, more present attacks).

## See also

- [Packages: Signalsmith Stretch](packages/signalsmith-stretch.md) — the MIT alternative and when to reach for it
