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

The installer installs Pulp CLI artifacts only. It does not install Shipyard
or GitHub CLI (`gh`); ordinary Pulp users do not need either tool to create,
build, run, or upgrade a plugin project.

### Claude Code plugin: install the CLI first

If you plan to use the Pulp [Claude Code plugin](claude-code-plugin.md), run
`install.sh` **before** `claude plugin install pulp`. The plugin's MCP server
is `pulp-mcp`, which ships in the CLI tarball into `~/.pulp/bin/` — the
plugin itself ships no binaries and locates `pulp-mcp` on `$PATH`. Reversing
the order leaves the plugin's `/mcp` panel reporting `pulp-mcp: cannot locate
binary` on first connect.

```bash
# 1. CLI first (drops pulp-mcp into ~/.pulp/bin/ and onto PATH).
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh

# 2. Plugin second (its MCP launcher resolves pulp-mcp from PATH).
claude plugin marketplace add danielraffel/pulp && claude plugin install pulp

# 3. Verify the wiring.
pulp doctor          # expects: pulp-mcp — <path> (pulp-mcp <version>)
```

`pulp upgrade --install` carries `pulp-mcp` forward to `~/.pulp/bin/` on
upgrade, so the plugin keeps working across CLI bumps without reinstalling
the plugin. Drift between an older installed `pulp-mcp` and a newer CLI is
advisory — `pulp doctor` reports the version pairing so you can see it.

Then create your first plugin:

```bash
pulp create my-plugin           # scaffolds, builds the default native outputs, and tests
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

When a newer Pulp release is available, use `pulp upgrade` to update
the installed CLI/SDK tool. Then, from an SDK-mode project, use
`pulp project bump` to move that project's `pulp.toml` `sdk_version`
and `find_package(Pulp ...)` pin together. This does not change the
project's own `project(... VERSION ...)` product version.

If you run `pulp create my-plugin` from inside a Pulp source checkout, Pulp still creates an SDK-mode product project by default. Unless you override it, that project is created next to the repo root rather than under `examples/`. In that case, Pulp reuses the pinned SDK dependencies from the checkout and caches a local SDK install instead of assuming a public SDK download is available.

What `pulp create` proves by default:
- native plugin/app outputs for the current platform
- generated tests passing
- a usable standalone app target for app-capable templates (`.app` on macOS, executable elsewhere)

What it does **not** emit by default:
- browser targets such as WAM/WebCLAP
- optional AAX outputs unless the AAX SDK is already configured

Fresh create/build proofs were re-run from clean `PULP_HOME` / project roots
on macOS, Ubuntu, and Windows. The default generated project emitted only
native outputs for the host platform plus the platform-native standalone target:

- macOS: `VST3`, `AU`, `CLAP`, and `Standalone` (`.app` bundle)
- Ubuntu/Linux: `VST3`, `CLAP`, `LV2`, and `Standalone`
- Windows: `VST3`, `CLAP`, and `Standalone`

No browser outputs (`.wasm`, web bundles, WAM/WebCLAP artifacts) were emitted
unless those formats were requested explicitly.

## Building from Source

