# Status Claim Reality Audit Spec

Date: 2026-03-26
Audience: review agent or senior engineer performing a repo-reality audit
Status: proposed follow-up audit

## Purpose

Audit whether Pulp's current public and internal claims are actually true.

This audit exists to answer a simple question:

- when Pulp says something is `complete`, `working`, `validated`, `usable`, or `what works today`, is that claim supported by the current repo?

The goal is not to punish ambition. The goal is to eliminate credibility drift.

## Why This Audit Matters

Pulp currently communicates capability and maturity through several overlapping surfaces:

- `planning/STATUS.md`
- `README.md`
- `VISION.md`
- `docs/status/*.yaml`
- `docs/reference/*`
- `docs/guides/*`
- example pages and example source

If those surfaces disagree, experienced engineers will distrust the whole repo, even if much of the implementation is real.

## Primary Questions

The audit must answer these questions directly.

1. For every major claim in `STATUS.md`, is the claimed feature actually implemented?
2. If implemented, is it wired into the intended path rather than just present somewhere in the tree?
3. If wired, is it validated by tests, validators, runtime checks, or documented manual verification?
4. Do `README.md`, `VISION.md`, `STATUS.md`, and `docs/status/*.yaml` tell the same truth at the same maturity level?
5. Are there any claims that are technically true but presented in a way that overstates maturity?

## Scope

This audit should review claims across four surfaces.

### 1. Status tracker claims

Review:

- phase completion labels
- feature completion bullets
- test counts
- example counts
- platform/format/rendering/support claims

### 2. Public-facing repo story

Review:

- `README.md`
- `VISION.md`
- top-level docs entry points

Questions:

- does the repo front door accurately describe what is current versus aspirational?
- does it distinguish "vision" from "working today" clearly enough?

### 3. Machine-readable docs/status truth

Review:

- `docs/status/support-matrix.yaml`
- `docs/status/modules.yaml`
- `docs/status/cli-commands.yaml`
- other YAML manifests that encode maturity/status

Questions:

- do these manifests match the code?
- do they match each other?
- do they match `README.md` and `STATUS.md`?

### 4. Examples and docs as proof surfaces

Review:

- examples
- getting-started docs
- module guides
- capability/reference docs

Questions:

- are examples demonstrating supported public usage or internal shortcuts?
- do examples and guides reinforce the same maturity story as the manifests?

## Non-Goals

Not required in this audit:

- rewriting implementation
- subjective architecture judgment beyond claim accuracy
- deep thread-safety review
- full test-depth review beyond whether a claim is supported at all

Those are separate audits.

## Audit Method

### Step 1: Extract claims

Build a claim inventory from:

- `planning/STATUS.md`
- `README.md`
- `VISION.md`
- docs manifests and reference pages

Classify claims like:

- complete
- working today
- validated
- usable
- experimental
- planned

### Step 2: Verify repo evidence

For each major claim, determine:

- implementation present
- implementation wired into intended path
- tests/validators/runtime evidence present
- docs consistency present

### Step 3: Grade each claim

Each claim should end up in one of these buckets:

- `true and well-supported`
- `true but overstated`
- `partially true / scope unclear`
- `not currently supported by repo evidence`

### Step 4: Identify drift clusters

Group problems by theme rather than isolated lines:

- counts and completion language
- platform support overstatement
- render/UI maturity overstatement
- docs/examples using internals
- validators/tests implied but not actually enforced

## Deliverables

### 1. Findings-first audit memo

The main output should be a review document with findings ordered by severity.

Each finding should include:

- the claim
- file/line references
- repo evidence for or against it
- why the claim matters

### 2. Claim matrix

A structured table or matrix with:

- claim text
- source file
- current verdict
- evidence
- recommended wording if current wording is too strong

### 3. Follow-up work items

For each major drift cluster, create a follow-up work item spec if needed.

Examples:

- status tracker hardening
- docs/story alignment
- example/public API cleanup
- platform/support claim correction

## Recommended Files To Inspect

At minimum:

- `README.md`
- `VISION.md`
- `planning/STATUS.md`
- `docs/status/`
- `docs/reference/`
- `docs/guides/`
- example source and example docs
- relevant tests and CI workflows when claims depend on validation

## Acceptance Criteria

This audit is complete when:

1. Major status and support claims have been inventoried.
2. Each major claim has a verdict grounded in repo evidence.
3. Mismatches between `README.md`, `VISION.md`, `STATUS.md`, and docs manifests are documented.
4. Overstated or ambiguous claims are identified explicitly.
5. Follow-up work items exist for the most important trust gaps.

## Suggested Prompt For An Agent

```text
Perform the status-claim reality audit described in planning/status-claim-reality-audit-spec.md.

Focus especially on:
- planning/STATUS.md
- README.md
- VISION.md
- docs/status/*.yaml
- examples and getting-started docs

Goals:
- determine whether current “complete”, “working today”, “validated”, and support claims are actually true
- identify where the repo is overstating or understating maturity
- document mismatches across status/docs/vision surfaces
- create concrete follow-up work item specs for the biggest claim-drift clusters

Requirements:
- do not implement product code
- be strict about evidence
- distinguish between implemented, wired, validated, and documented
- treat examples and docs as part of the claim surface
```

## Bottom Line

This audit exists to make Pulp's story match its reality closely enough that senior engineers stop second-guessing the repo on first contact.
