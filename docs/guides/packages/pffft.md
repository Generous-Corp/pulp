# PFFFT

SIMD-optimized FFT for real and complex transforms.

| | |
|---|---|
| **License** | FFTPACK license (UCAR), BSD-style; see [License](#license) |
| **URL** | https://github.com/marton78/pffft |
| **Version** | v1.1.0 |
| **Integration** | CMake (FetchContent) |
| **RT-safe** | Yes |
| **Platforms** | macOS (arm64, NEON), Windows (x64 SSE, arm64 NEON), Linux (x64 SSE, arm64 NEON) |

## What It Does

A small, fast FFT library that uses SIMD instructions (SSE on x64, NEON on ARM) for power-of-two size transforms. Significantly faster than general-purpose FFT implementations for typical audio buffer sizes (256–8192).

This is the marton78 fork which adds CMake support and maintains compatibility with the original PFFFT.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  pffft
  GIT_REPOSITORY https://github.com/marton78/pffft.git
  GIT_TAG        v1.1.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pffft)
target_link_libraries(MyPlugin PRIVATE PFFFT)
```

## Example Usage

```cpp
#include <pffft/pffft.h>

class FastSpectrum {
    PFFFT_Setup* setup = nullptr;
    float* work = nullptr;
    int fft_size = 0;

public:
    void init(int size) {
        fft_size = size;
        setup = pffft_new_setup(size, PFFFT_REAL);
        work = static_cast<float*>(pffft_aligned_malloc(size * sizeof(float)));
    }

    void forward(const float* input, float* output) {
        pffft_transform_ordered(setup, input, output, work, PFFFT_FORWARD);
    }

    void inverse(const float* input, float* output) {
        pffft_transform_ordered(setup, input, output, work, PFFFT_BACKWARD);
    }

    ~FastSpectrum() {
        if (setup) pffft_destroy_setup(setup);
        if (work) pffft_aligned_free(work);
    }
};
```

## Pulp Overlap

PFFFT overlaps with `signal/fft.hpp`. Use PFFFT when:

- You need maximum FFT throughput on a known power-of-two size
- You're doing heavy spectral processing (vocoder, convolution, analysis)
- Benchmarks show Pulp's built-in FFT is a bottleneck

Use Pulp's built-in FFT when:

- You need a simple FFT without managing PFFFT setup/teardown
- FFT performance is not the bottleneck
- You want zero additional dependencies

## License

PFFFT is distributed under the FFTPACK license from UCAR, in the project's
`LICENSE.txt` (copyrights: UCAR 2004, Julien Pommier 2013, Hayati Ayguen 2019,
Dario Mambro 2020). It is permissive and BSD-style, but it is not the SPDX
`BSD-3-Clause` text, so the registry records it as `LicenseRef-FFTPACK` and
`pulp add pffft` reports it for review. Its conditions:

- Source redistributions keep the copyright notices, the conditions and the
  disclaimer.
- Binary redistributions reproduce them in their documentation or other
  materials.
- The NCAR, UCAR and contributor names are not used to endorse or promote
  derived products without written permission.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`, and ship PFFFT's `LICENSE.txt`
with any binary:

```
PFFFT — FFTPACK license (UCAR), BSD-style — https://github.com/marton78/pffft
```
