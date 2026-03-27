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
- Phase 1: Repo Foundation and Build System
- Phase 2: Core Runtime and Platform Abstraction
- Phase 3: Build Tooling and Project Scaffolding (parallel with Phase 4)
- Phase 4: Audio/MIDI I/O and Standalone Apps (parallel with Phase 3)
- Phase 5: Plugin Formats and Parameter/State
- Phase 6: GPU Rendering, JS Scripting, Design System
- Phase 7: CLI + MCP Server, Claude Code Plugin
- Phase 8: Signing, Packaging, Auto-Updates
- Phase 9: Web/WASM, Examples, Documentation, DSP Library
- Phase 10: Hardening, DAW Validation, Public Launch

TASK:
- Read ~/Code/pulp/planning/14-phased-roadmap.md at the start of EVERY iteration.
- Read ~/Code/pulp/planning/STATUS.md for prior progress and active worktrees.
- Identify the NEXT incomplete deliverable in the current active phase.
- Implement it in the correct worktree.
- Update STATUS.md with progress.

EXECUTION ORDER:
- Phases MUST be completed in dependency order (see roadmap dependency graph).
- Phases 3 and 4 CAN run in parallel after Phase 2 completes.
- Within each phase, deliverables are sequential unless explicitly independent.
- If a deliverable is blocked, document in STATUS.md and continue to the NEXT deliverable.
- Before starting the next phase, ALL deliverables in the current phase must be done or have documented blockers.

WORKTREE MANAGEMENT:
- Create worktrees for each active phase: git worktree add ~/Code/pulp-phase-<name> -b phase/<name>
- For explorations: git worktree add ~/Code/pulp-explore-<topic> -b explore/<topic>
- Multiple worktrees can be active simultaneously for parallel phases.
- When a phase is complete: merge to main (via PR or direct merge), remove worktree.
- Use /worktree-manager:create and /worktree-manager:cleanup for worktree lifecycle.

PARALLELIZATION:
- Use /codex <task> for parallel work that would speed things up.
- Good for Codex: boilerplate CMake modules, test fixtures, CI workflow YAML, documentation stubs.
- Bad for Codex: core architecture decisions, format adapter integration, rendering engine setup.
- Run Codex in background and continue your own work.
- Check Codex output and test before marking work complete.
- Multiple worktrees enable true parallel work — delegate a phase to Codex in one worktree while you work in another.

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
- Work on the phase branch in its worktree. Create branch if it doesn't exist.
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
3. Identify the NEXT incomplete deliverable
4. Ensure correct worktree and branch exist (create if needed)
5. Implement the deliverable
6. Write or update tests
7. Build and verify: cmake --preset default && cmake --build build
8. Run relevant tests
9. Commit changes with a clean message
10. Update planning/STATUS.md with progress, decisions, and test results
11. If a sample project milestone is reached, verify it works end-to-end
12. Summarize what was done

PHASE COMPLETION CHECKLIST:
Before marking a phase complete:
- [ ] All deliverables implemented or blockers documented
- [ ] All tests pass
- [ ] Sample project for this phase works end-to-end
- [ ] Code builds on target platforms (macOS at minimum, ideally CI passes)
- [ ] No JUCE names or patterns in the code
- [ ] DEPENDENCIES.md and NOTICE.md are current
- [ ] STATUS.md is updated
- [ ] Clean commit history on the phase branch
- [ ] Ready to merge to main

COMPLETION CONDITION:
- All active phases listed above have completed their checklists
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
