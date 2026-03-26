# Pulp

A cross-platform framework for building audio plugins and applications.

**VST3 · Audio Unit · CLAP · Standalone** — more formats coming.
**macOS** first, Windows and Linux planned.

MIT-licensed. No royalties. No revenue thresholds. No copyleft.

**[Documentation](https://www.generouscorp.com/pulp/)** · **[Examples](https://www.generouscorp.com/pulp/examples-index.html)** · **[Capabilities](https://www.generouscorp.com/pulp/capabilities.html)**

## What Works Today

- **Three plugin formats** — VST3, AU v2, CLAP — validated with pluginval and auval
- **Effects and instruments** — both plugin types supported across all formats
- **Zero-boilerplate entry points** — one-line macros for each format
- **Parameter system** — thread-safe, automatable, with state serialization
- **Headless processing** — test and batch-process without a DAW
- **642 automated tests** — unit tests, golden-file audio tests, format validators, UI component tests, DSP tests

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

## Building

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

Requires CMake 3.24+ and a C++20 compiler (Clang 15+, GCC 13+, MSVC 2022+).

## Status

Under active development. Core framework operational — plugin formats, audio I/O, MIDI, parameter system, state serialization all working. GPU rendering (Dawn/Skia) and UI system are next.

See [VISION.md](VISION.md) for the full picture.

## License

[MIT](LICENSE.md)
