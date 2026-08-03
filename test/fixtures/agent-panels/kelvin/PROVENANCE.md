# kelvin

Authored by a model given Forge's own designed-panel brief (the text in
`src/gen/designed_panel_context.cpp`). Prompt: a synth voice binding five
macros — attack, release, cutoff, resonance, drive — plus an output meter.

Kept verbatim, for the same reason as [magneto](../magneto/PROVENANCE.md):
iterating the renderer does not need a model in the loop, and re-prompting each
round changes the subject under test.

It exists because one panel is not a corpus. magneto is a delay: a few large
knobs on a wide surround. kelvin is denser and more typographic — 44 text nodes
against magneto's 19 — so it weights text and small-control detail far more
heavily. A renderer tuned until one panel scores well has been fitted to that
panel.

Uses the GPU vocabulary the brief asks for: `mix-blend-mode`, `conic-gradient`,
`repeating-conic-gradient`, `backdrop-filter` and `filter: blur()`. One literal
colour, an entity rather than a hex.

Baseline — `tape-machine` pack, DPR 2, 760x717:

    check_pipeline_stages.py   exit 0, all seven stages
    score_native_panel.py      0.8479
    210 nodes: 166 frame, 44 text; 0 paint-order inversions

Failing ink by class, worst first, as a share of each class's own ink:

    blend        124,338px   44.0%   6/6 nodes
    gradient      98,816px   11.4%  31/31
    shadow        85,302px   17.6%  21/21
    text          52,393px   31.3%  44/44
    border        11,302px    3.6%  17/17
    filter         9,576px   99.8%   1/1

Read the percentage, not the pixel count. `gradient` loses more absolute pixels
than `shadow` while failing a third as much of what it draws — it is simply the
larger surface. The two classes actually failing are `blend` at 44% and
`filter` at essentially all of its single node.
