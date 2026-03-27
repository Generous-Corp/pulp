"You are continuing the hardening and gap-closure work on the Pulp cross-platform audio plugin framework. Loop 7 completed a full phase-by-phase reality audit (phases 0-20) and corrected the most critical docs/status inconsistencies. This loop focuses on executing the remaining high-priority gap closures identified in the audit.

GOVERNING RULES:
- All work happens in git worktrees, never directly on main.
- Branch names: fix/<name> for gap closures, audit/<name> for any re-audit needed.
- Windows VM access is available via `ssh win2`.
- Ubuntu VM access is available via `ssh ubuntu`.
- Platform quality priority:
  - macOS quality is the primary bar
  - iOS and Apple-platform architecture maturity are high priority
  - Windows and Linux support should remain honest; reduce claims rather than over-invest
- Read ~/Code/pulp/CLAUDE.md at the start of EVERY iteration.
- Read ~/Code/pulp/AGENTS.md at the start of EVERY iteration.
- Use RepoPrompt for deep codebase analysis before making changes.

CONTEXT FROM LOOP 7:
- Full reality audit completed: planning/phase-reality-audit-2026-03-26.md (gitignored)
- Gap matrix created: planning/phase-gap-matrix-2026-03-26.md (gitignored)
- Docs/status corrections committed on audit/loop7-reality-check branch
- Key findings:
  - All 631 tests pass on macOS (webview correctly excluded when PULP_BUILD_WEBVIEW=OFF)
  - pluginval/auval NOT running in CI — docs corrected but CI not yet fixed
  - support-matrix.yaml updated to reflect actual web/rendering state
  - VISION.md stale 'not implemented' list corrected
  - Test counts, format counts, validation claims all standardized

REMAINING GAP CLOSURES (from gap matrix, in priority order):

### Priority 1: CI/Validation Hardening
1. Install pluginval in macOS CI workflow (validate.yml)
   - Download from GitHub releases or brew
   - Run against PulpGain, PulpTone, PulpEffect, PulpCompressor VST3 bundles
   - If pluginval passes, restore the 'validated with pluginval' claim in docs
   - Add clap-validator for CLAP bundles if available

2. Consider adding auval CI step
   - Requires installing AU plugins to ~/Library/Audio/Plug-Ins/Components/
   - May need a dedicated CI step that builds, installs, and runs auval
   - If too complex for CI, document as 'local validation only' and move on

### Priority 2: Test Quality Improvements
3. Add functional plugin output tests (not just 'doesn't crash')
   - PulpGain: verify gain=0dB passes through unchanged, gain=-inf silences
   - PulpTone: verify oscillator produces non-zero output
   - PulpEffect: verify filter affects frequency content
   - PulpCompressor: verify compression reduces loud signals more than quiet

4. Add negative-path tests
   - Zero-length buffer processing
   - Extreme sample rates (1Hz, 384kHz)
   - NaN/Inf input handling
   - Corrupt state deserialization

5. Add golden-file audio reference tests
   - Render known input through each plugin at fixed params
   - Compare output against committed reference files
   - Tolerance-based comparison for floating-point

### Priority 3: Platform Validation
6. Windows VM validation (ssh win2)
   - Build and run tests
   - Verify WASAPI device enumeration
   - Verify Win32 MIDI
   - Document results

7. WASM browser validation
   - Build WASM targets
   - Load in browser test host
   - Verify audio passes through
   - Can use chrome-devtools MCP for automation

### Priority 4: Code Quality
8. Thread-safety integration test
   - Compose SeqLock + TripleBuffer + SPSC queue in a multi-threaded scenario
   - Verify no data races under stress
   - Run under TSan

9. Improve plugin matrix tests
   - Current matrix tests verify load/process don't crash
   - Add parameter automation verification
   - Add state save/restore round-trip

SOURCE OF TRUTH:
- ~/Code/pulp/planning/STATUS.md — audited version from Loop 7
- ~/Code/pulp/planning/phase-reality-audit-2026-03-26.md — audit findings
- ~/Code/pulp/planning/phase-gap-matrix-2026-03-26.md — gap list with dependencies
- ~/Code/pulp/README.md — corrected in Loop 7
- ~/Code/pulp/VISION.md — corrected in Loop 7
- ~/Code/pulp/docs/status/support-matrix.yaml — corrected in Loop 7

SEQUENCING:
- Work through gaps in the order listed above
- Each gap closure should include:
  - The implementation change
  - Tests that prove the gap is closed
  - STATUS.md update if the claim surface changes
  - Docs update if public-facing claims change
- Skip to the next gap if blocked; record why and what unblocks it

TESTING (MANDATORY):
- Run ctest after every code change
- Run docs consistency check after docs changes
- Do not promote a claim unless test evidence backs it

GIT DISCIPLINE:
- Commit at every natural boundary (each gap closure, each audit update)
- Clean commit messages explaining what was fixed and why
- Do NOT push unless asked

EACH ITERATION MUST:
1. Read STATUS.md and the gap matrix
2. Pick the next unblocked gap from the priority list
3. Implement the fix
4. Add or strengthen tests
5. Update STATUS.md and docs if claims change
6. Commit with clean message
7. Summarize what was done and what comes next

COMPLETION CONDITION:
- pluginval CI integration is done or honestly documented as infeasible
- At least 3 functional plugin output tests exist
- At least 3 negative-path tests exist
- STATUS.md reflects all changes from this loop
- A next-work plan exists for remaining items

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: GAP CLOSURE COMPLETE" --completion-promise "DONE" --max-iterations 80
