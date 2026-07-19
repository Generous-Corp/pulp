// Tests for material_audit.mjs — the declared-vs-emitted counter.
//
// An auditor's own tests have to prove it can FAIL. A checker that only ever
// reports green is indistinguishable from one that reports nothing, and this
// repo has already shipped three bugs behind exactly that: layout_parity green
// on a broken render, thumb_parity green on a dropped blend, fidelity_diff so
// loud (832 false failures) that its output stopped being read. So every case
// here pairs a positive control (a real drop IS caught) with a negative one (a
// legitimate emission is NOT flagged).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { auditMaterials, auditCodegen, parseAnchors, parseCalls } from './material_audit.mjs';

const materials = (nodes) => ({ nodes });
const envelope = (children) => ({ root: { node_id: 'root', name: 'Root', children } });

// A stand-in for `pulp-import-design --emit js` output. Only the two shapes the
// join depends on matter: the anchor line and the material calls keyed by js id.
const anchor = (jsId, nodeId) => `setAnchor('${jsId}', 'figma-plugin:${nodeId}');`;

test('a declared stroke opacity that did not survive is a silent drop', () => {
  const m = materials([
    { node_id: '0:1', name: 'Faint', type: 'FRAME',
      declared: { stroke: [{ type: 'SOLID', opacity: 0.2, color_alpha: 1 }] } },
  ]);
  const dropped = auditMaterials(m, envelope([
    { node_id: '0:1', name: 'Faint', style: { border: '1px solid #ffffff' } },
  ]), []);
  assert.equal(dropped.findings.length, 1);
  assert.equal(dropped.findings[0].property, 'stroke.opacity');

  // Negative control: the SAME declaration, emitted with its alpha, is silent.
  const kept = auditMaterials(m, envelope([
    { node_id: '0:1', name: 'Faint', style: { border: '1px solid #ffffff33' } },
  ]), []);
  assert.deepEqual(kept.findings, []);
});

test('a diagnosed degradation is not a silent drop — silence is what is audited', () => {
  // The invariant's second arm. Getting this wrong in either direction is fatal:
  // miss it and every honest degradation reads as a bug (fidelity_diff's 832);
  // over-apply it and real drops get excused.
  const m = materials([
    { node_id: '0:1', name: 'Oval', type: 'VECTOR',
      declared: { stroke: [{ type: 'GRADIENT_LINEAR', opacity: 1, color_alpha: 1 }] } },
  ]);
  const env = envelope([{ node_id: '0:1', name: 'Oval', style: {} }]);

  const silent = auditMaterials(m, env, []);
  assert.equal(silent.findings.length, 1, 'undiagnosed drop is a finding');
  assert.equal(silent.findings[0].property, 'stroke.GRADIENT_LINEAR');

  // The field is `code`. This assertion is the point of the test: reading `kind`
  // here (as the first draft did) indexed every diagnostic as "unknown", which
  // silently disabled this whole arm — an auditor with a broken join reports
  // confident nonsense, which is worse than no auditor.
  const spoken = auditMaterials(m, env, [
    { code: 'vector-simplified', node_id: '0:1', detail: 'stroke dropped' },
  ]);
  assert.deepEqual(spoken.findings, [], 'a drop the decoder announced is not silent');
  assert.equal(spoken.diagnosedCounts.stroke, 1,
    'and it is counted as DIAGNOSED, never folded into emitted');
  assert.equal(spoken.emittedCounts.stroke ?? 0, 0);
});

