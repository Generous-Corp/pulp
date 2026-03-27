# Docs System Implementation Spec

Date: 2026-03-25
Audience: implementation agent
Status: proposed work item

## Purpose

Build the first usable version of Pulp's local-first, branch-versioned documentation system.

This spec is the implementation target.
Use [versioned-developer-docs-system.md](/Users/danielraffel/Code/pulp/planning/versioned-developer-docs-system.md) for rationale and operating model.

## Scope

Implement the smallest useful slice that gives:

- local developer docs in `docs/`
- machine-readable status/reference manifests
- a local CLI docs reader
- an explicit code style and agent contribution contract

Do not build a hosted site in this first pass.
Do not try to solve full API reference generation in this first pass.

## Deliverables

### 1. Docs folder structure

Create:

```text
docs/
  README.md

  concepts/
    overview.md
    architecture.md

  guides/
    getting-started.md
    build.md
    testing.md
    examples.md

  reference/
    cli.md
    cmake.md
    modules.md

  status/
    docs-index.yaml
    support-matrix.yaml
    modules.yaml
    cli-commands.yaml
    cmake-functions.yaml
    style-rules.yaml

  policies/
    code-style.md
    agent-contribution-rules.md
```

### 2. Initial reference content

Populate the docs with accurate current-state content for this branch.

The initial docs must be grounded in the repo as it exists now, not in the aspirational vision.

### 3. Local CLI docs reader

Extend the `pulp` CLI with a minimal `docs` namespace:

```bash
pulp docs index
pulp docs search <query>
pulp docs open <slug>
pulp docs show support <thing>
pulp docs show command <name>
pulp docs show cmake <name>
pulp docs show style
```

The commands should read only local files from `docs/`.

### 4. Code style and agent contribution rules

Add:

- `docs/policies/code-style.md`
- `docs/policies/agent-contribution-rules.md`
- `docs/status/style-rules.yaml`

These should define the repository’s contribution contract in a way that is readable by both humans and agents.

## Content Requirements

### `docs/README.md`

Must explain:

- this docs tree is the source of truth for the current branch
- human docs live in Markdown
- support/reference status lives in YAML manifests
- hosted docs, if published later, mirror these files

### `docs/concepts/overview.md`

Must answer:

- what Pulp is
- what it is for
- what problems it solves today
- how it is organized at a high level

### `docs/concepts/architecture.md`

Must describe:

- major subsystems
- dependency direction
- public vs internal surfaces
- current modularity expectations

### `docs/reference/cli.md`

Must document:

- currently implemented commands only
- what each command does
- current caveats
- whether the command is `usable`, `experimental`, or `partial`

### `docs/reference/cmake.md`

Must document:

- `pulp_add_plugin()`
- `pulp_add_app()`
- current file conventions
- current limitations

### `docs/reference/modules.md`

Must list:

- each subsystem
- its purpose
- its maturity
- key dependencies
- key public headers

### `docs/policies/code-style.md`

Must include:

- public API boundary rules
- module boundary rules
- real-time audio safety rules
- platform isolation rules
- build hygiene rules
- tests/docs-update expectations

### `docs/policies/agent-contribution-rules.md`

Must include:

- what counts as a public-facing change
- when docs must be updated
- when support manifests must be updated
- when tests are required
- what agents should avoid changing casually

## Manifest Requirements

### `docs/status/docs-index.yaml`

Acts as the local table of contents for the CLI.

Example shape:

```yaml
docs:
  - slug: overview
    path: concepts/overview.md
    kind: concept
  - slug: architecture
    path: concepts/architecture.md
    kind: concept
  - slug: cli
    path: reference/cli.md
    kind: reference
```

### `docs/status/support-matrix.yaml`

Must encode current support levels for:

- platforms
- formats
- major subsystems

Use this status vocabulary only:

- `stable`
- `usable`
- `experimental`
- `partial`
- `planned`
- `unsupported`

### `docs/status/modules.yaml`

Must list modules with:

- name
- summary
- status
- dependencies
- docs path

### `docs/status/cli-commands.yaml`

Must list commands with:

- name
- status
- summary
- docs anchor/path

### `docs/status/cmake-functions.yaml`

Must list functions with:

- name
- status
- summary
- docs anchor/path

### `docs/status/style-rules.yaml`

Must list the highest-value repo rules in machine-readable form.

At minimum include:

- public headers only
- no `src/` includes in consumer docs/examples
- no allocation/locks/exceptions on audio thread
- public changes require docs updates
- support changes require manifest updates

## CLI Behavior Requirements

### `pulp docs index`

Print a readable list of locally available docs using `docs-index.yaml`.

### `pulp docs search <query>`

Search local docs content only.
Use a local file search approach.

### `pulp docs open <slug>`

Resolve the slug via `docs-index.yaml` and print the local file path or display the file content in a terminal-friendly way.

### `pulp docs show support <thing>`

Read from `support-matrix.yaml`.

### `pulp docs show command <name>`

Read from `cli-commands.yaml`, then point to the reference doc.

### `pulp docs show cmake <name>`

Read from `cmake-functions.yaml`, then point to the reference doc.

### `pulp docs show style`

Read from `style-rules.yaml` and point to:

- `docs/policies/code-style.md`
- `docs/policies/agent-contribution-rules.md`

## Quality Bar

This work should optimize for:

- accuracy over breadth
- local usability over polished rendering
- explicit support status over vague prose
- stable paths over clever structure

Do not over-engineer this first version.

## Non-Goals

Not required in this milestone:

- generated API docs from headers
- a hosted docs website
- branch publishing automation
- release-version snapshots
- full lint/CI validation of docs

Those can come later.

## Acceptance Criteria

This work is done when:

1. The `docs/` tree exists with the files listed above.
2. The initial docs reflect the current branch honestly.
3. The YAML manifests exist and are internally consistent.
4. The CLI can answer the listed `pulp docs ...` commands locally.
5. There is a clear style guide and agent contribution contract.
6. An agent can inspect the checkout and answer basic support/usage questions without web calls.

## Build-Against Guidance

If you are asking an implementation agent to do this work, tell it:

- Build against this spec first.
- Use `planning/versioned-developer-docs-system.md` for rationale and intended long-term direction.
- Keep the first pass local-first and current-branch-specific.
- Do not invent support claims that are not grounded in the repo.
- Prefer simple file-backed CLI behavior over a more elaborate framework.

## Suggested Prompt For An Agent

You can hand an agent something close to this:

```text
Implement the first local-first docs system for Pulp using planning/docs-system-implementation-spec.md as the build target and planning/versioned-developer-docs-system.md as the architectural rationale.

Requirements:
- create the docs tree and initial docs/manifests exactly within scope
- extend the CLI with the specified local `pulp docs` commands
- add `code-style.md`, `agent-contribution-rules.md`, and `style-rules.yaml`
- keep all content accurate to the current branch
- do not depend on web calls
- do not broaden scope into a hosted docs site or generated API docs

Before changing code, inspect the current CLI and docs layout. Then implement the smallest coherent version that satisfies the acceptance criteria.
```
