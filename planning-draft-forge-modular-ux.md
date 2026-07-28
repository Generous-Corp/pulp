# Forge Modular — Product Spec (UX draft)

Status: draft for review
Scope: standalone macOS app, own SKU, reusing the Forge shell (C++ + JS/Skia UI,
"Ink & Signal" theme, chat column + preview pane, `claude -p` agent backend).
Ships in one installer alongside our `.vcvplugin`.

Everything here is designed inside the verified constraints: Rack cannot
hot-load a plugin (new module = restart); patches load instantly and Rack can be
launched with a patch path; `.vcv` is plain JSON with port *indices*, not
positions; installed inventory + panel SVGs are readable locally; the public
library API knows plugins but not modules; we can deep-link but never install or
purchase.

---

## 0. One structural objection before the spec

The request says "a project window is specific to one patch" **and** "Module vs
Patch is a tab in the main UX." Those two ideas fight each other: a tab implies
the module generator lives *inside* a patch project window, but module
generation is not per-patch — a generated module lands in the shared
`.vcvplugin` and is usable by every patch and every project.

**Recommendation (what this spec designs):** keep the two tabs as asked, but
make their scopes honest:

- **Patch tab** = the project. Its state (chat history, revisions, provenance)
  belongs to this window's one patch.
- **Module tab** = an app-level workshop that happens to be *reachable* from any
  project window. Its chat history and module library are shared across windows.
  Building a module from inside a project additionally offers "add it to this
  patch" as the finishing move — that's the reason to have it as a tab at all.

If reviewers dislike the shared-state tab, the fallback is Module as a separate
window (`Window > Module Workshop`). The tab version is specced below because it
was asked for and the "generate a module because this patch needs it" flow is
genuinely better with both modes one click apart.

---

## 1. Screen anatomy

Both tabs share the shell: header with tab switch, left chat column (~380px),
right preview pane, footer status strip. Dark Ink & Signal, accent `#16DAC2`,
Jost for UI, JetBrains Mono for wiring text and slugs.

