# Repo Cleanliness And Onboarding Review

Date: 2026-03-25
Scope: repository review only, no implementation changes

## Verdict

Pulp is closer to "credible framework with real architectural taste" than "prototype held together by ambition." The codebase is organized, the subsystem split is understandable, the public core APIs are readable, and the test surface is much stronger than most early framework repos.

The main risk is not embarrassment from poor code quality. The main risk is embarrassment from **trust drift**:

- the public narrative is ahead of the implemented developer experience
- the onboarding docs do not fully match the real build/bootstrap flow
- the public/private API boundary is not yet clean
- a few productized-looking surfaces are still transitional underneath

If the repo were shown to experienced developers today, I think the reaction would be:

- "There is real substance here."
- "The architecture is promising."
- "The docs and packaging story are overstating the current maturity."

That is fixable, and it is fixable mostly with clarity, documentation structure, and a small amount of product-surface cleanup rather than deep rewrites.

## What Already Works Well

These are the parts that already read as solid and worth preserving.

### 1. The repository shape is good

The top-level layout is legible:

- `core/` for subsystems
- `examples/` for executable documentation
- `test/` for verification
- `tools/` for developer tooling
- `ship/` for release/update concerns

That gives experienced developers immediate orientation.

### 2. The module split is mostly sensible

The subsystem separation is one of the strongest parts of the repo:

- `pulp::runtime` underpins the lower layers
- `pulp::audio`, `pulp::midi`, `pulp::state`, `pulp::events` stay reasonably focused
- `pulp::format` sits at the framework edge
- `pulp::view` and `pulp::render` are separated rather than fused

The dependency direction in the CMake files is readable and mostly shallow rather than tangled.

### 3. The public core API is surprisingly approachable

The strongest public-facing API surfaces I reviewed were:

- `core/format/include/pulp/format/processor.hpp`
- `core/state/include/pulp/state/store.hpp`
- `core/state/include/pulp/state/binding.hpp`
- `core/view/include/pulp/view/view.hpp`
- `core/view/include/pulp/view/widgets.hpp`

These are concise, readable, and generally easy to reason about. They look like framework code rather than internal one-off code.

### 4. Tests are doing a lot of credibility work

The configured test surface is broad. Current build metadata reports **263 tests**, which is materially stronger than the README currently claims.

This matters. For experienced developers evaluating whether the project is "serious," the test inventory is one of the best signals in the repo.

### 5. The examples are useful and not purely decorative

The examples are not throwaway demos. They validate real framework paths and make the architecture more concrete.

`examples/pulp-gain` in particular is a good "minimum viable mental model" for the framework.

## Main Findings

### 1. The repo tells two stories at once: current reality and future vision

This is the single biggest issue.

### Evidence

- `README.md:12-17` presents concrete present-tense capability claims.
- `README.md:17` says there are `53 automated tests`.
- Current configured build reports `263` tests via `ctest --test-dir build -N`.
- `README.md:77` says GPU rendering and the UI system are "next," but there is already substantial `core/view/`, `core/render/`, `examples/ui-preview`, and related tests.
- `VISION.md:186-198` describes a far broader CLI/MCP story than the currently implemented tooling surface.

### Why this matters

Experienced developers are generally tolerant of ambitious roadmaps. They are not tolerant of ambiguity about what is shipped, what is experimental, and what is aspirational.

Right now the repo can create two opposite impressions at once:

- parts of the README understate what exists
- parts of the vision/tooling narrative overstate what is usable today

That inconsistency reduces trust.

### Suggestion

Create one canonical "state of the project" page and link everything to it.

Suggested structure:

- `Available now`
- `Experimental but usable`
- `In progress`
- `Planned`

Then revise:

- `README.md` to reflect present-day truth only
- `VISION.md` to explicitly label itself as strategic/future-facing
- `docs/getting-started.md` to describe only supported onboarding paths

### 2. The public/private API boundary is not clean enough yet

This is the second most important issue.

### Evidence

- `docs/getting-started.md:132` instructs users to include `core/format/src/au_v2_adapter.cpp`.
- `docs/getting-started.md:191` instructs users to include `au_v2_instrument.cpp`.
- `examples/pulp-gain/main.cpp:6` includes `../../core/format/src/standalone.hpp`.
- `examples/pulp-tone/main.cpp:6` includes `../../core/format/src/standalone.hpp`.
- `examples/pulp-gain/au_v2_entry.cpp:8` includes `../../core/format/src/au_v2_adapter.cpp`.
- `examples/pulp-tone/au_v2_entry.cpp:6` includes `../../core/format/src/au_v2_instrument.cpp`.
- `core/format/CMakeLists.txt:35-36` explicitly documents this transitional pattern.

### Why this matters

Once docs or examples tell users to include from `src/`, the project stops feeling productized and starts feeling mid-refactor.

