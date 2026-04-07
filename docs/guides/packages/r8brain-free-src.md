# r8brain-free-src

High-quality sample rate converter with low latency mode.

| | |
|---|---|
| **License** | MIT |
| **URL** | https://github.com/avaneev/r8brain-free-src |
| **Version** | version-6.5 |
| **Integration** | CMake (FetchContent) |
| **RT-safe** | No (allocates internally) |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

An audiophile-grade sample rate converter with configurable quality/latency tradeoffs. Achieves very high SNR (>200 dB for best quality mode) with options for low-latency operation suitable for real-time pipelines (though not for audio-thread use due to allocations).

Key quality modes:
- **Best quality** — highest SNR, highest latency
- **Normal** — good balance for most applications
- **Low latency** — suitable for real-time pipelines with moderate quality

## CMake Integration

r8brain-free-src does not ship a standard CMakeLists.txt. Create a wrapper:

```cmake
include(FetchContent)
FetchContent_Declare(
  r8brain
  GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
  GIT_TAG        version-6.5
  GIT_SHALLOW    TRUE
)
FetchContent_Populate(r8brain)

add_library(r8brain STATIC "${r8brain_SOURCE_DIR}/r8bbase.cpp")
target_include_directories(r8brain PUBLIC "${r8brain_SOURCE_DIR}")
target_link_libraries(MyPlugin PRIVATE r8brain)
```

## Example Usage

```cpp
#include "CDSPResampler.h"

class HighQualityResampler {
public:
    std::vector<double> resample(const double* input, int frames,
                                  double src_rate, double dst_rate) {
        r8b::CDSPResampler24 resampler(src_rate, dst_rate, frames);

        double* output = nullptr;
        int out_frames = resampler.process(input, frames, output);

        return std::vector<double>(output, output + out_frames);
    }
};
```

## Pulp Overlap

None. Pulp does not have built-in sample rate conversion.

## See Also

- [libsamplerate](libsamplerate.md) — alternative with different API style and quality profile

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
r8brain-free-src — MIT — https://github.com/avaneev/r8brain-free-src
```
