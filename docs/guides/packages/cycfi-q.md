# Q (Cycfi)

Pitch detection via bitstream autocorrelation.

| | |
|---|---|
| **License** | MIT |
| **URL** | https://github.com/cycfi/q |
| **Version** | version_1.0 |
| **Integration** | CMake (FetchContent) |
| **RT-safe** | Yes |
| **Platforms** | macOS (arm64), Windows (x64), Linux (x64, arm64) |

## What It Does

Fast, accurate monophonic pitch detection using a bitstream autocorrelation algorithm. Also provides DSP primitives (filters, envelope followers, zero-crossing detection). The pitch detector is the primary draw — there is no MIT-licensed equivalent with this accuracy.

**Platform note:** Windows arm64 is not verified. x64 works on all three platforms.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  q
  GIT_REPOSITORY https://github.com/cycfi/q.git
  GIT_TAG        version_1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(q)
target_link_libraries(MyPlugin PRIVATE libq)
```

Q depends on `cycfi::infra` which is fetched automatically by its CMakeLists.txt.

## Example Usage

```cpp
#include <q/pitch/pitch_detector.hpp>
#include <q/support/literals.hpp>

class TunerPlugin : public pulp::Processor {
    using namespace cycfi::q::literals;
    cycfi::q::pitch_detector pd { 50_Hz, 4000_Hz, 48000, -30_dB };

    void process(pulp::BufferView<float> buffer) override {
        auto* data = buffer.channel(0);
        for (int i = 0; i < buffer.num_samples(); ++i) {
            bool ready = pd(data[i]);
            if (ready) {
                float freq = pd.get_frequency();
                // Use detected frequency (tuner display, pitch correction, etc.)
            }
        }
    }
};
```

## Pulp Overlap

None. Pulp does not have built-in pitch detection.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
Q (Cycfi) — MIT — https://github.com/cycfi/q
```
