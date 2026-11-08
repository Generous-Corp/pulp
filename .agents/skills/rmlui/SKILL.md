---
name: rmlui
description: Where RmlUi fits in Pulp, and when not to use it. Load before proposing, building or porting an RmlUi (RML/RCSS, Lua skin) plugin editor, before touching Pulp's pulp/view/rmlui_* headers, and whenever a task asks which UI technology an editor should use. The default UI is Vellum on Skia/Dawn; RmlUi is an opt-in Vellum component for products that already ship RML/RCSS skins or Lua skin scripts, or for a developer who asks for it. Covers where the code lives (Vellum's standalone-buildable rmlui/ subproject, not Pulp), the pay-only-for-what-you-use build rule, bringing your own RmlUi build or renderer, and the licence line around GPL renderers.
---

# RmlUi in Pulp

**TL;DR.** RmlUi is not Pulp's UI and is not an equal choice next to Vellum. The
default editor stack is Vellum on Skia/Dawn (`CLAUDE.md`, "Choosing a UI
stack"). Use RmlUi only when a stated reason applies, keep it out of every build
that does not ask for it, and put RmlUi work in Vellum, not Pulp.

## When RmlUi is the right answer

Use it only when one of these is true:

- the product already ships RML/RCSS skins or Lua skin scripts, and porting them
  to Vellum would mean rewriting the skins (an emulator plug-in with user skins is
  the typical case);
- the developer explicitly asks for RmlUi.

Otherwise, use Vellum. "It looks lighter" or "it's HTML-like" are not reasons:
Vellum already imports HTML/CSS designs, and an agent that picks RmlUi on its
own leaves the product on the less-supported path. Don't mix RmlUi and a Vellum
view tree in one editor without a stated reason.

## Where the code lives

| Piece | Location | Notes |
|---|---|---|
| RmlUi support | Vellum `rmlui/` subproject | Its own CMake project (`cmake -S rmlui`); also added to a Vellum build by `VELLUM_ENABLE_RMLUI=ON`, which is OFF by default. Targets: `VellumRmlUi::Core`, `::SoftRenderer`, `::DevAssets`, `::Lua`, and `::VellumRenderer`. Only `::VellumRenderer` links Vellum itself. |
| RmlUi itself | fetched upstream, pinned | Used as-is. No fork and no vendored copy. |
| Pulp `pulp/view/rmlui_host_bridge.hpp`, `rmlui_importer_adapter.hpp` | Pulp `core/view` | A frozen seam behind `PULP_ENABLE_RMLUI_IMPORTER_ADAPTER=OFF`. It duplicates Vellum's `vellum/rmlui/host_bridge.hpp` and leaves Pulp along with `core/view`. Don't build new work on it or extend it. |
| How an RmlUi editor reaches the host | Pulp editor-host contract (`pulp/format/editor_host.hpp`) | A UI package's `EditorFactory`. It doesn't use `create_view()` or `ViewBridge`. |

A change to RmlUi support is Vellum work. Route the paths with the
`pulp-vellum-change-routing` skill before writing anything that crosses the two
repositories.

## Nobody compiles what they don't use

- A Pulp build with no UI package must not fetch or compile RmlUi, Skia or Dawn
  because of RmlUi.
- A default Vellum build must not fetch RmlUi.
- An RmlUi build must not need Skia or Dawn, unless it asks for the Vellum
  renderer.

When you add an RmlUi option, target or example, keep it OFF by default and
behind its own switch. Make it something a consumer opts into, never something
it must opt out of. A machine-wide FetchContent cache hides a violation: "nothing
was downloaded" can mean "it was already cached". So prove a dependency is
unused from what was *requested*, not from what was downloaded.

## Bring your own RmlUi or renderer

A product may supply its own RmlUi build, including a fork, and its own
`Rml::RenderInterface`. Design every seam for that:

- Take RmlUi and renderer objects as inputs. Never construct a Pulp-chosen RmlUi
  inside a seam a product cannot replace.
- Don't put Pulp types in a renderer. A renderer that depends only on RmlUi and
  the platform graphics API can serve any RmlUi user.
- Keep `Rml::Initialise` process-scoped. Two RmlUi copies in one process (a
  shared-library build plus a product's own) must not interpose on each other.

## Licence line

Some RmlUi renderers in the wild are GPL, for example the Metal renderer in
gearmulator. GPL code never enters Pulp or Vellum. Don't read a GPL renderer to
write a permissive one: a clean-room renderer is written from RmlUi's own
interface and the platform vendor's documentation, by someone who has not read
the GPL source. A product that uses a GPL renderer keeps it in its own
repository.
