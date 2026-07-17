#!/usr/bin/env node
// Count what a .fig DECLARES against what the import EMITS.
//
// Every other checker we own has a blind spot it has to declare, and each one
// has been green through a real bug: layout_parity compares boxes, so it cannot
// see the ink inside them; thumb_parity cannot resolve a 2px arc; fidelity_diff
// was once blind to our own opt-out sentinel. This one has no equivalent excuse,
// because it compares COUNTS: 16 declared drop shadows against 1 emitted is a
// number, and it sat in a file all evening with nothing to say it.
//
// The invariant, per declared material property:
//
//     survived  ∨  diagnosed-as-degraded  ∨  listed-in-known-unsupported
//
// Anything else is a SILENT DROP — the design said something, the import
// dropped it, and nobody was told. That is the failure class this exists for,
// and it exits 1.
//
// What it CANNOT see (and this is not a small caveat): it proves a property
// SURVIVED, never that it renders CORRECTLY. A gradient emitted with the wrong
// axis passes. A shadow emitted at the wrong offset passes. A blend mode mapped
// to the wrong CSS name passes. Presence is the floor, not fidelity — for
// fidelity, look at pixels (thumb_parity, montage) or boxes (layout_parity).
//
// Usage:
//   node tools/import-design/material_audit.mjs --dir <decode-out-dir>
//   node tools/import-design/material_audit.mjs --fig <file.fig> --frame <name|guid>
//   ... [--json report.json]
//
// --dir reads the materials.json + scene.pulp.json that `fig_decode.mjs emit`
// already wrote. --fig decodes in-process, so no temp dir is needed.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { unpackFig } from './fig/container.mjs';
import { ByteReader, readSchema, makeDecoder } from './fig/kiwi.mjs';
import { buildScene, findFrame, materializeFrame } from './fig/scene.mjs';

// Properties we knowingly do not lower AND cannot diagnose per-node. This is the
// weakest of the invariant's three arms and is meant to stay near-empty: an
// entry silences a whole property class file-wide, for every design, forever,
// which is indistinguishable from the bug it is supposed to excuse.
//
// LAYER_BLUR / BACKGROUND_BLUR were the obvious candidates and deliberately are
// NOT here. They have a node to point at, so they now raise `effect-unsupported`
// at decode time — which the importer's user actually SEES, and which this
// audit reads through the "∨ diagnosed" arm. Listing them here as well would
// mean the audit stayed green if that diagnostic ever regressed: a static excuse
// cannot notice that the dynamic one stopped firing.
//
// Reach for this only when there is genuinely no node to attach a diagnostic to.
const KNOWN_UNSUPPORTED = {};

function fail(msg, code = 2) {
  process.stderr.write(`material_audit: ${msg}\n`);
  process.exit(code);
}

function parseArgs(argv) {
  const o = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--dir') o.dir = argv[++i];
    else if (a === '--fig') o.fig = argv[++i];
    else if (a === '--frame') o.frame = argv[++i];
    else if (a === '--page') o.page = argv[++i];
    else if (a === '--json') o.json = argv[++i];
    else if (a === '--quiet') o.quiet = true;
    else o._.push(a);
  }
  return o;
}

function decodeInProcess(figPath, frameSel, page) {
  const container = unpackFig(readFileSync(figPath));
  const schema = readSchema(new ByteReader(container.schemaBytes));
  const dec = makeDecoder(schema);
  const message = dec.decodeMessage(new ByteReader(container.messageBytes), dec.rootIndex);
  const scene = buildScene(message);
  const frame = findFrame(scene, frameSel, page);
  if (!frame) fail(`no frame matched ${JSON.stringify(frameSel)}`);
  const { envelope, materials, diagnostics } = materializeFrame(scene, frame, {
    images: container.images,
    fileKey: 'local',
    parserVersion: '0.1.0-fig',
    compatSchemaVersion: '1',
    exportedAt: '1970-01-01T00:00:00Z',
  });
  return { envelope, materials, diagnostics };
}

/** Flatten the envelope tree to node_id → emitted node. */
function indexEnvelope(root) {
  const byId = new Map();
  (function walk(n) {
    if (n.node_id) byId.set(n.node_id, n);
    for (const c of n.children || []) walk(c);
  })(root);
  return byId;
}

/**
 * node_id → the diagnostic codes raised against it.
 *
 * The field is `code`. Reading `kind` here (as this first did) silently indexed
 * every diagnostic as "unknown", which quietly disabled the "∨ diagnosed" half
 * of the invariant and turned honest degradations into false silent drops. An
 * auditor whose own join is wrong is the fidelity_diff bug again, so the shape
 * of a diagnostic is asserted in the tests rather than assumed here.
 */
