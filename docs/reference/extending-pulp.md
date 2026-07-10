# Extending Pulp: `add` vs `tool` vs `kit` vs `content`

Pulp has four distinct ways to bring something extra into your workflow. They
look similar on the command line but serve different purposes, live in different
places, and have different blast radii. Pick the wrong one and you either ship a
developer tool inside a plugin or pollute a project lockfile with something that
should have stayed on your machine — so this page exists to keep the choice
obvious.

The one-sentence rule of thumb:

- **`pulp add` / packages** — add a third-party *audio DSP library* to a *project*.
- **`pulp kit`** — share reusable *Pulp code, UI, and templates* into a *project*.
- **`pulp content`** — install *data-only packs* (presets, samples) for an
  installed *plugin*.
- **`pulp tool`** — install *developer/agent tooling* onto your *machine*.

## Comparison

| Mechanism | Command | What it adds | Where it lands | Mutates your project? | Ships inside your plugin? | Reach for it when |
|---|---|---|---|---|---|---|
| **Packages** | `pulp add <pkg>` | Curated, license-checked third-party **audio DSP libraries** (pitch detection, time-stretch, SRC, file decoders, …) beyond `core/signal/` | `packages.lock.json` + generated CMake wiring in your project; `DEPENDENCIES.md` / `NOTICE.md` | **Yes** — lockfile, CMake, attribution | Yes — it becomes a build dependency of your plugin | You need a DSP capability your plugin will actually link and ship |
| **Kits** *(experimental)* | `pulp kit …` | Reusable **Pulp-authored code, UI, and templates**, gated by a review step before they touch a project | Your project source tree | **Yes** — adds reviewed code/UI/templates | Yes, if you build it in | You want to reuse or share UI/code scaffolding across projects |
| **Content** *(experimental)* | `pulp content …` | **Data-only** content packs (presets, sample sets, etc.) for an already-installed plugin | The plugin's content directories | No source change | No — it's data the plugin loads, not code | You want to distribute/install presets or samples for a plugin |
| **Tools** | `pulp tool install <id>` | **Machine-level developer/agent tooling**: format validators, framework importers, the `video-proof` recorder/composer | `~/.pulp/tools/` on your machine | **No** — never touches a project | **No** — never part of a shipped artifact or public CI | You need a dev tool, not a project dependency |

### The `pulp add` aliasing nuance

`pulp add` is a convenience entry point that dispatches by what you name:

- `pulp add <registry-package>` → the **package** lane (project dependency).
- `pulp add <importer>` → an **alias for `pulp tool install <importer>`** (a
  machine-level tool).

So the same verb can land in two different lanes. If you want to be unambiguous,
use `pulp add` only for DSP packages and `pulp tool install` for tools/importers.

## Why these are separate (not one mechanism)

The split is deliberate and load-bearing:

- **Trust boundary.** `pulp add` / kits / content can mutate a *project* —
  lockfiles, generated CMake, attribution files, plugin data. A plugin you ship
  to users should only ever contain code and data it actually needs at runtime.
- **No dev tooling in shipped artifacts.** `pulp tool` installs things like
  Node, npm packages, `ffmpeg`, and Remotion (for `video-proof`), or framework
  importers with bundled libclang. None of that belongs in a plugin binary, a
  project lockfile, or required public CI. Tools live under `~/.pulp/tools/` and
  are removable without touching any project.
- **License hygiene.** Packages are curated to MIT/BSD/Apache so they are safe to
  link and redistribute. Some tools (Remotion, ffmpeg static builds) carry
  developer-only license terms and get the same carve-out as vendor SDKs:
  developer-supplied, never vendored, never shipped.

## One consistent lifecycle: add → validate → use → remove

However different their blast radius, all four surfaces follow the same lifecycle
shape, so what you learn on one carries to the others:

- **Add is validated, never blind.** Every surface checks what it is about to
  bring in before it lands — a package is license- and pin-checked, a kit runs a
  review/validate step, a content pack is validated against its manifest, and a
  tool is fetched at a pinned version and (for binaries) SHA-256-verified. A
  failed validation is a hard stop, not a warning.
