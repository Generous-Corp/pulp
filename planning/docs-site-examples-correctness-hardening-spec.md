# Docs Site + Examples + Correctness Hardening Spec

Date: 2026-03-25
Audience: implementation agent
Status: proposed follow-up work item

## Purpose

Build the next documentation milestone for Pulp:

- a browsable hosted docs site generated from the repo-owned docs
- a stronger example-driven documentation surface
- correctness hardening so docs, manifests, examples, and the local docs CLI agree with the codebase

This spec builds on:

- [versioned-developer-docs-system.md](/Users/danielraffel/Code/pulp/planning/versioned-developer-docs-system.md)
- [docs-system-implementation-spec.md](/Users/danielraffel/Code/pulp/planning/docs-system-implementation-spec.md)
- [docs-platform-improvement-plan.md](/Users/danielraffel/Code/pulp/planning/docs-platform-improvement-plan.md)

## Goal

Move from “good local docs foundation” to “real documentation experience.”

The result should feel useful in three modes:

- local terminal usage via `pulp docs`
- local reading in the repo
- hosted online reading for humans

## Scope

This milestone has three tracks:

1. **Docs site**
Generate a simple hosted docs site from the existing `docs/` tree.

2. **Examples**
Turn examples into a first-class docs surface with richer pages and navigation.

3. **Correctness hardening**
Fix trust issues where docs/manifests/CLI/policies disagree with the actual code.

## Non-Goals

Not required in this milestone:

- full generated API reference from headers
- search backend service
- separate authoring system outside the repo
- large visual redesign work
- release-tagged docs snapshots beyond preparing the structure

## Deliverables

### 1. Hosted docs site generation

Implement a repo-based docs site build that renders the existing `docs/` content into static HTML.

Requirements:

- site source remains the existing Markdown/YAML in `docs/`
- generated site is branch-owned
- online structure supports both `main` and `develop`
- site nav is organized around:
  - Overview
  - Getting Started
  - Support Matrix
  - Modules
  - Examples
  - CLI
  - CMake
  - Testing
  - Policies

The static site generator may be simple. Favor straightforward maintainability over polish.

### 2. Example gallery and per-example pages

Expand the docs so examples are easy to browse and compare.

Requirements:

- add an examples index page
- add or improve one page per example
- classify each example as:
  - `reference`
  - `validation`
  - `experimental`
- each example page must include:
  - summary
  - what it demonstrates
  - supported formats
  - supported platforms
  - key files
  - known limitations
  - related examples

If feasible, include screenshots or output visuals for examples that benefit from them.

### 3. Capability browsing surface

Add a dedicated capabilities reference.

Create:

- `docs/reference/capabilities.md`

This page should organize current Pulp capabilities by area:

- formats
- platforms
- audio I/O
- MIDI I/O
- DSP/signal
- state/automation
- view/UI
- rendering/GPU
- tooling/CLI
- shipping/release

Each capability entry should be tied to:

- current status
- relevant module
- relevant docs
- relevant examples

### 4. Correctness hardening of docs and manifests

This is a required part of the milestone.

The docs system should not describe itself in ways that contradict the code.

At minimum:

- align module dependencies in docs with real `CMakeLists.txt` dependency graphs
- align support status docs with actual current repo behavior
- align policy docs with current examples/docs, or explicitly scope the policy as aspirational if not yet enforced
- remove or rewrite any docs claims that overstate current implementation

### 5. Correctness hardening of the local docs CLI

The `pulp docs` CLI must become reliable enough to trust for local lookup.

At minimum:

- make `show command` render nested subcommands correctly
- make `show support` perform structured lookup rather than loose text matching
- preserve local-only behavior
- improve formatting so outputs are readable and unambiguous

## Content Requirements

### Docs site landing page

The landing page should answer:

- what Pulp is
- what is supported today
- where to start
- where to find examples
- where to find CLI and CMake reference

### Support matrix page

The site should render support status in a clearly scannable way.

It should answer:

- what is stable
- what is usable
- what is experimental
- what is partial
- what is planned

This page must be generated from local manifests, not maintained separately.

### Module reference

The module reference should feel like a browseable framework manual, not just a manifest dump.

Expected elements:

- module index
- module status
- dependencies
- key public headers
- concise explanation of responsibility
- links to examples and related docs

### Examples section

This should be the most practical part of the site.

It should make it obvious:

- which example to start from
- which example demonstrates which capability
- which examples are polished references vs validation artifacts

## Suggested File Additions

Exact site generator choice is flexible, but the content additions should include something close to:

```text
docs/
  reference/
    capabilities.md

  examples/
    index.md
    pulp-gain.md
    pulp-tone.md
    pulp-effect.md
    pulp-compressor.md
    ui-preview.md

  assets/
    diagrams/
    examples/

site/
  ... generator or config as needed ...
```

If an existing docs layout should be reused instead of adding `site/`, that is acceptable.

## Branch And Publishing Model

The site must preserve the repo-owned docs model.

Requirements:

- `main` docs render from `main`
- `develop` docs render from `develop`
- the site visibly indicates which branch/version is being viewed

You do not need to implement release-tag docs publishing in this milestone, but the structure should not block it.

## Correctness Hardening Checklist

The implementation agent should explicitly verify:

1. Module dependencies documented in Markdown match actual CMake target dependencies.
2. Module dependency manifests match the same truth.
3. Support matrix entries are grounded in actual code/build/test reality.
4. Policy docs do not silently contradict shipped examples and guides.
5. The docs CLI reads manifests in a structured enough way to avoid misleading output.
6. All doc links/anchors referenced by manifests resolve.

## Acceptance Criteria

This work is done when:

1. A static docs site can be generated locally from the repo docs.
2. The site has clear navigation for Overview, Support Matrix, Modules, Examples, CLI, CMake, Testing, and Policies.
3. Examples are a first-class docs section with meaningful per-example pages.
4. `docs/reference/capabilities.md` exists and is useful.
5. The local docs CLI outputs for `show support` and `show command` are trustworthy and readable.
6. Docs/manifests/policies no longer materially disagree with the code in the key reviewed areas.
7. The system still works without web calls for local agents.

## Quality Bar

Optimize for:

- accuracy
- browseability
- practical usefulness
- local-first operation
- simple maintainable publishing

Do not optimize for:

- elaborate site theming
- large framework-specific infra
- speculative documentation for unimplemented features

## Suggested Prompt For An Agent

```text
Implement the follow-up docs milestone described in planning/docs-site-examples-correctness-hardening-spec.md.

Focus on three things:
- generating a browsable docs site from the repo-owned docs
- making examples a first-class documentation surface
- hardening correctness so docs, manifests, CLI output, and policy docs align with the actual codebase

Use these planning docs for context:
- planning/versioned-developer-docs-system.md
- planning/docs-system-implementation-spec.md
- planning/docs-platform-improvement-plan.md

Requirements:
- keep the repo-owned, local-first docs model
- keep the CLI usable without web calls
- do not invent support claims not grounded in the repo
- prefer a simple static site pipeline over a complex docs platform

Before editing, inspect the current docs tree, docs manifests, and `tools/cli/pulp_cli.cpp`. Then implement the smallest coherent milestone that satisfies the acceptance criteria.
```

## Bottom Line

This work item is about turning the docs into something people will genuinely want to read:

- online
- locally
- by topic
- by example
- by module
- by capability

while preserving the strongest part of the current architecture:

- the repo remains the source of truth.
