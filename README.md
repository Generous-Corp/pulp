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

### Verification

As an additional layer of security, download the shell installer and verify its
SHA-256 checksum before running it:

```bash
curl -fLso install.sh https://www.generouscorp.com/pulp/install.sh
curl -fsSL https://raw.githubusercontent.com/danielraffel/pulp/main/tools/install/SHA256SUMS \
  | shasum -a 256 -c --ignore-missing
sh install.sh
```

This matters because `curl | sh` is convenient but trust-heavy. The verified
path lets you download the script, prove it matches Pulp's published checksum,
inspect it if you want, then run it.

GitHub's release verification can also check immutable releases and downloaded
release assets:

```bash
version=0.614.0
asset=pulp-darwin-arm64.tar.gz
gh release download "v${version}" -R danielraffel/pulp -p "$asset"
gh release verify "v${version}" -R danielraffel/pulp
gh release verify-asset "v${version}" "$asset" -R danielraffel/pulp
```

These layers answer different questions. A verified commit or tag means GitHub
verified a signature from a configured maintainer or release-bot signing key for
the source ref. A SHA-256 check means the bytes you downloaded match the bytes
Pulp published. Neither replaces reading code you are about to execute, but
together they make the source identity and downloaded artifact integrity much
clearer.

Then:

```bash
pulp create my-plugin && cd my-plugin && pulp run
```

Three commands from zero to a working native plugin for your platform.