function indexDiagnostics(diags) {
  const byId = new Map();
  for (const d of diags || []) {
    const id = d.node_id || d.nodeId;
    if (!id) continue;
    if (!byId.has(id)) byId.set(id, []);
    byId.get(id).push(d.code || 'unknown');
  }
  return byId;
}

// ── the checks ───────────────────────────────────────────────────────────────
//
// Each returns a finding or null. A check answers exactly one question: did this
// declared property reach the envelope? It never judges whether the emitted
// value is RIGHT — see the caveat at the top.

function checkStroke(decl, emitted, diagKinds) {
  const border = emitted && emitted.style && emitted.style.border;
  const solid = decl.stroke.find((p) => p.type === 'SOLID');
  if (!solid) {
    // A non-solid stroke was very nearly waved through here as "a separate gap".
    // It is not: the file's 40 `Oval` GRADIENT_LINEAR strokes (white -> transparent,
    // the lit rim on every knob) reach neither style.border nor the path, and carry
    // no diagnostic. Skipping them would have made this tool blind to the single
    // biggest material drop in the design — the exact shape of blind spot it was
    // built to end.
    if (border) return null;
    if (diagKinds.includes('vector-simplified') || diagKinds.includes('gradient-approximated')) return null;
    return {
      property: `stroke.${decl.stroke[0].type}`,
      declared: decl.stroke.map((p) => p.type).join('+'),
      emitted: 'none',
    };
  }
  if (!border) {
    if (diagKinds.includes('vector-simplified')) return null;   // dropped out loud
    return { property: 'stroke', declared: `${decl.stroke.length} paint(s)`, emitted: 'none' };
  }
  // Figma multiplies paint.opacity by the colour's own alpha. A border emitted
  // at full strength for a paint declared at 24% is a drop of the opacity, even
  // though the stroke itself "survived" — so the count has to look past presence
  // to the one channel that silently disappears.
  const want = solid.opacity * solid.color_alpha;
  const m = /#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?\b/.exec(border);
  const got = m ? (m[1] === undefined ? 1 : parseInt(m[1], 16) / 255) : null;
  if (got === null) return null;
  if (Math.abs(got - want) > 1.5 / 255) {
    return {
      property: 'stroke.opacity',
      declared: `${want.toFixed(3)} (paint ${solid.opacity} x alpha ${solid.color_alpha})`,
      emitted: got.toFixed(3),
    };
  }
  return null;
}