test('a gradient stroke carried onto the path as fillGradient is emitted, not dropped', () => {
  // The tool's own stroke-blind spot, aimed at itself. A GRADIENT stroke on a
  // vector never becomes a `style.border`; it rides onto the PATH as a fill
  // gradient (the knob-rim strokes render via setSvgFillGradient). The decode
  // stage counted only style.border, so it flagged 40 rendered rims as silent
  // stroke drops — the exact class this tool exists to end, turned inward.
  const m = materials([
    { node_id: '0:1', name: 'Oval', type: 'VECTOR',
      declared: { stroke: [{ type: 'GRADIENT_LINEAR', opacity: 1, color_alpha: 1 }] } },
  ]);
  // Same node, but its stroke gradient reached the path as fillGradient.
  const env = envelope([{ node_id: '0:1', name: 'Oval', style: {},
                          fillGradient: 'linear-gradient(0deg, #ffffff3d, #00000000)' }]);

  const out = auditMaterials(m, env, []);
  assert.deepEqual(out.findings, [], 'a stroke gradient on the path is not a silent drop');
  assert.equal(out.emittedCounts.stroke, 1, 'and it counts as emitted');
  assert.equal(out.declaredCounts.stroke, 1);

  // Guard against over-silencing: the SAME declaration with NO path gradient is
  // still a finding (this is the existing invariant, restated so a future edit
  // that widens the escape hatch trips here).
  const envNoGrad = envelope([{ node_id: '0:1', name: 'Oval', style: {} }]);
  const strict = auditMaterials(m, envNoGrad, []);
  assert.equal(strict.findings.length, 1, 'a gradient stroke that reached neither border nor path IS a drop');
});

test('a non-uniform corner radius collapsed to one number is a partial drop', () => {
  const m = materials([
    { node_id: '0:1', name: 'Card', type: 'ROUNDED_RECTANGLE',
      declared: { corner_radius: [8, 8, 0, 0] } },
  ]);
  const f = auditMaterials(m, envelope([
    { node_id: '0:1', name: 'Card', style: { border_radius: 8 } },
  ]), []).findings;
  assert.equal(f.length, 1);
  assert.equal(f[0].property, 'corner_radius.non_uniform');

  // Negative control 1: four corners that agree, emitted, are silent.
  assert.deepEqual(auditMaterials(materials([
    { node_id: '0:1', name: 'Even', type: 'FRAME', declared: { corner_radius: [4, 4, 4, 4] } },
  ]), envelope([{ node_id: '0:1', name: 'Even', style: { border_radius: 4 } }]), []).findings, []);

  // Negative control 2: a node that declared NO rounding must never be flagged
  // for not emitting any — otherwise the check fires on every square box.
  assert.deepEqual(auditMaterials(materials([
    { node_id: '0:1', name: 'Square', type: 'FRAME', declared: { corner_radius: [0, 0, 0, 0] } },
  ]), envelope([{ node_id: '0:1', name: 'Square', style: {} }]), []).findings, []);
});

test('the diagnosed arm applies to EVERY property, not just the one it was wired for', () => {
  // The arm was implemented per-property and only stroke got it, so a corner
  // radius the decoder had announced still printed as an unaccounted drop. The
  // summary contradicted the findings, and the summary is the part that gets
  // read. Each property is checked here explicitly rather than trusting that a
  // shared helper covers them — that trust is exactly what failed.
  const cases = [
    ['corner_radius', { corner_radius: [8, 8, 0, 0] }, 'corner-radius-simplified'],
    ['effects.LAYER_BLUR', { effects: ['LAYER_BLUR'] }, 'effect-unsupported'],
    ['blend_mode', { blend_mode: 'LINEAR_BURN' }, 'blend-unsupported'],
    ['stroke', { stroke: [{ type: 'GRADIENT_LINEAR', opacity: 1, color_alpha: 1 }] }, 'vector-simplified'],
  ];
  for (const [prop, declared, code] of cases) {
    const r = auditMaterials(
      materials([{ node_id: '0:1', name: 'N', type: 'FRAME', declared }]),
      envelope([{ node_id: '0:1', name: 'N', style: {} }]),
      [{ code, node_id: '0:1' }]);
    assert.deepEqual(r.findings, [], `${prop}: a diagnosed drop is not silent`);
    assert.equal(r.diagnosedCounts[prop], 1, `${prop}: and the summary says so`);
    assert.equal(r.declaredCounts[prop] - (r.emittedCounts[prop] || 0) - r.diagnosedCounts[prop], 0,
      `${prop}: nothing left unaccounted, so the table cannot contradict the findings`);
  }
});

