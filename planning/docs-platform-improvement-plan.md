# Docs Platform Improvement Plan

Date: 2026-03-25
Purpose: give product-level feedback on how to evolve the current docs work into something that is easy to browse online, rich in examples, clear about platform capabilities, and useful to both humans and agents.

## Framing

This is not a bug list.
This is a plan for how to make the docs system more compelling, more usable, and more obviously valuable to outside developers.

The current work is a strong base:

- local docs exist
- machine-readable manifests exist
- the CLI can read local docs
- modules and support status are represented
- code style and agent contribution rules exist

That means the next stage should not be “more scaffolding.”
It should be “make this excellent to use.”

## Overall Feedback

The direction is right.

What it currently feels like:

- a good internal documentation framework
- useful for agents and maintainers
- structurally thoughtful

What it should become next:

- a docs platform that feels great for humans
- easy to browse online
- example-first
- explicit about support and capabilities
- clearly versioned for `main` and `develop`
- locally queryable from the CLI without web dependence

The key shift now is from **documentation system** to **documentation experience**.

## What “Good” Should Feel Like

For a new developer, the docs should answer these questions quickly:

1. What is Pulp, really?
2. What can I build with it today?
3. What modules exist?
4. What is stable vs experimental?
5. How do I make a plugin?
6. How do I make a standalone app?
7. How do I use the CLI?
8. How do I use the CMake API?
9. Which examples should I start from?
10. Where do I look if I want AU, VST3, CLAP, view, render, state, or testing details?

For an agent, the docs should answer:

1. What is supported on this branch?
2. Which module owns this concept?
3. What public APIs are stable?
4. What docs or manifests must be updated with a change?
5. What examples demonstrate this feature already?

## Improvement Themes

## 1. Make the docs browsable like a product, not just a repository

The current docs structure is logical, but the next improvement is information architecture for readers.

I would aim for a site experience closer to:

- JUCE for reference discoverability
- Pamplejuce for guide readability and practical workflow content

That means the online docs should foreground:

- a left-nav or top-nav that is stable and obvious
- a support/status page
- module reference
- examples
- CLI and CMake references
- getting-started guides

Suggested top-level online navigation:

- Overview
- Getting Started
- Support Matrix
- Modules
- Examples
- CLI
- CMake
- Testing
- Policies

## 2. Add a dedicated “Capabilities” surface

Yes, the platform should absolutely document its feature capabilities in an easy-to-browse way.

This deserves more than a single status matrix file.

I would add a dedicated docs page:

- `docs/reference/capabilities.md`

This page should organize capabilities by area:

- Formats
- Platforms
- Audio I/O
- MIDI I/O
- DSP/signal features
- State/automation
- UI/view features
- Rendering/GPU features
- Tooling/CLI features
- Shipping/release features

Each capability should show:

- what it is
- current status
- supported platforms
- relevant module
- relevant examples
- relevant docs

This is one of the highest-value improvements because it lets people answer “can Pulp do X?” without reading the whole repo.

## 3. Make modules first-class in the docs experience

Yes, you should list all modules, and not just in one reference page.

Modules should be a first-class browse path because that is how experienced developers reason about frameworks.

I would evolve module docs into a richer surface:

- one module index page
- one page per important module

Suggested pages:

- `docs/reference/modules.md` as the index
- `docs/reference/modules/runtime.md`
- `docs/reference/modules/state.md`
- `docs/reference/modules/format.md`
- `docs/reference/modules/view.md`
- `docs/reference/modules/render.md`
- and so on

Each module page should show:

- purpose
- maturity
- dependencies
- key public headers
- major concepts
- common use cases
- examples that use it
- current limitations

That makes the docs feel much more like a framework manual.

## 4. Move to an example-first developer experience

You are right that people will want to read online and see examples.

This is probably the biggest opportunity.

Right now examples are documented.
Next they should become the core onboarding surface.

I would turn examples into a dedicated section with:

- a gallery/index page
- one page per example
- screenshots or output images where relevant
- code file references
- “what this teaches” and “what this does not cover”

Suggested example page structure:

- Summary
- Why this example exists
- What it demonstrates
- Formats supported
- Platforms supported
- Key files
- Walkthrough
- Known limitations
- Related examples

Suggested example categories:

- Minimal reference examples
- Validation examples
- Experimental examples

This is the Pamplejuce lesson worth stealing most directly:

- people love practical guides
- people love example-driven learning
- people love “how this works in real life” docs

## 5. Treat support status as a browsable feature, not just metadata

The manifests are the right backbone.
But for humans, support needs to be visual and easy to scan.

I would add a rendered support matrix page that shows:

- rows by capability/module/format/platform
- badges like `stable`, `usable`, `experimental`, `partial`, `planned`
- links to the relevant docs and examples

Good support surfaces should answer:

