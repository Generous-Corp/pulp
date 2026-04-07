# Package Integration Guides

Pulp's built-in `core/signal/` subsystem covers 30+ DSP processors. These guides help you add third-party libraries for capabilities beyond what ships natively.

## Before You Add a Package

1. **Check built-ins first.** Pulp may already cover your need — see `docs/reference/modules.md` for the signal subsystem inventory.
2. **License matters.** Only MIT/BSD/Apache/ISC/zlib/BSL/public-domain libraries are compatible with Pulp's MIT license. The package registry enforces this.
3. **Platform coverage.** If your plugin targets macOS + Windows + Linux, prefer libraries that work on all three. Each guide lists platform support.

## Categories

### DSP (Filters, Effects, Analysis, Synthesis)

| Package | What It Provides | Integration |
|---------|-----------------|-------------|
| [Signalsmith Stretch](signalsmith-stretch.md) | Polyphonic pitch/time stretching | Header-only |
| [Signalsmith DSP](signalsmith-dsp.md) | Filters, FFT, envelopes, spectral tools | Header-only |
| [Q (Cycfi)](cycfi-q.md) | Pitch detection via bitstream autocorrelation | CMake |
| [PFFFT](pffft.md) | SIMD-optimized FFT | CMake |
| [DaisySP](daisysp.md) | Physical modeling, effects, drum synthesis | CMake |

### Audio I/O (File Formats, Resampling)

| Package | What It Provides | Integration |
|---------|-----------------|-------------|
| [dr_libs](dr-libs.md) | WAV, FLAC, MP3 decoding (single-header) | Header-only |
| [libsamplerate](libsamplerate.md) | High-quality sample rate conversion | CMake |
| [r8brain-free-src](r8brain-free-src.md) | Audiophile-grade resampling | CMake |

### Machine Learning

| Package | What It Provides | Integration |
|---------|-----------------|-------------|
| [RTNeural](rtneural.md) | Real-time neural network inference | CMake |

### UI

| Package | What It Provides | Integration |
|---------|-----------------|-------------|
| [fontaudio](fontaudio.md) | Audio-specific icon font (200+ icons) | Font file |

## Search Tips

Looking for a capability? Use `pulp search` (Phase 2) or check the registry directly:

```bash
python3 tools/packages/validate_registry.py  # verify registry integrity
cat tools/packages/registry.json | python3 -m json.tool  # browse packages
```

## Adding a Library Manually

Each guide provides the exact CMake code. The general pattern:

```cmake
include(FetchContent)
FetchContent_Declare(
  <name>
  GIT_REPOSITORY https://github.com/<org>/<repo>.git
  GIT_TAG        <version>
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(<name>)
target_link_libraries(MyPlugin PRIVATE <target>)
```

Header-only libraries can alternatively be added with `target_include_directories` after fetching.