test('a blur that lowered to its filter slot is emitted, not excused', () => {
  // FOREGROUND_BLUR/LAYER_BLUR now lower to style.filter and BACKGROUND_BLUR
  // to style.backdrop_filter, so the audit must read them through the
  // survived arm — and still catch the drop when the slot is missing.
  const cases = [
    ['FOREGROUND_BLUR', { filter: 'blur(8px)' }],
    ['LAYER_BLUR', { filter: 'blur(4px)' }],
    ['BACKGROUND_BLUR', { backdrop_filter: 'blur(12px)' }],
  ];
  for (const [t, style] of cases) {
    const m = materials([{ node_id: '0:1', name: 'B', type: 'FRAME', declared: { effects: [t] } }]);
    const kept = auditMaterials(m, envelope([{ node_id: '0:1', name: 'B', style }]), []);
    assert.deepEqual(kept.findings, [], `${t}: a lowered blur is silent`);
    assert.equal(kept.emittedCounts[`effects.${t}`], 1, `${t}: and counted as emitted`);

    // Positive control: the same declaration with an empty style, undiagnosed,
    // IS a silent drop — the survived arm must not become a blanket excuse.
    const dropped = auditMaterials(m, envelope([{ node_id: '0:1', name: 'B', style: {} }]), []);
    assert.equal(dropped.findings.length, 1, `${t}: an unlowered blur is caught`);
    assert.equal(dropped.findings[0].property, `effects.${t}`);
  }

  // The wrong slot must not count: a background blur emitted as `filter`
  // blurs the node itself instead of what sits behind it.
  const m = materials([{ node_id: '0:1', name: 'B', type: 'FRAME',
    declared: { effects: ['BACKGROUND_BLUR'] } }]);
  const wrongSlot = auditMaterials(m, envelope([
    { node_id: '0:1', name: 'B', style: { filter: 'blur(12px)' } }]), []);
  assert.equal(wrongSlot.findings.length, 1, 'a background blur needs backdrop_filter');
});

test('a declared blend mode that never reached the envelope is a silent drop', () => {
  // The file's single MULTIPLY noise layer, composited NORMAL, lightened every
  // panel in the design by ~20/255 and nothing said a word for an entire evening.
  const m = materials([
    { node_id: '0:1', name: 'noise@2x', type: 'RECTANGLE', declared: { blend_mode: 'MULTIPLY' } },
  ]);
  assert.equal(auditMaterials(m, envelope([
    { node_id: '0:1', name: 'noise@2x', style: {} }]), []).findings.length, 1);
  assert.deepEqual(auditMaterials(m, envelope([
    { node_id: '0:1', name: 'noise@2x', style: { mix_blend_mode: 'multiply' } }]), []).findings, []);
  // A mode we refuse ON PURPOSE (LINEAR_BURN composites backwards in CSS) is
  // announced, so it is a choice on the record rather than a silent drop.
  assert.deepEqual(auditMaterials(materials([
    { node_id: '0:1', name: 'x', type: 'RECTANGLE', declared: { blend_mode: 'LINEAR_BURN' } },
  ]), envelope([{ node_id: '0:1', name: 'x', style: {} }]),
    [{ code: 'blend-unsupported', node_id: '0:1' }]).findings, []);
});

test('declared shadows are counted, so 16-declared-against-1-emitted cannot hide', () => {
  // The case that motivated the whole tool. It is not subtle and it is not
  // pixel-dependent — it is a COUNT, and no checker we had was counting.
  const nodes = [];
  const emitted = [];
  for (let i = 0; i < 16; i++) {
    nodes.push({ node_id: `0:${i}`, name: `Card${i}`, type: 'FRAME',
                 declared: { effects: ['DROP_SHADOW'] } });
    emitted.push({ node_id: `0:${i}`, name: `Card${i}`,
                   style: i === 0 ? { box_shadow: '0px 2px 4px 0px #00000040' } : {} });
  }
  const r = auditMaterials(materials(nodes), envelope(emitted), []);
  assert.equal(r.declaredCounts['effects.DROP_SHADOW'], 16);
  assert.equal(r.emittedCounts['effects.DROP_SHADOW'], 1);
  assert.equal(r.findings.length, 15, 'every unaccounted shadow is named');
});

