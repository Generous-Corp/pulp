# magneto

Authored by a model given Forge's own designed-panel brief (the text in
`src/gen/designed_panel_context.cpp`) plus the `tape-machine` pack's CSS and
fonts. Prompt: a tape echo / magnetic delay binding exactly four macros —
time, feedback, tone, mix.

Kept verbatim so the renderer can be iterated against real generated markup
without paying for a model call every round. This is the artifact, not a
reduction of it: 0 literal CSS colours, tokens only, exactly four bound
macros, and it leans on the GPU vocabulary the brief asks for —
`mix-blend-mode` (screen / multiply / plus-lighter), `conic-gradient`,
`repeating-conic-gradient`, `backdrop-filter` and `filter: blur()`.

That last part is the point. Those are exactly the properties the native
renderer reproduces worst, so a panel that avoided them would score well and
prove nothing.

Baseline at the time it was captured — `tape-machine` pack, DPR 2, 760x716:

    check_pipeline_stages.py   exit 0, all seven stages
    score_native_panel.py      0.6732  (at capture; 0.8012 after the
                                        background-paint and box-shadow fixes)
    92 nodes lowered, 92 native, 0 bitmap fallback, 19 real text nodes

Failing ink by class at capture, worst first: blend 309,409px / gradient
191,300 / fill 89,304 / text 28,625 / shadow 23,793 / filter 19,712.

Two of those numbers moved for a reason worth recording, because both times the
class label pointed at the wrong subsystem. `background-color` painting
instead-of rather than beneath the image layers collapsed `blend` from 309,409
to 75,590 without touching blend code. And the knobs' "too contrasty" conic
faces were not a colour bug at all — the conics match Chrome within 4/255; the
softness IS a six-layer box-shadow stack the native resolver never applied. Add
it and `gradient` falls 171,856 -> 54,285 and `fill` 89,304 -> 5,266.

`fill` had been EXACTLY unchanged across every earlier fix, which is what
finally gave it away: the scorer charges each pixel to the smallest node
containing it, so fill-owned pixels are by construction the ones no gradient
touches — their failures were card shadows bleeding onto the surround.
