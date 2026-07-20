# Capability Claim Audit Baseline

**Last updated:** 2026-04-06

This page is the Phase 15 baseline for the repo's broad "agent-native" and
platform/runtime claims. It is not a roadmap pitch. It is the current
classification for the main public story on this branch.

Use this together with:

- [Capabilities Reference](capabilities.md)
- [`docs/status/support-matrix.yaml`](../status/support-matrix.yaml)
- Phase 15 capability validation (internal planning)

## Claim Baseline

| Claim | Status | Evidence | Notes / Follow-up |
|---|---|---|---|
| Fast agent feedback loops | partial | [local-ci](../guides/local-ci.md), [capabilities](capabilities.md#tooling-cli) | Strong local iteration exists, but narrower CI reruns/stage reuse are still open in [#115](https://github.com/Generous-Corp/pulp/issues/115), [#117](https://github.com/Generous-Corp/pulp/issues/117), and [#118](https://github.com/Generous-Corp/pulp/issues/118). |
| Screenshot capture and visual validation | usable / partial | [capabilities](capabilities.md#view-ui), `design_debug_harness` in [`support-matrix.yaml`](../status/support-matrix.yaml) | Headless screenshots and design-debug flows are real. The unified "one workflow for screenshots, validators, debugger hooks, and automation" story is still partial. |
| MCP-native plugin/app control | planned / partial substrate | `tools/mcp/pulp_mcp.cpp`, `tools/plugin-cli/plugin_cli.hpp` | Pulp ships a repo-level MCP server and a plugin CLI harness pattern. It does **not** yet ship a universal per-plugin or per-app MCP control contract. Active follow-up: [#142](https://github.com/Generous-Corp/pulp/issues/142). |
| Hot reload | usable | [capabilities](capabilities.md#view-ui), [getting-started](../guides/getting-started.md) | JS/UI and theme hot reload are real on the validated macOS standalone path. Plugin host and cross-platform reload behavior is not yet validated across all targets. |
| "Parameterize everything" | partial | [capabilities](capabilities.md#state-and-automation) | State/parameter surfaces are strong, but there is no universal capability schema proving every automation surface is consistently exposed. |
| DSLs as first-class citizens | usable | [FAUST guide](../guides/faust.md), [Cmajor guide](../guides/cmajor.md), [JSFX guide](../guides/jsfx.md), `core/dsl/`, `tools/scripts/` | FAUST offline codegen, Cmajor external toolchain, and bounded JSFX support are all shipped with examples, skills, and guides. All use the external toolchain pattern (developer supplies compiler/runtime). |
| macOS platform support | usable | [`support-matrix.yaml`](../status/support-matrix.yaml) | Primary validated platform. |
| Windows and Linux platform support | experimental | [`support-matrix.yaml`](../status/support-matrix.yaml) | CI-backed and increasingly usable, but still not truthful to present as parity with macOS. |
| GPU rendering via Dawn / Skia | experimental | [capabilities](capabilities.md#rendering-gpu), [`support-matrix.yaml`](../status/support-matrix.yaml) | macOS GPU path is the validated baseline. Windows D3D12 and Linux Vulkan surfaces remain experimental and not yet runtime-validated on hardware. |
| Accessibility | partial across platforms | `platform_maturity.accessibility` in [`support-matrix.yaml`](../status/support-matrix.yaml) | macOS VoiceOver path is usable. Windows UIA and Linux AT-SPI remain partial. |
| Optional WebView/native integration | experimental / partial | [capabilities](capabilities.md#view-ui), [#106](https://github.com/Generous-Corp/pulp/issues/106) | Optional embedding exists, but Windows/Linux live runtime parity remains an active follow-up. |

## Immediate Public-Docs Rule

Until Phase 15 closes:

- treat [Capabilities Reference](capabilities.md) and
  [`docs/status/support-matrix.yaml`](../status/support-matrix.yaml) as the
  authoritative "what works today" surface
- treat broader statements in `README.md`, `VISION.md`, and the docs concept
  pages as intentionally narrower than the full roadmap language
- do not describe per-plugin MCP control or universal cross-host hot reload
  as already shipped (DSL support is now shipped)
