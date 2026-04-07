# Pulp

A cross-platform audio plugin and application framework. MIT licensed, C++20 core, Swift on Apple, JS-scripted GPU UIs.

## Features

**Plugin Formats**
- VST3, Audio Unit v2, AUv3, CLAP, LV2, Standalone
- WAM / WebCLAP browser targets (experimental)
- Optional AAX support via external SDK

**Create & Build**
- `pulp create` scaffolds a full plugin project with all format targets
- `pulp build` builds all formats in one pass
- `pulp test` runs 1,622+ automated tests
- `pulp run` launches standalone for quick iteration
- `pulp doctor` checks your environment and reports missing dependencies

**UI & Design**
- JS-scripted GPU UIs via Dawn, Skia, and QuickJS
- CSS Flexbox + Grid layout engine with 81 CSS properties
- Responsive design via `matchMedia` queries
- 15+ widgets: Knob, Fader, Toggle, TextEditor, ComboBox, XYPad, WaveformView, SpectrumView, and more
- Hot reload for rapid UI iteration (standalone host; plugin host support planned)
- Screenshot capture and headless rendering
- Component inspector for debugging view hierarchy
- AI design tool (`pulp design`) for generating UIs from prompts
- Import from Figma, Stitch, and Pencil via MCP

**DSP & Audio**
- 30+ signal processors: oscillator, filters, compressor, reverb, delay, FFT, envelope follower, and more
- Thread-safe parameter system with atomic reads and gesture-aware bindings
- Headless audio processing for testing and offline rendering
- MIDI I/O with full event parsing
- Audio file read/write (WAV, AIFF, FLAC, MP3)
- Multi-bus I/O with sidechain support

**Rendering**
- GPU rendering via Dawn + Skia Graphite (Metal, D3D12, Vulkan)
- CoreGraphics fallback on macOS/iOS
- SkSL runtime shaders for custom GPU effects
- SDF shape primitives for resolution-independent drawing
- Waveform and spectrum visualization widgets
- Headless screenshot capture for CI and validation

**Shipping**
- Code signing and notarization on macOS
- DMG and PKG installer creation
- Appcast generation for Sparkle auto-updates
- Semantic versioning baked into the build
- `pulp ship sign`, `pulp ship package`, `pulp ship check`

**CI & Automation**
- Local CI runner (`pulp ci-local`) validates on Mac + remote VMs over SSH
- Namespace.so cloud CI integration
- GitHub Actions CI for the full build matrix
- MCP server for repo automation and tool integration
- Claude Code plugin for AI-assisted development

**Platform Support**
- macOS — primary, ARM64 (Apple Silicon)
- Windows — experimental, CI-backed
- Linux — experimental, CI-backed
- iOS — experimental
- Web/WASM — experimental

**DSL Support**
- FAUST — offline codegen into native Processor instances
- Cmajor — external toolchain with validation and code generation
- JSFX — bounded subset support via QuickJS
- Three.js bridge for 3D rendering in Dawn without a browser

## Quick Start

**Install:**
```bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
```

**Create and run your first plugin:**
```bash
pulp create my-plugin
cd my-plugin
pulp run
```

Three commands from zero to a working plugin in all formats.

### From Source

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh                                    # bootstrap deps
cmake -S . -B build && cmake --build build    # build
ctest --test-dir build --output-on-failure    # test
```

**Prerequisites:** CMake 3.24+, C++20 compiler (Clang 15+, GCC 13+, MSVC 2022+), git-lfs.

## Example

A Pulp plugin is a `Processor` subclass. Format adapters handle the rest.

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
PULP_CLAP_PLUGIN(create_my_gain)
PULP_VST3_PLUGIN(kUID, "MyGain", Vst::PlugType::kFx, "Vendor", "1.0.0", "url", create_my_gain)
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

## Claude Code Plugin

Pulp ships with a Claude Code plugin for the full development lifecycle.

**Install:**
```bash
claude plugin marketplace add danielraffel/pulp
claude plugin install pulp
```

The plugin also activates automatically when you open the Pulp repo in Claude Code.

Use slash commands directly in your Claude Code session:

- `/build` — configure, build, and test
- `/test` — run the test suite with filtering
- `/create` — scaffold a new plugin project
- `/design` — generate UI from a text prompt
- `/ship` — sign, package, and notarize
- `/import-design` — import from Figma, Stitch, or Pencil

Available skills: `ci` (PR creation, local/cloud CI, merge workflow), `import-design` (design tool integration), `aax` (AAX format support), `engine` (audio engine internals), `webview-ui` (web-based UI targets).

See [docs/guides/claude-code-plugin.md](docs/guides/claude-code-plugin.md) for setup.

## Documentation

**[Full Documentation](https://www.generouscorp.com/pulp/)** · **[Getting Started](https://www.generouscorp.com/pulp/getting-started.html)** · **[Capabilities](https://www.generouscorp.com/pulp/capabilities.html)** · **[Examples](https://www.generouscorp.com/pulp/examples-index.html)**

- [Getting Started](docs/guides/getting-started.md) — install, create, build, run
- [Capabilities Reference](docs/reference/capabilities.md) — full feature inventory with status
- [Module Reference](docs/reference/modules.md) — module-by-module API docs
- [CLI Reference](docs/reference/cli.md) — all `pulp` commands
- [CI & Local CI](docs/guides/local-ci.md) — local and cloud CI setup
- [Shipping Guide](docs/guides/shipping.md) — signing, notarization, packaging

## Contributing

Every change goes through: **branch → local CI → PR → GitHub Actions → merge on green**.

```bash
# Validate on macOS + Ubuntu + Windows before creating a PR
python3 tools/local-ci/local_ci.py run <branch>

# Or use the CI skill: "ship this"
```

Local CI runs on your Mac and validates cross-platform via SSH to Ubuntu/Windows VMs. GitHub Actions runs automatically on every PR as a backup gate. See [docs/guides/local-ci.md](docs/guides/local-ci.md) for setup.

## License

MIT. No royalties. No revenue thresholds. No copyleft.

See [LICENSE.md](LICENSE.md) for details. Third-party attribution in [NOTICE.md](NOTICE.md).
