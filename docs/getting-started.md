# Getting Started with Pulp

Build your first audio plugin in under 10 minutes.

## Prerequisites

- CMake 3.24+
- C++20 compiler (Clang 15+, GCC 13+, MSVC 2022+)
- macOS: Xcode Command Line Tools
- Optional: [pluginval](https://github.com/Tracktion/pluginval) for VST3 validation

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
  Info.plist.au           # AU bundle metadata
  Info.plist.vst3         # VST3 bundle metadata
```

## Step 1: Write Your Processor

Create `my_processor.hpp`:

```cpp
#pragma once
#include <pulp/format/processor.hpp>
#include <cmath>

namespace myplugin {

enum Params : pulp::state::ParamID {
    kGain   = 1,
    kBypass = 2,
};

class MyProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "MyPlugin",
            .manufacturer = "MyCompany",
            .bundle_id = "com.mycompany.myplugin",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .default_input_channels = 2,
            .default_output_channels = 2,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kGain, .name = "Gain", .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kBypass, .name = "Bypass", .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {
        if (state().get_value(kBypass) >= 0.5f) {
            // Pass-through
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
                auto in = input.channel(ch);
                auto out = output.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i)
                    out[i] = in[i];
            }
            return;
        }

        float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                out[i] = in[i] * gain;
        }
    }
};

inline std::unique_ptr<pulp::format::Processor> create_my_processor() {
    return std::make_unique<MyProcessor>();
}

} // namespace myplugin
```

## Step 2: Create Format Entry Points

Each plugin format needs a small entry point file. Thanks to Pulp's entry macros, these are minimal.

**CLAP** (`clap_entry.cpp`):
```cpp
#include "my_processor.hpp"
#include <pulp/format/clap_entry.hpp>
PULP_CLAP_PLUGIN(myplugin::create_my_processor)
```

**VST3** (`vst3_entry.cpp`):
```cpp
#include "my_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

// Generate a unique ID — use a UUID generator, then never change it
static const Steinberg::FUID kMyPluginUID(0x12345678, 0x9ABCDEF0, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(kMyPluginUID, "MyPlugin", Steinberg::Vst::PlugType::kFx,
                  "MyCompany", "1.0.0", "https://mycompany.com",
                  myplugin::create_my_processor)
```

**AU v2** (`au_v2_entry.cpp`) — macOS only:
```cpp
#include "my_processor.hpp"
#include "path/to/pulp/core/format/src/au_v2_adapter.cpp"
#include <pulp/format/au_v2_entry.hpp>

// Class name determines factory function: MyPluginAUFactory
// Must match factoryFunction in Info.plist.au
PULP_AU_PLUGIN(MyPluginAU, myplugin::create_my_processor)
```

## Step 3: Build

```bash
cmake -B build
cmake --build build
```

Output locations:
- VST3: `build/VST3/MyPlugin.vst3`
- CLAP: `build/CLAP/MyPlugin.clap`
- AU: `build/AU/MyPlugin.component`
- Standalone: `build/MyPlugin`

## Step 4: Validate

Before installing to system folders, validate:

```bash
# VST3
pluginval --validate build/VST3/MyPlugin.vst3 --strictness-level 5

# AU (macOS — installs to ~/Library/Audio/Plug-Ins/Components/ first)
cp -R build/AU/MyPlugin.component ~/Library/Audio/Plug-Ins/Components/
auval -v aufx MyPl MyCo
```

## Step 5: Install

Only after validation passes:

```bash
# VST3
cp -R build/VST3/MyPlugin.vst3 ~/Library/Audio/Plug-Ins/VST3/

# CLAP
cp -R build/CLAP/MyPlugin.clap ~/Library/Audio/Plug-Ins/CLAP/

# AU (already installed for validation)
```

Restart your DAW to scan the new plugins.

## Plugin Types

Pulp supports effects and instruments:

| Type | Category | AU Type | CLAP Feature | VST3 Category |
|------|----------|---------|--------------|---------------|
| Effect | `PluginCategory::Effect` | aufx | `audio-effect` | `Fx` |
| Instrument | `PluginCategory::Instrument` | aumu | `instrument` | `Instrument\|Synth` |

For instruments, use `PULP_AU_INSTRUMENT()` instead of `PULP_AU_PLUGIN()` and include `au_v2_instrument.cpp` instead of `au_v2_adapter.cpp`.

## Headless Processing

For testing and batch processing without a DAW:

```cpp
#include <pulp/format/headless.hpp>
#include "my_processor.hpp"

pulp::format::HeadlessHost host(myplugin::create_my_processor);
host.prepare(48000.0, 512);
host.state().set_value(1, -6.0f); // Set gain to -6 dB

pulp::audio::Buffer<float> in(2, 512), out(2, 512);
// ... fill input buffer ...

const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 512);
auto out_view = out.view();
host.process(out_view, in_view);
```

## Next Steps

- Add more parameters to your processor
- Implement state save/load (automatic via `StateStore::serialize()`)
- Add MIDI support (`accepts_midi = true` in descriptor)
- Test in multiple DAWs
