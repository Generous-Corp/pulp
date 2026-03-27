# Test Adequacy And Validation Depth Audit Spec

Date: 2026-03-26
Audience: review agent or senior engineer performing a validation-depth audit
Status: proposed follow-up audit

## Purpose

Audit whether Pulp's current tests and validators are as convincing as the repo implies.

This audit should answer:

- do the current tests actually justify the maturity claims made in `STATUS.md`, `README.md`, and docs?

The issue is not just test count. It is test depth, coverage of risky behavior, and whether validation is happening where it matters.

## Why This Audit Matters

A framework can have many tests and still feel weak if those tests do not cover:

- edge cases
- lifecycle transitions
- platform-specific behavior
- format-validator behavior
- state round-trips
- real-time-sensitive boundaries
- failure paths

Pulp already has a broad validation vision in [15-validation-plan.md](/Users/danielraffel/Code/pulp/planning/15-validation-plan.md). This audit should determine how much of that vision is real today.

## Primary Questions

1. Are the current tests covering the most important framework behaviors, or mainly happy paths?
2. Do test counts correspond to meaningful validation depth?
3. Which important surfaces have weak or missing coverage despite being presented as mature?
4. Are official or community validators actually integrated and enforced where the repo suggests they are?
5. Does current CI validate the same things the docs imply?

## Scope

This audit should review validation depth across these areas.

### 1. DSP and audio behavior

Look for coverage of:

- deterministic known-input / known-output processing
- impulse and latency checks
- frequency-response or accuracy checks
- parameter smoothing behavior
- sample-rate and buffer-size variation
- channel layout variation
- state-dependent audio behavior

### 2. Parameter and state behavior

Look for coverage of:

- parameter definition and metadata
- normalization and denormalization
- modulation paths
- gesture behavior
- save/restore round-trips
- cross-version state handling where claimed
- preset manager behavior

### 3. Plugin format behavior

Look for coverage of:

- plugin validators (`pluginval`, `auval`, `clap-validator`, etc.)
- adapter lifecycle
- multi-bus behavior
- latency/tail reporting
- parameter enumeration
- state persistence
- editor/host interactions where claimed

### 4. UI and render behavior

Look for coverage of:

- widget interaction semantics
- focus and keyboard traversal
- layout and invalidation behavior
- render-path behavior where maturity is claimed
- screenshot or visual regression coverage

### 5. Platform behavior

Look for coverage of:

- platform-specific runtime checks on macOS, Windows, Linux, iOS where claimed
- VM or device validation evidence
- packaging/signing/notarization/install flows where described as supported

### 6. Failure and teardown behavior

Look for coverage of:

- invalid inputs
- unsupported configurations
- cleanup and shutdown
- repeated open/close or attach/detach cycles
- fallback paths

## Non-Goals

Not required in this audit:

- rewriting tests
- broad architecture/style judgments
- deep thread-safety correctness review beyond test evidence gaps

That last point is covered by the dedicated thread-safety audit.

## Audit Method

### Step 1: Build a validation inventory

Map the current test and validation surfaces:

- unit tests
- integration tests
- golden-file tests
- validator invocations
- CI workflows
- manual/VM/device validation claims

### Step 2: Compare claims to evidence

For each major maturity claim, determine:

- what tests exist
- what validators exist
- what CI enforces
- what is only planned or documented

### Step 3: Score coverage depth

Use buckets such as:

- `strong`
- `adequate but shallow`
- `spotty`
- `missing`

These should be applied by surface, not just by subsystem.

### Step 4: Identify validation blind spots

Examples of likely blind spots to test for:

- buffer-size extremes
- sample-rate changes
- channel-layout edge cases
- validator strictness gaps
- packaging/signing flows that are documented but not verified
- negative-path and teardown coverage

## Deliverables

### 1. Findings-first audit memo

The main output should be a review document with findings ordered by severity.

Each finding should include:

- the claimed capability or maturity signal
- the current validation evidence
- the missing depth or gap
- why that gap matters

### 2. Coverage matrix

Produce a matrix by major area:

- DSP/audio
- state/parameters
- plugin formats
- UI/render
- platform/runtime
- shipping/release

For each area, show:

- current tests
- validators
- CI enforcement
- confidence level

### 3. Follow-up work item specs

For each major validation gap cluster, create a focused follow-up spec if needed.

Examples:

- format-validator hardening
- audio regression expansion
- platform validation hardening
- screenshot/visual regression hardening

## Recommended Files To Inspect

At minimum:

- test directories
- CI workflows
- validator invocations in scripts and CLI
- `planning/15-validation-plan.md`
- `planning/audio-validation-audit-and-hardening-plan.md`
- `planning/STATUS.md`
- `README.md`
- format adapter code where claims depend on validation behavior

## Acceptance Criteria

This audit is complete when:

1. Major test and validation surfaces are inventoried.
2. The audit distinguishes test count from validation depth.
3. Important blind spots are documented clearly.
4. The link between maturity claims and actual validation evidence is made explicit.
5. Follow-up work items exist for the biggest testing/validation gaps.

## Suggested Prompt For An Agent

```text
Perform the test adequacy and validation depth audit described in planning/test-adequacy-and-validation-depth-audit-spec.md.

Use these as context:
- planning/15-validation-plan.md
- planning/audio-validation-audit-and-hardening-plan.md
- planning/STATUS.md
- README.md

Goals:
- determine whether current tests and validators actually justify the repo’s maturity claims
- identify shallow or missing coverage in the most important areas
- separate strong validation from optimistic test-count storytelling
- create concrete follow-up work item specs for major validation gaps

Requirements:
- do not implement product code
- focus on evidence in tests, scripts, validators, and CI
- be strict about the difference between “has tests” and “is thoroughly validated”
```

## Bottom Line

This audit exists to answer whether Pulp's current test story is genuinely convincing, not just numerically impressive.
