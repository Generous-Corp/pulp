# Reusable Hybrid CI Extraction Proposal

**Date:** 2026-04-02  
**Status:** active Pulp implementation; phases 0-1 complete, phases 2-3 in progress  
**Revision:** v2  
**Working name:** `xci`  
**License target:** MIT  
**Target repo:** `github.com/danielraffel/xci`  

## Goal

Add Namespace support and clearer local-vs-cloud provider selection to Pulp's
existing CI stack first, while shaping the implementation so the reusable parts
can later move into a standalone tool instead of being rewritten from scratch.

The immediate Pulp-side goal is to make it easy to:

- keep local/SSH validation as the default fast path
- use GitHub Actions only when it is actually the right orchestrator
- switch GitHub-run workflow execution between GitHub-hosted and Namespace
  runner providers deliberately
- see where CI ran and what it cost

The longer-term extraction goal is to preserve the useful parts of Pulp's
local/cloud CI in a reusable standalone tool so future cross-platform projects
do not need to rebuild:

- local Mac + remote Ubuntu/Windows validation
- exact-SHA validation on remote machines
- machine-global queueing and saved results/logs
- multi-agent worktree-safe coordination
- SSH host / VM reuse
- deliberate switching between local and cloud backends
- deliberate switching between hosted runner providers
- merge-on-green and PR comment workflows
- local agent skill guidance for CI operation

The extraction should preserve what is already working in Pulp and make it easier
to configure new projects with:

- local VMs via UTM
- SSH-reachable cloud VMs
- optional GitHub-hosted workflow execution
- optional self-hosted GitHub Actions runners
- optional local workflow preflight runners
- optional container-native execution backends for the subset of jobs that fit

Related follow-on spec:

- `planning/desktop-automation-agent-spec-2026-04-03.md`

## Core insight

The thing worth extracting is **not** “yet another workflow runner.”

The thing worth extracting is a **hybrid CI control plane** for developers and
agents who work across multiple worktrees on one machine and need to:

- queue and prioritize jobs safely
- validate exact SHAs on real target machines
- switch between local hosts and cloud backends deliberately
- keep saved logs/results/status outside any one worktree
- support real GUI-capable VM testing when needed

That means the product should sit **above** execution backends, not try to
replace every backend itself.

For hosted workflow systems, it should also distinguish between:

- the **orchestrator** that receives events, owns workflow state, and reports
  checks
- the **runner provider** that supplies machines for those jobs

Example:

- GitHub Actions = orchestrator
- GitHub-hosted, Namespace, self-hosted, ARC-managed runners, Actuated, Depot,
  WarpBuild = runner provider choices under that orchestrator

## Product user requirements

These are the user-facing requirements that justify the product.

### Primary user stories

#### 1. Multi-worktree developer on one machine

As a developer working across multiple worktrees on one Mac, I want one shared CI
queue and one shared source of truth for status/logs/results so that:

- parallel worktrees do not trample each other
- I can keep developing while another worktree owns validation
- I do not lose track of what already passed on which SHA

#### 2. Coding agent operating from a worktree

As an agent running inside a repo worktree, I want to enqueue, monitor, tail
logs, and rerun the narrowest truthful scope through a stable CLI so that:

- I do not guess at shell flows
- I do not accidentally collide with other agents
- I can switch between local and cloud backends intentionally
- I can report accurate status to the user
- when a run is on the critical path, I can use a blocking wait mode and get an
  immediate completion signal instead of relying on manual polling

#### 3. Developer using local VMs instead of paid cloud minutes

As a developer with Ubuntu and Windows VMs on a Mac, I want to validate the exact
queued SHA on those machines over SSH, optionally waking or reusing the VMs, so
that:

- I can get real cross-platform proof without paying for hosted runners
- I can use GUI-capable local VMs when product validation needs a real window,
  plugin host, or device path
- I can keep my local machines as the first-line validation path

#### 4. Developer mixing local and cloud validation

As a developer, I want one tool that can target local hosts, cloud VMs, and
GitHub Actions as different backends without changing the mental model, so that:

- local is the fast/default path
- cloud is available when I need neutral hardware or hosted runners
- I can move a project between execution backends without rewriting the whole CI
  story

#### 4a. Developer switching GitHub runner providers

As a developer using GitHub Actions as the orchestrator, I want to switch
between GitHub-hosted, Namespace, or self-hosted runner providers without
rewriting the whole workflow model, so that:

- I can optimize for cost and speed without changing the higher-level CI flow
- provider-specific setup stays isolated
- the CLI and agent surfaces can clearly report where CI ran

#### 4b. Developer wanting usage and cost visibility

As a developer paying for CI, I want per-run and billing-period usage/cost
visibility in the CLI and agent/plugin surfaces, so that:

- I can tell whether a run was local, GitHub-hosted, Namespace, or self-hosted
- I can compare cost across providers
- I can keep that output on by default and turn it off if I do not want to see
  it

#### 5. Project maintainer extracting the stack to a new repo

As a maintainer starting a new cross-platform project, I want to configure target
topology, validators, and CI policy through a reusable config and a small set of
adapters, so that:

- I do not copy Pulp-specific scripts blindly
- I can bring up the same local/cloud CI model quickly in another repo
- I can preserve exact-SHA and queue semantics from day one

#### 6. Project maintainer dogfooding the abstraction in Pulp

