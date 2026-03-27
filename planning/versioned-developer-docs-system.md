# Versioned Developer Docs System

Date: 2026-03-25
Purpose: propose how Pulp should organize developer documentation so it is branch-aware, easy to keep current, locally consumable by agents, and referenceable from the CLI without requiring web calls.

## Recommendation

Create a **local-first, versioned docs system** where:

- the repo is the source of truth
- `main` and `develop` each carry their own docs with the code
- machine-readable status/reference files live beside human-readable docs
- the CLI reads those local docs/manifests directly
- any hosted docs site is just a published mirror of the same files

This should be a **new work item**, not an appendix to the audit memo.

The audit memo should remain evaluative.
This document should define the actual docs operating model.

## Design Goals

The docs system should optimize for five things:

1. **Truth stays close to the code**
Docs should version with the branch, not live as a separate drifting website source.

2. **Local consumption is first-class**
Agents, contributors, and CLI tools should be able to answer most questions from the checkout alone.

3. **Support status is explicit**
It should be easy to tell whether something is supported, experimental, partial, or planned.

4. **Reference material is structured**
CLI commands, CMake functions, modules, formats, and support matrices should have machine-readable manifests.

5. **Publishing is optional, not authoritative**
A docs site is useful, but it should be generated from the same local sources, not maintained separately.

## Recommended Layout

I would keep the source-of-truth docs in `docs/` and split them by purpose:

```text
docs/
  README.md

  concepts/
    overview.md
    architecture.md
    design-principles.md
    public-api.md

  guides/
    getting-started.md
    build.md
    testing.md
    examples.md
    releasing.md
    contributing.md

  reference/
    cli.md
    cmake.md
    modules.md
    formats.md
    state.md
    view.md
    troubleshooting.md

  status/
    support-matrix.yaml
    modules.yaml
    cli-commands.yaml
    cmake-functions.yaml
    formats.yaml
    style-rules.yaml
    docs-index.yaml
    deprecations.yaml

  examples/
    pulp-gain.md
    pulp-tone.md
    pulp-effect.md
    pulp-compressor.md
    ui-preview.md

  policies/
    doc-style.md
    code-style.md
    agent-contribution-rules.md
    versioning.md
    compatibility.md
```

## Human Docs vs Machine Docs

This distinction matters.

### Human-readable docs

These should be Markdown and written for developers:

- concepts
- guides
- architecture docs
- example walkthroughs
- troubleshooting

### Machine-readable docs

These should be YAML or JSON and designed for:

- CLI lookup
- local agent consumption
- CI validation
- docs-site generation

The machine-readable files should answer questions like:

- Is AU currently supported on macOS?
- Is Linux audio I/O implemented or planned?
- What does `pulp validate` do today?
- What arguments does `pulp_add_plugin()` support?
- Which examples are reference-grade vs experimental?
- What maturity level does `pulp::inspect` have?
- What coding rules must agents follow?
- Which kinds of changes require tests or docs updates?

## Branch Strategy

The easiest model is also the best one:

- `main` contains the docs for `main`
- `develop` contains the docs for `develop`
- release tags preserve release docs automatically

That means the docs branch strategy is the same as the code branch strategy.

### Publish model

If you publish hosted docs, mirror branch versions explicitly, similar to JUCE’s split between stable and development documentation:

- `/main/` or `/stable/`
- `/develop/`
- later: `/v0.1/`, `/v0.2/`, etc.

The important part is that the hosted docs are a **rendered view of branch-owned docs**, not a separate content system.

Reference:

- JUCE branch documentation landing page: https://juce.com/learn/documentation/
- JUCE stable docs: https://docs.juce.com/master/index.html
- JUCE development docs: https://docs.juce.com/develop/index.html

## Agent-Local Consumption Model

Yes, these docs can absolutely be made agent-friendly and local-first.

In fact, I think they should be designed that way from the start.

### What an agent should be able to do locally

From the repo checkout alone, an agent should be able to answer:

- what Pulp is
- what modules exist
- what is supported on this branch
- how to build
- how to define a plugin
- what CLI commands exist
- what CMake functions exist
- whether a feature is stable, experimental, partial, or planned

### What makes docs easy for agents

Use:

- plain Markdown
- predictable paths
- small focused files
- YAML manifests for status/reference
- one canonical docs index
- stable vocabulary for maturity