### 1.1 Patch tab (the project)

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● ● ●   Forge Modular — "Acid Bassline"        [ PATCH ] [ module ]    ⚙︎    │
├───────────────────────────────┬──────────────────────────────────────────────┤
│ CHAT                          │ PREVIEW — acid-bassline.vcv        rev 4     │
│                               │                                              │
│  you: make an acid bassline   │  ┌────┐ ┌─────┐ ┌────┐ ┌────┐ ┌───────┐     │
│                               │  │VCO │ │VCF  │ │VCA │ │ADSR│ │AUDIO-8│     │
│  forge:                       │  │    │ │     │ │    │ │    │ │       │     │
│  ┌─ PATCH PLAN ────────────┐  │  │ ●──┼─┼─●   │ │    │ │    │ │       │     │
│  │ 5 modules · 6 cables    │  │  │saw │ │  ●──┼─┼●   │ │●───┼─┼─● 1/2 │     │
│  │ all installed ✓         │  │  └────┘ │cut◉─┼─┘out │ │env │ └───────┘     │
│  └─────────────────────────┘  │         └─────┘└─────┘└────┘                 │
│                               │   (real vendor panel SVGs, real positions,   │
│  VOICE                        │    cables colored to match chat groups)      │
│  VCO SAW → VCF IN             │                                              │
│    rich harmonics for the     ├──────────────────────────────────────────────┤
│    filter to carve            │  ▸ Signal flow  ▸ Params  ▸ Revisions        │
│  VCF LPF → VCA IN             ├──────────────────────────────────────────────┤
│  ...                          │  ⟳ Edited in Rack? — patch unchanged on disk │
│                               │                                              │
│  ┌──────────────────────────┐ │            [ ▶ Open in Rack ]                │
│  │ add more squelch…        │ │                                              │
│  │              [✏ Will edit]│ │                                              │
│  └──────────────────────────┘ │                                              │
├───────────────────────────────┴──────────────────────────────────────────────┤
│ Rack 2.6.1 ✓ installed · 12 plugins / 214 modules known · verbosity: standard│
└──────────────────────────────────────────────────────────────────────────────┘
```

- **Chat column**: prompt, plan cards, wiring explanations (§3), diff cards
  (§5), premium cards (§8). The composer carries the **intent chip** (§4).
- **Preview pane**: rendered patch picture (§7). Hovering a cable in the
  preview highlights its line in the chat explanation and vice-versa — this
  cross-highlight is the single feature that makes "learning tool" real, and it
  is cheap because both sides are generated from the same patch JSON.
- **Sub-tabs under preview**: `Signal flow` (default, the picture),
  `Params` (table of non-default knob values with one-line rationale),
  `Revisions` (rev list, §5).
- **Footer**: Rack detection, inventory count, current verbosity tier.
- **Primary action**: `▶ Open in Rack` — always visible, bottom of preview.

### 1.2 Module tab (the workshop)

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● ● ●   Forge Modular — Module Workshop        [ patch ] [ MODULE ]    ⚙︎    │
├───────────────────────────────┬──────────────────────────────────────────────┤
│ CHAT (shared across windows)  │ PREVIEW — "Tidepool" (generated)             │
│                               │                                              │
│  you: a chorus with a         │        ┌───────────────┐                     │
│  seasick amount of wobble     │        │   TIDEPOOL    │                     │
│                               │        │  ◉ RATE       │                     │
│  forge: building…             │        │  ◉ DEPTH      │                     │
│  ✓ layout manifest            │        │  ◉ MIX        │                     │
│  ✓ DSP compiled               │        │  ● IN   ● OUT │                     │
│  ✓ audio gate: knobs alive    │        └───────────────┘                     │
│  ✓ installed to plugin        │   (the actual emitted panel SVG,             │
│                               │    exact port coords known)                  │
│  ⚠ Rack must restart to see   ├──────────────────────────────────────────────┤
│  new modules. [Relaunch Rack] │  YOUR MODULES (7)                            │
│                               │  Tidepool · Grit · Slewpy · …                │
│  ┌──────────────────────────┐ ├──────────────────────────────────────────────┤
│  │ describe a module…       │ │  [ ⟳ Relaunch Rack with test patch ]         │
│  └──────────────────────────┘ │  [ + Add Tidepool to "Acid Bassline" ]       │
├───────────────────────────────┴──────────────────────────────────────────────┤
│ building: audio gate 2/3 · ~40s left        · verbosity: standard            │
└──────────────────────────────────────────────────────────────────────────────┘
```

Differences from Patch tab:

- No intent chip — every module-tab message is a build or a question about the
  last build; the ask-vs-act problem barely exists here (a question never
  triggers a 90s build: build only on an explicit generate-shaped request, and
  the progress card makes it unmistakable when one starts).
- Preview shows the **generated panel SVG** — we emit it, so this is exact,
  including port positions.
- A **Your Modules** shelf (from our own `plugin.json`) replaces the
  revisions strip.
- Primary action is the restart flow (§2.1), not plain "Open in Rack".
- If opened from a project window, the contextual second action
  `+ Add <module> to <patch>` appears after a successful build; it switches
  back to the Patch tab and stages a mutation (§4 rules apply).

---

## 2. The two flows

### 2.1 Module flow (has an unavoidable restart — own it, don't hide it)

1. **Prompt** → chat shows a live pipeline card reusing the real stages we
   already run: `manifest ✓ → validate ✓ → SVG ✓ → compile … → audio gate …`.
   60–90s; the card shows elapsed + a stage-based estimate. Retries surface as
   "attempt 2 — first DSP failed the dead-knob gate", not silence. This honesty
   is free content: the user watches their module pass a behavioral audio test,
   which is a selling point, not a delay to apologize for.
2. **Built** → panel renders in the preview from the emitted SVG. Chat prints a
   3-line spec summary (I/O, params, tags).