- Is AU supported on macOS right now?
- Is Windows build support real or mostly stubbed?
- Is view usable today?
- Is render required to use format/state/audio?
- Is shipping production-ready or still partial?

This page should be one of the most-linked docs pages.

## 6. Publish `main` and `develop` as separate docs experiences

Yes, this is worth doing.

The docs should stay branch-owned, but the site should make the distinction obvious.

Online docs should expose:

- `main`
- `develop`
- later, tagged releases

Suggested URL model:

- `/docs/main/`
- `/docs/develop/`
- later: `/docs/v0.1/`

Suggested UI:

- branch/version switcher in the header
- persistent badge showing which branch docs you are reading

This matters because support status will differ between branches, and you do not want people reading a future-facing `develop` page as if it were stable truth.

## 7. Keep local-first, but add polished online rendering

Do not trade away the local-first model.

The right move is:

- local files remain source of truth
- CLI reads those files
- hosted docs render those files

That lets you have:

- great online reading for humans
- zero-web local lookup for agents
- no split-brain between website docs and repo docs

This is the correct architecture.

The improvement is in presentation, not in moving authority to the web.

## 8. Add screenshots, diagrams, and navigational visuals

For online docs, plain text will not be enough.

Especially for:

- examples
- module architecture
- UI/view/render topics
- CLI workflows

I would add:

- module dependency diagram
- plugin build/format pipeline diagram
- screenshots for examples and UI preview
- simple workflow diagrams for CLI and validation flows

These should live in the repo too, ideally under:

- `docs/assets/`

That keeps them versioned and local.

## 9. Improve the docs CLI from “reader” to “guide”

The local docs CLI is already useful.
The next improvement is to make it friendlier and more discoverable.

Ideas:

- `pulp docs home`
- `pulp docs examples`
- `pulp docs module format`
- `pulp docs capability vst3`
- `pulp docs capability hot-reload`
- `pulp docs branch`

The CLI should not just expose files.
It should expose the conceptual structure of the docs.

That makes it more useful for both humans and agents.

## 10. Add a documentation style for “manuals,” not just references

The current docs system is strong on structure.
The next improvement is editorial style.

You want two distinct voices:

### Reference docs

Short, precise, structured, searchable.

### Manual/guide docs

Practical, example-driven, readable, opinionated.

That Pamplejuce style is worth borrowing:

- explain why
- explain tradeoffs
- use real examples
- avoid sterile API-only writing

That will make the docs feel much more alive.

## Proposed Next Work Items

I would break the next stage into these work items.

### Work Item 1: Docs IA And Browseability

Goal:
Make the docs easy to browse online and in the CLI.

Deliverables:

- improved nav structure
- `capabilities.md`
- better docs landing page
- clearer categories for concepts/guides/reference/examples/policies

### Work Item 2: Module Documentation Expansion

Goal:
Make every major subsystem easy to understand in isolation.

Deliverables:

- module index page
- per-module pages
- module dependency diagram

### Work Item 3: Example Gallery

Goal:
Turn examples into the main onboarding surface.

Deliverables:

- example index
- per-example pages
- screenshots/output visuals
- tags for `reference`, `validation`, `experimental`

### Work Item 4: Hosted Docs Publishing

Goal:
Render the same docs tree as a polished site for `main` and `develop`.

Deliverables:

- static site generation
- branch/version switcher
- stable/develop URLs
- published support matrix and module pages

### Work Item 5: Docs CLI UX Expansion

Goal:
Make the CLI a real local documentation interface.

Deliverables:

- richer docs subcommands
- friendlier conceptual lookup
- better output formatting
- capability-oriented queries

## Recommended Priority

If the goal is developer delight, I would prioritize like this:

1. Example gallery
2. Capability and support browsing
3. Module docs expansion
4. Hosted docs publishing
5. CLI UX expansion

Reason:

- examples create immediate adoption value
- capability browsing reduces uncertainty
- module docs improve framework credibility
- hosted docs improve reach
- CLI improvements deepen local usability

## What To Ask An Agent To Build Next

If you want an agent to continue, I would not ask it to “fix docs.”
I would ask it to build the next docs product milestone.

A good prompt would be:

```text
Using planning/versioned-developer-docs-system.md and planning/docs-platform-improvement-plan.md, propose and implement the next docs experience milestone for Pulp.

Focus on:
- online browseability
- examples as a first-class section
- a dedicated capabilities reference
- richer module documentation
- preserving the local-first, branch-owned docs model

Do not treat this as a bug-fix pass. Treat it as a docs product/design pass that improves usefulness for humans while preserving local agent consumption.
```

## Bottom Line

The docs system now has a solid technical foundation.

The next step is not more infrastructure for its own sake.
The next step is to make the docs feel like a real framework manual:

- browseable
- example-rich
- explicit about capabilities
- organized around modules
- pleasant online
- still fully local and agent-friendly

That is the right direction from here.
