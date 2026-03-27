"You are building the Pulp cross-platform audio plugin and application framework.

GOVERNING RULES:
- The file ~/Code/pulp/planning/14-phased-roadmap.md defines the phased plan. Read it at the start of EVERY iteration.
- The file ~/Code/pulp/CLAUDE.md defines development methodology, clean-room rules, and testing requirements. Read it at the start of EVERY iteration.
- The file ~/Code/pulp/VISION.md defines what we're building and why. Reference it for architectural decisions.
- All work happens in git worktrees, never directly on main.
- Branch names: phase/foundation, phase/runtime, phase/build-tooling, phase/audio-midi, phase/formats, phase/rendering, phase/plugin, phase/shipping, phase/examples, phase/launch
- Exploration branches: explore/<topic> for uncertain work.

SOURCE OF TRUTH:
- ~/Code/pulp/planning/14-phased-roadmap.md — phased spec with all work items
- ~/Code/pulp/planning/STATUS.md — tracks progress, active worktrees, decisions, blockers (update after every iteration)
- ~/Code/pulp/planning/ — all spec docs (architecture, capability matrix, rendering strategy, etc.)
- ~/Code/pulp/planning/GAP-analysis-juce-vs-pulp.md — feature gaps identified from JUCE audit (read when starting a new phase to check for missed deliverables)
- ~/Code/pulp/DEPENDENCIES.md — dependency tracking (update when adding any dependency)

REFERENCE PROJECTS (read-only, for patterns and inspiration):
- ~/Code/pulp/planning/02-juce-audit.md — what the audited framework does (capabilities to match)
- ~/Code/pulp/planning/03-juce-dev-audit.md — plugin workflows to replicate
- ~/Code/pulp/planning/04-iplug3-review.md — architecture inspiration
- ~/Code/pulp/planning/05-swift-cpp-strategy.md — Swift/C++ interop patterns
- ~/Code/pulp/planning/08-architecture-spec.md — subsystem design
- ~/Code/pulp/planning/09-plugin-spec.md — CLI and Claude plugin design
- ~/Code/pulp/planning/12-rendering-strategy.md — Dawn/Skia/QuickJS rendering
- Use RepoPrompt to reference code from safe-to-study projects (VST3 SDK, AU SDK, CLAP, iPlug2, AudioKit, SignalKit, Dawn, Skia, QuickJS). NEVER load JUCE source as implementation context.
- Use RepoPrompt context_builder for deep codebase analysis, architecture planning, and code review before making changes.

SAMPLE PROJECTS (built incrementally to validate each phase):
- PulpGain — trivial gain plugin (Phase 2-3 validation)
- PulpTone — oscillator synth with MIDI + musical typing keyboard (Phase 4 validation)
- PulpEffect — filter with parameters, automation, state save/load (Phase 5 validation)
- PulpDrums — generative drum sequencer MIDI effect with GPU UI (Phase 6 validation)
- PulpSynth — macro oscillator synth with presets + MCP interface (Phase 7 validation)
- PulpSampler — YouTube audio sampler with waveform editor (Phase 9 validation)

WORKING REPO:
- ~/Code/pulp — main branch (clean, committed, public-ready)
- Worktrees created as ~/Code/pulp-<name>/ (e.g., ~/Code/pulp-phase-runtime/)

ACTIVE PHASES:
- Phase 7: CLI + MCP — mark complete (all items done, update STATUS.md overview table)
- Phase 8: Signing, Packaging, Auto-Updates — concrete tasks below
- Phase 10: Hardening/Launch — concrete tasks below

OUTSTANDING WORK — PARAMETER MODEL GAPS (Phase 5 polish):
These are gaps in the format adapters and parameter system that should be fixed before launch.
Work in a `phase/param-hardening` branch.

