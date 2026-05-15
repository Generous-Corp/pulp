# Pulp

[![SDK](https://img.shields.io/github/v/release/danielraffel/pulp?label=SDK%20%2F%20CLI)](https://github.com/danielraffel/pulp/releases)
[![Claude plugin](https://img.shields.io/github/v/tag/danielraffel/pulp?filter=plugin-v*&label=Claude%20plugin)](https://github.com/danielraffel/pulp/tags)
[![Coverage](https://codecov.io/gh/danielraffel/pulp/branch/main/graph/badge.svg)](https://app.codecov.io/gh/danielraffel/pulp)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE.md)

A cross-platform audio plugin and application framework. MIT licensed, C++20 core, Swift on Apple, JS-scripted GPU UIs.

## Install

```bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
```

Then:

```bash
pulp create my-plugin && cd my-plugin && pulp run
```

Three commands from zero to a working plugin in all formats.

The CLI works great with any AI coding agent (Claude, Codex, Cursor). If you use **Claude Code**, you can additionally install the [Pulp plugin](docs/agent-integrations.md#claude-code-with-the-optional-plugin) for slash-command shortcuts (`/build`, `/test`, `/ship`) and a native MCP server:

```bash
claude plugin marketplace add danielraffel/pulp && claude plugin install pulp
```

See [docs/agent-integrations.md](docs/agent-integrations.md) for details on each agent path.

<details>
<summary><strong>What you get</strong> (click to expand)</summary>

## Features

**Plugin Formats**
- VST3, Audio Unit v2, AUv3, CLAP, LV2, Standalone
- WAM / WebCLAP browser targets (experimental)
- Optional AAX support via external SDK

**Create & Build**
- `pulp create` scaffolds a full plugin project with all format targets
- `pulp build` builds all formats in one pass
- `pulp test` runs the local test suite (`ctest` over the project's build directory); cross-platform execution and coverage roll up to CI — coverage is tracked at [codecov.io/gh/danielraffel/pulp](https://app.codecov.io/gh/danielraffel/pulp)
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
- [Shipyard](https://github.com/danielraffel/Shipyard) cross-platform CI: local Mac + SSH to Linux/Windows VMs
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

</details>

<details>
<summary><strong>Build from source</strong> (contributors)</summary>

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh                                    # bootstrap deps
cmake -S . -B build && cmake --build build    # build
ctest --test-dir build --output-on-failure    # test
```

**Prerequisites:** CMake 3.24+, C++20 compiler (Clang 15+, GCC 13+, MSVC 2022+), git-lfs.

</details>

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

## Agent integrations

Pulp's CLI works with any AI coding agent. Skills (`.agents/skills/`) are auto-loaded by Claude Code and Codex; `AGENTS.md` redirects Codex to the same `CLAUDE.md` Claude reads. No agent-specific install is required to use the CLI.

**Claude Code users** can additionally install the Pulp plugin for slash-command shortcuts and a native MCP server:

```bash
claude plugin marketplace add danielraffel/pulp && claude plugin install pulp
```

The plugin extends Claude Code with `/build`, `/test`, `/create`, `/design`, `/ship`, `/import-design`, `/version`, `/upgrade` plus an MCP server exposing build/test/inspect tools (highest value: the live-plugin inspector tools).

Full breakdown of which agent gets what: [docs/agent-integrations.md](docs/agent-integrations.md). Plugin-specific setup details: [docs/guides/claude-code-plugin.md](docs/guides/claude-code-plugin.md).

## Documentation

**[Full Documentation](https://www.generouscorp.com/pulp/)** · **[Getting Started](https://www.generouscorp.com/pulp/getting-started.html)** · **[Capabilities](https://www.generouscorp.com/pulp/capabilities.html)** · **[Examples](https://www.generouscorp.com/pulp/examples-index.html)**

- [Getting Started](docs/guides/getting-started.md) — install, create, build, run
- [Capabilities Reference](docs/reference/capabilities.md) — full feature inventory with status
- [Module Reference](docs/reference/modules.md) — module-by-module API docs
- [CLI Reference](docs/reference/cli.md) — all `pulp` commands
- [CI & Local CI](docs/guides/local-ci.md) — local and cloud CI setup
- [Shipping Guide](docs/guides/shipping.md) — signing, notarization, packaging

## Contributing

Every change goes through: **branch → CI → PR → merge on green**. Pulp accepts third-party PRs.

```bash
# Validate on macOS + Ubuntu + Windows before creating a PR
shipyard run                              # validate current branch
shipyard pr                               # create, track, validate, and merge on green
```

Pulp uses [Shipyard](https://github.com/danielraffel/Shipyard) for cross-platform CI — it validates on macOS (local), Linux (SSH), and Windows (SSH), and gates merges on per-SHA evidence across all three platforms. Public Pulp installs include the Pulp CLI only. Source-checkout contributors install the pinned Shipyard tool with `./tools/install-shipyard.sh` for the first install, then use `shipyard update` (Shipyard v0.55.0+) as the preferred in-tool upgrade path; GitHub CLI (`gh`) is only needed for GitHub-facing contributor workflows. See [docs/guides/local-ci.md](docs/guides/local-ci.md) for setup and [CONTRIBUTING.md](CONTRIBUTING.md) for the full contributor expectations.

### Security & CI policy

- Pulp's `main` branch is protected: every change must go through a PR, and a PR cannot merge until macOS, Linux, and Windows builds + tests are all green.
- Release tags (`v*`) are immutable — once published they cannot be force-pushed, deleted, or updated.
- The repository default for the CI workflow token is read-only. Workflows that need write access (the release workflow's `contents: write`, `auto-label-issues.yml`'s `issues: write`, `freshness-check.yml`'s `issues: write`, and `docs-deploy.yml`'s `pages: write` + `id-token: write`) declare those scopes explicitly per job rather than inheriting a broad default.
- Pulp is currently a **single-maintainer project**, so the governance settings are tuned to a "solo profile". Settings will be revisited if/when Pulp gains co-maintainers — see [CONTRIBUTING.md](CONTRIBUTING.md) for the current contract.

These practices follow patterns documented in [Astral's open-source security post](https://astral.sh/blog/open-source-security-at-astral) and are managed (or in the process of being managed) by [Shipyard](https://github.com/danielraffel/Shipyard).

## License

MIT. No royalties. No revenue thresholds. No copyleft.

See [LICENSE.md](LICENSE.md) for details. Third-party attribution in [NOTICE.md](NOTICE.md).
