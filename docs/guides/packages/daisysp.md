# DaisySP

DSP library with oscillators, filters, effects, and physical modeling.

| | |
|---|---|
| **License** | MIT |
| **URL** | https://github.com/electro-smith/DaisySP |
| **Version** | V1.0.0 |
| **Integration** | CMake (FetchContent) |
| **RT-safe** | Yes |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

Originally built for the Daisy embedded audio platform, DaisySP is a portable DSP library with a focus on synthesis and physical modeling:

- **Physical modeling:** Karplus-Strong, modal synthesis, plucked string
- **Drum synthesis:** analog kick, snare, hi-hat models
- **Oscillators:** FM, formant, harmonic, particle noise
- **Effects:** bitcrush, decimator, flanger, tremolo, fold
- **Filters:** ladder, Moog-style, comb, allpass
- **Utilities:** compressor, limiter, crossfade, sample-and-hold

## CMake Integration

DaisySP does not ship a CMakeLists.txt for desktop use. Create a wrapper:

```cmake
include(FetchContent)
FetchContent_Declare(
  DaisySP
  GIT_REPOSITORY https://github.com/electro-smith/DaisySP.git
  GIT_TAG        V1.0.0
  GIT_SHALLOW    TRUE
)
FetchContent_Populate(DaisySP)

file(GLOB_RECURSE DAISYSP_SOURCES "${daisysp_SOURCE_DIR}/Source/**/*.cpp")
add_library(DaisySP STATIC ${DAISYSP_SOURCES})
target_include_directories(DaisySP PUBLIC "${daisysp_SOURCE_DIR}/Source")
target_link_libraries(MyPlugin PRIVATE DaisySP)
```

## Example Usage

```cpp
#include "Synthesis/modalvoice.h"
#include "Drums/analogbassdrum.h"

class PhysicalSynth : public pulp::Processor {
    daisysp::ModalVoice modal;
    daisysp::AnalogBassDrum kick;

    void prepare(double sample_rate, int max_block) override {
        modal.Init(static_cast<float>(sample_rate));
        kick.Init(static_cast<float>(sample_rate));

        modal.SetFreq(440.0f);
        kick.SetFreq(60.0f);
    }

    void process(pulp::BufferView<float> buffer) override {
        auto* out = buffer.channel(0);
        for (int i = 0; i < buffer.num_samples(); ++i) {
            out[i] = modal.Process(false) + kick.Process(false);
        }
    }
};
```

## Pulp Overlap

DaisySP has significant overlap with Pulp built-ins:

| DaisySP | Pulp Built-in | When to Use DaisySP |
|---------|---------------|---------------------|
| Oscillators | `signal/oscillator.hpp` | For FM, formant, harmonic, particle oscillators |
| Biquad filters | `signal/biquad.hpp` | Only if you need DaisySP's ladder/Moog filters |
| ADSR | `signal/adsr.hpp` | Generally prefer Pulp's built-in |
| Reverb | `signal/reverb.hpp` | Generally prefer Pulp's built-in |
| Compressor | `signal/compressor.hpp` | Generally prefer Pulp's built-in |

**Use DaisySP for:** Physical modeling (Karplus-Strong, modal), drum synthesis, and exotic oscillators. **Use Pulp built-ins for:** Standard filters, envelopes, reverb, and compression.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
DaisySP — MIT — https://github.com/electro-smith/DaisySP
```