As the maintainer of Pulp, I want Pulp to become the first consumer of the
extracted tool, so that:

- the abstraction is proven against a real demanding project
- regressions are caught quickly
- future improvements happen in one place

### Must-have v1 requirements

- machine-global queue across worktrees and agents
- stable CLI with structured output
- exact-SHA remote validation on SSH targets
- saved job status, per-target logs, and results outside any one worktree
- explicit target/backend topology config
- local/SSH/GitHub Actions backend support
- hosted runner-provider abstraction for GitHub Actions
- priorities, supersedence, and narrow rerun support
- small agent skill pack that maps intent to CLI commands
- prepared-build test-subset reruns after compile succeeds
- explicit same-SHA prepared-state policy controls (`auto`, `clean`, `reuse`)
- stage-aware rerun semantics for slow targets when full clean reruns are not
  required for truth
- richer structured failure summaries so agents/operators can triage without
  reopening raw logs immediately
- a first-class blocking `run/check` mode that waits for completion and emits a
  trustworthy notification/signal when the job finishes
- live target-state progress streaming while a blocking run is attached
- line-buffered attached output so long-running jobs remain visibly alive
- provider-neutral run provenance surface that reports orchestrator, runner
  provider, and target location
- usage/cost telemetry for providers that expose enough data, with normalized
  estimated totals when only partial billing data is available

### Nice-to-have later

- `act` integration for workflow preflight
- Dagger integration for containerizable step execution inside runners
- TUI/dashboard
- richer MCP integration
- secret-provider abstractions
- architecture-aware Linux backend selection
- desktop automation agent/session adapters for GUI-capable validation targets

## Current dogfood evidence from Pulp

The extraction should preserve the concrete within-target speedups that are now
working in Pulp's local CI reference implementation:

- prepared-build `ctest` subset reruns via `pulp ci-local retest`
- explicit same-SHA prepared-state policy via `--prepared auto|clean|reuse`
- stage-aware prepared resume via `pulp ci-local resume --from-stage <stage>`
- blocking attached runs that stream live target-state changes while waiting
- line-buffered attached output so quiet phases remain visibly alive
- persisted structured failure summaries that expose failed/skipped tests
  without reopening raw logs

Future dogfood extension:

- desktop-session automation on GUI-capable targets for launch, interaction,
  screenshots, logs, and artifact bundles as defined in
  `planning/desktop-automation-agent-spec-2026-04-03.md`

Those are not speculative requirements anymore; they are dogfooded behaviors the
extracted tool should carry forward.

### Non-goals for v1

- replacing GitHub Actions entirely
- inventing a new workflow DSL
- owning release/sign/notarize pipelines as the core product
- generic GUI automation framework
- full Kubernetes-native scheduling

## Problem

Pulp now has genuinely useful CI capabilities, but they are tightly coupled to the
repo:

- `tools/local-ci/local_ci.py`
- `validate-build.sh`
- `validate-build.ps1`
- `tools/local-ci/config.json`
- `.agents/skills/ci/SKILL.md`
- GitHub workflow assumptions under `.github/workflows/`

That means every future project would either:

1. copy the same logic again, or
2. fall back to weaker tools that do not preserve the same guarantees

The explicit goal of this proposal is to avoid rebuilding this again.

## Scope Decision

### v1 supports

- CI orchestration / control plane
- local and SSH target execution
- backend switching between local and cloud providers
- runner-provider switching inside GitHub Actions
- exact-SHA delivery to remote targets
- saved queue / status / logs / results
- GitHub PR helpers as an optional integration
- reusable examples/templates for GitHub Actions
- agent-friendly CLI with structured output
- minimal agent skills pack
- usage/cost visibility in status and reporting surfaces

### v1 does not need to support

- full release automation / signing / notarization as core product behavior
- GitHub Pages deployment
- docs deployment
- repo-specific dependency audit pipelines
- product-specific smoke tests

Those should remain example integrations until the reusable CI core is proven.

Short version:

- **CI:** yes
- **CD:** examples later, not core on day one
- **Control plane:** yes
- **Generic workflow runner replacement:** no

## Why Not Just Use Existing Tools?

The extraction only makes sense if it solves something the existing tools do not.

### Comparison matrix

| Capability | GitHub Actions + hosted runners | GitHub self-hosted runner | `act` | `wrkflw` | Dagger | Earthly | Pulp local CI today | Needed extracted tool |
|---|---|---|---|---|---|---|---|---|
| Run standard CI workflows | yes | yes | yes | yes | yes | yes | partial/manual | optional |
| Cloud execution backend | yes | partial | no | trigger only | yes | yes | manual via `gh workflow run` | optional |
| Real macOS + Windows + Ubuntu host validation | hosted only, not your local fleet | one runner per machine | weak/incomplete | weak/incomplete | weak for native host parity | weak for native host parity | yes | yes |
| Shared queue across local worktrees/agents | no | no | no | no | no | no | yes | yes |
| Exact-SHA validation on remote hosts independent of branch visibility | no | no | no | no | partial for pinned modules, not generic host validation | partial for builds, not generic host validation | yes | yes |
| SSH orchestration across hosts | no | no | no | no | no | no | yes | yes |
| VM fallback / UTM boot support | no | no | no | no | no | no | yes | yes |
| Saved local operator state/logs/results | limited UI | limited UI | terminal only | terminal/TUI only | traces, but different model | build logs/cache, different model | yes | yes |
| Priorities / supersedence / narrow rerun truth | no | no | no | no | workflow/check concurrency only | CI-level queueing only | yes | yes |
| GUI-capable local VM workflows for plugin testing | no | indirect at best | no | no | no | no | yes | yes |
| Agent-first CLI + skill flow | partial | partial | partial | partial | promising | weak | yes | yes |
| Best role in this architecture | cloud provider | execution backend | local workflow preflight | workflow runner companion | optional container backend | design reference only | current reference implementation | target design |