This is fine internally. It is not fine as an outward-facing framework posture.

### Suggestion

Define a hard rule:

- consumers only include from public `include/` trees
- any helper that must be included by consumers gets promoted to a public header or a public implementation shim

Treat every `src/` include in examples/docs as a cleanup task. This is worth fixing before broad external promotion.

### 3. The build/bootstrap story is more complex than the docs admit

### Evidence

- `README.md:67-74` presents build as a simple CMake flow.
- `docs/getting-started.md` does not explain the full dependency/bootstrap path.
- Root `CMakeLists.txt` expects SDKs and third-party pieces in specific forms.
- `.github/workflows/build.yml:23-33` clones SDK dependencies before configure.
- `tools/scripts/setup.sh` exists but is not referenced by the main docs.

### Why this matters

For experienced developers, first-run experience is the first trust test. If the repo says "just build" but CI and local setup depend on non-obvious bootstrap behavior, that friction will be noticed immediately.

### Suggestion

Document one official bootstrap path.

Pick one of these and make it authoritative:

- "clone and run `tools/scripts/setup.sh`"
- "clone required SDKs manually, then configure"
- "everything is fetched automatically"

Do not imply all three at once.

Also add a first-run prerequisites section that explicitly answers:

- what is fetched automatically
- what must already exist
- what is optional
- what is macOS-only

### 4. Consumer packaging looks more finished than it actually is

### Evidence

- `templates/plugin/CMakeLists.txt.in:8` says `find_package(Pulp REQUIRED)`.
- I did not find a `PulpConfig.cmake` export/package config in the repo.
- `tools/cmake/PulpUtils.cmake` provides a good local build API, but there is not yet a correspondingly documented install/export story.
- `tools/cli/CMakeLists.txt:6-7` installs the CLI to `${CMAKE_SOURCE_DIR}`, which reads as repo-local convenience rather than real packaging.

### Why this matters

This is a high-signal issue for experienced users. A template that implies package-manager-style consumption creates a stronger promise than the repo currently fulfills.

### Suggestion

Choose one clear consumption model and document it honestly:

- in-repo framework development only
- vendored/submodule usage
- installable CMake package

If installed package consumption is the goal, document it only after the package export/config story exists.

Until then, avoid templates that imply a polished external package flow.

### 5. Build hygiene has avoidable rough edges

### Evidence

- `core/audio/CMakeLists.txt:35,42` writes placeholder source files into the source tree.
- `core/midi/CMakeLists.txt:30` writes a placeholder source file into the source tree.
- `inspect/CMakeLists.txt:14` writes a placeholder source file into the source tree.
- the placeholder files are tracked, but configure-time writes still mutate files under source control.

### Why this matters

Writing to tracked source files during configure is surprising. Even if harmless, it feels sloppy to experienced build engineers.

### Suggestion

Use one of these approaches instead:

- check in static placeholder files and stop rewriting them
- generate placeholders into the build directory only

A good rule for the repo:

- configure/build should never modify tracked source files

### 6. The documentation set is too thin for a framework project

### Evidence

Externally relevant docs are currently quite sparse:

- `README.md`
- `docs/getting-started.md`
- `CONTRIBUTING.md`
- `DEPENDENCIES.md`
- `VISION.md`

There is no obvious public documentation set for:

- architecture overview
- subsystem map
- build API reference
- support matrix
- public API stability expectations
- example guide

### Why this matters

The code is readable, but good framework repos do not rely on source reading alone for orientation.

### Suggestion

Promote a curated docs set under `docs/`:

- `docs/overview.md`
- `docs/architecture.md`
- `docs/support-matrix.md`
- `docs/build-api.md`
- `docs/examples.md`
- `docs/public-api.md`

These do not need to be long. They need to be authoritative.

### 7. The examples need better curation and labeling

### Evidence

- `examples/CMakeLists.txt` is phase-oriented, which is useful internally but not necessarily user-facing.
- There are no per-example READMEs.
- `examples/pulp-tone/main.cpp` contains an explicit simplification comment around MIDI injection rather than a finished standalone interaction path.

### Why this matters

Examples are being asked to do multiple jobs at once:

- validate framework phases
- serve as onboarding material
- prove format support

Those are not the same thing.

### Suggestion

Label each example as one of:

- `Reference`
- `Validation`
- `Experimental`

Then give each example a short README covering:

- what it demonstrates
- what it validates
- supported formats/platforms
- known limitations
- how to run it

Keep one example aggressively clean and public-API-only as the canonical first example.

### 8. CLI and MCP are promising, but they are still early tooling

### Evidence

- `tools/cli/pulp_cli.cpp` currently implements a limited command surface relative to the much broader tooling story in `VISION.md`.
- `tools/mcp/pulp_mcp.cpp` uses minimal hand-rolled JSON extraction and shell-based delegation.
- The screenshot tooling is real and concrete, which is good, but the broader tool story is still early.