If you want to build the framework itself (to contribute, modify core code, or work without pre-built binaries):

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
```

**macOS / Linux**
```bash
./setup.sh
```

**Windows (PowerShell)**
```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```

`setup.sh` is the shared bootstrap implementation, but the supported Windows entrypoint is `setup.ps1`. The wrapper imports the Visual Studio C++ environment, uses Git Bash from Git for Windows, and temporarily maps the repo to a short drive alias so first-time dependency bootstrap does not trip Windows path-length limits.

Options:
- `./setup.sh --ci` — non-interactive mode for CI/automation
- `./setup.sh --deps-only` — bootstrap SDKs and dependencies without configuring/building
- `./setup.sh --dry-run` — show what would be done without doing it
- `powershell -ExecutionPolicy Bypass -File .\setup.ps1 --ci` — Windows non-interactive bootstrap
- `powershell -ExecutionPolicy Bypass -File .\setup.ps1 --deps-only` — Windows dependency-only bootstrap

## Prerequisites

- **git**
- **CMake 3.24+**
- **C++20 compiler** (Clang 15+, GCC 13+, MSVC 2022+)
- **git-lfs** — required for pre-built Skia GPU rendering binaries
  - macOS: `brew install git-lfs && git lfs install`
  - Linux: `sudo apt install git-lfs && git lfs install`
    - if you installed `git-lfs` with a per-user tool such as `~/.local/bin/git-lfs`, make sure that location is also on the non-interactive PATH used by SSH/CI
  - Windows: install Git for Windows, then run `git lfs install`
- **Windows:** Git for Windows (the supported wrapper uses its bundled Git Bash)
- **macOS:** Xcode Command Line Tools (`xcode-select --install`)
- **Linux:** ALSA dev headers (`sudo apt install libasound2-dev`)
  - For a fresh Ubuntu machine that needs full native UI/GPU-capable builds, install the desktop/dev package set once:
    ```bash
    sudo apt install libx11-dev libxext-dev libxrandr-dev libxrender-dev libxfixes-dev libxi-dev libxinerama-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libgbm-dev libdrm-dev libdbus-1-dev
    ```
- Optional: [pluginval](https://github.com/Tracktion/pluginval) for VST3 validation
- Optional: [clap-validator](https://github.com/free-audio/clap-validator) for CLAP validation
- Optional on macOS/Windows: AAX SDK + DigiShell/AAX Validator for local AAX builds and validation. See [AAX Setup](aax.md)

### Environment Diagnosis

After building, use `pulp doctor` to check your environment:

```bash
pulp doctor             # show status of all checks
pulp doctor --fix       # auto-fix issues where possible
pulp doctor --ci        # CI mode — exit codes only, no prompts
pulp doctor --dry-run   # show what --fix would do
```

In SDK mode, `pulp doctor` validates the generated project's `pulp.toml`, installed SDK location, checkout hint, and build state. In source-tree mode, it validates the active Pulp checkout and pinned external SDKs instead.

### Cross-Platform CI (Optional)

If you want to validate builds on macOS, Ubuntu, and Windows before merging, see the [Local CI guide](local-ci.md) — it covers SSH key setup for Windows and Linux VMs, host configuration, and the `pulp ci-local` runner.

## Project Structure

A minimal Pulp plugin project:

```
my-plugin/
  CMakeLists.txt
  my_processor.hpp       # Your DSP code
  vst3_entry.cpp         # VST3 format entry
  clap_entry.cpp         # CLAP format entry
  au_v2_entry.cpp        # AU format entry (macOS)
  aax_entry.cpp          # AAX format entry (optional, macOS/Windows)
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

**AAX** (`aax_entry.cpp`, optional on macOS/Windows):
```cpp
#include "my_processor.hpp"
#include <pulp/format/aax_entry.hpp>
PULP_AAX_PLUGIN(create_my_gain)
```

## Step 3: Add CMakeLists.txt

```cmake
pulp_add_plugin(MyGain
    FORMATS VST3 AU CLAP AAX Standalone
    PLUGIN_NAME "MyGain"
    BUNDLE_ID "com.mycompany.mygain"
    MANUFACTURER "MyCompany"
    PLUGIN_CODE "MyGn"
    MANUFACTURER_CODE "MyCo"
    AAX_PRODUCT_CODE "MyGa"
    AAX_NATIVE_CODE "MyGn"
)
```

If you want AAX, enable it explicitly at configure time with a developer-supplied
SDK:

```bash
cmake -S . -B build -DPULP_ENABLE_AAX=ON -DPULP_AAX_SDK_DIR=$HOME/SDKs/avid/aax-sdk/current
```

On Linux and Ubuntu, AAX is unsupported and must be omitted.

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
- VST3:
  - macOS: `build/VST3/MyGain.vst3`
  - Linux: `build/VST3/libMyGain.so`
  - Windows: `build/VST3/Debug/MyGain.dll`
