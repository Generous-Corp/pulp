# Signalsmith DSP

Filters, envelopes, FFT, delay, and spectral processing tools.

| | |
|---|---|
| **License** | MIT |
| **URL** | https://github.com/Signalsmith-Audio/dsp |
| **Version** | v1.7.1 |
| **Integration** | Header-only |
| **RT-safe** | Yes |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

A comprehensive DSP toolkit: filters (biquad, SVF, crossovers), envelope followers, FFT with spectral processing, delay lines with interpolation, and windowing functions. Well-documented, designed for real-time audio.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  signalsmith-dsp
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/dsp.git
  GIT_TAG        v1.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(signalsmith-dsp)

# Header-only — add include path
target_include_directories(MyPlugin PRIVATE
  ${signalsmith-dsp_SOURCE_DIR}
)
```

## Example Usage

```cpp
#include "spectral.h"

class SpectralProcessor : public pulp::Processor {
    signalsmith::spectral::STFT<float> stft;

    void prepare(double sample_rate, int max_block) override {
        stft.setSize(2048);
    }

    void process(pulp::BufferView<float> buffer) override {
        auto* data = buffer.channel(0);
        int n = buffer.num_samples();

        stft.analyseBlock(data, n);
        // Manipulate frequency bins in stft.spectrum()
        stft.synthesiseBlock(data, n);
    }
};
```

## Pulp Overlap

Signalsmith DSP overlaps with several Pulp built-ins:

| Signalsmith | Pulp Built-in | When to Use Signalsmith |
|-------------|---------------|------------------------|
| Biquad filters | `signal/biquad.hpp` | When you need Signalsmith's specific filter topologies |
| SVF | `signal/svf.hpp` | When you need the combined spectral toolkit |
| Delay line | `signal/delay_line.hpp` | When you need interpolated delay with specific modes |
| FFT | `signal/fft.hpp` | When you need integrated STFT with spectral processing |

If you only need one of these capabilities, prefer Pulp's built-in. Use Signalsmith DSP when you need its spectral processing pipeline or multiple components together.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
Signalsmith DSP — MIT — https://github.com/Signalsmith-Audio/dsp
```