- [ ] **CLAP parameter gestures** — Handle CLAP_EVENT_PARAM_GESTURE_BEGIN/END in clap_adapter.cpp process(). Wire to store.begin_gesture()/end_gesture(). Currently only VST3 wires gesture callbacks.
- [ ] **CLAP parameter modulation** — Handle CLAP_EVENT_PARAM_MOD events in clap_adapter.cpp. Add a `float mod_offset` to ParamValue or a parallel modulation accumulator. This enables per-voice modulation which is a key CLAP differentiator.
- [ ] **CLAP parameter output events** — When plugin code calls set_value() during process(), emit CLAP_EVENT_PARAM_VALUE to the output event list so the host sees automation output. Currently the host can't observe parameter changes made by the plugin itself.
- [ ] **VST3 parameter output changes** — Same issue: when plugin sets values during process(), write to data.outputParameterChanges so the host sees them. Required for proper automation recording.
- [ ] **VST3 parameter groups** — Wire ParamGroup to VST3 UnitInfo. StateStore already supports groups (ParamGroup with parent_id) but the VST3 adapter ignores them. Implement IUnitInfo or use ParameterInfo::unitId.
- [ ] **AU gesture callbacks** — Wire set_gesture_callbacks() in the AU adapter like VST3 does. AU hosts expect AudioUnitParameterListener notifications for undo grouping.
- [ ] **AU parameter value strings** — Implement GetParameterValueStrings for stepped/enum parameters. Currently returns kAudioUnitErr_InvalidPropertyValue unconditionally.
- [ ] **Custom to_string in CLAP** — params_value_to_text() in clap_entry.hpp hardcodes "%.2f". Should use ParamInfo::to_string if available.
- [ ] **Latency reporting** — Add `int latency_samples() const` to Processor (default 0). Wire to CLAP latency extension, VST3 getLatencySamples(), AU GetLatency().
- [ ] **Tail time propagation** — PluginDescriptor::tail_samples exists but AU GetTailTime() returns 0.0. CLAP has no tail extension wired. VST3 should override getTailSamples(). Wire all three.
- [ ] **AU v3 parameterTree** — au_adapter.mm returns an empty AUParameterTree (`[AUParameterTree createTreeWithChildren:@[]]`). This means AU v3 hosts see zero parameters. Build AUParameter objects from StateStore params and return a populated tree. This is a critical gap for modern AU hosting.
- [ ] **StateStore listener mutex on audio thread** — `set_value()` holds `listener_mutex_` while calling all listeners. If the host calls set_value() from the audio thread (VST3 parameter changes, CLAP param events), this can block. Fix: either invoke listeners outside the lock (copy-then-call pattern), or use a lock-free notification queue that the UI thread drains.
- [ ] **Tests** — For each fix above, add a test (or extend existing format validation tests) that verifies the behavior. At minimum: gesture round-trip test, modulation accumulation test, output event test, AU v3 parameterTree non-empty test.

OUTSTANDING WORK — SYNC PRIMITIVE STRATEGY (Phase 10 hardening):
The audio thread currently uses only std::atomic<float> for parameters and CHOC SPSC FIFO for meter data.
This is sufficient for current plugins but needs expansion for production robustness.
Work in a `phase/sync-primitives` branch.

- [ ] **SeqLock** — Implement `pulp::runtime::SeqLock<T>` for coherent multi-field reads.
  Use case: ProcessContext has tempo + beat position + time sig that must be read as a consistent snapshot. Currently each field is read independently which can produce torn reads.
  Design: writer increments a sequence counter (odd = writing), writes fields, increments again (even = complete). Reader retries if sequence was odd or changed during read.
  Location: core/runtime/include/pulp/runtime/seqlock.hpp
  Tests: concurrent writer/reader stress test, verify no torn reads.

- [ ] **TripleBuffer** — Implement `pulp::runtime::TripleBuffer<T>` for latest-value publication without blocking.
  Use case: swapping large config blobs (wavetable data, IR buffers, routing graphs) from main thread to audio thread. Writer publishes to back buffer, atomically swaps. Reader always gets latest complete value.
  Design: three buffers + atomic index. Writer writes to back, swaps back↔middle. Reader swaps middle↔front if newer, reads front.
  Location: core/runtime/include/pulp/runtime/triple_buffer.hpp
  Tests: concurrent publish/read stress test, verify no allocation on read path.