### Read on the alternatives

#### GitHub self-hosted runners

Good for:

- running standard GitHub Actions jobs on machines you control
- cloud and on-prem machines
- standard repository CI

Missing compared with Pulp's current value:

- no machine-global queue across worktrees
- no exact-SHA bundle sync to remote mirrors
- no UTM-specific host lifecycle
- no saved local operator model for `status`, `logs`, `bump`, and narrow reruns

This is useful as an execution backend or compatibility target, not a substitute
for the orchestration layer.

#### GitHub Actions itself

Good for:

- standard cloud CI/CD
- hosted runner access
- branch protections and repository-native status checks
- release/signing pipelines

Missing:

- local worktree-aware queueing
- exact-SHA remote proof on your own SSH/VM fleet
- saved local operator state independent of the GitHub UI

Conclusion:

- **authoritative cloud provider**
- **not the local orchestration layer**

#### `act`

Good for:

- fast local GitHub Actions workflow debugging
- containerized Linux-style jobs
- catching YAML and shell mistakes before pushing

Missing:

- SSH host orchestration
- real Windows/macOS host validation parity
- exact-SHA remote proof
- queueing and operator workflow

Conclusion:

- **useful local workflow-preflight tool**
- **not a replacement**

#### `wrkflw`

Good for:

- running CI definitions locally
- validating GitHub/GitLab workflow files
- borrowing ideas for secrets handling, runtime modes, and TUI ergonomics

Missing:

- host topology orchestration
- SSH/VM validation
- exact-SHA remote proof
- real native macOS/Windows validation path

Conclusion:

- **useful design input**
- **not a replacement**

#### Dagger

Good for:

- programmable CI logic in code
- local/CI/cloud backend switching for container-friendly workloads
- module pinning and SHA-aware fetch flows
- traceability, caching, and generated GitHub Actions wrappers

Missing for this problem:

- machine-global queueing across agent worktrees
- SSH/VM host orchestration
- GUI-capable host testing
- native host fleet validation semantics

Conclusion:

- **credible step-level execution model for containerizable checks**
- **good design reference**
- **not the control plane**

#### Earthly

Good for:

- BuildKit-first local/remote build acceleration
- cache reuse and backend selection ideas
- CI strictness and remote-runner concepts

Missing for this problem:

- host/VM orchestration
- agent/worktree queueing
- native GUI host testing
- a good long-term dependency story here, especially given maintenance risk

Conclusion:

- **useful design reference**
- **not a recommended dependency**

## Control plane vs orchestrators vs execution backends

The v2 recommendation is:

- build a thin **control plane**
- support multiple **execution backends**
- support hosted-workflow **orchestrators**
- support hosted-workflow **runner providers**
- do not reinvent workflow engines where an existing backend is already good

### Control plane responsibilities

- queueing across worktrees/agents
- persistent state/logs/results
- exact-SHA remote materialization
- host/VM selection and fallback
- rerun truth / priorities / supersedence
- agent-friendly CLI and status surface
- provider-neutral status, usage, and cost reporting
- completion signaling so a human or agent can block on the active job and react
  immediately when it finishes

### Operator and agent notification rule

The Pulp Phase 14 loop exposed an important workflow requirement:

- when a validation run is the active blocker, the preferred control-plane UX is
  a blocking `run`/`check` path that waits for completion
- that blocking path should emit an immediate completion signal, such as:
  - terminal bell
  - desktop notification
  - clear exit code
  - structured final status/result output

Status polling should still exist, but it should be the secondary surface for
observation and triage, not the primary completion mechanism for active repair
loops.

### Hosted workflow orchestrators

Recommended first-class hosted orchestrators:

- GitHub Actions dispatch

GitHub should remain the workflow/event/checks layer where that model is useful.
The control plane should not try to replace branch protections, repository
checks, or GitHub's workflow scheduler.

### Runner providers for hosted workflow orchestrators

Recommended first-class provider model for GitHub Actions:

- GitHub-hosted runners
- Namespace runners
- self-hosted runners
- ARC-managed runner scale sets
- provider-managed GitHub-compatible runners such as Actuated, Depot, and
  WarpBuild

The minimal abstraction here is not “a new runner.” It is a small
provider-selection layer that can:

- describe where a hosted job will run
- render `runs-on` labels/groups/profile selectors
- add any required provider bootstrap or setup steps
- surface provider-specific usage/cost metadata back into a common run record

### Direct execution backends

Recommended first-class direct backends:

- local command execution
- SSH POSIX execution
- SSH Windows execution

Potential later integrations around those backends:

- `act` as a local GitHub Actions workflow-preflight tool
- Dagger as a step-level execution model inside a direct backend or hosted
  runner

Not recommended as core dependencies:

- Earthly
- any backend that cannot preserve native host proof when that proof matters

## What Should Be Extracted

## Core reusable engine

