"You are building and hardening the Pulp cross-platform audio plugin and application framework. For this loop, do NOT focus on adding new product features first. Focus first on auditing what we already claim, resetting our status/docs to reality, generating the gap list, creating the next work items needed to actually complete what we say is complete, and then begin executing the highest-priority gap closures.

GOVERNING RULES:
- All work happens in git worktrees, never directly on main.
- Branch names: audit/<name> for audit loops, plan/<name> for work-item/spec creation, fix/<name> for narrowly scoped corrections only when unavoidable.
- Windows VM access is available via `ssh win2`.
- Ubuntu VM access is available via `ssh ubuntu`.
- Use those VMs when runtime validation on Windows or Linux is specifically needed, but do not let VM-driven platform work dominate the loop.
- Platform quality priority for this loop:
  - macOS quality is the primary bar
  - iOS and Apple-platform architecture maturity are high priority where relevant
  - Windows and Linux support should remain honest and solid, but it is acceptable to leave some claims reduced or some validation work pending rather than over-investing there early
- Do not burn excessive time chasing Windows/Linux parity if the higher-value work is clarifying or hardening the macOS/iOS path and the docs/status language can be made truthful in the meantime.
- Missing Windows/Linux VM validation must not block the loop. If runtime evidence is incomplete there, reduce the claim honestly, record the follow-up work, and keep moving.
- The source of truth for what Pulp claims about itself is spread across:
  - ~/Code/pulp/planning/STATUS.md
  - ~/Code/pulp/README.md
  - ~/Code/pulp/VISION.md
  - ~/Code/pulp/docs/status/
  - ~/Code/pulp/docs/
  Treat these as a single consistency surface. If they say different things, that is a first-class finding.
- Read ~/Code/pulp/CLAUDE.md at the start of EVERY iteration. It defines development methodology, clean-room rules, and testing expectations.
- Read ~/Code/pulp/AGENTS.md at the start of EVERY iteration. It defines docs maintenance rules.
- Use RepoPrompt aggressively for this loop:
  - Use RepoPrompt context_builder for deep codebase analysis before making changes.
  - Use RepoPrompt review-style analysis to inspect code, tests, docs, and diffs.
  - Use RepoPrompt to inspect the actual implementation behind every major claim.
- Consult and delegate to /codex where appropriate during the audit:
  - Good Codex tasks: bounded phase audits, test inventory extraction, docs/status consistency sweeps, claim matrix preparation, file-by-file evidence gathering, drafting focused follow-up work-item specs.
  - Bad Codex tasks: final judgment on repo-wide truth claims, cross-cutting prioritization, architectural calls, or anything requiring integration of many conflicting sources.
  - Use Codex in parallel for concrete slices, then integrate the results yourself.
- Before finalizing any major audit memo, run the framing past /codex as a second-opinion reviewer when useful and incorporate useful pushback.
- Do NOT broaden this loop into speculative new features. The point is to establish truth, identify gaps, create the implementation plan that follows, and then start closing the most important verified gaps.
- Time-box the audit so it reaches execution:
  - do not spend unlimited iterations polishing one audit track
  - establish the baseline, then move into closure work

PRIMARY OBJECTIVE:
Audit the entire repo across phases 0-20 (and any post-phase additions already worked on), determine what is actually implemented and validated, update STATUS.md to a real evidence-backed version, identify the gaps, create concrete work items to close them, and then work through executing the highest-priority gap closures while keeping STATUS.md current.

PLATFORM EMPHASIS FOR THIS LOOP:
- Keep the cross-platform audit honest, but prioritize work that makes the framework genuinely high-quality on macOS and Apple-platform paths first.
- Treat Windows/Linux as important support surfaces, not as the main sink for implementation time unless a critical cross-platform claim depends on them.
- If Windows/Linux evidence is weaker, prefer reducing the claim honestly over forcing premature parity work.
- Do not let “runtime validation on Windows VM” or similar pending non-Apple checks block Apple-focused hardening, docs/status correction, or creation of the next loop prompt.

SEQUENCING PRINCIPLE:
- Work sequentially through the gap list in dependency-aware order.
- For every skipped or deferred item, record:
  - why it was skipped
  - what dependency blocks it
  - what exact condition should cause it to be revisited
- Maintain a “return-to” queue for blocked items.
- Whenever a dependency is closed, re-check the queue and pull newly unblocked items back into active work.
- Do not let “deferred” become a graveyard. Every deferral must have a return condition.
- Preferred audit order:
  - first do a broad phase-by-phase sweep across 0-20 to establish reality and refresh STATUS.md
  - then deepen the highest-risk areas using the audit tracks
  - then move into gap closure work
  - after that, continue alternating between targeted re-audit and gap closure as needed