- [ ] **AudioBridge overflow handling** — The meter FIFO is size 16. If the UI thread stalls (window occluded, debugger attached), the FIFO fills and push_meter() silently drops data. Options:
  (a) Increase FIFO size to 64 (simple, covers most stalls)
  (b) Switch meter data to TripleBuffer since we only care about latest value anyway (cleaner)
  (c) Add a dropped-frame counter for diagnostics
  Recommend (b): replace FIFO with TripleBuffer for meter data. Keep FIFO for ordered event streams (MIDI, parameter automation).

- [ ] **Atomic memory ordering audit** — ParamValue uses memory_order_relaxed everywhere. This is fine for single values on x86 but may need memory_order_acquire/release on ARM for correct visibility ordering when a parameter change must be visible before the audio buffer it applies to. Audit all atomic usage and document the ordering rationale.

- [ ] **AU parameter sync optimization** — AU adapter reads ALL parameters via GetParameter() every process() call. For plugins with many parameters (PulpSynth has 10, but a real synth could have 100+), this is wasteful. Consider: only sync params that changed (use AU's parameter listener or a dirty bitset).

- [ ] **Document sync strategy** — Add a section to docs/ or a header comment in core/runtime/ that documents which primitive to use when:
  - Single value, latest-wins: std::atomic (parameters, flags)
  - Multi-field coherent snapshot: SeqLock (transport state)
  - Large data swap: TripleBuffer (wavetables, IR buffers, config blobs)
  - Ordered event stream: SPSC FIFO (MIDI events, automation points, UI commands)
  - Never on audio thread: std::mutex, std::condition_variable, allocation

OUTSTANDING WORK — PHASE 8 (Signing/Packaging/Distribution):
ship/ already has: codesign, notarization, pkg, appcast, Ed25519 signing.
CI already has: build.yml, validate.yml, sign-and-release.yml, sanitizers.yml.
Work in a `phase/shipping` branch.

- [ ] **DMG creation** — Add `create_dmg()` to pulp::ship. Use hdiutil to create a drag-to-install DMG with background image and Applications alias. This is the standard macOS distribution format for standalone apps.
- [ ] **Combined multi-format installer** — Add `create_combined_pkg()` that packages all plugin formats (VST3 + AU + CLAP) plus standalone app into a single .pkg with a choice dialog. Uses productbuild with a distribution.xml.
- [ ] **pulp ship CLI command** — Add `pulp ship` subcommand to the CLI that orchestrates: build → validate → sign → notarize → package → update appcast. Reads configuration from a `ship.toml` or similar config file with signing identity, Apple ID, team ID.
- [ ] **Windows signing stub** — Add `codesign_win.cpp` with signtool wrappers (even if not fully functional yet, the API surface should exist): `codesign()` via signtool, certificate import, timestamping.
- [ ] **Linux packaging stub** — Add `package_linux.cpp` with functions for creating .deb and AppImage. Even if not fully tested, the API should exist alongside macOS.
- [ ] **AU component in installer** — sign-and-release.yml only packages VST3 and CLAP. Add AU .component to the installer pipeline. The install-location should be ~/Library/Audio/Plug-Ins/Components/.
- [ ] **Appcast integration in CI** — After successful release, auto-generate appcast.xml with the new version entry and upload alongside release artifacts. Wire the Ed25519 signing from appcast.hpp into the release workflow.
- [ ] **Entitlements template** — Create a default entitlements.plist for audio plugins (audio input, network client for updates). The codesign() function accepts entitlements but no template is provided.
- [ ] **Installer validation** — After creating a .pkg, verify it installs correctly to a temp directory (using `installer -pkg ... -target /tmp/...`). Add as a CI step or test.
- [ ] **Mark Phase 8 complete** — Update STATUS.md when all items above pass.

OUTSTANDING WORK — PHASE 7/9/10 CLEANUP:
- [ ] **Mark Phase 7 complete in STATUS.md** — All Phase 7 items are checked off. Update the overview table from "In progress" to "Complete".
- [ ] Phase 9 deferred items (Python/Node bindings, PulpSampler) — leave as deferred, no action needed.
- [ ] Phase 10 deferred items (compute shaders, WebCLAP, libremidi) — leave as deferred, no action needed.

GAP AWARENESS:
- ~/Code/pulp/planning/GAP-analysis-juce-vs-pulp.md lists features JUCE provides that Pulp must match or exceed.
- When starting a new phase, check the GAP file for TRUE GAPs assigned to that phase.
- Intentional passes (do NOT implement):
  - Product unlocking/licensing (use third-party services)
  - Analytics/telemetry (use dedicated SDKs like Sentry/PostHog)
  - Video/camera (not aligned with audio focus)
  - Audio CD reading (obsolete)
  - OpenGL (replaced by WebGPU/Dawn/Skia — architectural decision)
  - Box2D physics (novelty, not framework concern)
  - Custom cryptography (use platform crypto APIs)
  - Code editor widget (use WebView or Monaco if needed)
  - System tray (niche, trivial to wrap per-platform if needed)
  - VST2/Unity/ARA formats (dead SDK / niche / post-launch consideration)
  - WebView embedding (contradicts GPU-first vision; CHOC choc::ui::WebView available if needed)

PRIORITY ORDER:
1. Mark Phase 7 complete in STATUS.md (trivial, do first)
2. Parameter model gaps (Phase 5 polish) — these affect plugin correctness in real DAWs
3. Phase 8 shipping tasks — needed for distribution
4. Sync primitive strategy (Phase 10) — robustness hardening
5. Update STATUS.md after each section completes

TASK:
- Read ~/Code/pulp/planning/14-phased-roadmap.md at the start of EVERY iteration.
- Read ~/Code/pulp/planning/STATUS.md for prior progress and active worktrees.
- Follow the PRIORITY ORDER above. Complete each section before moving to the next.
- Implement in the correct worktree/branch as specified per section.
- Update STATUS.md with progress.

EXECUTION ORDER:
- Work items within each section are sequential unless explicitly independent.
- If a deliverable is blocked, document in STATUS.md and continue to the NEXT deliverable.
- Before marking a section complete, ALL deliverables must be done or have documented blockers.

WORKTREE MANAGEMENT:
- Create worktrees for each active section:
  - git worktree add ~/Code/pulp-phase-params -b phase/param-hardening
  - git worktree add ~/Code/pulp-phase-shipping -b phase/shipping
  - git worktree add ~/Code/pulp-phase-sync -b phase/sync-primitives
- For explorations: git worktree add ~/Code/pulp-explore-<topic> -b explore/<topic>
- When a section is complete: merge to main (via PR or direct merge), remove worktree.
- Use /worktree-manager:create and /worktree-manager:cleanup for worktree lifecycle.

PARALLELIZATION:
- Use /codex <task> for parallel work that would speed things up.
- Good for Codex: boilerplate CMake modules, test fixtures, CI workflow YAML, documentation stubs, stub implementations for Windows/Linux.
- Bad for Codex: core architecture decisions, format adapter integration, sync primitive implementation.
- Run Codex in background and continue your own work.
- Check Codex output and test before marking work complete.

IMPLEMENTATION RULES:
- C++20 standard. Use std:: where possible. Don't reinvent the standard library.
- Use C++20 concepts for plugin interfaces (not virtual inheritance).
- Every subsystem has its own CMakeLists.txt with explicit dependency declarations.
- Platform-specific code goes in platform/ subdirectories, NEVER interleaved with cross-platform code.
- Public headers in include/pulp/<subsystem>/. Private implementation in src/.
- All naming is original Pulp vocabulary. NO names from the audited framework (JUCE).
- Design tokens are JSON. Themes are data, not code.
- JS UI files are loaded at runtime, not compiled into binaries.

CLEAN-ROOM DISCIPLINE:
- You can use Repo Prompt to understand how things are done in JUCE /Users/danielraffel/Code/JUCE
- You can: Read the JUCE source; Learn patterns, architecture, APIs; Reimplement similar ideas in your own code
- You should not do the following from JUCE: Copy/paste any JUCE code; Derive code too directly (i.e. recognizable structure/implementation)
- NEVER reference JUCE source code during implementation. Use format SDKs and safe-to-study projects.
- If implementing a plugin format adapter, read the format SDK documentation — not how another framework wraps it.
- If a proposed name matches a JUCE name, reject it and pick an original name.
- When adding any dependency: check license first, add to DEPENDENCIES.md (alphabetical), add to NOTICE.md (alphabetical).
- Only MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0 licensed dependencies. NO copyleft.

TESTING (MANDATORY):
- Every subsystem has unit tests (Catch2).
- After any build system change: cmake --preset default && cmake --build build (must succeed).
- After audio code changes: run golden-file comparison tests.
- After format adapter changes: run format validators (auval for AU, pluginval for VST3, clap-validator for CLAP).
- After UI/rendering changes: capture screenshots for visual regression.
- After each sample project milestone: launch and verify it works.
- Use XcodeBuildMCP for macOS/iOS build validation.
- Use chrome-devtools MCP for web/WASM validation.
- Document test results in STATUS.md.
- If tests fail, fix them before moving on. No skipping broken tests.

GIT DISCIPLINE:
- Commit at the end of EVERY iteration where changes were made.
- Commits must be clean, focused, and describe what was built (imperative mood, explain WHY).
- No "WIP", "fix", "stuff" commits. Every commit should be something we're proud of.
- Work on the appropriate branch in its worktree. Create branch if it doesn't exist.
- Do NOT push to remote unless asked or merging a completed phase.
- When merging to main: squash or rebase for clean history.

DEPENDENCY ADDITIONS:
- Before adding ANY dependency: run conceptual equivalent of `pulp audit` — check license, check for clean-room risk.
- Add to DEPENDENCIES.md in alphabetical order.
- Add license text to NOTICE.md in alphabetical order.
- Preserve original LICENSE file in external/<dep>/ if vendoring.

EACH ITERATION MUST:
1. Read planning/14-phased-roadmap.md
2. Read planning/STATUS.md
3. Identify the NEXT incomplete deliverable from PRIORITY ORDER
4. Ensure correct worktree and branch exist (create if needed)
5. Implement the deliverable
6. Write or update tests
7. Build and verify: cmake --preset default && cmake --build build
8. Run relevant tests
9. Commit changes with a clean message
10. Update planning/STATUS.md with progress, decisions, and test results
11. If a sample project milestone is reached, verify it works end-to-end
12. Summarize what was done

SECTION COMPLETION CHECKLIST:
Before marking a section complete:
- [ ] All deliverables implemented or blockers documented
- [ ] All tests pass
- [ ] Code builds on target platforms (macOS at minimum, ideally CI passes)
- [ ] No JUCE names or patterns in the code
- [ ] DEPENDENCIES.md and NOTICE.md are current
- [ ] STATUS.md is updated
- [ ] Clean commit history on the branch
- [ ] Ready to merge to main

COMPLETION CONDITION:
- All sections in PRIORITY ORDER have completed their checklists
- Sample projects build and run correctly
- Tests pass
- STATUS.md reflects completion

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: PHASE COMPLETE

IF STUCK:
- After 5 iterations without progress on a deliverable, document in STATUS.md:
  - What is blocked
  - Why
  - What was attempted
  - What assumption may be wrong
- Consider creating an explore/ branch to prototype a solution
- If truly stuck, ask the user for help" --completion-promise "DONE" --max-iterations 120
