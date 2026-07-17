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
import { auditMaterials } from './material_audit.mjs';

const materials = (nodes) => ({ nodes });
const envelope = (children) => ({ root: { node_id: 'root', name: 'Root', children } });

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