- queue model
- file-lock ownership model
- per-job / per-target result model
- per-job orchestrator/provider provenance model
- exact-SHA bundle creation and upload
- local executor
- SSH POSIX executor
- SSH Windows executor
- host reachability and fallback hooks
- machine-global state directory
- JSON output suitable for agents and automation
- normalized usage/cost summary model with provider-specific enrichment hooks

## Project adapters

Each consuming project should provide:

- bootstrap command(s)
- validation scripts or command templates
- smoke/full semantics
- target definitions
- release-specific workflows if needed

This keeps the reusable tool from hardcoding Pulp behavior.

## Optional integrations

- GitHub CLI integration for PR comment / merge-on-green
- workflow-template generation or examples
- optional `act` or GitHub-runner execution adapters later
- optional Dagger execution adapter later for container-native checks

## Pulp-first phased rollout

This should land in Pulp first. Extraction to `xci` should happen only after the
provider seam, status model, and operator UX are proven in the real repo.

### Current phase status

- Phase 0. CI truth cleanup: completed on this branch
- Phase 1. Clarify the current Pulp CI model: completed on this branch
- Phase 2. Add the first-class cloud operator surface in Pulp: completed on this branch
- Phase 3. Add the minimal GitHub runner-provider seam: completed on this branch
- Phase 4. Add Namespace support plus provenance/cost telemetry: in progress
- Phase 5. Pages/docs-site/website integration: deferred later phase
- Phase 6. Extract proven pieces into `xci`: deferred

### Current implementation snapshot

Implemented on the active Pulp branch today:

- docs/workflow truth cleanup around Pages artifact deploy versus legacy
  `gh-pages` wording
- a first-class `pulp ci-local cloud workflows|defaults|run|status` operator
  surface
- persisted GitHub dispatch/run records beside local CI state
- provider-aware workflow metadata and validation in `tools/local-ci/local_ci.py`
- Namespace routing proven in both `.github/workflows/docs-check.yml` and the
  mixed-provider `build.yml` path
- CLI/docs/skill updates that separate local-vs-cloud from
  orchestrator-vs-provider decisions
- thin `pulp ci-local cloud namespace doctor|setup` helpers around the upstream
  `nsc` CLI
- saved cloud-run timing metadata so queued-versus-executing time can be
  compared later across GitHub-hosted, Namespace, and local evidence
- initial provider-usage truth in `cloud status`, including Namespace runtime,
  machine shape, and honest "cost unavailable" reporting when the provider CLI
  cannot supply billing totals yet
- `pulp ci-local cloud history|compare|recommend` for saved cloud-run timing,
  provider comparisons, and first-pass recommendation heuristics
- local-config-driven estimated billing output for per-run and billing-period
  cloud reporting, explicitly labeled `estimated; verify provider pricing`
- opt-in provider-reported GitHub billing totals shown separately from tracked
  run estimates, still off by default until the signal is more battle-tested

Still deferred from the current branch:

- provider capability reporting beyond the current narrow workflow metadata
- broader provider routing across the workflow inventory
- cross-scope local-vs-cloud comparison views
- enabling provider-reported billing totals by default for all operators
- reusable extraction into `xci`

### Recommended execution order now

Implement these phases now, in this order:

1. Phase 0
2. Phase 1
3. Phase 2
4. Phase 3
5. Phase 4

Do not start Phase 5 or Phase 6 yet.

Some overlap is fine:

- Phase 0 and Phase 1 can progress together in one branch
- Phase 2 can start once the provenance model from Phase 1 is stable enough
- Phase 4 should start from the proven narrow provider pilot and keep the
  bootstrap surface thin

### Definition of done for the current rollout

This rollout is considered done when:

- Pulp has one truthful CI story across docs, workflows, and planning
- local/SSH and GitHub-orchestrated runs are modeled separately but reported
  through consistent status/provenance surfaces
- GitHub-hosted versus Namespace is a provider choice, not a bespoke parallel
  system
- at least one narrow GitHub workflow path runs on Namespace successfully
- saved run history is rich enough to compare timing across provider choices
- CLI/plugin/operator surfaces can report where a run executed and its
  usage/cost summary
- the proposal reflects the implemented state and remaining deferred phases

### Immediate next slices

Recommended first implementation slices:

1. Finish dogfooding the Phase 2 cloud operator surface
   - verify `pulp ci-local cloud workflows|run|status` on real branch dispatches
   - keep cloud-run persistence and status summaries additive beside the local queue
2. Finish the Phase 3 provider seam pilot
   - verify `docs-check.yml` on both `github-hosted` and `namespace`
   - keep provider routing repo-owned via workflow input plus repo variable
3. Start Phase 4 from the narrow pilot
   - attach provider-neutral timing/provenance telemetry to the provider-routed
     cloud path first
   - broaden to mixed per-target routing only after the provider/status
     contract is stable
   - add cost enrichment and comparison views after there is enough saved run
     history to make the output useful

### Phase implementation rule

Until the extraction happens, each phase in this section should track both:

- current status
- concrete implementation scope in the Pulp repo

When a phase starts or finishes, update the bullets here instead of leaving that
state only in PRs or issue comments.

### Living document rule

Until the reusable extraction is complete, this document should be treated as
the source of truth for CI architecture direction in Pulp.

Any change that materially affects CI behavior should update this document in
the same branch, including:

