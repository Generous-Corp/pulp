# Plan — Forge Modular as a fourth Forge shell

Status: proposal. Nothing below is built yet.

Written after two failed attempts to match Forge by hand: the first copied
tokens from a `chrome.cpp` 431 commits stale, the second matched values but kept
a parallel implementation. Matching values is not reuse, and it drifts.

## The finding that makes this cheap

Forge already varies one chrome across three products. It is not a coincidence
of structure — it is a declared seam:

```cpp
// include/forge/shell.hpp
enum class ShellKind { effect, instrument, midi_effect };
```

`src/chrome.cpp` is 9,154 lines and switches on that enum in **13 places**, all
of them named, most of them pure string selection:

```cpp
const char* default_build_title(ShellKind kind);
const char* prompt_placeholder(ShellKind kind);
const char* followup_placeholder(ShellKind kind);
std::string shell_kind_badge(ShellKind kind);   // "FORGE INST" / "FORGE MIDI" / "FORGE FX"
```

`fx_shell.cpp`, `instrument_shell.cpp` and `midi_shell.cpp` are ~900 lines each
and contain **no UI** — they are DSP, descriptor and parameter surface. The whole
look lives in the one chrome.

So Forge Modular should be the **fourth value of that enum**, not a fourth
implementation. `ShellKind::modular`, a `modular_shell.cpp` beside its three
siblings, and every visual difference behind `kind == ShellKind::modular`.

## Why this cannot leak into Forge Instrument

The user's hard constraint. Three reasons it holds:

1. **Every difference is behind an enum branch.** An `if (kind_ ==
   ShellKind::modular)` cannot affect `instrument` — there is no path.
2. **`-Wswitch` finds every site.** Adding an enum value makes each of the 13
   switches fail to compile until it has a `modular` case. Nothing is missed by
   forgetting.
3. **A test asserts it.** Build the chrome once per `ShellKind`, render each
   headlessly, and assert the three existing renders are **byte-identical** to
   their committed baselines. If a "Forge Modular" change alters Forge
   Instrument by one pixel, that test goes red. This is the guarantee, not the
   intention.

That third point is the one that makes the constraint real, and it is the first
thing to build — before any visual work.

## Where the code lives

**Recommendation: the app moves into the Forge repo; the Rack side stays here.**

| Piece | Repo | Why |
|---|---|---|
| `modular_shell.cpp`, `ShellKind::modular`, chrome branches | **forge** | Beside its three siblings, sharing the one chrome. This is the reuse. |
| The `.vcvplugin`, `generate.py`, `patch.py`, the gates, the cartographer | **pulp-modular-rack** | The only artifact that links the GPLv3 Rack SDK, plus a Python pipeline Forge has no use for. |

The app invokes the generator as a subprocess and already resolves it from a
bundled `Resources/tools`, so the split needs no new mechanism — the app ships
the pipeline as a payload, exactly as it does today.

The alternative — extracting chrome into a library both repos consume — is more
correct in the abstract and much more work: chrome touches Forge's generation
loop, its project store, its marketplace surface. It would mean drawing an
interface through all of that. Not worth it to serve one sibling product.

**What this costs:** Forge Modular's app stops being buildable from this
checkout alone. That is a real loss and worth naming.

## The visual deltas, and how each is expressed

Each is additive and guarded. None replaces Forge behaviour.

### 1. Home tabs — Module | Patch

Forge Modular makes two kinds of thing; Forge makes one. The tabs sit above the
composer and select which.

Extension: a `ModularTabs` view chrome builds only for `ShellKind::modular`,
inserted in the home column above the composer card. Radio-grouped, so one or
the other. Selecting one changes the hero wording, the primary button label, the
title-bar artifact badge, and which generator the submit reaches.

Nothing else in chrome learns about tabs.

### 2. `@` mentions — a native overlay, typed into the composer

The prototype's behaviour, and the piece worth doing properly: typing `@` in the
composer opens a dropdown over it, filtering 4,705 library modules as you type,
Enter or click inserts the mention, Escape dismisses.

This is a chrome-level addition because it has to sit **over** the composer card
and follow the caret:

- A `MentionOverlay` view, absolutely positioned above the composer, built only
  for `ShellKind::modular`.
- Fed by `TextEditor::on_text_input`: on `@`, capture the token being typed and
  filter; on space or Escape, dismiss.
- Rows show a module's brand, name and one of three states — `ready`,
  `available`, `paid`. **Only `ready` can be inserted**, because Rack keeps
  missing modules as placeholders and offers to fetch them, so an unavailable
  mention is an offer rather than a wall.
- Keyboard first: up/down/Enter, because a dropdown you must reach for with the
  mouse mid-sentence is not native-feeling.

**A bridge gap blocks this today.** An overlay needs both axes of absolute
positioning; the widget bridge exposes `start`/`end` insets and nothing
vertical. Building this in chrome's C++ sidesteps the bridge entirely — one more
reason the C++ route is the right one.

### 3. The composer variant