3. **Install** → already part of the pipeline (module lands in the shared
   `.vcvplugin`).
4. **The restart moment.** State-dependent single button:
   - **Rack not running:** button reads `▶ Launch Rack with Tidepool` — we
     generate a tiny test patch (the new module + Audio-8 + a VCO if it's an
     effect, wired sensibly — we know our own port coords) and launch
     `Rack /path/to/tidepool-test.vcv`. Plugin loads at startup; zero friction.
     This is the best case and we should steer users into it: if Rack isn't
     running, never mention restarting at all.
   - **Rack running:** button reads `⟳ Relaunch Rack with Tidepool`, with one
     sub-line of honesty: *"Rack can't load new modules while running."* Click →
     we quit Rack (normal app quit — Rack itself prompts to save if the current
     patch is dirty; we must not suppress that), wait for exit, relaunch with
     the test patch. One click, one possible Rack-owned save dialog. If Rack
     doesn't exit within ~15s (user cancelled the save prompt), the button
     reverts and chat notes "Rack is still running — relaunch when you're
     ready."
5. **Iterate**: "make the wobble slower" in the module tab regenerates → the
   restart button reappears. Chat batches the nag: after the second rebuild
   without a relaunch, stop repeating the warning; the button state carries it.

What we never do: pretend hot-load exists, silently `kill` Rack, or bury the
restart in a settings-y dialog. It's one labeled button whose verb changes.

### 2.2 Patch flow (instant — exploit it)

1. **Empty state** (new project window):

```
│  What do you want to hear?                    │
│                                               │
│  ▸ a classic VCO→VCF→VCA mono synth voice     │
│  ▸ generative ambient from two LFOs           │
│  ▸ krell patch with random melodies           │
│                                               │
│  Using your 12 plugins (214 modules).         │
```

2. **Prompt → plan card** (before building): module list with availability
   badges (§8). If everything is installed, the plan card auto-collapses and we
   go straight to build — don't make people approve happy paths.
3. **Build**: agent emits patch JSON constrained to installed inventory
   (from local `plugin.json` files) → we write `<project>/<name>.vcv` + the
   provenance sidecar (§5) → preview renders → wiring explanation prints (§3).
   Seconds, not minutes — no compile, no gate.
4. **`▶ Open in Rack`**: launch `Rack /path/to/name.vcv`. If Rack is already
   running, launching with a patch argument opens it in the running instance's
   next launch — behavior to verify; if a running Rack ignores the argument,
   fall back to revealing the `.vcv` in Finder + chat line "drag it onto Rack,
   or use File ▸ Open" (open question OQ-1, §9).
5. **Iterate** in chat (mutations, §4) or in Rack (reconciliation, §5). Every
   mutation is a new revision; `Open in Rack` always opens the latest on-disk
   state.

A chat turn can end with an **action chip** rendered inline:
`[▶ Open in Rack]`, `[⟳ Relaunch Rack]`, `[View diff]`, `[Get Plateau ↗]` —
the model proposes the chip, the shell renders and executes it. Chips are how
"a chat turn could end with an action that opens a specific patch or module"
works without giving the model shell access.

---

## 3. The wiring-explanation format (heart of patch mode)

### 3.1 Principles

- **Group by role, not by cable.** A cable-by-cable list is how the file is
  stored, not how a human thinks. Chains read as signal paths.
- **One line of *why* per non-obvious connection; zero lines for obvious
  ones.** `VCA OUT → Mixer IN` needs no rationale. `LFO TRI → VCF CUTOFF` does.
- **Deterministic skeleton, generated garnish.** The chain topology, module
  names, and port names are derived *locally* from the patch JSON + inventory —
  zero tokens, always accurate. Only the short rationale lines come from the
  model. This is both the token-cost answer and the correctness answer: the
  explanation can never describe a cable that doesn't exist.
- **Monospace, arrows, color-keyed.** Each group gets a color; the preview
  draws that group's cables in the same color. JetBrains Mono.

