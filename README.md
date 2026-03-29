# Pulp

A cross-platform framework for building audio plugins and applications.

**VST3 · Audio Unit · CLAP · LV2 · Standalone** — plus WAM/WebCLAP for the browser.
**macOS** primary, **Windows** and **Linux** supported.

MIT-licensed. No royalties. No revenue thresholds. No copyleft.

**[Documentation](https://www.generouscorp.com/pulp/)** · **[Examples](https://www.generouscorp.com/pulp/examples-index.html)** · **[Capabilities](https://www.generouscorp.com/pulp/capabilities.html)**

## What Works Today

- **Six plugin formats** — VST3, AU v2, AUv3, CLAP, LV2, WAM/WebCLAP — with format-specific validation tests
- **Effects and instruments** — both plugin types supported across all formats
- **Zero-boilerplate entry points** — one-line macros for each format
- **Parameter system** — thread-safe, automatable, with state serialization
- **Headless processing** — test and batch-process without a DAW
- **646 automated tests** — unit tests, golden-file audio tests, format validation, UI component tests, DSP tests

### Example: Create a Plugin

Write your processor:

```cpp
class MyGain : public pulp::format::Processor {
    void process(BufferView<float>& out, const BufferView<const float>& in, ...) override {
        float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch)
            for (std::size_t i = 0; i < out.num_samples(); ++i)
                out.channel(ch)[i] = in.channel(ch)[i] * gain;
    }
};
```

Add format entry points (one line each):

```cpp
// CLAP
PULP_CLAP_PLUGIN(create_my_gain)

// VST3
PULP_VST3_PLUGIN(kUID, "MyGain", Vst::PlugType::kFx, "Vendor", "1.0.0", "url", create_my_gain)

// AU
PULP_AU_PLUGIN(MyGainAU, create_my_gain)
```

Add to CMakeLists.txt:

```cmake
pulp_add_plugin(MyGain
    FORMATS VST3 AU CLAP Standalone
    PLUGIN_NAME "MyGain"  BUNDLE_ID "com.example.mygain"
    MANUFACTURER "Example"  PLUGIN_CODE "MyGn"  MANUFACTURER_CODE "Exmp"
)
```

Build all formats:

```bash
cmake -B build && cmake --build build
```

See the [Getting Started guide](https://www.generouscorp.com/pulp/getting-started.html) for the full walkthrough.

## Quick Start

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh
```

`setup.sh` handles everything: checks prerequisites, installs git-lfs, clones external SDKs (VST3, AudioUnit), configures, builds, and runs tests. Works on macOS, Linux, and Windows (Git Bash/MSYS2).

### Prerequisites

- **CMake 3.24+**
- **C++20 compiler** — Clang 15+, GCC 13+, or MSVC 2022+
- **git-lfs** — required for pre-built Skia binaries (`brew install git-lfs` / `apt install git-lfs`)
- **macOS:** Xcode Command Line Tools (`xcode-select --install`)
- **Linux:** ALSA dev headers (`sudo apt install libasound2-dev`)

### Building Manually

If you prefer manual setup over `setup.sh`:

```bash
git lfs install && git lfs pull              # pull Skia binaries
git clone --depth 1 --recursive \
    https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
cmake -B build && cmake --build build
ctest --test-dir build
```

### Create a New Plugin

After building, use `pulp create` to scaffold a plugin project:

```bash
./build/tools/cli/pulp create "My Gain"                    # effect (default)
./build/tools/cli/pulp create "My Synth" --type instrument  # instrument
./build/tools/cli/pulp create "My App" --type app           # standalone application
./build/tools/cli/pulp create "My Project" --type bare      # minimal skeleton
```

This checks your environment, scaffolds from templates, builds, runs tests, and reports artifact locations. Formats are platform-gated (macOS gets VST3+AU+CLAP+Standalone by default). Use `--no-interactive` for CI/scripting. Works both inside the repo and as a standalone project (downloads SDK automatically).

### Environment Diagnosis

After building, use `pulp doctor` to check your environment:

```bash
./build/tools/cli/pulp doctor        # show status of all checks
./build/tools/cli/pulp doctor --fix  # auto-fix what it can
./build/tools/cli/pulp run           # launch a standalone plugin
./build/tools/cli/pulp upgrade       # update CLI to latest version
```

## Status

Under active development. Core framework operational — plugin formats, audio I/O, MIDI, parameter system, state serialization, GPU rendering (Dawn/Skia/Graphite), UI widget library, and 30+ DSP processors all working. macOS is the primary platform; Windows and Linux have audio/MIDI I/O and CI coverage.

See [VISION.md](VISION.md) for the full picture.

## License

[MIT](LICENSE.md)