- CLAP:
  - macOS / Linux: `build/CLAP/MyGain.clap`
  - Windows: `build/CLAP/Debug/MyGain.clap`
- LV2 (Linux): `build/LV2/MyGain.lv2/`
- AU (macOS): `build/AU/MyGain.component`
- AAX (macOS/Windows, opt-in): `build/AAX/MyGain.aaxplugin`
- Standalone:
  - macOS / Linux: `build/MyGain`
  - Windows: `build/Debug/MyGain.exe`

`pulp create` still proves the native outputs were built; the exact on-disk path depends on the generator and configuration.

## Step 5: Validate

```bash
pulp validate
```

Or manually:

```bash
pluginval --validate build/VST3/MyGain.vst3 --strictness-level 5
auval -v aufx MyGn MyCo
```

If DigiShell + AAX Validator are installed, `pulp validate` also validates any
built `.aaxplugin` bundles. See [AAX Setup](aax.md) for the download and local
installation workflow.

## Step 6: Install

Only after validation passes, copy to system plugin folders:

```bash
cp -R build/VST3/MyGain.vst3 ~/Library/Audio/Plug-Ins/VST3/
cp -R build/CLAP/MyGain.clap ~/Library/Audio/Plug-Ins/CLAP/
cp -R build/AU/MyGain.component ~/Library/Audio/Plug-Ins/Components/
```

Restart your DAW to scan the new plugins.

## Step 7: Add a UI with Your First Knob

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

## Step 8: Hot-Reload

With the standalone running on macOS, edit `ui/main.js` — changes appear instantly. No rebuild needed.

Try changing the background color or adding a second knob. The scripted standalone host watches `ui/main.js` and re-evaluates your script whenever it changes. Widget values are restored across reloads. JavaScript heap state is not preserved.

```bash
# Terminal output when a reload happens:
# [pulp] hot-reload: ui/main.js changed, reloading...
# [pulp] hot-reload: 3 widgets restored
```

This is currently the supported live-reload path for scripted UI. Plugin targets can load the same `UI_SCRIPT`, but live JS/theme reload is not yet a cross-host guarantee.

`pulp create` verifies the fresh project path by scaffolding, configuring, building the default native outputs for the platform, and running the generated tests. Use `pulp build` after `pulp create` when you want to rebuild, select explicit targets, or materialize optional deliverables after changing configuration. Browser targets such as WAM/WebCLAP are separate lanes and are not emitted by default by the generated project.

## Step 9: Apply a Theme

Pulp has three built-in themes: `dark` (default), `light`, and `pro_audio`. Switch themes from JS:

```js
setTheme("pro_audio");
```

Or define a custom theme in JSON:

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

Save this as `ui/theme.json` next to `ui/main.js`:

```js
// Themes cascade — child views inherit parent tokens
setBackground("root", getThemeColor("background"));
setTextColor("title", getThemeColor("on_surface"));
```

Scripted UI sessions automatically apply a sibling `theme.json` when the UI loads. In the macOS standalone hot-reload lane, edits to `theme.json` are also picked up live. Design tokens resolve by walking up the view tree: child → parent → root → built-in fallback. This lets you override tokens for specific sections of your UI without affecting the rest.

## Next Steps

- **[From React/CSS Guide](from-react-css.md)** — mapping your web skills to Pulp
- **[Cookbook](cookbook.md)** — 10 recipes for common UI patterns
- **[API Reference](../reference/js-bridge.md)** — all 110+ JS bridge functions
- **[Design Tokens Guide](design-tokens.md)** — deep dive into theming
- **[Custom Rendering Guide](custom-rendering.md)** — Canvas, SDF, and WebGPU drawing
- **[Examples Gallery](examples.md)** — walkthroughs of all example plugins
- **[CMake Reference](../reference/cmake.md)** — full `pulp_add_plugin()` documentation
- **[CLI Reference](../reference/cli.md)** — all CLI commands
