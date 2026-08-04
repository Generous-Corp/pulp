# PLAN — close the gap to the prototype

The prototype (`ForgeModular.dc.html`) showed things that were designed and then
not landed. This names the gap precisely, so it can be closed, and names what is
deliberately deferred, so "not landed" and "not wanted" stop looking the same.

An independent review found the first version of this list incomplete: three
gaps named, seven more real. That is recorded here rather than quietly fixed,
because a gap list that is wrong is worse than no list — it declares done.

The house style is Forge Instrument's. The prototype's *content* is what we
want, in the chrome the other three products already use. Nothing here adds a
new visual language.

---

## Landed

- **A module draws its own panel.** The emitter had been writing 23 KB of
  artwork per module beside a preview that never read it. Drawn now, clipped to
  its slot, and only on OUR modules — matching artwork by model slug alone drew
  our panel on a vendor's VCO, which is plausible and a lie about what is in the
  rack.
- **The rack is drawn from the patch's real geometry.** A `.vcv` records no
  panel width and no jack positions, so every module drew in the same default
  slot and cables hung off the edges. Both travel in the sidecar now.
- **Role grouping in the explanation** — AUDIO / PITCH & GATE / CLOCK /
  MODULATION, with counts and dots, in signal order, and no heading for a role
  the patch does not use.
- **Wrapping follows the pane.** It was a fixed 118 columns, right for the old
  820pt stage and running off the edge of the 430pt chat column.

## Still to land

- **The module spec table** — WIDTH / CONTROLS / I/O / DSP / PANEL, every field
  derived from the generated manifest, never retyped. A spec that disagrees with
  the module is worse than no spec; a row we cannot derive is not shown.
- **Honest degradation.** A module whose artwork is missing must SAY so — a
  hatched face at its true width — rather than render an indistinguishable grey
  box. Cables should be withheld when either end's jack positions are unknown,
  rather than drawn at a guessed spot. This is the one the current Gap 1 check
  would pass without: an empty grey panel and a broken panel look identical.
- **The stage cable legend and stage-side hover.** Hovering a cable in the rack
  should light its line in the explanation, not only the reverse.
- **The Rack presence and launch model**, surfaced rather than inferred: whether
  Rack is running, installed, or absent changes what the button should say.
- **One icon system** — artifact, role and availability — in the existing Forge
  geometric vocabulary, used in tabs, toolbar title, group headings and mention
  rows. The cheapest recognisable step toward the prototype.
- **The toolbar meta pill** (`MODULE · 12 HP`, `PATCH · 8 MODULES · 9 CABLES`)
  and the derived patch intro line. Both come from data already loaded.
- **A structure-derived overview for patches nobody generated** — imported and
  example patches have no per-cable prose and never will; a computed summary
  plus the verified idiom's `is` line is the only explanation they can have.

---

## Deliberately deferred

Written down so the absence is a decision rather than an oversight.

- **The build animation** (cables landing by role, a travelling pulse during the
  wait). Real design work, tracked separately; the status card already says what
  is happening, which is the part that carries information.
- **The Eurorack-parts page.** A catalogue of our own drawn components. Useful
  for a contributor, not for someone building a patch.
- **The mention picker's richer states** — tags, match counts, chips. The
  availability pill and the one-line local-index footer are the part that
  explains the model; the rest is decoration on a menu.
- **The installer's SDK acknowledgement screen.** Tracked with the installer
  work, not the UI.
- **Format pills and the joined-tab composer.** Prototype styling that predates
  the shared Forge chrome; adopting them would fork the house style for one
  product.

---

## Ordering

Degradation states first: they are the difference between "we could not draw
this" and "this is broken", and every other visual claim rests on the reader
being able to tell those apart. Then the spec table and the meta pill, which are
derived from data already in hand. Icons and the legend last — most surface,
least meaning.

None of this changes generation. It changes what the app tells you about what it
generated, which is where the prototype was ahead of us.
