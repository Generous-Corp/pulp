# libsamplerate

High-quality sample rate conversion (Secret Rabbit Code).

| | |
|---|---|
| **License** | BSD-2-Clause |
| **URL** | https://github.com/libsndfile/libsamplerate |
| **Version** | 0.2.2 |
| **Integration** | CMake (FetchContent) |
| **RT-safe** | No (allocates internally) |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

Industry-standard sample rate conversion with multiple quality modes:

- **SRC_SINC_BEST_QUALITY** — highest quality, highest CPU
- **SRC_SINC_MEDIUM_QUALITY** — good balance
- **SRC_SINC_FASTEST** — fast sinc interpolation
- **SRC_LINEAR** — linear interpolation
- **SRC_ZERO_ORDER_HOLD** — zero-order hold

Use for offline resampling, format conversion, or prepare-time sample rate adaptation. Not suitable for real-time audio thread use due to internal allocations.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  libsamplerate
  GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
  GIT_TAG        0.2.2
  GIT_SHALLOW    TRUE
)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsamplerate)
target_link_libraries(MyPlugin PRIVATE SampleRate::samplerate)
```

## Example Usage

```cpp
#include <samplerate.h>

class Resampler {
public:
    std::vector<float> resample(const float* input, long frames_in,
                                double src_ratio, int quality = SRC_SINC_MEDIUM_QUALITY) {
        long frames_out = static_cast<long>(frames_in * src_ratio) + 1;
        std::vector<float> output(frames_out);

        SRC_DATA data{};
        data.data_in = input;
        data.input_frames = frames_in;
        data.data_out = output.data();
        data.output_frames = frames_out;
        data.src_ratio = src_ratio;

        src_simple(&data, quality, 1);
        output.resize(data.output_frames_gen);
        return output;
    }
};
```

## Pulp Overlap

None. Pulp does not have built-in sample rate conversion.

## See Also

- [r8brain-free-src](r8brain-free-src.md) — alternative with different quality/latency tradeoffs

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
libsamplerate — BSD-2-Clause — https://github.com/libsndfile/libsamplerate
```
