# Skills

Skills are Markdown playbooks that teach an AI coding agent how to work
on a specific part of Pulp — the conventions, the gotchas, and the exact
commands for a subsystem. They live in `.agents/skills/<name>/SKILL.md`,
**ship with the Pulp Claude Code plugin** (`plugin.json` points at that
directory), and are read by **both Claude Code and Codex** from the same
source of truth — there is no separate per-agent copy.

You rarely invoke a skill by name. Each one activates automatically when
your request matches what it covers (its `description` lists the triggers),
and many also have a matching `/slash-command`. The table below is the full
catalog of the 54 skills Pulp ships; open a skill's `SKILL.md`
for its complete, authoritative guidance.

| Skill | What it does |
|-------|--------------|
| `aax` | Optional AAX support for Pulp, including developer-supplied Avid SDK setup, CMake enablement, DigiShell/AAX Validator workflows, and local AAX builds on macOS or Windows. |
| `android` | Android platform development for Pulp — NDK cross-compilation, Oboe audio, Dawn/Skia GPU rendering, JNI bridge, touch interaction, emulator workflows, and end-to-end smoke validation. |
| `ara` | Optional ARA support for Pulp, including developer-supplied ARA SDK setup, CMake enablement, adapter companion APIs, validation, and ARA-aware plugin implementation guidance. |
| `audio-harness` | The measurement surface for ALL Pulp DSP and audio-pipeline work — read it BEFORE writing or gating DSP, not only when something already sounds wrong. |
| `audio-headless-debug` | Reproduce and debug "only happens in a DAW" audio plugin bugs (cutouts, glitches, parameter-change failures) entirely offline — headless Processor scenes for DSP bugs and a standalone AudioUnit host probe for adapter/host-interaction bugs. |
| `auv2` | Audio Unit v2 adapter work for Pulp — picking the right AU component type (aufx/aumf/aumi/aumu), wiring MIDI input, and avoiding the DAW-side component cache that silently masks repackaging. |
| `auv3` | Audio Unit v3 (AUAudioUnit) format adapter for Pulp — render-block wiring, parameter tree bridging, MIDI / sysex via AURenderEvent, sidechain pulls, state persistence, iOS extension surface, and the pitfalls discovered while wiring the adapter. |
| `ci` | Local and cloud CI for Pulp — validate branches, create PRs, merge on green. |
| `clap` | CLAP format adapter for Pulp — how Processor bridges to clap_plugin_t, how parameters / modulation / sidechain / MPE / UMP / sysex flow, and the pitfalls discovered while wiring the adapter. |
| `cli-maintenance` | Checklist and decision tree for adding, modifying, or removing CLI commands. |
| `cmajor-external` | Use Pulp's MIT-safe Cmajor support lane via source-owned patches, an external `cmaj` toolchain, and explicit generated-artifact workflows. |
| `code-comments` | How to write source comments, doc comments, and test names/tags that have lasting value — and what to never write. |
| `content` | Validate, install, update, list, rescan, remove, and reveal data-only Pulp content packs for installed plugins. |
| `daw-smoke` | Real-DAW (REAPER) functional smoke for reload/editor/format-adapter changes — opt-in, scoped, headless-safe, zero-pollution |
| `engine` | Query, recommend, and switch the Pulp JS engine backend (QuickJS, JavaScriptCore, V8). |
| `faust` | Create FAUST DSP plugins in Pulp using offline codegen, pre-generated C++ headers, and the FaustProcessor template wrapper. |
| `friction-report` | Turn a moment of friction — a conflicting PR, a wedged runner, a mysterious red check, a repeated manual chore — into a durable, actionable report. |
| `handoff` | Coordinate a cross-session or cross-machine handoff — snapshot the open work, write a status doc to the pulp-planning repo on main, and emit a ready-to-paste goal prompt that links it, so a fresh session (often on another machine) can pick up and finish. |
| `hosting` | Load, run, and test VST3 / AU / CLAP / LV2 plugins from Pulp code. |
| `import-design` | Import designs from Figma, Stitch, v0, Pencil, React Native, or Claude Design into Pulp web-compat JS with automated visual validation. |
| `installable-tools` | The acceptance bar for anything Pulp can install (a `pulp tool` registry entry, `pulp add` package, or any downloadable). |
| `intel-canary` | Maintain Pulp's macOS Intel (x86_64) portability lint and CI tiering — the PULP_INTEL_CANARY configure gate, intel_canary_lint.py + its allowlist, and the Tier 0-3 workflows (build.yml canary step, intel-portability.yml, nightly-intel.yml, release-cli.yml universal gate). |
| `ios` | iOS platform development for Pulp — iPhone/iPad AUv3 app extensions, iOS Simulator builds, UIKit window host, CoreAudio IO audio, touch & Apple Pencil input, XcodeBuildMCP automation. |
| `jsfx-subset` | Work in Pulp's bounded JSFX lane using source-only examples, subset validation, and explicit exclusions like no `@gfx`. |
| `kits` | Search, inspect, plan, apply, remove, pack, and scaffold local Pulp package manifests. |
| `moonbase` | Optional Moonbase license-activation integration for Pulp — load-bearing compile settings, OpenSSL-at-configure caveat, the moonbase-pulp User-Agent contract, audio-thread gating + click-free fade, async start/pump, the interactive native (no-WebView) activation editor (frame-tick polling + the don't-rebuild-mid-event trap), loadable plugin/standalone formats, and headless screenshots. |
| `motion` | Debug or validate Pulp animations / transitions / scroll behavior using the runtime motion-trace system. |
| `mpe` | Build an MPE-aware Pulp synth — opt into MPE via PluginDescriptor, consume per-note pitch bend / pressure / timbre from MpeBuffer, and route voices through MpeVoiceAllocator without reinventing channel tracking. |
| `packages` | Search, suggest, add, and browse third-party audio packages. |
| `playback` | Pulp timeline transport, immutable compiled playback programs, bounded compile execution and arrangement note/audio rendering, block-level publication latches, stable shells, and ProcessContext projection. |
| `pr-batching` | Decide whether several finished branches ship as ONE PR or stay separate. |
| `pr-review-sweep` | Sweep a PR's automated + human review comments and act on them — especially for material (large / logic-bearing) PRs. |
| `prototype-loop` | Leveraged-prototype dev loop (`pulp loop`) — focus marker plus normal watch/rebuild loop, with AOT analyzer guidance and deferred ar-swap/PR-monitor playbook. |
| `pulp-web-demo` | Generate and maintain browser demos of Pulp audio plugins (both web ABIs — WAM and WCLAP) from one declarative config, so every demo mounts the SAME shared player and the two ABIs stay in lockstep. |
| `screenshot` | Capture faithful PNGs of Pulp view trees / imported UIs headlessly. |
| `screenshot-sync` | Keep a plugin/demo repo's screenshots in sync with its UX. |
| `sdf-text` | Work with Pulp's SDF / MSDF / PSDF glyph atlases — building, sampling via SkSL, and the shared text-layout helpers. |
| `ship` | Sign, notarize, package, and distribute Pulp plugins and apps across macOS, Windows, and Android |
| `skia-gpu-build` | Enable a Skia + Dawn GPU build of Pulp (MacGpuWindowHost, Skia Graphite). |
| `streams` | Pick the right Pulp Stream for a given I/O task, wire async callbacks correctly without deadlocking the worker, and avoid the backpressure / cancellation footguns in `pulp::runtime::AsyncStream`. |
| `stretch` | Offline time-stretch / pitch / varispeed — character modes, fine-tune presets, A/B toolkit, and the honest quality state, so an agent can pick a mode, dial it in, and ship a plugin with it. |
| `tart-ci` | Stand up a fast, cached, isolated, disposable macOS CI lane on Tart — layered golden VM images, ephemeral per-job GitHub Actions runners, host-mounted caches, and a reusable per-repo vm-image manifest. |
| `threejs-bridge` | Build or iterate on Pulp's native Dawn-backed Three.js workflow using the real three.webgpu.js renderer, focused bridge tests, and native demo capture. |
| `timebase` | Pulp musical/media time primitives, immutable compiled tempo maps, monotonic transport beats, exact sample anchors, corrected inverse conversion, and shared transport quantization arithmetic. |
| `timeline` | Pulp immutable timeline model, typed command transactions, bounded journal and undo, durable assets, schema registry, and exact JSON persistence. |
| `trace-analysis` | The investigation harness for "why is this slow?" over a Pulp Perfetto trace (.pftrace). |
| `trace-sql` | SQL discipline for querying Pulp Perfetto traces (.pftrace) with trace_processor — idempotent CREATE OR REPLACE PERFETTO views, GLOB not LIKE, dur = -1 incomplete-slice handling, EXTRACT_ARG for span args, joining on stable utid/upid, SPAN_JOIN PARTITIONED, and the draft→validate→execute loop. |
| `update-demos` | Rebuild, re-pin, and republish Pulp's downstream demo/example repos against a new or the latest SDK. |
| `upgrade` | Guide users through `pulp upgrade` — discover new CLI releases, interpret migration notes for the hop they're performing, and apply breaking-change fixes (CMake macro renames, API surface changes, config file moves). |
| `video-proof` | Record, compose, publish, serve, and review short desktop validation video proofs for Pulp UX/test-harness work. |
| `view-bridge` | Editor lifecycle and multi-view attach for Pulp plugins — when to override Processor::create_view(), the open → notify_attached → resize → close protocol, release_view() ownership rules, and secondary-view roles. |
| `vst3` | VST3 format adapter for Pulp — SingleComponentEffect wiring, bus arrangement negotiation, parameter / MIDI event routing, state round-trip, and the pitfalls discovered while wiring the adapter against Steinberg's SDK. |
| `web-plugins` | Pulp in the browser — the WAM v2 and WebCLAP adapters, the wasm runtime, the Skia/WebGL2 browser window host, and the WebGPU (emdawnwebgpu) GPU-audio lane. |
| `webview-ui` | Build or iterate on a Pulp WebView UI using the native WebView bridge, embedded assets, directory-backed dev resources, and focused WebView validation. |

---

This catalog is generated from each skill's `SKILL.md` frontmatter by
`tools/scripts/skills_doc_check.py`. Do not edit it by hand — after adding
or changing a skill, regenerate it with:

```bash
python3 tools/scripts/skills_doc_check.py --write
```