THIS LOOP MUST COVER:
1. Full phase-by-phase reality audit (0-20 or whatever is currently claimed)
2. Status claim reality
3. Test adequacy and validation depth
4. Real-time/thread-safety correctness
5. Docs/story alignment across README.md, VISION.md, STATUS.md, and docs/
6. Gap generation and follow-up work-item creation
7. A prioritized plan for what to implement next so the repo actually delivers on its claims
8. Work through the highest-priority verified gaps and update the refreshed STATUS.md as progress is made

SOURCE OF TRUTH:
- ~/Code/pulp/planning/STATUS.md — current status tracker, but treat it as UNTRUSTED until audited
- ~/Code/pulp/README.md — public “what works today” story
- ~/Code/pulp/VISION.md — long-range vision and architecture claims
- ~/Code/pulp/docs/status/ — machine-readable manifests
- ~/Code/pulp/docs/ — guides, references, capabilities, examples
- ~/Code/pulp/planning/status-claim-reality-audit-spec.md — methodology/spec for the status-claim audit
- ~/Code/pulp/planning/test-adequacy-and-validation-depth-audit-spec.md — methodology/spec for the validation-depth audit
- ~/Code/pulp/planning/realtime-thread-safety-correctness-audit-spec.md — methodology/spec for the thread-safety audit
- ~/Code/pulp/planning/repo-engineering-polish-audit-2026-03-26.md
- ~/Code/pulp/planning/audio-validation-audit-and-hardening-plan.md
- ~/Code/pulp/planning/module-maturity-closure-plan.md
- ~/Code/pulp/planning/apple-render-surface-integration-spec.md
- ~/Code/pulp/DEPENDENCIES.md

REFERENCE PROJECTS / ANALYSIS CONTEXT:
- ~/Code/pulp/planning/02-juce-audit.md — feature/capability expectations
- ~/Code/pulp/planning/04-iplug3-review.md — architecture inspiration
- ~/Code/pulp/planning/08-architecture-spec.md — subsystem design
- ~/Code/pulp/planning/12-rendering-strategy.md — Dawn/Skia/QuickJS rendering
- Use RepoPrompt to inspect safe-to-study upstreams (VST3 SDK, AU SDK, CLAP, iPlug2, AudioKit, SignalKit, Dawn, Skia, QuickJS) when comparing capability or validation expectations.
- NEVER use JUCE source as implementation context for actual coding; clean-room rules still apply.

CURRENT STATE (TREAT AS CLAIMS, NOT FACTS, UNTIL VERIFIED):
- STATUS.md currently presents broad phase completion and capability claims.
- README.md presents a “what works today” story.
- VISION.md presents both long-range aspirations and some language that can be mistaken for shipped capability.
- Docs/status YAML manifests encode support and maturity labels.
- There are existing planning docs for module maturity, docs hardening, Apple render, audio validation, and repo polish.
- The problem to solve now is not “what should we build someday?” It is “what is actually true now, what is overstated, what is missing, and what should be implemented next to close the gap?”

PHASE-BY-PHASE AUDIT MANDATE:
- Review ALL phases from 0 through 20, plus any post-phase additions or top-level “complete” sections currently in STATUS.md.
- Do not assume that because a phase is labeled complete it is actually complete.
- For each phase, classify every major claimed deliverable as:
  - implemented and validated
  - implemented but only partially validated
  - present in some form but not productionized
  - claimed but not sufficiently supported by repo evidence
- Generate a gap list for each phase.
- Identify explicit dependencies between gaps and phases.
- If STATUS.md is overstating, rewrite it to a real version grounded in evidence.
- If STATUS.md is understating something genuinely complete, fix that too.
- Time-box depth per audit slice:
  - do enough work to determine truth, major gaps, and dependency status
  - do not attempt infinite perfection before moving on

AUDIT TRACKS:

