# RTNeural

Real-time neural network inference optimized for audio.

| | |
|---|---|
| **License** | BSD-3-Clause |
| **URL** | https://github.com/jatinchowdhury18/RTNeural |
| **Version** | 1fb1f075 (untagged, pinned commit) |
| **Integration** | CMake (FetchContent) |
| **RT-safe** | Yes |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

A lightweight neural network inference library designed for real-time audio. Supports common layer types (Dense, Conv1D, GRU, LSTM, BatchNorm) with compile-time or runtime model sizing. Used widely for guitar amp modeling and audio effect emulation.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  RTNeural
  GIT_REPOSITORY https://github.com/jatinchowdhury18/RTNeural.git
  GIT_TAG        1fb1f075
)
FetchContent_MakeAvailable(RTNeural)
target_link_libraries(MyPlugin PRIVATE RTNeural)
```

To use the header-only mode instead:

```cmake
target_compile_definitions(MyPlugin PRIVATE RTNEURAL_USE_EIGEN=1)
```

## Example Usage

```cpp
#include <RTNeural/RTNeural.h>

class AmpModel : public pulp::Processor {
    // Compile-time model: input=1, Dense(8), tanh, Dense(1)
    RTNeural::ModelT<float, 1, 1,
        RTNeural::DenseT<float, 1, 8>,
        RTNeural::TanhActivationT<float, 8>,
        RTNeural::DenseT<float, 8, 1>
    > model;

    void prepare(double sample_rate, int max_block) override {
        model.reset();
        // Load weights from JSON — see RTNeural docs
    }

    void process(pulp::BufferView<float> buffer) override {
        auto* data = buffer.channel(0);
        for (int i = 0; i < buffer.num_samples(); ++i) {
            float input[] = { data[i] };
            data[i] = model.forward(input);
        }
    }
};
```

## Pulp Overlap

None. Pulp has no built-in neural network inference.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
RTNeural — BSD-3-Clause — https://github.com/jatinchowdhury18/RTNeural
```
