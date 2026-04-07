# dr_libs

Single-file WAV, FLAC, and MP3 decoding.

| | |
|---|---|
| **License** | MIT-0 (public domain equivalent) |
| **URL** | https://github.com/mackron/dr_libs |
| **Version** | wav-0.14.5 |
| **Integration** | Header-only (single file per format) |
| **RT-safe** | No (file I/O) |
| **Platforms** | macOS (arm64), Windows (x64, arm64), Linux (x64, arm64) |

## What It Does

Zero-dependency single-header decoders for common audio formats:

- **dr_wav.h** — WAV reading and writing
- **dr_flac.h** — FLAC decoding
- **dr_mp3.h** — MP3 decoding

Each header is independent — include only what you need. Decode-only for FLAC and MP3 (WAV supports writing too).

**Note:** dr_libs uses per-header version tags (`wav-0.14.5`, `mp3-0.7.3`, `flac-0.13.3`). The registry pins to the WAV tag as the primary reference.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG        wav-0.14.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(dr_libs)

# Header-only — add include path
target_include_directories(MyPlugin PRIVATE
  ${dr_libs_SOURCE_DIR}
)
```

## Example Usage

```cpp
// In exactly ONE .cpp file, define the implementation:
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// Then use anywhere:
std::vector<float> load_wav(const char* path) {
    unsigned int channels, sample_rate;
    drwav_uint64 frame_count;
    float* data = drwav_open_file_and_read_pcm_frames_f32(
        path, &channels, &sample_rate, &frame_count, nullptr);

    std::vector<float> result(data, data + frame_count * channels);
    drwav_free(data, nullptr);
    return result;
}
```

## Pulp Overlap

None directly. CHOC provides WAV read/write via `choc::audio::AudioFileFormat`, which may be sufficient for basic WAV needs. dr_libs adds FLAC and MP3 decoding.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
dr_libs — MIT-0 — https://github.com/mackron/dr_libs
```
