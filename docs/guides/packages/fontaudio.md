# fontaudio

Audio-specific icon font with 200+ icons for plugin UIs.

| | |
|---|---|
| **License** | MIT |
| **URL** | https://github.com/fefanto/fontaudio |
| **Version** | 1.1 |
| **Integration** | Font file (header-only codepoints) |
| **RT-safe** | N/A (UI asset) |
| **Platforms** | All (macOS, Windows, Linux, iOS, WASM) |

## What It Does

A TrueType font containing 200+ audio-specific icons: knobs, faders, waveforms, speakers, MIDI connectors, plugin format logos, transport controls, and more. Designed specifically for audio software UIs.

The font file (`fontaudio.ttf`) can be loaded by any text rendering system. The companion header provides Unicode codepoint constants.

## CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
  fontaudio
  GIT_REPOSITORY https://github.com/fefanto/fontaudio.git
  GIT_TAG        1.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fontaudio)

# Copy the font file to your resources
file(COPY ${fontaudio_SOURCE_DIR}/font/fontaudio.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/resources)

# Include the codepoint header
target_include_directories(MyPlugin PRIVATE
  ${fontaudio_SOURCE_DIR}/src
)
```

## Example Usage

```cpp
#include "fontaudio/IconHelper.h"

// In your UI drawing code:
void draw_transport(pulp::Canvas& canvas) {
    // Load fontaudio.ttf as a font resource, then draw icons by codepoint
    canvas.set_font("fontaudio", 24.0f);
    canvas.draw_text(fontaudio::IconName::fad_play, 10, 10);
    canvas.draw_text(fontaudio::IconName::fad_stop, 40, 10);
    canvas.draw_text(fontaudio::IconName::fad_record, 70, 10);
}
```

## Pulp Overlap

None. Pulp does not ship an audio icon font.

## Attribution

Add to `DEPENDENCIES.md` and `NOTICE.md`:

```
fontaudio — MIT — https://github.com/fefanto/fontaudio
```