Forge's composer card is text above one action row. Forge Modular's differs in
its row contents only: `@` and `Random` on the left, `Ask` and `Build` on the
right, where Forge has `+` / `Random` and `Select model` / `Create`.

Extension: chrome's composer builder takes the row contents from a small
per-kind description rather than hard-coding them. Forge's three kinds keep the
description they have now; `modular` supplies its own. This is the one change
that touches shared code rather than adding beside it, so it needs the identical
-render test above to prove the other three are untouched.

### 4. Wording

`prompt_placeholder`, `default_build_title`, `followup_placeholder` and
`shell_kind_badge` all already take `kind`. Adding a `modular` case to each is
the whole change — four one-line additions.

### 5. The rack preview and wiring lines

Forge Modular's working screen shows the rack being wired, with each cable's
*why*. Forge's shows a plugin's controls. This is a whole additive surface, not
a variant: a `RackPreview` view chrome mounts in the workspace only for
`modular`. `patch_layout.hpp` already computes the geometry and is tested.

## Staying true to the prototype

The prototype (`design/prototype/ForgeModular.dc.html`) and Forge sometimes
disagree. The rule, from the user: **be different where it matters, not where it
doesn't.**

- **Forge wins** on palette, type, radii, spacing, rail, title bar, card
  geometry, motion — anything that makes two apps feel like one product.
- **The prototype wins** on what Forge Modular *does*: the tabs, the mention
  picker, the wiring lines with why clauses, the rack preview, Eurorack wording.

Where a conflict is not obviously one or the other, it goes in a decisions list
with the reasoning, not resolved silently.

## A/B methodology — before and after, every pass

`tools/rack/compare_renders.py` exists and does the sheet plus a mean
per-pixel difference. What is missing is discipline about *what* is compared.

Three comparisons, each with a committed baseline:

1. **Ours vs Forge Instrument** — the brand check. Both built from source and
   screenshotted the same way. Currently 17.7/255. This number should fall and
   never rise without a stated product reason.
2. **Ours vs the prototype** — the product check, per screen. Only the home
   screen has a reference today; the rest are blocked on multi-screen capture
   (see below).
3. **Forge Instrument vs its own baseline** — the **no-leak check**. Must be
   byte-identical. This is the one that protects the other products.

Before/after on every visual change: capture, compare, record the number, name
every remaining difference. A number with no list is not honest — a shell can
score well and still have a dead button, which is exactly what happened here.

**Blocked:** the prototype's other screens. `pulp import-design` captures
initial state only; multi-screen capture is coming as deterministic CDP actions.
Do **not** inject scripts into the prototype — twelve attempts, four strategies,
two distinct images. An isolated DevTools session is the interim route if a
reference is needed sooner.

## What I can prove on this machine, and what I cannot

**Can, and will, before handing over:**

- Build Forge Instrument and Forge Modular from source and A/B them.
- Drive every control headlessly — `simulate_click` at real coordinates through
  the root, both sides of every boolean, every gate negative-controlled.
- Type into the composer, open the mention overlay, filter, insert, dismiss.
- Click Build and watch the real generator run, refuse, or retry.
- Generate a module and a patch, and confirm Rack loads the plugin from its own
  log.
- Launch Rack, see the modules, capture it.
- `auval` the AU, `clap-validator` the CLAP, load-probe the VST3.
- Sign, notarize, verify Gatekeeper accepts.

**Cannot, and will say so rather than imply otherwise:**

- **`auval` on m5 over SSH** — AU registration needs a GUI login session, so it
  fails regardless of the plugin.
- **The plugins in a DAW** — no DAW automation here; this needs a human.
- **How it feels.** Latency, hover, focus, whether a dropdown lands where the
  hand expects. A screenshot cannot answer it and neither can a test.

## Order of work

1. **The no-leak test first.** Baseline renders for the three existing shells,
   asserted byte-identical. Nothing else is safe until this exists.
2. `ShellKind::modular` + `modular_shell.cpp`, wording cases, badge. Prove the
   three baselines still pass.
3. The composer row description — the one shared-code change. Baselines again.
4. The tabs, guarded. A/B against the prototype.
5. The mention overlay, keyboard-first. Drive it headlessly.
6. The rack preview and wiring lines on the working screen.
7. Wire what remains: patch cards, module library, the module shelf listing the
   real modules, settings.
8. Re-validate all formats, sign, notarize, install on m5.
9. Hand over with the A/B sheets, the numbers, and the list of what is still
   different and why.

## Open questions for you

1. **Does the app moving into the Forge repo work for you?** It is what makes
   this reuse rather than cloning, and it means Forge Modular's app is no longer
   buildable from this checkout alone.
2. **Is `ShellKind::modular` in Forge's enum acceptable**, given Forge Modular
   is a separate SKU? It puts a Rack-shaped product in Forge's core types.
3. **Should the `.vcvplugin` and the Python pipeline stay here?** I think yes —
   it is the only GPLv3-linking artifact and Forge has no use for it.
