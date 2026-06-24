# Signalsmith Stretch

Polyphonic pitch and time stretching.

| | |
|---|---|
| **License** | MIT |
| **URL** | https://github.com/Signalsmith-Audio/signalsmith-stretch |
| **Version** | 1.1.0 |
| **Integration** | Header-only |
| **RT-safe** | Yes |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

High-quality polyphonic pitch shifting and time stretching. Handles complex audio material (chords, drums, vocals) without the phase artifacts common in simpler algorithms. MIT-licensed alternative to Rubber Band (GPL).

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  signalsmith-stretch
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
  GIT_TAG        1.1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(signalsmith-stretch)

# Header-only — add include path
target_include_directories(MyPlugin PRIVATE
  ${signalsmith-stretch_SOURCE_DIR}
)
```

## Example Usage

```cpp
#include "signalsmith-stretch.h"

class PitchShifter : public pulp::Processor {
    signalsmith::stretch::SignalsmithStretch<float> stretch;

    void prepare(double sample_rate, int max_block) override {
        stretch.presetDefault(2, static_cast<float>(sample_rate));
    }

    void process(pulp::BufferView<float> buffer) override {
        // Shift pitch up by 2 semitones
        stretch.setTransposeSemitones(2.0f);

        float* inputs[2]  = { buffer.channel(0), buffer.channel(1) };
        float* outputs[2] = { buffer.channel(0), buffer.channel(1) };

        stretch.process(inputs, buffer.num_samples(), outputs, buffer.num_samples());
    }
};
```

## Pulp Overlap

**Pulp ships its own MIT-licensed pitch/time stretching** in `core/signal`, so for
most cases you do not need an external library:

- **`pulp::signal::OfflineStretch`** — whole-buffer (offline) tempo-stretch + pitch
  shift with exact output length, material-adaptive FFT windowing
  (`recommend_window()`: sharp 1024/128 for percussion, 8192/512 for bass), formant
  modes, repitch-linked resampling, and character modes (`clean` peak-locked phase
  vocoder, `varispeed` tape). This is what **PulpTempoSampler** uses.
- **`pulp::signal::RealtimePitchTimeProcessor`** — the streaming, hop-quantized
  realtime core (Laroche-Dolson phase propagation with identity phase locking),
  used for live pitch/time.

Reach for **Signalsmith Stretch** when you specifically want its algorithm/voicing,
a drop-in header, or to A/B against Pulp's engine. Both are MIT, so either is fine
for a shipped MIT plugin. See the [Time-Stretch & Pitch guide](../time-stretch.md)
for Pulp's engine, character modes, the A/B harness, and tunable presets.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
Signalsmith Stretch — MIT — https://github.com/Signalsmith-Audio/signalsmith-stretch
```