- backend or provider choices
- workflow routing and runner selection
- status/logging/result semantics
- usage/cost telemetry behavior
- CLI or plugin/operator surfaces
- scope, phase ordering, or extraction assumptions

If the change is small, update the relevant phase bullets and the reuse-vs-build
section. If the change alters direction, add a short decision note here instead
of leaving the reasoning only in PR discussion.

### CI change update checklist

When landing CI-related work in Pulp, update this document to reflect:

- what phase the change belongs to
- what became implemented versus still planned
- whether the change is Pulp-specific or part of the future extraction seam
- whether local/cloud/provider behavior changed
- whether usage/cost reporting changed
- what remains before the next phase can start

### Phase 0. CI truth cleanup

Status: completed on this branch

Implementation scope:

- reconcile docs, workflow files, and planning/status docs so they describe one
  truthful current CI story
- remove stale `gh-pages` wording where the repo now uses Pages artifact deploy
- clarify which workflows are active, which are manual-dispatch, and which files
  are template-only or deferred
- document current local-first behavior and current cloud behavior separately

Current execution note:

- repo docs, skills, CLI help, and workflow wording now describe the actual
  Pages artifact deploy model, the local-first CI model, and the new
  `pulp ci-local cloud ...` operator surface instead of older `gh-pages` or
  raw-`gh` conventions

Likely file set:

- `.agents/skills/ci/SKILL.md`
- `docs/guides/local-ci.md`
- `docs/guides/docs-site.md`
- `docs/guides/docs-maintenance.md`
- `docs/guides/web-plugins.md`
- `docs/examples/screenshots.md`
- `.github/workflows/build.yml`
- `.github/workflows/validate.yml`
- `.github/workflows/docs-deploy.yml`
- any status/planning doc that claims an outdated `gh-pages` or workflow model

Exit criteria:

- docs and planning no longer disagree on Pages deployment model
- local-vs-cloud behavior is described accurately
- current workflow inventory is explicit enough to support later provider work

### Phase 1. Clarify the current Pulp CI model

Status: completed on this branch

- define a stable provenance schema for run/result records before adding more
  cloud behavior
- keep `pulp ci-local` as the default local/SSH control surface
- add explicit run metadata for:
  - direct backend kind
  - hosted orchestrator
  - hosted runner provider
- update docs and skill text so “local vs cloud” and “GitHub-hosted vs
  Namespace” are separate choices

Likely file set:

- `tools/local-ci/local_ci.py`
- `tools/cli/pulp_cli.cpp`
- `.agents/skills/ci/SKILL.md`
- `docs/guides/local-ci.md`
- `docs/status/cli-commands.yaml`

Exit criteria:

- run/result records have a stable provenance model
- docs and CLI language distinguish backend choice from provider choice
- the proposal remains the source of truth for this rollout

Current execution note:

- branch work now records stable local provenance in job submissions, saved
  results, evidence summaries, status output, and PR comments so GitHub and
  Namespace can plug into one schema later instead of inventing new fields

### Phase 2. Add the first-class cloud operator surface in Pulp

Status: in progress

Implementation scope:

- add a repo-local operator surface for GitHub Actions dispatch/status instead of
  relying only on skill-level `gh workflow run` conventions
- keep orchestrator config separate from direct local/SSH target config
- define how cloud results/status/provenance are persisted alongside local CI

Likely file set:

- `tools/local-ci/local_ci.py`
- `tools/cli/pulp_cli.cpp`
- `tools/local-ci/config.example.json`
- `.agents/skills/ci/SKILL.md`
- `docs/guides/local-ci.md`

Exit criteria:

- cloud execution is a first-class operator surface in Pulp
- GitHub orchestration can be selected without overloading the direct target
  model
- status output can report both local and GitHub-run evidence truthfully

Current execution note:

- branch work now adds `pulp ci-local cloud workflows`, `cloud defaults`,
  `cloud run`, and `cloud status` in `tools/local-ci/local_ci.py`, plus
  additive persisted GitHub-run records and recent cloud summaries in
  `pulp ci-local status`

### Phase 3. Add the minimal GitHub runner-provider seam

Status: in progress

- introduce a tiny provider abstraction for GitHub-run workflows
- support at least:
  - `github-hosted`
  - `namespace`
  - `self-hosted`
- keep provider-specific logic isolated to:
  - capability detection
  - optional provisioning/bootstrap
  - routing via labels/groups/profile selectors
  - execution contract for what “run this GitHub job” means on that provider
  - provider-specific status/usage metadata

Likely file set:

- `tools/local-ci/local_ci.py`
- `tools/local-ci/config.example.json`
- `tools/cli/pulp_cli.cpp`
- `.github/workflows/docs-check.yml`

Exit criteria:

- provider-specific logic is isolated to the runner-provider seam
- GitHub-hosted remains the default provider path
- Namespace can be added without leaking provider-specific logic into core config
  and status models

Current execution note:

- branch work now pilots the seam in `.github/workflows/docs-check.yml` and the
  mixed-provider `build.yml` path via a `runner_provider`
  workflow_dispatch input plus explicit runner-selector override support, with
  local config defaults and repo-variable fallbacks for Namespace while keeping
  the workflow inventory repo-owned instead of provider-hardcoded
- Namespace operator setup should currently prefer the upstream `nsc` CLI plus
  `nsc login`; any future Pulp wrapper should stay a thin validation/setup layer
  on top of that instead of re-implementing Namespace account bootstrap
