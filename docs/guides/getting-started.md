# Getting Started

Build your first audio plugin with Pulp.

## Quick Install

Install the Pulp CLI with one command:

**macOS / Linux:**
```bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
```

**Windows (PowerShell):**
```powershell
irm https://www.generouscorp.com/pulp/install.ps1 | iex
```

Then create your first plugin:

```bash
pulp create my-plugin           # scaffolds, builds, and tests
pulp run                     # launch the standalone host
```

That's it — three commands from zero to a working plugin.

If you're developing Pulp itself and want to scaffold a framework example instead of a standalone product project, use:

```bash
pulp create debug-knob --in-tree
```

Pulp has two explicit creation/build modes:
- **SDK mode**: the default for external projects. The generated project builds against a pinned installed SDK artifact and records that expectation in `pulp.toml`.
- **Source-tree mode**: used for the Pulp repo itself and in-repo examples. These builds use the live checkout directly.

If you run `pulp create my-plugin` from inside a Pulp source checkout, Pulp still creates an SDK-mode product project by default. Unless you override it, that project is created next to the repo root rather than under `examples/`. In that case, Pulp reuses the pinned SDK dependencies from the checkout and caches a local SDK install instead of assuming a public SDK download is available.

## Building from Source

If you want to build the framework itself (to contribute, modify core code, or work without pre-built binaries):

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh
```

`setup.sh` handles first-time bootstrap: it checks prerequisites, verifies git-lfs is available and initialized, clones external SDKs, configures, builds, and runs tests. It works on macOS, Linux, and Windows (Git Bash/MSYS2).

Options:
- `./setup.sh --ci` — non-interactive mode for CI/automation
- `./setup.sh --deps-only` — bootstrap SDKs and dependencies without configuring/building
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

In SDK mode, `pulp doctor` validates the generated project's `pulp.toml`, installed SDK location, checkout hint, and build state. In source-tree mode, it validates the active Pulp checkout and pinned external SDKs instead.

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

## Step 6: Add a UI with Your First Knob

Pulp UIs are defined in JavaScript and hot-reloaded. Create a `ui/main.js` file next to your plugin source:

```js
// ui/main.js — Gain plugin UI

const root = createCol("root");
setFlex("root", "padding", 24);
setFlex("root", "gap", 16);
setFlex("root", "align_items", "center");
setBackground("root", "#1a1a2e");

// Title
const title = createLabel("title", "MyGain", "root");
setFontSize("title", 18);
setFontWeight("title", 700);
setTextColor("title", "#e0e0e0");

// Gain knob bound to parameter
const knob = createKnob("gain-knob", "root");
setFlex("gain-knob", "width", 80);
setFlex("gain-knob", "height", 80);
setValue("gain-knob", getParam("Gain"));
setLabel("gain-knob", "Gain");

on("gain-knob", "change", (v) => {
    setParam("Gain", v);
});

// Value readout
const readout = createLabel("readout", "0.0 dB", "root");
setFontSize("readout", 14);
setTextColor("readout", "#888888");

on("gain-knob", "change", (v) => {
    const db = -60 + v * 84; // map 0..1 to -60..+24 dB
    setText("readout", db.toFixed(1) + " dB");
});
```

Wire the UI into your plugin by adding the JS path to your CMakeLists.txt:

```cmake
pulp_add_plugin(MyGain
    FORMATS VST3 AU CLAP Standalone
    PLUGIN_NAME "MyGain"
    BUNDLE_ID "com.mycompany.mygain"
    MANUFACTURER "MyCompany"
    PLUGIN_CODE "MyGn"
    MANUFACTURER_CODE "MyCo"
    UI_SCRIPT "ui/main.js"
)
```

Rebuild and launch the standalone:

```bash
pulp build
./build/MyGain
```

You should see a window with a centered knob labeled "Gain" and a dB readout below it.

## Step 7: Hot-Reload

With the standalone running, edit `ui/main.js` — changes appear instantly. No rebuild needed.

Try changing the background color or adding a second knob. The HotReloader watches the `ui/` directory and re-evaluates your script whenever a file changes. Widget state (parameter bindings) is preserved across reloads.

```bash
# Terminal output when a reload happens:
# [pulp] hot-reload: ui/main.js changed, reloading...
# [pulp] hot-reload: 3 widgets restored
```

This makes UI iteration as fast as web development. Edit, save, see.

## Step 8: Apply a Theme

Pulp has three built-in themes: `dark` (default), `light`, and `pro_audio`. Switch themes from JS:

```js
setTheme("pro_audio");
```

Or define a custom theme in JSON and load it:

```json
{
    "colors": {
        "background": "#0d1117",
        "surface": "#161b22",
        "accent": "#58a6ff",
        "on_surface": "#c9d1d9",
        "primary": "#58a6ff"
    },
    "dimensions": {
        "corner_radius_sm": 4,
        "corner_radius_md": 8,
        "spacing_sm": 8,
        "spacing_md": 12
    }
}
```

Save this as `ui/theme.json` and load it from your script:

```js
// Themes cascade — child views inherit parent tokens
setBackground("root", getThemeColor("background"));
setTextColor("title", getThemeColor("on_surface"));
```

Design tokens resolve by walking up the view tree: child → parent → root → built-in fallback. This lets you override tokens for specific sections of your UI without affecting the rest.

## Next Steps

- **[From React/CSS Guide](from-react-css.md)** — mapping your web skills to Pulp
- **[Cookbook](cookbook.md)** — 10 recipes for common UI patterns
- **[API Reference](../reference/js-bridge.md)** — all 110+ JS bridge functions
- **[Design Tokens Guide](design-tokens.md)** — deep dive into theming
- **[Custom Rendering Guide](custom-rendering.md)** — Canvas, SDF, and WebGPU drawing
- **[Examples Gallery](examples.md)** — walkthroughs of all example plugins
- **[CMake Reference](../reference/cmake.md)** — full `pulp_add_plugin()` documentation
- **[CLI Reference](../reference/cli.md)** — all CLI commands