test('a fill stack emitted as its bottom paint alone is a silent drop', () => {
  // The slider-thumb bug, as a count. The design declares #4b4d51 with white@0.55
  // over it; Figma composites that to #aeafb1. Emitting the bare base is not a
  // missing property — it is a WRONG COLOR, which got attributed to style
  // precedence and then to a dropped node before anyone counted the paints.
  const thumb = (rgb) => [
    { type: 'SOLID', opacity: 1, color_alpha: 1, rgb: '#4b4d51' },
    { type: 'SOLID', opacity: 0.55, color_alpha: 1, rgb: '#ffffff' },
  ];
  const m = materials([
    { node_id: '0:1', name: 'Fader Position 3', type: 'ELLIPSE', declared: { fill: thumb() } },
  ]);
  const bug = auditMaterials(m, envelope([
    { node_id: '0:1', name: 'Fader Position 3', style: { background_color: '#4b4d51' } },
  ]), []);
  assert.equal(bug.findings.length, 1);
  assert.equal(bug.findings[0].property, 'fill.stack');

  // Negative control, and the one that matters: the FIXED output must be silent.
  // An earlier cut compared only the alpha channel, and #aeafb1 and #4b4d51 are
  // both opaque — so it flagged the fix as the bug. A checker that cries wolf on
  // correct code gets switched off, and then it protects nothing.
  const fixed = auditMaterials(m, envelope([
    { node_id: '0:1', name: 'Fader Position 3', style: { background_color: '#aeafb1' } },
  ]), []);
  assert.deepEqual(fixed.findings, [], 'the composited color is not flagged');
  assert.equal(fixed.emittedCounts['fill.stack'], 1);

  // Negative control: a lone paint is not a stack and is never checked.
  assert.deepEqual(auditMaterials(materials([
    { node_id: '0:1', name: 'Solo', type: 'FRAME',
      declared: { fill: [{ type: 'SOLID', opacity: 1, color_alpha: 1, rgb: '#4b4d51' }] } },
  ]), envelope([{ node_id: '0:1', name: 'Solo', style: { background_color: '#4b4d51' } }]), []).findings, []);

  // Negative control: if everything above the base is fully transparent, the base
  // IS the right answer — emitting it is correct, not a drop.
  assert.deepEqual(auditMaterials(materials([
    { node_id: '0:1', name: 'Ghost', type: 'FRAME', declared: { fill: [
      { type: 'SOLID', opacity: 1, color_alpha: 1, rgb: '#4b4d51' },
      { type: 'SOLID', opacity: 0, color_alpha: 1, rgb: '#ffffff' },
    ] } },
  ]), envelope([{ node_id: '0:1', name: 'Ghost', style: { background_color: '#4b4d51' } }]), []).findings, []);
});

test('a paint-level blend mode with no lowering is reported, not waved through', () => {
  // The check must be able to FIRE. The reference design cannot prove it: its
  // only paint-level blend sits on a `visible: false` node, so a green run there
  // is silence, not evidence — exactly the "positive-only green" that makes a
  // checker worthless.
  const m = materials([
    { node_id: '0:1', name: 'Waveform', type: 'VECTOR', declared: {
      fill: [{ type: 'SOLID', opacity: 1, color_alpha: 1, rgb: '#ff0000', blend_mode: 'LIGHTEN' }] } },
  ]);
  const r = auditMaterials(m, envelope([{ node_id: '0:1', name: 'Waveform', style: {} }]), []);
  assert.equal(r.findings.length, 1);
  assert.equal(r.findings[0].property, 'fill.paint_blend');
  assert.equal(r.declaredCounts['fill.paint_blend'], 1);

  // Negative control: a paint with no mode of its own declares nothing.
  assert.deepEqual(auditMaterials(materials([
    { node_id: '0:1', name: 'Plain', type: 'FRAME', declared: {
      fill: [{ type: 'SOLID', opacity: 1, color_alpha: 1, rgb: '#ff0000', blend_mode: null }] } },
  ]), envelope([{ node_id: '0:1', name: 'Plain', style: {} }]), []).findings, []);
});

