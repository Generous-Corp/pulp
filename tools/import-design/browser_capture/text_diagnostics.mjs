// SPDX-License-Identifier: MIT
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';

const SCHEMA = 'pulp-text-diagnostics-v1';
const unavailable = reason => ({ status: 'unavailable', reason });
const observed = (value, source) => ({ status: 'observed', source, value });
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const METRICS = ['resolved_faces', 'fragment_boxes', 'line_height', 'glyph_bounds',
  'glyph_advances', 'baselines', 'ascent', 'descent', 'line_boxes'];

// Retained CDP evidence is intentionally independent of runtime text bindings:
// a run rejected by native materialization can still explain a mismatch.
export function buildTextDiagnostics(snapshot, fonts, captureBasis) {
  if (!Array.isArray(snapshot?.documents) || !Array.isArray(snapshot?.strings) ||
      fonts?.schema !== 'pulp-browser-platform-fonts-v1' || !Array.isArray(fonts.runs) ||
      typeof captureBasis !== 'string' || !captureBasis) {
    throw new Error('Expected a DOM snapshot, platform-font report, and capture basis');
  }
  const document = snapshot.documents[0];
  if (!document) throw new Error('Missing primary document');
  const layout = document.layout ?? {};
  const boxes = document.textBoxes ?? {};
  const strings = snapshot.strings;
  const names = snapshot.computedStyleNames ?? [];
  const readStyle = (index, name) => {
    const stringIndex = layout.styles?.[index]?.[names.indexOf(name)];
    return Number.isInteger(stringIndex) ? strings[stringIndex] ?? null : null;
  };
  const boxesByLayout = new Map();
  for (const [index, layoutIndex] of (boxes.layoutIndex ?? []).entries()) {
    if (!boxesByLayout.has(layoutIndex)) boxesByLayout.set(layoutIndex, []);
    boxesByLayout.get(layoutIndex).push(index);
  }
  const ids = new Set();
  const runs = fonts.runs.map(run => {
    const index = run.layout_index;
    const id = `document:0/layout:${index}`;
    if (!Number.isInteger(index) || index < 0 || ids.has(id) ||
        layout.nodeIndex?.[index] !== run.node_index) {
      throw new Error('Font run does not uniquely identify snapshot layout');
    }
    ids.add(id);
    const text = strings[layout.text?.[index]];
    if (typeof text !== 'string') throw new Error('Missing snapshot text');
    const fragments = (boxesByLayout.get(index) ?? []).map(i => ({
      bounds: boxes.bounds?.[i], start: boxes.start?.[i], length: boxes.length?.[i],
    }));
    const validFragments = fragments.length > 0 && fragments.every(fragment =>
      Array.isArray(fragment.bounds) && fragment.bounds.length === 4 &&
      fragment.bounds.every(Number.isFinite) && fragment.bounds[2] >= 0 &&
      fragment.bounds[3] >= 0 && Number.isInteger(fragment.start) &&
      Number.isInteger(fragment.length) && fragment.start >= 0 &&
      fragment.length > 0 && fragment.start + fragment.length <= text.length);
    const lineHeight = readStyle(index, 'line-height');
    return {
      id, text, owner_node_index: run.owner_node_index,
      requested_font: { ...run.requested },
      font_resolution_scope: 'owner-element; counts are not per-run or per-character',
      fallback: unavailable('Face census exposes participation, not CSS-family alias resolution or cluster attribution'),
      metrics: {
        resolved_faces: Array.isArray(run.resolved) && run.resolved.length > 0
          ? observed(run.resolved, 'CSS.getPlatformFontsForNode')
          : unavailable('No resolved face evidence'),
        fragment_boxes: validFragments
          ? observed(fragments, 'DOMSnapshot.textBoxes; UTF-16 ranges; not ink or CSS line boxes')
          : unavailable('Missing or invalid text-fragment geometry'),
        line_height: typeof lineHeight === 'string' && lineHeight.length > 0
          ? observed(lineHeight, 'computed CSS line-height; normal is not a measured pixel height')
          : unavailable('Computed line-height was not retained'),
        glyph_bounds: unavailable('CDP snapshot does not expose per-glyph ink bounds'),
        glyph_advances: unavailable('Fragment width is not a glyph advance'),
        baselines: unavailable('CDP snapshot does not expose laid-out baselines'),
        ascent: unavailable('CDP snapshot does not expose face ascent'),
        descent: unavailable('CDP snapshot does not expose face descent'),
        line_boxes: unavailable('Text fragments do not establish CSS inline line boxes'),
      },
    };
  });
  return {
    schema: SCHEMA, producer: 'chromium-retained-capture', capture_basis: captureBasis,
    coordinate_space: 'primary-document CSS pixels; snapshot coordinates',
    coverage: { ...fonts.summary, scope: 'primary document only' }, runs,
  };
}