### 3.2 Format

```
<ROLE HEADER>                       ← AUDIO / PITCH & GATE / MODULATION / OUTPUT
  <Module> <PORT> → <Module> <PORT>
      <one short clause of why — only when non-obvious>
```

Chains collapse into a single line when they're a straight series:
`A OUT → B IN → C IN` renders as `A → B → C` with port names only where
ambiguous.

### 3.3 Worked example — classic mono voice with wobble

Patch: VCO → VCF → VCA → audio out; ADSR opens the VCA; LFO wobbles the
filter cutoff. (All Fundamental modules; 9 cables.) At **standard** verbosity
this is the entire explanation:

```
AUDIO      VCO SAW → VCF IN → VCA IN → Audio-8 1/2
           saw is harmonically rich — the filter has something to carve

PITCH & GATE   (from your MIDI keyboard)
           MIDI-CV V/OCT → VCO V/OCT        pitch
           MIDI-CV GATE  → ADSR GATE        key down = envelope starts

MODULATION
           ADSR ENV → VCA CV
           the envelope *is* the loudness shape — attack opens the VCA,
           release closes it
           LFO TRI → VCF CUTOFF
           slow triangle sweeps brightness up and down — the "wobble";
           RATE on the LFO sets how seasick
```

~85 words of prose on top of a locally-generated skeleton. Terse verbosity
(§6) drops every rationale line and keeps only the three chain lines. Learning
mode expands each rationale to a short paragraph and appends "try this: turn
LFO RATE past noon and listen to the cutoff."

Rules that keep it short at scale:

- ≥ 12 cables: print chains only, rationales become hover/tap-to-expand
  (`▸ why?` toggle per group).
- Mults/stacked cables: one line, `LFO TRI → VCF CUTOFF + VCA CV (same wobble
  on brightness and volume)`.
- Attenuated/attenuverted mod: note the depth inline — `(via ATV, 30%)` — only
  when the patch sets a non-default value.

### 3.4 On mutation

A mutating turn does **not** reprint the whole explanation. It prints only the
delta in the same format, under a diff header:

```
+ ADDED
  MODULATION
    S&H OUT → VCF CUTOFF   stepped random brightness, clocked by the LFO
~ CHANGED
    LFO RATE 0.3 → 2.1 Hz  faster wobble to read as movement, not drift
```

Full explanation is always re-derivable from `Signal flow` sub-tab (free —
skeleton is local; rationales are cached per-cable and only regenerated for
cables that changed).

---

## 4. Ask vs act

### 4.1 The rule

Every composer submission in the Patch tab is classified **before send** into:

- **Ask** — answered from patch context; the patch file is never touched.
- **Act** — produces a patch mutation (new revision) + delta explanation.

The user must be able to see which one will happen *before* pressing return,
and override it in one gesture.

### 4.2 The intent chip