- the current branch has already validated this pilot on both providers through
  `pulp ci-local cloud run ...`: Namespace run `23988705512`
  (`namespace-profile-generouscorp`) and GitHub-hosted comparison run
  `23988717946` both passed on `8a82956`
- branch work now also adds `pulp ci-local cloud namespace doctor|setup` as a
  thin operator wrapper around the upstream `nsc` CLI so provider setup can be
  verified from the same CLI without turning Pulp into a second Namespace client

### Phase 4. Add Namespace support plus provenance/cost telemetry

Status: in progress

Implementation scope:

- update workflow/config generation so GitHub Actions jobs can target Namespace
  profiles via `runs-on`
- support any required Namespace bootstrap/setup actions only where needed
- recommend `nsc` install plus `nsc login` as the default operator bootstrap
  path
- add only thin Pulp wrapper commands such as `pulp ci-local cloud namespace
  doctor` or `setup` if they materially reduce confusion; do not build a second
  Namespace client inside Pulp
- expose status so operators can see:
  - orchestrator=`github-actions`
  - provider=`namespace`
  - selected runner/profile
- preserve the existing local-first flow so Namespace is an option, not a forced
  replacement
- start with a narrow workflow set rather than the full workflow inventory
- recommended narrow initial workflow set:
  - `.github/workflows/docs-check.yml`
- persist per-run provenance and usage metadata
- persist provider-neutral timing metadata such as queue delay and elapsed
  duration before adding richer comparison or cost views
- show per-run and billing-period totals in CLI/plugin surfaces
- prefer provider-reported usage when available
- fall back to normalized estimates when providers expose runtime but not final
  billing numbers
- keep provider-reported billing opt-in and off by default until the signal is
  reliable enough to justify turning it on for everyone

Recommended execution inside Phase 4:

1. Persist provider-neutral timing/provenance metadata on the current narrow
   provider-routed path
2. Extend mixed routing to a real workflow with per-target provider choices,
   starting with the Ubuntu leg of `build.yml`
3. Extend the same pattern to Windows once the Ubuntu mixed-routing path is
   stable, while keeping macOS local-first for day-to-day Pulp work
4. Validate at least one macOS Namespace run so the provider story covers the
   full platform set even if macOS does not become the default day-to-day path
5. Add cost enrichment and comparison views only after the mixed-routing and
   timing data is trustworthy
6. Keep recommendation heuristics last; they should be derived from saved run
   history instead of hardcoded guesses

Likely file set:

- `.github/workflows/build.yml`
- `.github/workflows/validate.yml`
- `tools/local-ci/local_ci.py`
- `tools/local-ci/test_local_ci.py`
- `tools/cli/pulp_cli.cpp`
- `.agents/skills/ci/SKILL.md`
- `docs/guides/local-ci.md`
- `docs/reference/cli.md`
- this proposal file

Current execution note:

- branch work has started the bootstrap slice with `pulp ci-local cloud
  namespace doctor|setup`, explicit `nsc` guidance in docs, and real
  Namespace-vs-GitHub-hosted runs on both `docs-check` and `build`
- the current branch now also persists queue-delay and elapsed-duration timing
  for tracked cloud runs, exposes effective defaults via `cloud defaults`, and
  records Namespace runtime/machine-shape truth in `cloud status` so the later
  history rather than bespoke timing notes
- current branch spot-check on `feature/ci-namespace-plan`:
  - GitHub-hosted `docs-check` run `23989141956` completed in `22s`
  - Namespace `docs-check` run `23989153315` on
    `namespace-profile-generouscorp` completed in `19s`
- current policy direction after the first mixed-provider build:
  - cloud `build` should default to Linux and Windows only
  - macOS remains local-first for day-to-day Pulp work
  - macOS Namespace remains an explicit validation/comparison path, not a
    default cloud leg
  - one-off `build` overrides should stay command-driven rather than config
    driven, so operators can temporarily add a macOS Namespace leg with
    `--macos-runner-selector-json` without mutating saved defaults
- one explicit macOS Namespace validation was recorded on run `23989741216`:
  GitHub marked the `macOS (ARM64) [namespace]` leg successful, but
  `nsc instance history --all -o json` showed the backing instances for
  `namespace-profile-generouscorp` were all `linux/amd64` with `4 vCPU` and
  `8192 MB`, so the current profile is effectively an `S` Linux profile rather
  than a true macOS Namespace profile
- a follow-up one-off validation on run `23990730052` used
  `namespace-profile-generouscorp-macos`, and `nsc instance history --all -o
  json` confirmed a real `macos/arm64` runner with `6 vCPU` and `14336 MB`
- policy remains unchanged despite that success: macOS stays local-first by
  default, and the macOS Namespace selector should remain unset in shared config
  unless a team explicitly wants cloud macOS by default
- runner-profile creation is still documented as a Namespace dashboard task in
  this phase; `nsc` is used for login, instance inspection, and telemetry
  verification rather than GitHub Actions profile CRUD
- direct `nscloud-*` machine labels are also viable for ad hoc runs, so one-off
  experiments do not require permanent profile creation when a team just wants
  to sample a specific OS/shape

Exit criteria:

- at least one narrow GitHub workflow path can run on Namespace
- at least one real workflow can express mixed per-target provider routing
- at least one macOS Namespace validation run has been recorded for comparison
- status surfaces can report provider and cost/provenance truthfully
- local-first operation remains the default