// Native adapters must join the reference run IDs and capture basis explicitly.
// Unavailable evidence never becomes a match, including on a self-comparison.
export function compareTextDiagnostics(reference, candidate, tolerance = 0.25) {
  if (!Number.isFinite(tolerance) || tolerance < 0) throw new Error('Invalid tolerance');
  for (const report of [reference, candidate]) {
    if (report?.schema !== SCHEMA || !report.capture_basis || !Array.isArray(report.runs) ||
        new Set(report.runs.map(run => run.id)).size !== report.runs.length) {
      throw new Error('Invalid text diagnostic report');
    }
  }
  if (reference.capture_basis !== candidate.capture_basis ||
      reference.coordinate_space !== candidate.coordinate_space) {
    throw new Error('Incompatible capture basis or coordinate space');
  }
  const equal = (a, b, key = '') => {
    if (typeof a === 'number' && typeof b === 'number')
      return Number.isFinite(a) && Number.isFinite(b) &&
        (['start', 'length', 'glyph_count'].includes(key) ? a === b : Math.abs(a - b) <= tolerance);
    if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') return a === b;
    const keys = Object.keys(a);
    return Array.isArray(a) === Array.isArray(b) && keys.length === Object.keys(b).length &&
      keys.every(key => Object.hasOwn(b, key) && equal(a[key], b[key], key));
  };
  const rows = new Map(candidate.runs.map(run => [run.id, run]));
  const findings = [];
  for (const run of reference.runs) {
    const other = rows.get(run.id);
    rows.delete(run.id);
    if (!other || other.text !== run.text) {
      findings.push({ run: run.id, metric: 'run', status: 'incomparable', reason: 'Missing run or different text' });
      continue;
    }
    for (const metric of METRICS) {
      const a = run.metrics?.[metric];
      const b = other.metrics?.[metric];
      const measurable = a?.status === 'observed' && b?.status === 'observed' &&
        Object.hasOwn(a, 'value') && Object.hasOwn(b, 'value');
      // Counts, ranges and identities compare exactly; only geometry uses px tolerance.
      const geometry = ['fragment_boxes', 'glyph_bounds', 'glyph_advances',
        'baselines', 'ascent', 'descent', 'line_boxes'].includes(metric);
      const matches = measurable && (geometry ? equal(a.value, b.value)
        : JSON.stringify(a.value) === JSON.stringify(b.value));
      findings.push({ run: run.id, metric,
        status: measurable ? (matches ? 'match' : 'mismatch') : 'unavailable',
        reference: a ?? unavailable('Metric absent'),
        candidate: b ?? unavailable('Metric absent'),
      });
    }
  }
  for (const id of rows.keys()) findings.push({ run: id, metric: 'run', status: 'incomparable', reason: 'Extra candidate run' });
  return { schema: 'pulp-text-diagnostic-comparison-v1', tolerance_css_px: tolerance,
    coverage: { reference: reference.coverage, candidate: candidate.coverage }, findings };
}

async function main(args) {
  if (args.length !== 2) throw new Error('Usage: node text_diagnostics.mjs <dom-snapshot.json> <platform-fonts.json>');
  const [snapshot, fonts] = await Promise.all(args.map(file => readFile(file)));
  const basis = hash(Buffer.concat([Buffer.from(hash(snapshot)), Buffer.from(hash(fonts))]));
  process.stdout.write(`${JSON.stringify(buildTextDiagnostics(JSON.parse(snapshot), JSON.parse(fonts), basis), null, 2)}\n`);
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main(process.argv.slice(2)).catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
}