The composer carries a live chip at its right edge, updated as you type by a
**local heuristic** (no tokens: imperative verbs / module nouns / "add,
replace, make, remove, faster, instead" → Act; interrogatives / "why, what,
how, explain, which" → Ask; ambiguous → sticky-previous, defaulting to Ask):

```
┌───────────────────────────────────────────────┐
│ why is the LFO on the cutoff?      [? Answer] │   ← Ask: quiet, gray chip
└───────────────────────────────────────────────┘
┌───────────────────────────────────────────────┐
│ add a reverb after the VCA        [✏ Will edit]│   ← Act: accent-colored chip
└───────────────────────────────────────────────┘
```

- **Click the chip** (or ⌘⇧Return) to flip the mode for this message.
- The chip's *final* state is sent with the message as a hard instruction to
  the agent: an Ask-tagged turn runs with no patch-write capability, so a
  misclassified "add a reverb" tagged Ask comes back as *"That's an edit —
  send it as one?"* with a one-click `[✏ Do it]` chip, rather than a surprise
  mutation. Fail closed: **misclassification can lose a click, never a patch.**
- If the model itself decides an Act-tagged message is actually a question, it
  may answer without mutating — the reverse direction is always safe.
- Act turns end with a one-line receipt + `[↩ Undo]` chip (revision revert,
  §5). Undo is what makes a wrong Act cheap enough that we don't need a
  confirmation dialog on every edit.

### 4.3 Why not a modal "edit mode" toggle

A persistent mode switch (chat mode vs edit mode) was considered and rejected:
users won't maintain it, and a stale mode makes the *dangerous* direction
(unwanted mutation) more likely, not less. Per-message classification with a
visible, flippable chip keeps the cost of being wrong at one click.

---

## 5. Provenance and reconciliation

### 5.1 What a project stores

A project is a folder:

```
Acid Bassline.forgemod/
  patch.vcv              ← the live patch; the file Rack opens. Single source
                            of truth for CONTENT.
  provenance.json        ← everything Forge knows ABOUT the patch
  revisions/
    r1.vcv … r4.vcv      ← full snapshot per revision (patches are tiny JSON)
  chat.jsonl             ← transcript
```

`provenance.json` per revision: `{rev, timestamp, origin: "forge"|"external",
prompt?, sha256(patch.vcv), cable_rationales: {cableKey: text},
module_roles: {moduleId: "voice"|"modulation"|…}}`. Cable key =
`(outModule.slug,outPortId)→(inModule.slug,inPortId)` — stable across Rack's
re-numbering of `id`s. Rationale cache is what makes re-explaining free.

The chip `rev 4` in the preview header opens the Revisions strip; any revision
can be previewed and `[Restore]`d (restore = new revision, never history
rewrite).

### 5.2 Detecting external edits

On window focus, before every Act, and before `Open in Rack`: hash
`patch.vcv`, compare to the last recorded hash. Mismatch ⇒ external edit.

### 5.3 The reconciliation UX

**Never silently overwrite. Never silently ignore.** External edit detected:

```
┌ PATCH CHANGED IN RACK ────────────────────────────┐
│ Since rev 4 you (in Rack):                        │
│  + added   Plateau (reverb)                       │
│  + cabled  VCA OUT → Plateau IN L                 │
│  ~ moved   3 modules                              │
│  ~ params  VCF CUTOFF 0.42 → 0.61                 │
│                                                   │
│ [✓ Keep my Rack edits]        [View in preview]   │
└───────────────────────────────────────────────────┘
```

- `Keep my Rack edits` (default, and the only accept path): the on-disk patch
  becomes rev 5, `origin: "external"`, diff summary auto-generated **locally**
  (structural diff of two small JSONs — module adds/removes, cable
  adds/removes, param deltas, positions; zero tokens). Preview re-renders.
  New/changed cables get rationale `"you added this in Rack"` until the user
  asks about them.
- There is deliberately **no "discard their Rack edits" button** in this card.
  Throwing away work the user did by hand in Rack should require intent:
  it lives only in the Revisions strip as `Restore rev 4`.
- **The conflict case** — user sends an Act while unreconciled external edits
  exist: the reconciliation card interposes *first*; the Act is held (shown
  queued in the composer), applied on top of the accepted rev 5. Forge
  mutations therefore always apply to the latest on-disk state; a
  lost-update is structurally impossible.
- Positions-only / params-only drift (no topology change): auto-accept as a
  minor revision with a one-line chat notice instead of the card —
  reconciliation cards for knob tweaks would train users to ignore them.
- **While Rack is running** with our patch open, Forge Acts still write to
  `patch.vcv`, but Rack won't see the change until reopen — chat receipt for
  any Act made while Rack has the patch open appends: *"Rack is showing an
  older version — reopen the patch to see this."* (We cannot push into a
  running Rack; don't pretend.) The corollary risk — Rack later autosaving its
  older in-memory state over our newer file — is exactly the conflict case
  above and is caught by the hash check, restorable from `revisions/`.

---

## 6. Verbosity settings

Setting: **Explanations** — `Terse / Standard / Learning (coming later)`,
in ⚙︎ and as a right-click on any explanation ("explain less like this").

| Tier | What prints | Token cost |
|---|---|---|
| **Terse** | Chain skeletons + diff receipts only. No rationale prose. | ~0 — skeleton and diffs are computed locally |
| **Standard** (default) | Skeleton + one short rationale clause per non-obvious cable, capped ≈ 120 words per explanation; ≥ 12 cables ⇒ rationales collapse behind `▸ why?` (generated on expand, then cached) | Small, bounded per turn |
| **Learning** (v2) | Standard + expandable per-group tutorials, "try this" prompts, jargon glossary on hover | Highest; generated lazily per expansion, never up front |

Cost posture: the *patch* itself and the skeleton never cost explanation
tokens; only rationale clauses do, they're cached per cable-key in provenance,
and mutations regenerate only changed cables. Ask turns reuse the cached
context. Learning mode ships later precisely so its cost model can be
lazy-on-expand from day one.

---

## 7. Preview rendering

### 7.1 Composition

All local, all Skia (which already renders SVG in the Forge shell):

1. Parse `patch.vcv` → modules with `pos` (Rack grid units → px) + cables.
2. For each module, resolve its panel SVG from the installed plugin's `res/`
   (dark variant to match Ink & Signal, falling back to light). Our own
   modules use the SVGs we emitted.
3. Composite panels at their real positions on a rack-rail background.
4. Draw cables as catenary-ish beziers, colored by explanation group (§3),
   with Rack-like sag. Hover: thicken + highlight chat line.
5. Module missing from disk (uninstalled since generation): draw a slate-gray
   placeholder panel of correct HP width with slug + `not installed` badge —
   never a blank hole.

### 7.2 The unknown-port-position problem (constraint 6) — recommendation

We know exact port coordinates for **our** modules (from the manifest) and for
nothing else: third-party port positions live in compiled widget code.

**v1 recommendation: docked port badges — precision where we have it, honest
abstraction where we don't.**

- Endpoint with known coords → cable terminates exactly on the port.
- Endpoint unknown → cable terminates in a small labeled badge **docked to the
  bottom edge of that module's panel**, ordered left-to-right by port index:

```
   ┌─────────────┐
   │  (vendor    │
   │   panel     │
   │   artwork)  │
   │             │
   └─┬───────────┘
    ◖IN L◗ ◖CUT◗      ← badges; cables plug into these
```

- Port badge labels come from… nowhere reliable (`plugin.json` has no port
  names either). Badge shows the port index (`IN 0`, `OUT 2`); the chat
  explanation is where human-readable meaning lives — which it does, because
  the agent names ports semantically ("filter cutoff") from model knowledge.
  When agent-asserted names exist for a cable we show them on the badge, tagged
  as unverified styling (dimmer), since the model can be wrong about indices.
- Do **not** attempt SVG heuristics (detecting port holes in vendor artwork).
  It will be wrong often, and a cable ending confidently on the wrong jack is
  worse than a badge — it teaches the user something false. This spec's whole
  learning premise dies if the picture lies.

**v2 enhancement (verify before promising): the cartographer.** Our
`.vcvplugin` runs *inside* Rack; a hidden helper in it can plausibly walk the
running Rack scene and dump every visible module's true port coordinates to a
local `portmap.json` cache keyed by `plugin/model` + plugin version. Every
patch the user opens in Rack silently upgrades future previews from badges to
exact ports, for exactly the modules they use. Needs API verification
(OQ-2). Ship v1 without it; badges remain the fallback forever (first-ever
preview of a module can't have been mapped yet).

A curated hand-built portmap for the top ~10 plugins (Fundamental first — 39
modules, and it's in every default install) is cheap insurance and makes the
common first-run preview fully exact.

---

## 8. Premium / unavailable modules

### 8.1 Hard rules (from the constraints)

- Generation is **constrained to installed inventory**. We can only wire what
  has a local `plugin.json` (and only *name* modules for uninstalled plugins if
  the model happens to know them — never wire them: no module-level API data,
  no port info, no slugs to trust).
- We can deep-link (`pluginUrl` / library page); we can never install or buy.
- We know `premium: true` and `openSource` per plugin; we do **not** know
  prices, nor whether any specific plugin is included in VCV+. Do not claim
  either.

### 8.2 UX

The plan card separates what it *built* from what it *suggests*:

```
┌ PATCH PLAN — "lush generative ambient" ────────────────┐
│ BUILT with your modules (all installed ✓)              │
│   Fundamental VCO ×2 · Random · VCF · VCA · Delay      │
│                                                        │
│ ▸ WOULD BE BETTER WITH (2 suggestions, not wired)      │
│   Valley — Plateau        reverb        FREE           │
│      the Delay is standing in for a real reverb tail   │
│      [Get in VCV Library ↗]                            │
│   Vult — Caudal           chaos mod     ● PREMIUM      │
│      [View in VCV Library ↗]  see price on its page    │
│                                                        │
│   Premium plugins are sold individually; some are      │
│   included with VCV+ ($29/mo, $19/mo annual) — each    │
│   plugin's Library page says which. [About VCV+ ↗]     │
│   After installing: I'll pick it up automatically.     │
└────────────────────────────────────────────────────────┘
```

- The patch **always works with what's installed** — suggestions are additive,
  never blockers. If the user's inventory genuinely can't express the request
  ("granular resynthesis" with only Fundamental), say so plainly, build the
  nearest honest approximation, and lead with the suggestion.
- `FREE` badge only when `premium: false && openSource: true`-class signals
  support it; `PREMIUM` from the flag; price is always "see its page".
- **After install** (user returns from the Library, having synced in Rack): we
  detect the new `plugin.json` on next scan/focus → chat offers
  *"Plateau is installed now — swap it in for the Delay? [✏ Do it]"*. That
  swap is a normal Act. Note the ordering quirk: Rack's library sync itself
  installs plugins for Rack's *next* launch — our scan sees the plugin folder
  as soon as Rack has downloaded it, so the offer may arrive before Rack can
  load it; the receipt says "restart Rack to load it" when we detect that.

---

## 9. Risks, open questions, and what to cut from v1

### Weakest points (in honesty order)

1. **The preview can still mislead.** Badges fix the port problem, but vendor
   dark/light SVG variants, SVG features Skia's parser doesn't cover, and
   panels drawn partly in code (some vendors draw widgets programmatically)
   mean some panels will render wrong or empty. Mitigation: placeholder-panel
   fallback per module (never per patch), and a "preview is approximate"
   micro-caption the first time any badge or placeholder appears.
2. **Intent classification will annoy someone.** The fail-closed design means
   annoyance = one extra click, never data loss — but if the heuristic is
   below ~90% on real usage, the chip becomes noise. Instrument it (local
   counts only) and tune before adding any smarter (token-costing) classifier.
3. **Quitting the user's running Rack** (module relaunch) intersects with
   Rack's own save prompts and with patches *other* than ours being open.
   The 15s-revert design covers it, but this is the flow to usability-test
   first.
4. **Agent patch quality is unproven at inventory scale.** Module generation
   has a behavioral gate; patches have none — a syntactically valid patch can
   still be silent (VCA never opened, no clock). v1 should at least run a
   local *static* lint (every audio-path module reachable from an output;
   every VCA has a CV source or non-zero level; warn on silence-shaped
   topologies) before showing "done". A behavioral audio gate for patches
   (headless Rack render) is not currently possible — no headless mode is
   assumed — so the lint is the honest ceiling (OQ-3).

### Open questions

- **OQ-1**: does `Rack /path/to.vcv` open the patch in an *already running*
  Rack instance (macOS `open` semantics), or only at launch? Determines
  whether `Open in Rack` needs the Finder-reveal fallback.
- **OQ-2**: can a plugin legitimately walk the Rack scene graph and read other
  modules' port widget positions (the cartographer, §7.2)? Verify against the
  Rack 2 API before it appears on any roadmap slide.
- **OQ-3**: is there any sanctioned headless/offline render path for Rack that
  could give patches a behavioral gate like modules have?
- **OQ-4**: multiple project windows editing patches that share *our*
  generated modules — a module regeneration (Module tab) can change a module's
  I/O and silently break old patches at next Rack start. Do we version module
  slugs (`tidepool`, `tidepool2`) or mutate in place? Lean: never mutate a
  module's I/O in place once any provenance file references it; params-only
  changes may mutate.

### Cut from v1

- **Learning mode** (ship the setting stub, greyed "coming soon" — it anchors
  Standard as the middle tier, which the owner asked for).
- **Cartographer portmap** (v2, pending OQ-2). Curated Fundamental portmap
  stays — it's static data, not engineering risk.
- **Cable-level editing gestures in the preview** (drag-to-repatch). The
  preview is a *picture with hover*, not an editor — Rack is the editor.
  Duplicating Rack's editor badly is the single biggest scope trap in this
  product; the chat is our editing surface.
- **Any uninstalled-module wiring**, even "the model is pretty sure Plateau's
  input is port 0". Recommend-only, hard line.
- **Multi-patch projects / setlists.** One window, one patch, as asked.

---

## 10. Resolutions to the open questions (verified against the SDK and Rack binary)

Three of §9's four open questions are answered. One of them changes §7.

### OQ-2 — the cartographer is viable. **Yes.**

`app::RackWidget::getModules()` returns `std::vector<ModuleWidget*>` and is public
API, not `PRIVATE`. `app::PortWidget` carries `portId` and `type`, and inherits a
Widget `box`, so its position is readable. A plugin running inside Rack can
therefore walk the scene and record every visible module's true port geometry.

It is better than §7.2 assumed. `engine::PortInfo` carries `name` ("Sine",
"Pitch input", "Mode CV") **and** an optional `description`, reachable from
`PortWidget::getPortInfo()`. So the cartographer yields exact coordinates *and*
real vendor port names — which removes the §7.2 concession that badge labels
"come from nowhere reliable" and can only show indices.

### OQ-3 — Rack has a headless mode. **Yes, with a caveat.**

`-h / --headless` launches with no window. That is the missing half of a patch
behavioural gate, so §9's "not currently possible" is too pessimistic.

The caveat is real though: headless loads the *autosaved* patch rather than an
arbitrary file, and it still runs the audio engine against a live device. A gate
built on it must control the user folder (`-u`) to place the patch as the
autosave, and must route audio somewhere silent. Until that is proven, the
static lint stands as the shipping check.

### OQ-1 — still open, and needs a real launch.

Whether a patch path reaches an already-running instance is not documented.
Answering it means launching Rack, which opens an audio device, so it should be
done deliberately and announced rather than casually.

### A finding that rewrites §7: `--screenshot`

`-t / --screenshot <zoom>` captures **every installed module** to
`<user folder>/screenshots/<plugin slug>/<module slug>.png`.

This retires the preview's biggest fidelity risk (§9.1). Rather than parsing
vendor SVGs through Skia and hoping — which fails on SVG features the parser
misses and on the vendors who draw panels programmatically rather than in
artwork — we let Rack render its own panels once, and composite the PNGs. Rack
drawing its own modules cannot disagree with Rack.

Revised recommendation for §7:

- **Panel imagery**: Rack's own screenshots, refreshed after any plugin install.
  SVG parsing becomes the fallback, not the primary path.
- **Port geometry**: the cartographer (OQ-2), with docked badges as the
  first-run fallback exactly as §7.2 designed.

The two compose cleanly: screenshots give a faithful picture, the cartographer
gives faithful cable endpoints, and each degrades to something honest alone.