### Track 1: Status Claim Reality
- Use ~/Code/pulp/planning/status-claim-reality-audit-spec.md
- Audit all “complete”, “working today”, “validated”, “usable”, and support claims
- Focus especially on:
  - planning/STATUS.md
  - README.md
  - VISION.md
  - docs/status/*.yaml
  - examples and getting-started docs
- Distinguish:
  - implemented
  - wired into intended path
  - validated
  - documented honestly

### Track 2: Test Adequacy And Validation Depth
- Use ~/Code/pulp/planning/test-adequacy-and-validation-depth-audit-spec.md
- Determine whether current tests and validators actually justify maturity claims
- Be skeptical about raw test counts
- Focus on:
  - macOS/iOS quality and Apple-platform validation first
  - deterministic DSP/audio validation
  - state/parameter coverage
  - plugin validators
  - platform/runtime validation
  - negative-path, teardown, lifecycle, and edge-case coverage

### Track 3: Real-Time And Thread-Safety Correctness
- Use ~/Code/pulp/planning/realtime-thread-safety-correctness-audit-spec.md
- Verify whether the documented thread model matches the implementation
- Focus on:
  - audio/UI/host/background thread boundaries
  - lock-free primitive usage
  - atomic ordering assumptions
  - real-time safety on hot paths

### Track 4: Docs / Story Alignment
- Treat docs consistency as an engineering problem, not a marketing problem
- Specifically compare and align:
  - README.md
  - VISION.md
  - planning/STATUS.md
  - docs/status/*.yaml
  - key guides and reference pages
- Identify where VISION language is being mistaken for shipped capability
- Identify where “what works today” is out of sync with code or test evidence
- Create work items to close those gaps

### Track 5: Gap Generation And Next Work
- For every major mismatch, create the follow-up work item needed to close it
- Group work by themes, not one-file-at-a-time chores
- Examples:
  - status tracker hardening
  - docs/story alignment hardening
  - example/public API cleanup
  - validator/CI hardening
  - thread-safety hardening
  - platform maturity closure
- Produce a prioritized implementation order after the audit
- Mark dependency chains explicitly
- Maintain a return-to list for anything blocked

### Track 6: Execute Gap Closures
- After the audit baseline is established:
  - refresh STATUS.md to truth
  - create the gap matrix / work-item files
  - generate `planning/ralph-loop-prompt-8.md` for the next major loop
  - start working on the highest-priority gaps that are clearly scoped and high leverage
  - keep going through those gaps rather than stopping after the first one
- Work in sequence unless parallel work is clearly safe and non-blocking
- Prefer early gap work that improves trust quickly:
  - status/docs consistency hardening
  - public/private API cleanup in docs/examples
  - validation/CI hardening
  - claim-surface corrections
- Keep STATUS.md current as the gap work lands
- Do not promote a claim back to “complete”, “working”, or “validated” until strong testing or validation evidence is in place
- When a gap is skipped because of a dependency, add it to the return-to list and revisit it as soon as the prerequisite lands

REQUIRED OUTPUTS FROM THIS LOOP:

1. A real, evidence-backed STATUS.md
- Update STATUS.md so it reflects the actual repo, not the aspirational or drifted story
- Review all major sections and phases
- Ensure counts, completion labels, platform support, examples, test counts, and subsystem claims are internally consistent

2. Findings-first audit docs in planning/
- Recommended filenames:
  - status-claim-reality-audit-YYYY-MM-DD.md
  - test-adequacy-and-validation-depth-audit-YYYY-MM-DD.md
  - realtime-thread-safety-correctness-audit-YYYY-MM-DD.md
  - docs-story-alignment-audit-YYYY-MM-DD.md (if useful separately)
  - phase-gap-matrix-YYYY-MM-DD.md or similar, if needed
  - return-to-queue-YYYY-MM-DD.md or similar, if useful

3. Follow-up work-item specs in planning/
- Create implementation-target specs for the biggest gaps uncovered
- These should be what later implementation loops build against

4. A next loop prompt
- Create `planning/ralph-loop-prompt-8.md`
- Write it in the same quoted operational style as the prior Ralph loop prompts
- Base it on the audited reality established by this loop, not on stale assumptions
- Target the highest-priority coherent next workstream that should run after Loop 7
- If the next workstream has unresolved prerequisites, say so explicitly in the prompt and scope the loop accordingly

5. Gap-closure work
- After the audit baseline is set, continue executing high-priority gap closure work
- Update STATUS.md as items move from claim to verified reality
- Keep this work tightly coupled to the audit findings
- Prefer completing a smaller number of high-value closures well over opening many shallow workstreams

6. A next-work plan
- At the end of the loop, state:
  - what should be implemented next
  - in what order
  - which items are prerequisites for others
  - which claims should remain reduced until that work lands
  - which deferred items must be revisited when prerequisites are complete

STATUS RESET RULES:
- STATUS.md is not a historical monument in this loop. It is an audited artifact.
- If existing completion language is too strong, weaken it.
- If something marked complete is only partially true, split the claim:
  - implemented
  - validated
  - documented
  - productized
- If test counts or example counts are inconsistent, fix them.
- If a feature appears only in VISION or docs but not in repo evidence, do not let STATUS.md imply it is done.

REPOPROMPT USAGE (MANDATORY):
- Use RepoPrompt context_builder for any deep subsystem audit before making findings.
- Use RepoPrompt review mode when inspecting whether a phase/deliverable is truly complete.
- Use RepoPrompt to inspect code, tests, docs, manifests, and CMake surfaces together.
- Use RepoPrompt to build the evidence base behind each major STATUS.md correction.
- Use RepoPrompt again after STATUS.md refreshes to verify that the first round of gap-closure work still matches the updated reality.

PARALLELIZATION:
- Use /codex <task> for parallel work that would speed things up.
- Good for Codex:
  - bounded phase audits
  - docs/status consistency sweeps
  - claim-matrix preparation
  - test inventory extraction
  - evidence gathering across many files
  - drafting focused follow-up work-item specs
  - small docs/status hardening passes once the decision is already made
- Bad for Codex:
  - final repo-wide truth judgments
  - cross-cutting prioritization
  - core architecture decisions
  - format adapter integration
  - sync primitive implementation
  - anything where multiple conflicting sources need one final call
- Run Codex in background and continue your own work.
- Check Codex output and verify it against repo evidence before marking work complete or updating STATUS.md.
- If /codex is unavailable or errors out, continue the loop yourself rather than blocking on delegation.

CHECKPOINTS:
- Every 10 iterations:
  - summarize what has been audited
  - summarize what has been corrected in STATUS.md/docs
  - list what remains unverified
  - list what gap closures have landed
  - decide whether to keep auditing the next slice or pivot harder into closure work
- Do not let the loop stay in pure-audit mode indefinitely once a credible baseline exists.

NO-SLOP STANDARD:
- Do not smooth over contradictory claims.
- Do not let “code exists somewhere in the tree” count as completion.
- Do not let “tests exist” count as thorough validation.
- Do not let “VISION says it” count as shipped capability.
- Do not rewrite STATUS.md cosmetically; make it more truthful.

TESTING / VERIFICATION (MANDATORY FOR THIS LOOP):
- When changing STATUS.md or docs claims, verify against actual repo evidence:
  - code paths
  - tests
  - validators
  - CI workflows
  - examples
- Run relevant doc consistency checks after doc/status edits.
- If you make a strong truth claim in an audit memo, back it with file/line evidence.
- When closing a gap after the audit, verify the gap is actually closed before promoting the claim in STATUS.md.
- Strong testing is mandatory for gap closure work:
  - add or strengthen tests where the gap exposed weak coverage
  - run validators where the claim depends on validator behavior
  - prefer evidence that proves the claim, not just evidence that the code builds
  - do not accept “probably works” as sufficient to restore a stronger status label

GIT DISCIPLINE:
- Commit at the end of every iteration where audit docs or planning artifacts were updated.
- Commits must be focused and explain what was audited or what planning artifact was created.
- No “WIP” commits.
- Do NOT push unless asked.

EACH ITERATION MUST:
1. Read STATUS.md, README.md, VISION.md, and the relevant audit specs
2. Pick the next audit slice or phase range to verify
3. Use RepoPrompt to inspect the actual code/tests/docs behind the claims
4. Delegate bounded evidence-gathering or drafting slices to /codex where that speeds things up
5. Write findings-first audit notes
6. Update STATUS.md if the evidence is strong enough to justify the correction
7. Create or refine follow-up work-item specs for gaps uncovered
8. Update the dependency map / return-to list for any skipped or blocked items
9. Once the audit baseline is strong enough, pick the next unblocked highest-priority gap and continue closing gaps in priority order
10. Add or strengthen tests/validation needed to justify the closure
11. Run relevant docs consistency checks and any directly relevant verification for the gap work
12. Re-check whether any previously blocked items have become unblocked
13. Commit audit/planning/closure changes with a clean message
14. Summarize what was audited, what was corrected in STATUS.md/docs, what gap work completed, what testing/validation now backs it, what remains blocked, and what should come next

COMPLETION CONDITION:
- STATUS.md has been reset to an evidence-backed version
- README.md / VISION.md / docs/status mismatches are documented and major ones have follow-up work items
- All major phases 0-20 have been reviewed or explicitly triaged
- Test adequacy gaps are documented
- Real-time/thread-safety audit findings are documented
- A phase-gap matrix or equivalent gap artifact exists
- High-priority gap-closure work has been executed, not just queued
- STATUS.md has been updated to reflect both the audited baseline and the confirmed closure work
- A prioritized next-work plan exists
- The repo’s biggest trust gaps are identified clearly

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: AUDIT COMPLETE

IF STUCK:
- If a claim cannot be verified conclusively, mark it as unresolved instead of guessing
- If evidence is mixed, weaken the status claim and document why
- If a subsystem is too large, split by phase or capability cluster and continue
- If truly stuck, ask the user for help" --completion-promise "DONE" --max-iterations 120
