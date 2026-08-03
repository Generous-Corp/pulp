# lattice

Authored by a model given Forge's own designed-panel brief (the text in
`src/gen/designed_panel_context.cpp`). Prompt: an arpeggiator / step sequencer
binding five macros — rate, gate, swing, octaves, velocity.

Kept verbatim, for the same reason as [magneto](../magneto/PROVENANCE.md).

It is in the corpus for its shape. A 96-cell step grid is the case neither
other panel covers: many small sibling nodes of nearly identical style, each
with its own shadow, tiled edge to edge. That turns per-node cost and
edge-adjacent seam accuracy into the dominant term, where magneto's few large
knobs hide both. It is also the panel most likely to expose a per-node
regression as a visible pattern rather than a number.

Uses `mix-blend-mode`, `conic-gradient`, `backdrop-filter` and `filter: blur()`.
One literal colour. No `repeating-conic-gradient` — that is kelvin's job.

Baseline — `tape-machine` pack, DPR 2, 760x717:

    check_pipeline_stages.py   exit 0, all seven stages
    score_native_panel.py      0.7570
    323 nodes: 255 frame, 68 text; 0 paint-order inversions

Failing ink by class, worst first, as a share of each class's own ink:

    blend        401,927px   43.7%  21/21 nodes
    shadow       190,474px   30.7%  72/72
    text          57,001px   33.2%  68/68
    gradient      53,208px   10.6%  20/20
    border        11,129px    3.7%  20/20
    filter         6,034px   55.6%   1/1

The lowest score of the three panels, and the most useful one. `shadow` fails
30.7% of its ink across 72 nodes here versus 17.6% on kelvin — the grid is
exactly the many-small-shadows case. `blend` fails ~44% on both panels
independently, which is what makes it a renderer defect rather than a property
of any one design.