Avoid:

- burying truth only in prose
- storing support status only in marketing docs
- forcing web lookups for basic questions
- having multiple conflicting "status" sources

## Code Style And Agent Contribution Contract

Yes, this is useful, and especially useful for agents.

The important thing is that this should not be just a formatting note.
It should be a short, repo-specific contribution contract.

I would split it into three files:

### `docs/policies/code-style.md`

Human-readable rules for:

- public API design expectations
- naming and file layout
- header and source organization
- comment style
- platform isolation
- build hygiene
- testing expectations

### `docs/policies/agent-contribution-rules.md`

Agent-focused rules for:

- what counts as a public-facing change
- when docs must be updated
- when status manifests must be updated
- when tests are required
- what not to do in real-time audio code
- what not to do to public API boundaries

### `docs/status/style-rules.yaml`

Machine-readable summary for the CLI and local agents.

Example:

```yaml
rules:
  public_headers_only:
    status: active
    summary: Consumer-facing docs and examples must not include from src/.

  audio_thread_no_alloc:
    status: active
    summary: No allocation, locks, or exceptions on the audio thread.

  docs_update_required_for_public_changes:
    status: active
    summary: Public behavior changes require docs/reference updates.

  support_manifest_update_required:
    status: active
    summary: Support-level changes require status manifest updates.
```

That gives you:

- prose for humans
- structured rules for agents
- local files the CLI can query

## What The Code Style Guide Should Cover

I would keep it short and opinionated.

The highest-value sections are:

1. Public API boundaries
- consumers include only from `include/`
- consumer docs/examples must not include from `src/`
- internal helper exposure must be deliberate

2. Module boundaries
- allowed dependency direction
- platform code belongs in `platform/`
- avoid cross-subsystem leakage

3. Real-time audio safety
- no heap allocation on audio thread
- no locks on audio thread
- no exceptions on audio thread
- avoid non-deterministic work in process callbacks

4. Build hygiene
- configure/build must not modify tracked source files
- generated files belong in build outputs unless intentionally checked in

5. Public docs obligations
- public behavior changes require doc updates
- support changes require manifest updates
- examples must declare whether they are reference, validation, or experimental

6. Tests
- new public behavior needs tests
- bug fixes should add or adjust a test when practical

7. Comments and naming
- comments explain constraints or non-obvious decisions
- avoid redundant comments
- naming should favor framework clarity over cleverness

### Suggested machine-readable status vocabulary

Use a fixed maturity enum across docs:

- `stable`
- `usable`
- `experimental`
- `partial`
- `planned`
- `unsupported`

That vocabulary should appear in:

- `status/support-matrix.yaml`
- `status/modules.yaml`
- `status/formats.yaml`
- `status/cli-commands.yaml`

## Example Manifests

These are the files I would treat as the local contract for tools and agents.

### `docs/status/support-matrix.yaml`

```yaml
platforms:
  macos:
    status: stable
  windows:
    status: experimental
  linux:
    status: experimental

formats:
  vst3:
    macos: stable
    windows: partial
    linux: partial
  au_v2:
    macos: usable
    windows: unsupported
    linux: unsupported
  clap:
    macos: usable
    windows: partial
    linux: partial

subsystems:
  runtime: stable
  state: stable
  format: usable
  view: experimental
  render: experimental
  inspect: planned
  ship: partial
```

### `docs/status/cli-commands.yaml`

```yaml
commands:
  build:
    status: usable
    summary: Configure and build the current project.
    args:
      - name: extra_args
        kind: passthrough
    docs: reference/cli.md#build

  validate:
    status: usable
    summary: Run available plugin validation checks.
    docs: reference/cli.md#validate

  ship:
    status: experimental
    summary: Signing and release-related commands.
    docs: reference/cli.md#ship
```

### `docs/status/cmake-functions.yaml`

```yaml
functions:
  pulp_add_plugin:
    status: usable
    docs: reference/cmake.md#pulp_add_plugin
    required:
      - FORMATS
    optional:
      - PLUGIN_NAME
      - BUNDLE_ID
      - MANUFACTURER
      - VERSION
      - CATEGORY
      - PLUGIN_CODE
      - MANUFACTURER_CODE
      - SOURCES

  pulp_add_app:
    status: partial
    docs: reference/cmake.md#pulp_add_app
```

