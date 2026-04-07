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

None. Pulp does not have built-in pitch/time stretching.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
Signalsmith Stretch — MIT — https://github.com/Signalsmith-Audio/signalsmith-stretch
```
