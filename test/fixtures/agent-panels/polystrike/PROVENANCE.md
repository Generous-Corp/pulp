# polystrike — a NEGATIVE fixture

Unlike [kelvin](../kelvin/PROVENANCE.md), [lattice](../lattice/PROVENANCE.md)
and [magneto](../magneto/PROVENANCE.md), this panel is kept because it is
**broken**, and it passes by being **refused**.

Authored by a model given Forge's designed-panel brief, same as the others, and
kept verbatim. Its root declares `height:500px` while its own content needs
about 609, and the panel below it sets `overflow:hidden` — so four controls
(`param_2`, `param_3`, `param_4`, `param_6`) are cut by 109px.

The import refuses it:

    capture-control-clipped: param_2/3/4/6 cut by 109px inside .pulp-panel

That refusal is correct, and it is the point. **Chrome clips those controls
too**, so the oracle itself is broken — there is nothing to compare a render
against, and no renderer change can recover a control the design already cut.
It is also distinct from scaling an oversized panel into a smaller host window:
no display-side scaling helps content that overflows the panel's own declared
frame.

`capture-control-clipped` has no other end-to-end test. Deleting this fixture
would leave that gate covered only by the code that implements it, which is how
a gate stops firing without anyone noticing. So the acceptance bar for the
fixture set is "the positive panels import, and this one is rejected by name" —
a fixture that fails on purpose is not a failing suite.

Registered as `agent-panel-clipped-is-rejected`, which demands the NAMED
rejection reason rather than merely a non-zero exit: any import error would
otherwise satisfy "it failed", and the gate could regress behind an unrelated
crash.

Two things make this fixture stop being valid, and both need a human rather
than an edit here:

  - the import SUCCEEDS — the gate regressed, or someone repaired the panel;
  - it fails with a different reason — something upstream now breaks first, so
    the clipping gate is no longer the thing under test.