## CLI Integration

Yes, the CLI should reference this local docs system directly.

That is one of the biggest leverage points here.

## Suggested CLI commands

I would add a `pulp docs` namespace with local-first behavior:

```bash
pulp docs index
pulp docs open getting-started
pulp docs open reference/cli
pulp docs search "sidechain"
pulp docs show support vst3
pulp docs show module view
pulp docs show command validate
pulp docs show cmake pulp_add_plugin
pulp docs path support-matrix
```

### Behavior

- `pulp docs index`
  Prints the local docs map from `docs/status/docs-index.yaml`

- `pulp docs search <query>`
  Uses local file search only

- `pulp docs show support <thing>`
  Reads from `support-matrix.yaml`

- `pulp docs show command <name>`
  Reads from `cli-commands.yaml` and opens the relevant reference section

- `pulp docs show cmake <name>`
  Reads from `cmake-functions.yaml`

- `pulp docs open <slug>`
  Opens the matching local Markdown file path

### Why this matters

Then:

- agents do not need web calls for basic repo truth
- contributors have one obvious interface
- the CLI becomes a thin reader over the same canonical docs files
- docs drift is reduced because docs are now part of the tool surface

## Docs Site Strategy

If you publish docs, publish them from the repo files with a static generator.

The docs site should not become the authoring system.

Suggested model:

- author in Markdown + YAML in `docs/`
- generate HTML for `main` and `develop`
- optionally generate release-version snapshots from tags

That gives you:

- local docs for agents
- CLI-readable docs
- website docs for humans
- one source of truth

## How To Keep Docs Current

This is where most teams fail.

You do not keep docs current by asking people nicely.
You keep them current by making the structure enforceable.

## Suggested maintenance rules

1. Every public surface has an owner doc

Examples:

- CLI commands -> `reference/cli.md` + `status/cli-commands.yaml`
- CMake functions -> `reference/cmake.md` + `status/cmake-functions.yaml`
- module support -> `reference/modules.md` + `status/modules.yaml`
- format support -> `reference/formats.md` + `status/formats.yaml`

2. Any PR that changes public behavior must touch the relevant docs or manifest

This can be lightweight, but it should be expected.

3. CI should validate docs structure

Examples:

- every manifest entry points to an existing doc
- every referenced command exists in code metadata
- every referenced module exists in the repo
- every maturity value uses the allowed enum

4. Stop using prose as the only status source

Support status should come from manifests first, then be rendered into prose/reference.

## Recommended Authoring Rules

To make the docs easy to maintain and easy for agents to read:

- keep files focused
- prefer one topic per file
- put status in YAML, not buried in paragraphs
- use stable headings
- use predictable filenames
- avoid giant “everything docs” pages
- keep examples linked to exact files
- keep every doc branch-owned

## Suggested First Milestone

I would not try to build the full docs system at once.

Start with this first slice:

1. Create the folder structure.
2. Add:
   - `docs/concepts/overview.md`
   - `docs/concepts/architecture.md`
   - `docs/reference/cli.md`
   - `docs/reference/cmake.md`
   - `docs/reference/modules.md`
   - `docs/status/support-matrix.yaml`
   - `docs/status/cli-commands.yaml`
   - `docs/status/cmake-functions.yaml`
   - `docs/status/modules.yaml`
   - `docs/status/docs-index.yaml`
3. Add a minimal `pulp docs` reader that can query those files locally.
4. Publish `main` and `develop` docs from those same files.

That gets you most of the value quickly.

## Recommendation On Placement

This should be a **new planning work item**, not folded into the review memo.

Reason:

- the audit memo answers "what is weak today"
- this work item answers "what system should we build"

That separation will keep both documents cleaner.

If you want, the audit memo can later gain a one-line pointer to this work item under the documentation section, but it should not absorb this whole proposal.

## Bottom Line

Yes, this is worth doing.

Yes, it can be branch-aware without becoming complicated.

Yes, it can be made easy for agents to consume locally.

Yes, the CLI should read from the same local docs/manifests.

The right model is:

- docs live with the code
- manifests provide machine-readable truth
- CLI reads local docs
- the website mirrors the repo
- `main` and `develop` each describe themselves

That gives you something much better than a web-only docs portal: a documentation system that is useful in the terminal, in editors, in agents, in CI, and on the web without drifting across those environments.
