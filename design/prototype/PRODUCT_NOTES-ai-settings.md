# Forge — AI Settings & Providers: product notes / open specs

Built from the AI Settings design pass (account menu → AI Settings, Third‑party
Providers, chat model quick‑picker, first‑run model gating). Several behaviors
are **not yet specced** — flagged here so we're explicit about what needs a
product decision before implementation.

## What's designed (in the prototype)
- **Account menu** (avatar, bottom‑left): identity + plan, Usage, AI Settings
  (shows the active engineering model), Third‑party Providers, Invites, a DEBUG
  group (Canvas FPS meter, Canvas ruler), Report a Problem, Sign out.
- **AI Settings** modal — hybrid pipeline: pick an **Engineering model** (writes
  the DSP / manifest) and a **UI designer model** (builds the interface),
  **Reasoning effort** (Low → Max), and **Generation mode** (Sequential /
  Parallel). The chat quick‑picker sets both roles at once.
- **Providers** — "Connect your AI subscription or key" grid (Anthropic Claude
  Code, OpenAI GPT Codex, Google Gemini, Cursor, Moonshot, DeepSeek, OpenRouter,
  Together, OpenCode Zen, xAI, Fireworks, Z.AI, GitHub Copilot) with per‑provider
  setup (subscription vs API key, provider console links).
- **First‑run gating** — until at least one provider is connected, hitting Submit
  raises a "connect a model first" prompt instead of building.
- **Permissions / MCP** panes (advanced settings): notifications, "dangerously
  skip permissions", external MCP connectors.

## OPEN — needs product spec (do NOT assume in build)
1. **Effort ↔ mode coupling.** Is Reasoning effort only meaningful in Sequential,
   or does it also apply per‑role in Parallel? The chat picker currently exposes
   a single Effort; confirm whether effort is per‑role or global.
2. **Which models are real at launch.** Initial target is Claude Code + ChatGPT
   (Codex). The other providers are shown as a system pattern; decide the actual
   launch set and hide the rest behind a flag.
3. **Auth methods per provider.** We show subscription / API key / (Bedrock,
   Vertex, Foundry, Custom for Anthropic). Confirm the real supported auth paths
   per provider and the exact copy/links.
4. **OAuth vs local CLI vs key.** For subscription auth (Claude Pro/Max, ChatGPT
   Plus/Pro) — is it an OAuth handoff, a local CLI detection ("External config"),
   or both? Status chips (Connected / Signed in / External config / Checking)
   imply all three; we need the real state machine.
5. **Credential storage & scope.** Where do keys live (local only, per‑machine)?
   Confirm the "never shared with us" guarantee and whether keys are per‑project
   or global.
6. **Permission granularity.** "Dangerously skip permissions" is binary today.
   Do we want granular grants (filesystem / shell / network) instead?
7. **MCP.** Which external tools auto‑configure on startup, and what does the
   copy‑MCP‑config payload look like for Forge specifically?
8. **Default unconfigured model state.** When nothing is connected, what does the
   chat pill read — a neutral "Select a model", or a greyed default? Currently
   neutral + gated submit.
9. **Usage / Invites / Plan tiers.** "VIP · Unlimited", Usage, and Invites are
   placeholders — need real billing/usage model (out of scope per brief §7, so
   these stay non‑functional in the prototype).

## Provider console links used (from Daniel)
OpenRouter, DeepSeek, Moonshot/Kimi, Google AI Studio (Gemini), Cursor, Z.AI, xAI.

## FUTURE MILESTONE — Sampler / Sample banks (M6+, not in original spec)
Reached from the **Bundle samples** toolbar button (audio‑lines icon). Full‑canvas
screen for building sample banks that ship inside a plugin (sampler / drum
instruments):
- **Import** audio files or a whole folder, or drag‑and‑drop onto the list or the
  zone map. Searchable "All samples" list on the left with duration / size / format.
- **Zone map** center: a keyboard × velocity grid; drag samples onto it to map
  them to key ranges and velocity layers (round‑robin, layered, etc.). Named maps
  via the "Keyboard Map" type + Create.
- **Banks** on the right: multiple named banks per plugin (e.g. "Snare RR",
  "Library"); these get bundled and loaded when the plugin ships.
- Open questions: max bundle size / compression, streaming vs in‑memory, velocity
  layer + round‑robin authoring UX, per‑zone tuning/loop/ADSR, sample license
  metadata, and how banks are referenced from the DSP manifest. Not scoped yet —
  this screen just establishes where the button goes and what it becomes.

## FUTURE — Export plugin button (toolbar, package icon)
The toolbar **Export plugin** button opens the **Export Plugin** modal — build a
native plugin for a chosen **platform** (macOS / Windows / Linux) and **format**
(VST3 / AU / AUv3 / CLAP / Standalone). Flow: Ready → Start Export → Building
(progress + queued log) → Installed, showing the on‑disk location (e.g.
`~/Library/Audio/Plug‑Ins/VST3/…`) and a Reveal‑in‑Finder action. This is a
**local native install** — building the thing you made into a standalone plugin
that runs on its own in any DAW, distinct from Marketplace install (which would
ship a PKG/installer). Phase TBD, **not covered in the product spec** — call out.

OPEN for export:
- **Code signing / notarization.** macOS requires code‑sign + notarize (Developer
  ID certs); Windows needs Authenticode. We likely need a **Signing** setting
  where the user points Forge at their Apple / Microsoft signing identity. Pulp
  handles this its own way — model ours on that later. Unsigned/local‑dev builds
  need a story too (self‑signed / ad‑hoc?).
- Cost/credits model for exports (VIP unlimited shown) — needs billing decision.
- Which formats per platform are actually supported at launch.

## Code view — real project tree (detail not in original spec)
The Code screen now mirrors a real Pulp plugin repo (ref: pulp-bendr) rather than
a toy tree: chat stays full‑height on the far left, a **Files / Search** column
sits between chat and the editor (VS Code layout), and the editor has closeable
file tabs with per‑type icon badges. Tree is a **C++20 + Cmajor** project —
`*_entry.cpp` (VST3/AU/CLAP), `CMakeLists.txt`, `main.cpp`, `dsp/main.cmajor`,
`src/*.hpp/.cpp`, `ui/design.json` + `visual-renderers/`, `scripts/`, README,
LICENSE. Names use the demo project (Lo‑Fi Multi‑FX). Notes:
- **Cmajor** is supported on the **desktop** build; the web demo shows it read‑only.
- We are **C++‑first** and have **no intention to license C++ or Faust**. If Faust
  ever needs licensing, it's out. Keep the tree/codegen C++ + Cmajor only.
- Open: real syntax highlighting, editing/saving, search behavior, and how the
  web demo's tree maps to the desktop repo on export.