The CLI works great with any AI coding agent (Claude, Codex, Cursor). If you use **Claude Code**, you can additionally install the [Pulp plugin](docs/agent-integrations.md#claude-code-with-the-optional-plugin) for slash-command shortcuts (`/build`, `/test`, `/ship`) and a native MCP server:

```bash
claude plugin marketplace add danielraffel/pulp && claude plugin install pulp
```

> **Install the CLI first.** The plugin's MCP server is `pulp-mcp`, which
> ships with the CLI tarball (above) into `~/.pulp/bin/`. The plugin itself
> contains no binaries — it locates `pulp-mcp` on `$PATH`. If you install
> the plugin before running `install.sh`, `/mcp` will report `pulp-mcp:
> cannot locate binary`. Run `pulp doctor` to confirm `pulp-mcp` is found
> and matches your CLI version.
>
> (Building from a source checkout instead? The repo's project-local MCP
> server uses the binary from your build tree — see
> [Build from source](#build-from-source). No CLI install needed.)

See [docs/agent-integrations.md](docs/agent-integrations.md) for details on each agent path.

<details>
<summary><strong>What you get</strong> (click to expand)</summary>

## Features

**Plugin Formats**
- VST3, Audio Unit v2, AUv3, CLAP, LV2, Standalone
- WAM / WebCLAP browser targets (experimental)
- Optional AAX support via external SDK

**Create & Build**
- `pulp create` scaffolds a full plugin project with the default native targets for your platform
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
- AI-assisted design tool (`pulp design`) for token, shader, and widget-style iteration
- Design import from Figma/Figma plugin, Stitch, v0, Pencil/OpenPencil, Claude Design, and DESIGN.md exports
- Agent-first motion observability — runtime traces, fixture record/replay/assert, visual analysis, Swift + Kotlin facades, scrubber, cost attribution ([guide](docs/guides/motion-observability.md))

**DSP & Audio**
- 30+ signal processors: oscillator, filters, compressor, reverb, delay, FFT, envelope follower, and more
- Thread-safe parameter system with atomic reads and gesture-aware bindings
- Headless audio processing for testing and offline rendering
- MIDI I/O with parsed short messages, SysEx, UMP helpers, and documented backend limits
- Audio file import for WAV, AIFF/AIFF-C, FLAC, MP3, Ogg Vorbis, and CoreAudio-backed Apple formats
- Audio file export for WAV/AIFF, with optional FLAC, MP3, AAC, and ALAC encoders via `pulp add`
- Multi-bus I/O with sidechain support

**Rendering**
- GPU rendering via Dawn + Skia Graphite (Metal, D3D12, Vulkan)
- CoreGraphics fallback on macOS/iOS
- SkSL runtime shaders for custom GPU effects
- SDF shape primitives for resolution-independent drawing
- Waveform and spectrum visualization widgets
- Headless screenshot capture for CI and validation via `capture_view()`

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

<a id="build-from-source"></a>
<details>
<summary><strong>Build from source</strong> (contributors)</summary>

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh                                    # bootstrap deps
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release  # configure
cmake --build build                            # build
ctest --test-dir build --output-on-failure    # test
```

**Prerequisites:** CMake 3.24+, C++20 compiler (Clang 15+, GCC 13+, MSVC 2022+), git-lfs.

**Claude Code users:** this checkout ships a project-local `.mcp.json` that
exposes a `pulp` MCP server (build/test/inspect tools) backed by the source
build. The build above produces `build/tools/mcp/pulp-mcp`, which the server
auto-detects — so enable the `pulp` MCP server *after* your first build.
Before that there's no binary to run and `/mcp` reports `pulp: failed to
connect`; no released-CLI or Claude-plugin install is needed for the
source-tree server. Run `pulp doctor` to confirm.

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

`pulp_add_plugin()` resolves the format-specific bundle metadata from that one
declaration, including AU/AUv3 component type, four-character codes, and plist
version values.

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

Pulp is early and actively evolving — contributions and plugin-author
feedback are very welcome. Most people working on Pulp clone the repo (to
extend the framework, or to build a plugin against source); see
[Build from source](#build-from-source) to get set up.

### Help wanted (no Pulp internals required)

Two high-impact ways to contribute that don't require digging into the framework:

- **[#3040 — Run PulpHostBench in your DAW](https://github.com/danielraffel/pulp/issues/3040)** — ~30 min per DAW. Install a small plugin, follow a numbered script, attach the resulting log. Graduates DAW-quirk rows from `Speculative` → `Validated`. Priority hosts without checked-in results: Logic, Live, Bitwig, Cubase, Studio One, Wavelab, FL Studio, and AUM.
- **[#3042 — Validate AAX with an Avid SDK setup](https://github.com/danielraffel/pulp/issues/3042)** — AAX is optional and requires a developer-supplied Avid SDK plus DigiShell/AAX Validator. If you have Avid access and can run local Pro Tools validation, comment so we can verify the adapter and host-quirk rows.

### Workflow

Every change goes through: **branch → CI → PR → merge on green**. Pulp accepts third-party PRs.

```bash
# Validate on macOS + Ubuntu + Windows before creating a PR
shipyard run                              # validate current branch
shipyard pr                               # create, track, validate, and merge on green
```

Pulp uses [Shipyard](https://github.com/danielraffel/Shipyard) for cross-platform CI — it validates on macOS (local), Linux (SSH), and Windows (SSH), and gates merges on per-SHA evidence across all three platforms. The maintainer's optional local VM lanes are powered by [tartci](https://github.com/danielraffel/tartci) and described by the parseable profile in [`.shipyard/ci-profiles/normal-local-fast.toml`](.shipyard/ci-profiles/normal-local-fast.toml). Public Pulp installs include the Pulp CLI only. Source-checkout contributors install the pinned Shipyard tool with `./tools/install-shipyard.sh` for the first install, then use `shipyard update` (Shipyard v0.55.0+) as the preferred in-tool upgrade path; GitHub CLI (`gh`) is only needed for GitHub-facing contributor workflows. See [docs/guides/local-ci.md](docs/guides/local-ci.md) for setup, optional Shipyard/tartci runner timing metrics, and [CONTRIBUTING.md](CONTRIBUTING.md) for the full contributor expectations.

### Security & CI policy

- Pulp's `main` branch is protected: every change must go through a PR, and a PR cannot merge until macOS, Linux, and Windows builds + tests are all green.
- Release tags (`v*`) are signed by the release bot and protected from force-push, deletion, or update. Published GitHub Releases are immutable after publication.
- The repository default for the CI workflow token is read-only. Workflows that need write access (the release workflow's `contents: write`, `auto-label-issues.yml`'s `issues: write`, `freshness-check.yml`'s `issues: write`, and `docs-deploy.yml`'s `pages: write` + `id-token: write`) declare those scopes explicitly per job rather than inheriting a broad default.
- Pulp is currently a **single-maintainer project**, so the governance settings are tuned to a "solo profile". Settings will be revisited if/when Pulp gains co-maintainers — see [CONTRIBUTING.md](CONTRIBUTING.md) for the current contract.

These practices follow patterns documented in [Astral's open-source security post](https://astral.sh/blog/open-source-security-at-astral) and are managed (or in the process of being managed) by [Shipyard](https://github.com/danielraffel/Shipyard).

## License

MIT. No royalties. No revenue thresholds. No copyleft.

See [LICENSE.md](LICENSE.md) for details. Third-party attribution in [NOTICE.md](NOTICE.md).