### Phase 5. Pages/docs-site/website integration

Status: deferred later phase

Implementation scope:

- integrate the existing Pages/docs-site/browser-host/install-script deployment
  surfaces into the CI architecture intentionally
- keep this as a repo-specific phase, not part of the reusable CI core
- reconcile docs-site/build/deploy helpers with any later cloud/provider changes

Why this is separate:

- it is operationally important, but it is repo-specific deployment work rather
  than core CI control-plane behavior
- the release/install story depends on it, so it should be explicit instead of
  treated as accidental fallout from CI changes

Likely file set later:

- `.github/workflows/docs-deploy.yml`
- `docs/guides/docs-site.md`
- `docs/guides/web-plugins.md`
- `tools/build-docs.py`
- `tools/build-api-docs.sh`
- `tools/install/install.sh`
- `tools/install/install.ps1`

### Phase 6. Extract the proven seam

- move the queue/control-plane/provider model into `xci`
- keep Pulp as the first consumer
- only extract once Pulp has already proven:
  - CI truth cleanup
  - local/SSH execution
  - GitHub orchestration
  - Namespace switching
  - usage/cost reporting
  - Pages/docs-site integration boundaries
  - docs and operator workflow

Status: deferred

### Temporal note

Temporal is not part of the current rollout.

Reason:

- Temporal is a durable workflow orchestration system for application/job flows
  and worker routing
- the current Pulp need is much narrower: local-first CI control, GitHub
  orchestration, and runner-provider switching
- adding Temporal now would introduce a second orchestration system without
  solving the immediate runner-provider problem

Revisit only if the CI control plane later grows into:

- long-lived multi-day orchestration
- durable retries/recovery across many external systems
- complex cross-repo or cross-service workflows that exceed the current local +
  GitHub model

## Proposed product shape

### 1. Core CLI

Example shape:

```bash
xci run
xci run --targets mac,ubuntu,windows
xci run --validation smoke
xci enqueue --priority low
xci status --json
xci status --json --show-cost
xci logs <job-id> --target windows
xci bump <job-id> high
xci cancel <job-id>
xci config doctor
xci config set ci.github.provider namespace
xci config set ci.github.namespace.profile namespace-profile-pulp-linux
xci cost status --period month
```

### 2. Stable config

Example:

```json
{
  "project": {
    "name": "pulp",
    "root": "."
  },
  "targets": {
    "mac": {
      "kind": "local",
      "enabled": true,
      "validator": "ci/validate-posix.sh"
    },
    "ubuntu": {
      "kind": "ssh-posix",
      "host": "ubuntu",
      "repo_path": "/home/daniel/Code/pulp-validate",
      "validator": "ci/validate-posix.sh"
    },
    "windows": {
      "kind": "ssh-windows",
      "host": "win",
      "repo_path": "C:\\\\Users\\\\Daniel\\\\pulp-validate",
      "validator": "ci/validate-windows.ps1"
    },
    "cloud": {
      "kind": "github-actions",
      "workflow": "build.yml",
      "ref_mode": "exact-sha",
      "provider": {
        "kind": "namespace",
        "runner_profile": "namespace-profile-pulp-linux"
      }
    }
  }
}
```

The important point is that the config must describe **topology and backend
choice**, not just “which workflow file to run.”

For GitHub-orchestrated jobs, config should also describe the runner provider
separately from the orchestrator itself. The same workflow may run on:

- GitHub-hosted runners
- Namespace profiles
- self-hosted labels/groups
- ARC or provider-managed scale-set labels

without changing the control-plane mental model.

### 2a. Minimal runner-provider interface

This should stay deliberately small. It only applies to hosted orchestrators
such as GitHub Actions.

```text
RunnerProvider
- name() -> github-hosted | namespace | self-hosted | ...
- detect_capabilities(provider_config) -> os/arch/gpu/runtime metadata
- provision(job_requirements, provider_config) -> optional bootstrap/provisioning
- route(job_requirements, provider_config) -> labels/groups/profile selectors
- execution_contract(job_requirements, provider_config) -> how a hosted job is
  expected to execute on this provider
- describe(run_metadata) -> human/JSON status summary
- usage_adapter(run_metadata) -> optional usage/cost enrichment
```

This is enough to support provider switching without leaking provider-specific
logic all over the control plane.

### 3. Minimal agent pack

Ship a tiny skill pack, not a heavy plugin on day one.

Recommended:

- generic `.agents/skills/ci/SKILL.md`
- commands mapped directly to CLI surface
- no custom runtime logic in the skill

This keeps:

- CLI as the product
- agent pack as a small convenience layer

### 4. MCP/plugin later, if justified

If the tool proves itself across multiple repos, add:

- MCP server or JSON-RPC wrapper
- richer Claude/Codex integration
- structured artifact browsing

But that should follow the stable CLI, not precede it.

## Reuse versus build

### Reuse directly

- GitHub Actions as the hosted workflow orchestrator
- GitHub `runs-on` labels/groups as the base routing model
- GitHub self-hosted runner and ARC patterns for scale-set-backed providers
- Namespace's GitHub Actions migration/profile model instead of inventing a new
  hosted-runner protocol
- provider-managed GitHub-compatible runner patterns from Actuated, Depot, and
  WarpBuild instead of bespoke control-plane integrations on day one