// ── the codegen stage ────────────────────────────────────────────────────────
//
// The stage the envelope tests above cannot see. Every case here pairs a real
// drop with the fixed output, because "green" from a checker that cannot go red
// is the thing this file exists to disprove.

test('a property the envelope carries but the JS never emits is a codegen drop', () => {
  // The measured bug: nodes declared a stroke, it reached style.border, and the
  // generated JS carried no stroke call at all. Envelope mode reads green here —
  // correctly, from where it stands — which is the whole reason --js exists.
  const m = materials([{ node_id: '0:1', name: 'Vector', type: 'VECTOR', declared: { stroke: [] } }]);
  const env = envelope([{ node_id: '0:1', name: 'Vector', style: { border: '1px solid #ffffff33' } }]);

  const dropped = auditCodegen(m, env, anchor('Vector_10013', '0:1'));
  assert.equal(dropped.findings.length, 1);
  assert.equal(dropped.findings[0].property, 'stroke');
  assert.equal(dropped.findings[0].stage, 'codegen');
  assert.equal(dropped.emittedCounts.stroke ?? 0, 0);

  // Negative control: the same envelope, lowered. Both call names satisfy it —
  // a VECTOR strokes via setSvgStroke, a frame via setBorder.
  for (const call of ["setSvgStroke('Vector_10013', '#ffffff33');",
                      "setBorder('Vector_10013', '#ffffff33', 1, 0);"]) {
    const kept = auditCodegen(m, env, `${anchor('Vector_10013', '0:1')}\n${call}`);
    assert.deepEqual(kept.findings, [], `${call} lowers the stroke`);
    assert.equal(kept.emittedCounts.stroke, 1);
  }
});

test('a node declaring two shadows and emitting one is a drop — presence is not enough', () => {
  // Measured on the reference frame: 19 knob bases carry
  // "0px 16px 6px 0px #0000001a, 0px 4px 4px 0px #00000040" in the envelope and
  // emit a single setBoxShadow, because codegen takes box_shadow.front() and
  // stops. The bridge call carries ONE layer, so the second shadow dies with
  // nothing said. Counting nodes instead of layers reads 49-of-49 and green.
  const m = materials([{ node_id: '0:1', name: 'knob base', type: 'ELLIPSE',
                         declared: { effects: ['DROP_SHADOW', 'DROP_SHADOW'] } }]);
  const env = envelope([{ node_id: '0:1', name: 'knob base', style: {
    box_shadow: '0px 16px 6px 0px #0000001a, 0px 4px 4px 0px #00000040' } }]);

  const bug = auditCodegen(m, env,
    `${anchor('knob_base23', '0:1')}\nsetBoxShadow('knob_base23', 0, 16, 6, 0, '#0000001a');`);
  assert.equal(bug.findings.length, 1);
  assert.equal(bug.findings[0].property, 'effects.shadow.layers');
  assert.equal(bug.declaredCounts['effects.shadow.layers'], 2);
  assert.equal(bug.emittedCounts['effects.shadow.layers'], 1);

  // Negative control: both layers emitted is silent.
  const fixed = auditCodegen(m, env,
    `${anchor('knob_base23', '0:1')}\n`
    + "setBoxShadow('knob_base23', 0, 16, 6, 0, '#0000001a');\n"
    + "setBoxShadow('knob_base23', 0, 4, 4, 0, '#00000040');");
  assert.deepEqual(fixed.findings, []);
  assert.equal(fixed.emittedCounts['effects.shadow.layers'], 2);

  // Negative control: a single-layer shadow, emitted once, is the common case
  // and must never be flagged — otherwise the check fires on 49 healthy nodes.
  assert.deepEqual(auditCodegen(
    materials([{ node_id: '0:1', name: 'Card', type: 'FRAME', declared: { effects: ['DROP_SHADOW'] } }]),
    envelope([{ node_id: '0:1', name: 'Card', style: { box_shadow: '0px 2px 4px 0px #00000040' } }]),
    `${anchor('Card1', '0:1')}\nsetBoxShadow('Card1', 0, 2, 4, 0, '#00000040');`).findings, []);
});

