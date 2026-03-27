# Getting Started

Build your first audio plugin with Pulp.

## Setup

The fastest way to get started:

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh
```

`setup.sh` checks prerequisites, installs git-lfs, clones external SDKs, configures, builds, and runs tests. It works on macOS, Linux, and Windows (Git Bash/MSYS2).

Options:
- `./setup.sh --ci` — non-interactive mode for CI/automation
- `./setup.sh --dry-run` — show what would be done without doing it

## Prerequisites

- **CMake 3.24+**
- **C++20 compiler** (Clang 15+, GCC 13+, MSVC 2022+)
- **git-lfs** — required for pre-built Skia GPU rendering binaries
  - macOS: `brew install git-lfs && git lfs install`
  - Linux: `sudo apt install git-lfs && git lfs install`
  - Windows: `winget install git-lfs`
- **macOS:** Xcode Command Line Tools (`xcode-select --install`)
- **Linux:** ALSA dev headers (`sudo apt install libasound2-dev`)
- Optional: [pluginval](https://github.com/Tracktion/pluginval) for VST3 validation
- Optional: [clap-validator](https://github.com/free-audio/clap-validator) for CLAP validation

### Environment Diagnosis

After building, use `pulp doctor` to check your environment:

```bash
pulp doctor             # show status of all checks
pulp doctor --fix       # auto-fix issues where possible
pulp doctor --ci        # CI mode — exit codes only, no prompts
pulp doctor --dry-run   # show what --fix would do
```

## Project Structure

A minimal Pulp plugin project:

```
my-plugin/
  CMakeLists.txt
  my_processor.hpp       # Your DSP code
  vst3_entry.cpp         # VST3 format entry
  clap_entry.cpp         # CLAP format entry
  au_v2_entry.cpp        # AU format entry (macOS)
  main.cpp               # Standalone entry
```

## Step 1: Write Your Processor

Subclass `pulp::format::Processor`:

```cpp
#pragma once
#include <pulp/format/processor.hpp>
#include <cmath>

enum Params : pulp::state::ParamID {
    kGain = 1,
};

class MyGain : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "MyGain",
            .manufacturer = "MyCompany",
            .bundle_id = "com.mycompany.mygain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kGain, .name = "Gain", .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {
        float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                output.channel(ch)[i] = input.channel(ch)[i] * gain;
    }
};

inline std::unique_ptr<pulp::format::Processor> create_my_gain() {
    return std::make_unique<MyGain>();
}
```

## Step 2: Create Format Entry Points

Each format needs a small entry file using Pulp's one-line macros.

**CLAP** (`clap_entry.cpp`):
```cpp
#include "my_processor.hpp"
#include <pulp/format/clap_entry.hpp>
PULP_CLAP_PLUGIN(create_my_gain)
```

**VST3** (`vst3_entry.cpp`):
```cpp
#include "my_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID kUID(0x12345678, 0x9ABCDEF0, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(kUID, "MyGain", Steinberg::Vst::PlugType::kFx,
                  "MyCompany", "1.0.0", "https://mycompany.com",
                  create_my_gain)
```

**AU v2** (`au_v2_entry.cpp`, macOS only):
```cpp
#include "my_processor.hpp"
#include "path/to/pulp/core/format/src/au_v2_adapter.cpp"
#include <pulp/format/au_v2_entry.hpp>
PULP_AU_PLUGIN(MyGainAU, create_my_gain)
```

## Step 3: Add CMakeLists.txt

```cmake
pulp_add_plugin(MyGain
    FORMATS VST3 AU CLAP Standalone
    PLUGIN_NAME "MyGain"
    BUNDLE_ID "com.mycompany.mygain"
    MANUFACTURER "MyCompany"
    PLUGIN_CODE "MyGn"
    MANUFACTURER_CODE "MyCo"
)
```

## Step 4: Build

```bash
cmake -B build
cmake --build build
```

Or use the CLI:

```bash
pulp build
```

Output locations:
- VST3: `build/VST3/MyGain.vst3`
- CLAP: `build/CLAP/MyGain.clap`
- AU: `build/AU/MyGain.component`
- Standalone: `build/MyGain`

## Step 5: Validate

```bash
pulp validate
```

Or manually:

```bash
pluginval --validate build/VST3/MyGain.vst3 --strictness-level 5
auval -v aufx MyGn MyCo
```

## Step 6: Install

Only after validation passes, copy to system plugin folders:

```bash
cp -R build/VST3/MyGain.vst3 ~/Library/Audio/Plug-Ins/VST3/
cp -R build/CLAP/MyGain.clap ~/Library/Audio/Plug-Ins/CLAP/
cp -R build/AU/MyGain.component ~/Library/Audio/Plug-Ins/Components/
```

Restart your DAW to scan the new plugins.

## Next Steps

- See `docs/guides/examples.md` for walkthroughs of all example plugins
- See `docs/reference/cmake.md` for full `pulp_add_plugin()` documentation
- See `docs/reference/cli.md` for all CLI commands