- `act` for local workflow-file preflight when the goal is GitHub workflow
  debugging, not native host proof
- Dagger as an optional way to express containerizable job payloads inside an
  existing orchestrator or direct backend, not as a replacement control plane

### Build minimally

- machine-global queueing, exact-SHA delivery, logs/results, and merge-on-green
  control plane
- the tiny GitHub runner-provider adapter described above
- a normalized run record that captures:
  - direct backend or hosted orchestrator
  - hosted runner provider when applicable
  - target/platform
  - usage/cost estimate or provider-reported spend when available
- CLI/plugin status surfaces that can say where a run executed and what it cost

## What we should explicitly support

### Supported in principle

- local developer machines
- local VMs on the same Mac
- SSH-reachable Linux and Windows machines
- cloud VMs reachable over SSH
- GitHub-hosted workflow dispatch
- self-hosted GitHub Actions runners as an execution backend
- Namespace as a GitHub Actions runner provider
- other GitHub-compatible runner providers that route through `runs-on`
  selectors rather than bespoke APIs

### Not promised in v1

- generic GUI automation backend
- Kubernetes-native scheduling
- Dagger-native full orchestration
- Earthly-native full orchestration
- replacing GitHub Actions release pipelines

## Name options

Working choice:

- `xci`

Why:

- short
- neutral
- cross-platform implication
- easy CLI name

Alternatives worth keeping:

- `rungate`
- `branchgate`
- `valci`
- `shipyard-ci`
- `ciq`

## Dogfooding plan

The cleanest validation path is:

1. land the provider seam and Namespace support in Pulp first
2. prove that Pulp still supports:
   - mac local validation
   - Ubuntu SSH validation
   - Windows SSH validation
   - GitHub Actions orchestration when needed
   - GitHub-hosted vs Namespace runner-provider switching
   - saved state/logs/results
   - usage/cost reporting
   - PR workflow
3. extract only the proven core into `github.com/danielraffel/xci`
4. keep Pulp as the first consuming project after extraction
5. only then call the extraction successful

That keeps the abstraction honest.

## Recommended architecture decision

Build `xci` as:

1. a **control plane**
2. a set of **direct-backend adapters**
3. a small **hosted runner-provider layer**
4. a small **agent skill layer**

Not as:

1. a replacement for GitHub Actions
2. a replacement for Dagger/Earthly
3. a monolithic new workflow DSL

## Future enhancements worth tracking

- Ubuntu architecture selection parity with Windows auto-detection
  - see [#73](https://github.com/danielraffel/pulp/issues/73)
- optional GitHub Actions backend mode
- provider-neutral cost telemetry with:
  - per-run summaries
  - billing-period totals
  - optional estimated-vs-provider-reported breakdown
- optional `act` preflight mode for workflow debugging
- optional Dagger backend mode for containerizable checks
- optional secure secret providers
- optional TUI / dashboard
- optional MCP server once the CLI surface is stable

## Phase 14 dogfooding requirements pulled forward in Pulp

Phase 14 proved that some local-CI improvements are not just “nice later
optimizations.” They materially reduce the cost of closing the rest of the v3
program, especially on Windows.

Those dogfooding requirements should land in Pulp first and then feed back into
the extracted `xci` design. The first slices are already proven on the Phase 14
branch: prepared-build `ctest` subset reruns, live target-state streaming
during blocking runs, and explicit prepared-state policy control with exact-SHA
normalization for same-SHA reuse loops.

- [#115](https://github.com/danielraffel/pulp/issues/115) first-class
  test-subset reruns after compile succeeds
- [#116](https://github.com/danielraffel/pulp/issues/116) intentional same-SHA
  prepared-state reuse for narrow follow-up runs
- [#117](https://github.com/danielraffel/pulp/issues/117) explicit reusable
  Windows prepare/configure/build/test stages
- [#118](https://github.com/danielraffel/pulp/issues/118) richer persisted
  failing-test summaries in logs/results/status

These are worth treating as control-plane requirements, not repo-local quirks,
because they make the difference between:

- “target reruns are narrow in theory,” and
- “agents can actually iterate quickly on the slowest target without leaving the
  trusted CI surface.”

## Recommended tracking issue scope

The first tracking issue for this extraction should ask for:

1. architecture note and boundary definition
2. comparison matrix versus GitHub Actions, self-hosted runners, `act`,
   `wrkflw`, Dagger, and Earthly
3. extraction plan for queue + executor + bundle sync core
4. clear CI-vs-CD support boundary
5. clear control-plane-vs-backend split
6. minimal agent-skill packaging plan
7. Pulp dogfooding plan as the first consumer
8. explicit non-goals for v1

## Recommendation

Pulp should keep using the current repo-local stack for now.

But it is now mature enough to justify planning a reusable extraction, because:

- it solves a real problem not covered by existing tools
- it already has battle-tested behavior
- it is exactly the kind of infrastructure that will otherwise be rebuilt badly

So the right move is:

- **yes, create a future tracking issue now**
- **yes, keep the proposal scoped**
- **yes, land the minimal provider seam in Pulp before extracting anything**
- **yes, design it as a thin hybrid control plane over standard backends**
- **yes, treat Namespace as a GitHub runner-provider option, not a new CI
  system**
- **no, do not start rewriting it immediately while higher-priority post-v3 work is active**