- **Remove is explicit and confined.** Every surface can undo what it added, and
  a removal that deletes files makes you confirm it (`--yes`, or a gesture in a
  review UI), only ever touches the area that surface owns (your project's
  managed entries, the plugin's content directory, or `~/.pulp/tools/`), refuses
  any id or path that would resolve outside that area, and names what it removed.
  Nothing deletes silently or reaches beyond its own lane.
- **Every surface ships tests.** Add *and* remove are both covered by tests that
  run in CI — installing and uninstalling from outside a checkout, round-tripping
  a pack, refusing a hostile id — so "it installs" and "it cleanly uninstalls"
  are both proven, not assumed.

Each surface has an agent skill carrying its surface-specific recipe (see the
[skills catalog](skills.md) — `packages`, `kits`, `content`,
`installable-tools`). If you are adding a *new* thing Pulp can install on any of
these surfaces, the `installable-tools` skill codifies the acceptance bar that
makes the lifecycle above real: validate the install **and** the uninstall from
*outside* a checkout, and ship both tests, before the README ships.

## FAQ

**Why isn't `video-proof` a `pulp add` package?**
Because it is machine-level developer/agent tooling (Node, ffmpeg, Remotion,
screen-recording permissions, Terminal handoff, simulators), not something your
plugin links or ships. Making it a `pulp add` package would put unshipped
developer tooling — and license-sensitive binaries — into your project's
dependency graph. It installs with `pulp tool install video-proof` and lives
under `~/.pulp/tools/`. See
[desktop-video-proofs.md](desktop-video-proofs.md).

**Is `pulp tool` a new mechanism built for video-proof?**
No. The `pulp tool` lane already existed for format validators and framework
importers (see
[framework-importer-packaging.md](framework-importer-packaging.md)).
`video-proof` is registered as one more tool on that existing lane — it reuses
what was already built rather than inventing a new install path.

**What's the difference between a kit and a package?**
A **package** is an external third-party DSP library wired into your build via
`pulp add`. A **kit** is reusable *Pulp-authored* code, UI, or templates shared
into your project with a review step. Packages bring outside code in; kits move
Pulp-shaped building blocks between projects.

**Content vs kit?**
**Content** is data only — presets, samples, and similar packs an installed
plugin loads at runtime. A **kit** carries code/UI/templates. If it runs, it's a
kit; if the plugin merely reads it, it's content.

**Does installing a tool change my repository?**
No. Tools install under `~/.pulp/tools/` and mutate nothing in your project — no
lockfile, no CMake, no attribution. Uninstalling a tool (and, for importers, its
skill) leaves the project untouched.

**If a tool isn't installed and I ask for something that needs it, what happens?**
The command degrades to a clear skip with a single remediation line (for
example, `pulp tool install video-proof`) and still produces whatever it can
without the tool — never a false success.

## Every added tool must be user-updatable and overridable

When you add a `managed_by_pulp` tool to
`tools/packages/tool-registry.json`, it must be updatable and
version-overridable by the user **without waiting for Pulp to bump its
committed pin**. This is a hard convention for the opt-in tool lane (it does
not apply to shipped-by-default dependencies like Skia or Dawn — those are
pinned by the build, not by `pulp tool`).

Concretely, a managed tool must declare a non-empty `pinned_version`. That
version string is the anchor the update/override path keys off:

- **Update** — `pulp tool update <id>` re-installs the tool at the registry
  pin (the latest known-good version Pulp ships), and `pulp tool update <id>
  --version <v>` re-installs at an explicit version.
- **Override** — a user can pin a tool to their own version and have it survive
  future Pulp registry-pin bumps, three ways (highest precedence first):
  1. `PULP_TOOL_<ID>_VERSION` env var (session-scoped; the id upper-cased with
     non-alphanumerics turned into `_`, e.g. `PULP_TOOL_AUDIO_QUALITY_LAB_VERSION`),
  2. `$PULP_HOME/tool-overrides.json` (durable; what `--version` on
     `install`/`update` writes),
  3. the registry `pinned_version` (the shipped default).

  `pulp tool info <id>` (and `--json`) reports the **active version** and its
  **source** so it is always explicit which version is in effect and why.

This convention is enforced: `tools/packages/validate_registry.py` fails if any
`managed_by_pulp` tool ships without a `pinned_version`, so we never again ship
a tool users cannot update. Add the pin (and any user-facing update note) in the
same change that registers the tool.

## See also

- [skills.md](skills.md) — the agent-skills catalog; the `packages`, `kits`,
  `content`, and `installable-tools` skills carry the per-surface recipes
- [cli.md](cli.md) — full command reference for `add`, `tool`, `kit`, `content`
- [framework-importer-packaging.md](framework-importer-packaging.md) — the
  importer/tool packaging contract
- [desktop-video-proofs.md](desktop-video-proofs.md) — the `video-proof` tool
- [packages/README.md](../guides/packages/README.md) — the package manager