### Why this matters

If the repo is pitched as AI-native or agent-friendly infrastructure, the tooling surface becomes part of the product, not just an internal helper.

Experienced developers will notice when flagship tooling is still implemented as a thin shell wrapper with fragile parsing.

### Suggestion

Present CLI/MCP as experimental developer tooling until the surface is more robust.

Near-term improvements to document or make before heavier promotion:

- explicit implemented-command table
- stronger error handling semantics
- argument handling that does not depend on shell string concatenation
- clearer separation between stable and experimental commands/tools

### 9. The test story is stronger than the docs, but not well explained

### Evidence

- Current build enumerates 263 tests.
- `README.md:17` still says 53.
- The test suite mixes unit tests, validation tests, platform-sensitive tests, and example/plugin checks.

### Why this matters

The good news is that this is a strength. The problem is that the strength is not being communicated cleanly.

### Suggestion

Document the test taxonomy:

- fast unit tests
- integration tests
- format validation tests
- host/platform-dependent tests

Then provide recommended commands for:

- contributors
- CI
- release validation

This makes the repo feel much more intentional.

### 10. A few unfinished modules should be framed more honestly

### Evidence

- `inspect/` is currently placeholder-backed.
- `ship/src/appcast.cpp:162-170` has a stubbed `sign_file_ed25519`.
- platform support remains partial in several areas.

### Why this matters

Unfinished code is not embarrassing. Pretending it is finished is.

### Suggestion

Mark modules with an explicit maturity tag in docs:

- `Stable`
- `Usable`
- `Experimental`
- `Stubbed / planned`

This turns uncertainty into professionalism.

## Recommended Documentation Set

If the goal is "experienced developers should find this a joy to interact with," I would add or promote the following first:

1. `docs/overview.md`
Purpose:
What Pulp is, what problems it solves, and who it is for.

2. `docs/architecture.md`
Purpose:
Subsystem map, dependency direction, public vs internal layers, design constraints.

3. `docs/support-matrix.md`
Purpose:
Formats, platforms, tooling, and feature maturity with `Available / Experimental / Planned`.

4. `docs/build-api.md`
Purpose:
`pulp_add_plugin()`, `pulp_add_app()`, file conventions, outputs, validation/install workflow.

5. `docs/public-api.md`
Purpose:
What users may include, what they should never include, and how API stability is defined.

6. `docs/examples.md`
Purpose:
Guide to examples, what each proves, and which one to start from.

7. `docs/testing.md`
Purpose:
Test classes, local commands, CI expectations, validator dependencies.

## Suggested README Direction

The README should become more boring and more trustworthy.

I would optimize it for:

- one-sentence value proposition
- present-day support matrix
- one clean minimal example
- exact first-run build steps
- exact current CLI/tooling status
- links to deeper docs

Move most future-facing storytelling to `VISION.md`.

That will make the repo feel more confident, not less ambitious.

## Modularity Assessment

Overall assessment: **good, with some leakage**.

What is good:

- subsystem boundaries are understandable
- dependency direction is mostly reasonable
- public includes are organized by module
- the architecture does not read as monolithic

What weakens the modularity story:

- examples/docs including files from `src/`
- build conventions that depend on internal file naming rather than documented public contracts
- placeholder-backed modules that are present in the tree but not clearly maturity-labeled

Conclusion:

The codebase is modular enough to be respected. It just needs the repo surface to enforce the same discipline that the structure already suggests.

## "Will This Embarrass Us?" Assessment

My answer is:

- **No, not if it is presented honestly.**
- **Yes, if the repo is presented as more finished than it is.**

What will impress people:

- the architecture
- the module layout
- the quality of the core API shapes
- the breadth of tests
- the seriousness of the examples

What will raise eyebrows:

- public docs drifting away from actual implementation
- `src/` includes in onboarding and examples
- templates implying package flows that are not visibly implemented
- placeholder/source-tree-write build behavior
- tooling narrative getting ahead of the actual CLI/MCP robustness

## Priority Order

If I were prioritizing cleanup for external credibility, I would do it in this order:

1. Align docs with current reality.
2. Clean up public vs internal API boundaries.
3. Make bootstrap/build instructions explicit and authoritative.
4. Clarify packaging/consumption model.
5. Curate examples and add per-example READMEs.
6. Add support/maturity matrix docs.
7. Remove source-tree writes during configure.
8. Tighten CLI/MCP positioning and documentation.

## Bottom Line

This repository already has enough substance and taste to earn serious interest from experienced developers.

The code structure is not what will embarrass you.

What can embarrass you is the gap between:

- what the repo implies
- what a new developer actually experiences on day one

Close that gap, and the project will feel significantly more polished, more trustworthy, and much more enjoyable to build with.