function checkFill(decl, emitted, diagKinds) {
  // A stack of visible SOLID paints must composite. We cannot check the
  // composite ARITHMETIC from here — that is correctness, and out of remit (see
  // the caveat at the top). What we CAN catch is the drop that actually bit: the
  // emitted colour being verbatim the BOTTOM paint while paints sit above it,
  // which is exactly what `.find(SOLID)` off a stack produces.
  //
  // Compare RGB, not alpha. The first cut of this compared only the alpha
  // channel — and #aeafb1 (the correct composite) and #4b4d51 (the bug) are both
  // opaque, so it fired on every stack including the fixed ones. An auditor that
  // reports a fixed bug as broken gets switched off, and then it protects nothing.
  const solids = decl.fill.filter((p) => p.type === 'SOLID');
  if (solids.length < 2) return null;
  if (diagKinds.includes('gradient-approximated')) return null;
  const style = (emitted && emitted.style) || {};
  const got = style.background_color || (emitted && emitted.fill);
  if (!got || !/^#[0-9a-fA-F]{6}/.test(got)) return null;

  const b = solids[0];
  if (!b.rgb) return null;                       // sidecar predates rgb; nothing to compare
  const bottomHex = b.rgb.toLowerCase();
  if (got.slice(0, 7).toLowerCase() !== bottomHex) return null;   // composited: not the bare bottom

  // The emitted colour IS the bottom paint. That is only correct if everything
  // above it is invisible or fully transparent — otherwise the stack was dropped.
  const above = solids.slice(1);
  const anythingAbove = above.some((p) => p.opacity * p.color_alpha > 0.004);
  if (!anythingAbove) return null;
  return {
    property: 'fill.stack',
    declared: `${solids.length} SOLID paints; ${above.length} above the base`,
    emitted: `${got} — the bottom paint alone`,
  };
}

function checkPaintBlend(decl) {
  // A PAINT-level blendMode. Figma composites an individual paint with its own
  // mode; we have no per-paint primitive, and the node-level mix_blend_mode is
  // NOT a substitute (lifting one there is only sound when the node has exactly
  // one visible fill and nothing else — plan row 6.8).
  //
  // This was triple-silent: unread by the decoder, unnamed by any diagnostic,
  // unrecorded by the sidecar. The last of those is the one that mattered — a
  // property the audit cannot SEE is a hole in the audit's own thesis, and it
  // would have reported "everything survived" over a design that dropped this.
  //
  // Only visible paints count; the walk never reaches an invisible node, so
  // node-visibility is already handled upstream. The reference file's only
  // paint-level blend sits on `Waveform B`, which is visible:false — so it
  // renders in neither Figma nor here, and this correctly stays quiet.
  //
  // There is deliberately no "∨ diagnosed" arm here: no diagnostic for this
  // exists. An earlier cut guarded on a `paint-blend-unsupported` code that is
  // emitted nowhere — a guard for a phantom, which reads to the next person as
  // proof the decoder announces this when it does not. When such a diagnostic
  // lands, wire the arm then; until it does, this finding IS the only notice.
  const blended = decl.fill.filter((p) => p.blend_mode);
  if (!blended.length) return null;
  return {
    property: 'fill.paint_blend',
    declared: blended.map((p) => p.blend_mode).join('+'),
    emitted: 'none — no per-paint blend primitive; the paint composites NORMAL',
  };
}

function checkEffects(decl, emitted, diagKinds) {
  const out = [];
  const shadowKinds = new Set(['DROP_SHADOW', 'INNER_SHADOW']);
  const emittedShadow = emitted && emitted.style && emitted.style.box_shadow;
  const declaredShadows = decl.effects.filter((t) => shadowKinds.has(t));
  if (declaredShadows.length && !emittedShadow) {
    out.push({ property: 'effects.shadow', declared: declaredShadows.join('+'), emitted: 'none' });
  }
  for (const t of decl.effects) {
    if (shadowKinds.has(t)) continue;
    if (KNOWN_UNSUPPORTED[t]) continue;              // declared, and said so
    if (diagKinds.includes('effect-unsupported')) continue;
    out.push({ property: `effects.${t}`, declared: t, emitted: 'none' });
  }
  return out;
}

function checkCornerRadius(decl, emitted, diagKinds) {
  const r = decl.corner_radius;
  if (!r.some((v) => v > 0)) return null;
  const emittedR = emitted && emitted.style && emitted.style.border_radius;
  const uniform = r.every((v) => v === r[0]);
  if (emittedR === undefined || emittedR === null) {
    if (diagKinds.includes('corner-radius-simplified')) return null;   // said aloud
    return {
      property: 'corner_radius',
      declared: `[${r.join(', ')}]${uniform ? '' : ' (non-uniform)'}`,
      emitted: 'none',
    };
  }
  // A non-uniform radius collapsed to one number is a partial drop: three of the
  // four corners are now wrong, and nothing said so.
  if (!uniform) {
    return {
      property: 'corner_radius.non_uniform',
      declared: `[${r.join(', ')}]`,
      emitted: String(emittedR),
    };
  }
  return null;
}

function checkBlendMode(decl, emitted, diagKinds) {
  const got = emitted && emitted.style && emitted.style.mix_blend_mode;
  if (got) return null;
  if (diagKinds.includes('blend-unsupported')) return null;   // refused out loud
  return { property: 'blend_mode', declared: decl.blend_mode, emitted: 'none' };
}

export function auditMaterials(materials, envelope, diagnostics) {
  const byId = indexEnvelope(envelope.root || envelope);
  const diagById = indexDiagnostics(diagnostics);
  const findings = [];
  const declaredCounts = {};
  const emittedCounts = {};
  // Diagnosed-but-dropped is its OWN column, never folded into "emitted". A
  // summary that counts an honest degradation as a survival reads green while
  // the design renders wrong — which is how the whole class got here.
  const diagnosedCounts = {};

  const bump = (bag, k) => { bag[k] = (bag[k] || 0) + 1; };

  for (const entry of materials.nodes) {
    const emitted = byId.get(entry.node_id);
    const diagKinds = diagById.get(entry.node_id) || [];
    const d = entry.declared;
    const found = [];

    // Each property is tallied the SAME way — declared, then emitted-or-diagnosed
    // — so the summary can never disagree with the findings. It did: the
    // diagnosed column was wired for strokes alone, so a corner radius the
    // decoder had announced still printed as unaccounted. A summary that
    // contradicts its own findings is worse than no summary, because the number
    // is what gets read.
    const tally = (prop, survived, spoken) => {
      bump(declaredCounts, prop);
      if (survived) bump(emittedCounts, prop);
      else if (spoken) bump(diagnosedCounts, prop);
    };
    const style = (emitted && emitted.style) || {};

    if (d.fill) {
      const solids = d.fill.filter((p) => p.type === 'SOLID');
      if (solids.length > 1) {
        const f = checkFill(d, emitted, diagKinds);
        tally('fill.stack', !f, false);
        if (f) found.push(f);
      }
      if (d.fill.some((p) => p.blend_mode)) {
        const f = checkPaintBlend(d);
        tally('fill.paint_blend', !f, !!f && false);
        if (f) found.push(f);
      }
    }
    if (d.stroke) {
      tally('stroke', !!style.border,
            diagKinds.includes('vector-simplified') || diagKinds.includes('gradient-approximated'));
      const f = checkStroke(d, emitted, diagKinds);
      if (f) found.push(f);
    }
    if (d.effects) {
      for (const t of d.effects) {
        const isShadow = t === 'DROP_SHADOW' || t === 'INNER_SHADOW';
        tally(`effects.${t}`,
              isShadow ? !!style.box_shadow : false,
              !isShadow && diagKinds.includes('effect-unsupported'));
      }
      found.push(...checkEffects(d, emitted, diagKinds));
    }
    if (d.corner_radius && d.corner_radius.some((v) => v > 0)) {
      tally('corner_radius', style.border_radius != null,
            diagKinds.includes('corner-radius-simplified'));
      const f = checkCornerRadius(d, emitted, diagKinds);
      if (f) found.push(f);
    }
    if (d.blend_mode) {
      tally('blend_mode', !!style.mix_blend_mode, diagKinds.includes('blend-unsupported'));
      const f = checkBlendMode(d, emitted, diagKinds);
      if (f) found.push(f);
    }

    for (const f of found) {
      findings.push({ node_id: entry.node_id, name: entry.name, type: entry.type, ...f });
    }
  }

  return { findings, declaredCounts, emittedCounts, diagnosedCounts };
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  let envelope, materials, diagnostics;
  if (opts.dir) {
    envelope = JSON.parse(readFileSync(join(opts.dir, 'scene.pulp.json'), 'utf8'));
    materials = JSON.parse(readFileSync(join(opts.dir, 'materials.json'), 'utf8'));
    diagnostics = envelope.diagnostics || [];
  } else if (opts.fig) {
    if (!opts.frame) fail('--fig needs --frame');
    ({ envelope, materials, diagnostics } = decodeInProcess(opts.fig, opts.frame, opts.page));
  } else {
    fail('need --dir <decode-out-dir> or --fig <file.fig> --frame <name>');
  }

  const { findings, declaredCounts, emittedCounts, diagnosedCounts } = auditMaterials(materials, envelope, diagnostics);

  if (!opts.quiet) {
    const keys = Object.keys(declaredCounts).sort();
    process.stdout.write(`Material audit — ${materials.nodes.length} node(s) declaring material\n\n`);
    process.stdout.write(`  ${'PROPERTY'.padEnd(28)} ${'DECLARED'.padStart(8)} ${'EMITTED'.padStart(8)} ${'DIAGNOSED'.padStart(9)}\n`);
    for (const k of keys) {
      const dc = declaredCounts[k];
      const ec = emittedCounts[k] || 0;
      const gc = diagnosedCounts[k] || 0;
      const unaccounted = dc - ec - gc;
      const flag = unaccounted > 0 ? `  <-- ${unaccounted} unaccounted` : '';
      process.stdout.write(`  ${k.padEnd(28)} ${String(dc).padStart(8)} ${String(ec).padStart(8)} ${String(gc).padStart(9)}${flag}\n`);
    }
    process.stdout.write('\n');
    if (findings.length) {
      process.stdout.write(`SILENT DROPS — ${findings.length} finding(s):\n`);
      for (const f of findings) {
        process.stdout.write(`  ${f.name} [${f.node_id}] ${f.property}: `
          + `declared ${f.declared}, emitted ${f.emitted}\n`);
      }
    } else {
      process.stdout.write('No silent drops: every declared property survived, '
        + 'was diagnosed as degraded, or is known-unsupported.\n');
    }
  }

  if (opts.json) {
    writeFileSync(opts.json, JSON.stringify({
      declared: declaredCounts, emitted: emittedCounts, diagnosed: diagnosedCounts, findings,
      known_unsupported: KNOWN_UNSUPPORTED,
    }, null, 2));
  }
  process.exit(findings.length ? 1 : 0);
}

if (import.meta.url === `file://${process.argv[1]}`) main();
