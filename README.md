# Pulp

[![SDK](https://img.shields.io/github/v/release/danielraffel/pulp?label=SDK%20%2F%20CLI)](https://github.com/danielraffel/pulp/releases)
[![Claude plugin](https://img.shields.io/github/v/tag/danielraffel/pulp?filter=plugin-v*&label=Claude%20plugin)](https://github.com/danielraffel/pulp/tags)
[![Coverage](https://codecov.io/gh/danielraffel/pulp/branch/main/graph/badge.svg)](https://app.codecov.io/gh/danielraffel/pulp)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE.md)

A cross-platform audio plugin and application framework. MIT licensed, C++20 core, Swift on Apple, JS-scripted GPU UIs.

## Install

**macOS / Linux**

```bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
```

**Windows (PowerShell)**

```powershell
irm https://www.generouscorp.com/pulp/install.ps1 | iex
```

<details>
<summary><strong>Optional: Verify before installation</strong> (click to expand)</summary>

For an additional layer of security, you can download the installer and verify its SHA-256 checksum before running it:

**macOS / Linux**

```bash
(
  set -e
  curl -fLso install.sh https://www.generouscorp.com/pulp/install.sh
  curl -fLso SHA256SUMS https://raw.githubusercontent.com/danielraffel/pulp/main/tools/install/SHA256SUMS
  if command -v sha256sum >/dev/null; then
    sha256sum -c SHA256SUMS --ignore-missing
  else
    shasum -a 256 -c SHA256SUMS --ignore-missing
  fi
  sh install.sh
)
```

**Windows (PowerShell)**

```powershell
Invoke-WebRequest https://www.generouscorp.com/pulp/install.ps1 -OutFile install.ps1
Invoke-WebRequest https://raw.githubusercontent.com/danielraffel/pulp/main/tools/install/SHA256SUMS -OutFile SHA256SUMS
$expected = (Select-String -Path .\SHA256SUMS -Pattern ' install\.ps1$').Line.Split()[0]
$actual = (Get-FileHash .\install.ps1 -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw "Checksum mismatch for install.ps1" }
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

The checksum confirms that the installer you downloaded matches the script
Pulp publishes in source control. It does not replace reading a network script
before running it, but it avoids executing bytes that differ from the published
checksum.

Prefer not to run the installer? You can [build Pulp from source](#build-from-source).

GitHub's release verification can also check immutable releases and downloaded
release assets. This path requires the GitHub CLI:

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

</details>

### Optional: install the Claude Code plugin

```bash
claude plugin marketplace add danielraffel/pulp && claude plugin install pulp
```

<details>
<summary><strong>Learn more</strong></summary>

The CLI works great with any AI coding agent (Claude, Codex, Cursor). If you use
**Claude Code**, you can additionally install the
[Pulp plugin](docs/agent-integrations.md#claude-code-with-the-optional-plugin)
for slash-command shortcuts (`/build`, `/test`, `/ship`) and a native MCP
server.

Install the CLI first. The plugin's MCP server is `pulp-mcp`, which ships with
the CLI tarball (above) into `~/.pulp/bin/`. The plugin itself contains no
binaries; it locates `pulp-mcp` on `$PATH`. If you install the plugin before the
CLI, `/mcp` will report `pulp-mcp: cannot locate binary`. Run
`pulp doctor` to confirm `pulp-mcp` is found and matches your CLI version.

Building from a source checkout instead? The repo's project-local MCP server
uses the binary from your build tree. See [Build from source](#build-from-source).
No CLI install is needed.

</details>

See [docs/agent-integrations.md](docs/agent-integrations.md) for details on each agent path.

## Features

The full status inventory lives in the
[Capabilities Reference](docs/reference/capabilities.md).

- **Plugin formats:** VST3, Audio Unit v2, AUv3, CLAP, LV2, standalone, [WAMv2 and WebCLAP](docs/reference/web-plugin-support.md) browser/WASM targets, and optional AAX with a developer-supplied SDK.
- **GPU Accelerated Native UI:** scripted UIs render with [Dawn](https://dawn.googlesource.com/dawn) + [Skia](https://skia.org/) and QuickJS, with modern Flexbox/Grid layout through [Yoga](https://www.yogalayout.dev/).
- **Design import:** bring in screens from Claude Design, Figma `.fig` files, Figma REST/file JSON, React/JSX, Stitch, v0, and Pencil/OpenPencil, plus token systems from DESIGN.md exports. See [Importing Designs](docs/guides/importing-designs.md).
- **DSP:** processors, realtime [SignalGraph](docs/reference/signal-graph.md) node graphs, MIDI, audio file I/O, sidechains, headless processing, signed `.pulpbake` graph artifacts, and DSP hot reload for reloadable plugins. See [DSP Hot-Reload](docs/guides/dsp-hot-reload.md).
- **Testing and inspection:** [Audio Inspector](docs/guides/audio-inspector.md), optional [Audio Quality Lab](docs/guides/audio-quality-lab.md) with ViSQOL, PEAQ, AQUA-Tk, and aubio adapters, golden-file audio tests, headless screenshots, visual regression, motion traces, and MCP tools for agent-driven validation.
- **Migration and interop:** embed Pulp's GPU front end in existing [JUCE](docs/guides/juce-embed.md) or [iPlug2](docs/guides/juce-embed.md#iplug2) projects, or import existing projects into Pulp. The iPlug2 importer is public; the JUCE importer is in private beta.
- **Shipping:** macOS signing/notarization, DMG/PKG packaging, Sparkle appcasts, release assets, and `pulp ship` commands.
- **Automation:** GitHub Actions for the public matrix, optional [Shipyard](https://github.com/danielraffel/Shipyard) maintainer merge-on-green orchestration, optional [tartci](https://github.com/danielraffel/tartci) local VM lanes, and agent-facing CLI/MCP/Claude plugin surfaces.

## Create your first plugin

```bash
pulp create my-plugin && cd my-plugin && pulp run
```

Three commands from zero to a working native plugin for your platform.

## Documentation

**[Full Documentation](https://www.generouscorp.com/pulp/)** · **[Getting Started](https://www.generouscorp.com/pulp/getting-started.html)** · **[Capabilities](https://www.generouscorp.com/pulp/capabilities.html)** · **[Examples](https://www.generouscorp.com/pulp/examples-index.html)**

- [Getting Started](docs/guides/getting-started.md) — install, create, build, run
- [Capabilities Reference](docs/reference/capabilities.md) — full feature inventory with status
- [Module Reference](docs/reference/modules.md) — module-by-module API docs
- [CLI Reference](docs/reference/cli.md) — all `pulp` commands
- [CI & Local CI](docs/guides/local-ci.md) — local and cloud CI setup
- [Shipping Guide](docs/guides/shipping.md) — signing, notarization, packaging

<a id="build-from-source"></a>
<details>
<summary><strong>Build from source</strong> (contributors)</summary>

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh                                    # macOS / Linux bootstrap
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release  # configure
cmake --build build                            # build
ctest --test-dir build --output-on-failure    # test
```

On Windows, use the supported PowerShell bootstrap before configuring/building:

```powershell
git clone https://github.com/danielraffel/pulp.git
cd pulp
powershell -ExecutionPolicy Bypass -File .\setup.ps1
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

**Claude Code users** can additionally install the optional Pulp plugin for slash-command shortcuts and a native MCP server. The plugin extends Claude Code with `/build`, `/test`, `/create`, `/design`, `/ship`, `/import-design`, `/version`, `/upgrade` plus build/test/inspect MCP tools.

Full breakdown of which agent gets what: [docs/agent-integrations.md](docs/agent-integrations.md). Plugin-specific setup details: [docs/guides/claude-code-plugin.md](docs/guides/claude-code-plugin.md).

## FAQ

<details>
<summary><strong>How does Pulp handle minimum OS support (Windows, Linux, macOS)?</strong></summary>

Depends on the platform and on whether you enable V8 (optional; default is QuickJS/JSC). We track the lowest OS the prebuilts support and occasionally sit a step above for toolchain reasons. Numbers live at the source, not here — your floor is the highest of whatever you link:

| Floor | Where |
|---|---|
| Pulp, resolved (all platforms) | [`tools/deps/min_os.json`](tools/deps/min_os.json) |
| Skia + Dawn (always linked) | [skia-builder](https://github.com/danielraffel/skia-builder#minimum-os-versions-deployment-targets) |
| V8 (only with `PULP_JS_ENGINE=v8`) | [v8-builder](https://github.com/danielraffel/v8-builder#minimum-os-versions-deployment-targets--glibc-floor) |

</details>

## Contributing

Pulp is early and actively evolving — contributions and plugin-author
feedback are very welcome. People seeking to extend the Pulp framework should clone the repo. See
[Build from source](#build-from-source) to get set up.

### Workflow

Every change goes through: **branch → PR → CI → merge on green**. Pulp accepts third-party PRs.

```bash
# Optional maintainer-style flow from a source checkout
./tools/install-shipyard.sh --status      # compare installed vs pinned Shipyard
shipyard pr                               # create, track, validate, and merge on green
```

Pulp uses GitHub Actions for the public macOS, Linux, and Windows build matrix.
Maintainers use [Shipyard](https://github.com/danielraffel/Shipyard) on top of
that for exact-SHA validation, PR tracking, and merge-on-green. Source-checkout
contributors who want the same flow install the pinned Shipyard tool with
`./tools/install-shipyard.sh`; ordinary Pulp users do not need Shipyard or GitHub
CLI to create, build, run, or upgrade projects.

The maintainer's optional disposable local VM lanes, host leases, and timing
metrics are powered by [tartci](https://github.com/danielraffel/tartci) and
described by the parseable profile in
[`.shipyard/ci-profiles/normal-local-fast.toml`](.shipyard/ci-profiles/normal-local-fast.toml).
See [docs/guides/local-ci.md](docs/guides/local-ci.md) for setup and
[CONTRIBUTING.md](CONTRIBUTING.md) for the full contributor expectations.

### Security & CI policy

Pulp follows patterns documented in [Astral's open-source security post](https://astral.sh/blog/open-source-security-at-astral) and uses [Shipyard](https://github.com/danielraffel/Shipyard) to manage the merge and release controls where possible.

<details>
<summary><strong>Learn more</strong></summary>

- Pulp's `main` branch is protected: every change must go through a PR, and the stable `macos`, `linux`, `windows`, and `Enforce version & skill sync` checks must pass before merge.
- Release tags (`v*`) are signed by the release bot and protected from force-push, deletion, or update. Published GitHub Releases are immutable after publication.
- The repository default for the CI workflow token is read-only. Workflows that need write access, including release publishing, issue automation, freshness checks, and docs deploys, declare those scopes explicitly per job rather than inheriting a broad default.
- Pulp is currently a **single-maintainer project**, so the governance settings are tuned to a "solo profile". Settings will be revisited if/when Pulp gains co-maintainers; see [CONTRIBUTING.md](CONTRIBUTING.md) for the current contract.

</details>

## License

MIT. No royalties. No revenue thresholds. No copyleft.

See [LICENSE.md](LICENSE.md) for details. Third-party attribution in [NOTICE.md](NOTICE.md).