test('an rgba() shadow is one layer — the layer split must not count its commas', () => {
  // rgba(0, 0, 0, 0.25) carries three commas of its own. Splitting the list on
  // every comma turns one honest shadow into four demanded calls and reports a
  // drop on correct output — the cry-wolf failure that gets a checker ignored.
  const m = materials([{ node_id: '0:1', name: 'Card', type: 'FRAME', declared: { effects: ['DROP_SHADOW'] } }]);
  const env = envelope([{ node_id: '0:1', name: 'Card', style: {
    box_shadow: '0px 2px 4px 0px rgba(0, 0, 0, 0.25)' } }]);
  const r = auditCodegen(m, env,
    `${anchor('Card1', '0:1')}\nsetBoxShadow('Card1', 0, 2, 4, 0, 'rgba(0, 0, 0, 0.25)');`);
  assert.equal(r.declaredCounts['effects.shadow.layers'], 1, 'one shadow, not four');
  assert.deepEqual(r.findings, []);
});

test('a rounded VECTOR carries its radius in the path, so setSvgPath satisfies it', () => {
  // 78 of the reference frame's 106 rounded nodes are vectors: the arc IS the
  // rounding, so codegen emits no setCornerRadius and the corner still renders.
  // Demanding one would report 78 drops a screenshot disproves.
  const m = materials([{ node_id: '0:1', name: 'knob base', type: 'VECTOR',
                         declared: { corner_radius: [14, 14, 14, 14] } }]);
  const env = envelope([{ node_id: '0:1', name: 'knob base', style: { border_radius: 14 } }]);
  assert.deepEqual(auditCodegen(m, env,
    `${anchor('knob_base23', '0:1')}\nsetSvgPath('knob_base23', 'M0 14 A14 14 0 1 0 28 14 Z');`).findings,
    [], 'the path geometry is the rounding');

  // Positive control: a node with NEITHER call dropped the radius for real.
  const bare = auditCodegen(m, env, anchor('knob_base23', '0:1'));
  assert.equal(bare.findings.length, 1);
  assert.equal(bare.findings[0].property, 'corner_radius');
});

test('a declaring node with no anchor is reported, never silently skipped', () => {
  // The join is the tool's own weakest link: the web-compat lane emits
  // setAnchor(v3._id, ...) — a variable this cannot resolve — so fed that JS the
  // map comes back empty. Auditing whatever subset happens to join and printing
  // a clean bill is the exact bug this file exists to end, so an unjoinable node
  // is a finding and the coverage number is printed either way.
  const m = materials([
    { node_id: '0:1', name: 'Seen', type: 'FRAME', declared: { effects: ['DROP_SHADOW'] } },
    { node_id: '0:2', name: 'Unseen', type: 'FRAME', declared: { effects: ['DROP_SHADOW'] } },
  ]);
  const env = envelope([
    { node_id: '0:1', name: 'Seen', style: { box_shadow: '0px 2px 4px 0px #00000040' } },
    { node_id: '0:2', name: 'Unseen', style: { box_shadow: '0px 2px 4px 0px #00000040' } },
  ]);
  const r = auditCodegen(m, env,
    `${anchor('Seen1', '0:1')}\nsetBoxShadow('Seen1', 0, 2, 4, 0, '#00000040');`);
  assert.equal(r.coverage.declaring, 2);
  assert.equal(r.coverage.anchored, 1, 'coverage is reported, not assumed');
  const anchorFindings = r.findings.filter((f) => f.property === 'anchor');
  assert.equal(anchorFindings.length, 1);
  assert.equal(anchorFindings[0].node_id, '0:2');

  // And the unanchored node is NOT counted as a survival: a tool that scores an
  // un-audited node green is worse than one that never looked.
  assert.equal(r.emittedCounts['effects.shadow.layers'] ?? 0, 1);

  // Negative control: everything anchored means no anchor finding and 100%.
  const full = auditCodegen(m, env,
    `${anchor('Seen1', '0:1')}\nsetBoxShadow('Seen1', 0, 2, 4, 0, '#00000040');\n`
    + `${anchor('Unseen2', '0:2')}\nsetBoxShadow('Unseen2', 0, 2, 4, 0, '#00000040');`);
  assert.deepEqual(full.findings, []);
  assert.equal(full.coverage.anchored, 2);
});

test('one fig node lowering to several JS elements survives if ANY of them carries it', () => {
  // An instance can lower to more than one element. Keying the check to the
  // first-seen id alone would invent drops on nodes that render correctly.
  const m = materials([{ node_id: '0:1', name: 'Panel', type: 'INSTANCE', declared: { stroke: [] } }]);
  const env = envelope([{ node_id: '0:1', name: 'Panel', style: { border: '1px solid #ffffff33' } }]);
  const js = `${anchor('Panel_wrap', '0:1')}\n${anchor('Panel_inner', '0:1')}\n`
    + "setSvgStroke('Panel_inner', '#ffffff33');";
  assert.deepEqual(auditCodegen(m, env, js).findings, [], 'the second element carries the stroke');
});

test('codegen mode is scoped to its own hop and does not re-report a decode drop', () => {
  // The 40 gradient-stroke Ovals never reach the envelope, so they are envelope
  // mode's finding. Reporting them again here would double every decode drop and
  // bury the codegen ones underneath — the noise that killed fidelity_diff.
  const m = materials([{ node_id: '0:1', name: 'Oval', type: 'VECTOR',
                         declared: { stroke: [{ type: 'GRADIENT_LINEAR', opacity: 1, color_alpha: 1 }] } }]);
  const env = envelope([{ node_id: '0:1', name: 'Oval', style: {} }]);   // decoder dropped it
  assert.deepEqual(auditCodegen(m, env, anchor('Oval_1', '0:1')).findings, [],
    'nothing in the envelope to lower, so codegen dropped nothing');
  // ... and envelope mode still catches it, so the pair loses nothing.
  assert.equal(auditMaterials(m, env, []).findings.length, 1);
});

test('the anchor and call parsers read the shapes codegen actually emits', () => {
  // Asserted rather than assumed: the join is silent when it is wrong, and a
  // wrong join reports confident nonsense in either direction.
  const anchors = parseAnchors("setAnchor('knob_base23', 'figma-plugin:0:786/0:83/0:16');");
  assert.deepEqual(anchors.get('0:786/0:83/0:16'), ['knob_base23'],
    'instance-path node ids are the common case, not bare guids');

  // The web-compat lane's variable form is NOT resolvable, and must not be
  // silently read as if it were.
  assert.equal(parseAnchors("setAnchor(v3._id, 'figma-plugin:0:1');").size, 0);

  const calls = parseCalls("setBoxShadow('knob_base23', 0, 16, 6, 0, '#0000001a');\n"
    + "setBoxShadow('knob_base23', 0, 4, 4, 0, '#00000040');\n"
    + "setSvgFill('knob_base23', 'none');");
  assert.equal(calls.get('knob_base23').get('setBoxShadow'), 2, 'repeat calls are counted, not deduped');
  assert.equal(calls.get('knob_base23').get('setSvgFill'), 1);
});
