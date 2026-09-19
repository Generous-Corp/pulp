// SPDX-License-Identifier: MIT
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { buildTextDiagnostics, compareTextDiagnostics } from './text_diagnostics.mjs';

function inputs() {
  return {
    snapshot: { strings: ['A😀', 'normal'], computedStyleNames: ['line-height'], documents: [{
      layout: { nodeIndex: [3, 4], text: [0, 0], styles: [[1], [1]] },
      textBoxes: { layoutIndex: [0, 1], bounds: [[10, 20, 30, 15], [40, 20, 30, 15]],
        start: [0, 0], length: [3, 3] },
    }] },
    fonts: { schema: 'pulp-browser-platform-fonts-v1', summary: { truncated: false, text_runs: 2 },
      runs: [0, 1].map(index => ({ layout_index: index, node_index: index + 3, owner_node_index: 2,
        requested: { font_family: 'Missing, sans-serif' },
        resolved: [{ family_name: 'Helvetica', post_script_name: 'Helvetica', glyph_count: 1 },
          { family_name: 'Apple Color Emoji', post_script_name: 'AppleColorEmoji', glyph_count: 1 }],
      })) },
  };
}
function report() { const { snapshot, fonts } = inputs(); return buildTextDiagnostics(snapshot, fonts, 'capture-fixture'); }

test('preserves font participation, UTF-16 fragments and normal without inventing glyph metrics', () => {
  const result = report();
  assert.equal(result.runs.length, 2);
  const run = result.runs[0];
  assert.equal(run.metrics.resolved_faces.value[1].glyph_count, 1);
  assert.equal(run.metrics.fragment_boxes.value[0].length, 3);
  assert.equal(run.metrics.line_height.value, 'normal');
  for (const metric of ['glyph_bounds', 'glyph_advances', 'baselines', 'ascent', 'descent', 'line_boxes'])
    assert.equal(run.metrics[metric].status, 'unavailable');
  assert.equal(run.fallback.status, 'unavailable');
  assert.match(run.font_resolution_scope, /owner-element/);
  assert.equal(result.faces_by_glyph_count, undefined);
});

test('missing fonts, invalid fragments, missing styles and truncation remain visible', () => {
  const { snapshot, fonts } = inputs();
  fonts.runs[0].resolved = null;
  fonts.summary.truncated = true;
  snapshot.documents[0].textBoxes.length[0] = 4;
  snapshot.computedStyleNames = [];
  const result = buildTextDiagnostics(snapshot, fonts, 'capture-fixture');
  assert.equal(result.coverage.truncated, true);
  for (const metric of ['resolved_faces', 'fragment_boxes', 'line_height'])
    assert.equal(result.runs[0].metrics[metric].status, 'unavailable');
});

test('refuses duplicate or stale layout joins', () => {
  const { snapshot, fonts } = inputs();
  fonts.runs.push(fonts.runs[0]);
  assert.throws(() => buildTextDiagnostics(snapshot, fonts, 'basis'), /uniquely/);
  fonts.runs.pop();
  fonts.runs[0].node_index = 99;
  assert.throws(() => buildTextDiagnostics(snapshot, fonts, 'basis'), /uniquely/);
});

test('self-comparison matches observed metrics while missing instrumentation stays unavailable', () => {
  const result = compareTextDiagnostics(report(), report());
  assert.equal(result.findings.filter(row => row.status === 'match').length, 6);
  assert.equal(result.findings.filter(row => row.status === 'unavailable').length, 12);
});

test('controlled wrong face and shifted fragment produce attributable mismatch evidence', () => {
  const reference = report();
  const candidate = structuredClone(reference);
  candidate.runs[0].metrics.resolved_faces.value[0].post_script_name = 'WrongFace';
  candidate.runs[1].metrics.fragment_boxes.value[0].bounds[1] += 2;
  const mismatches = compareTextDiagnostics(reference, candidate).findings.filter(row => row.status === 'mismatch');
  assert.deepEqual(mismatches.map(row => [row.run, row.metric]), [
    ['document:0/layout:0', 'resolved_faces'], ['document:0/layout:1', 'fragment_boxes'],
  ]);
  assert.equal(mismatches[1].candidate.value[0].bounds[1] - mismatches[1].reference.value[0].bounds[1], 2);
});

test('geometry tolerance does not hide changed UTF-16 ranges or font counts', () => {
  const reference = report();
  const candidate = structuredClone(reference);
  candidate.runs[0].metrics.fragment_boxes.value[0].bounds[0] += 0.125;
  assert.equal(compareTextDiagnostics(reference, candidate).findings[1].status, 'match');
  candidate.runs[0].metrics.fragment_boxes.value[0].start += 1;
  candidate.runs[0].metrics.resolved_faces.value[0].glyph_count += 1;
  const result = compareTextDiagnostics(reference, candidate, 2);
  assert.equal(result.findings[0].status, 'mismatch');
  assert.equal(result.findings[1].status, 'mismatch');
});

test('different capture coordinates, missing runs and changed text cannot pass as parity', () => {
  const reference = report();
  const candidate = structuredClone(reference);
  candidate.capture_basis = 'other';
  assert.throws(() => compareTextDiagnostics(reference, candidate), /Incompatible/);
  candidate.capture_basis = reference.capture_basis;
  candidate.coordinate_space = 'native-local';
  assert.throws(() => compareTextDiagnostics(reference, candidate), /Incompatible/);
  candidate.coordinate_space = reference.coordinate_space;
  candidate.runs.pop();
  candidate.runs[0].text = 'different';
  assert.ok(compareTextDiagnostics(reference, candidate).findings.every(row => row.status === 'incomparable'));
});

test('offline CLI emits deterministic content-bound evidence and rejects missing input', async () => {
  const directory = await mkdtemp(path.join(tmpdir(), 'pulp-text-diagnostics-'));
  try {
    const { snapshot, fonts } = inputs();
    const files = [path.join(directory, 'snapshot.json'), path.join(directory, 'fonts.json')];
    await Promise.all([writeFile(files[0], JSON.stringify(snapshot)), writeFile(files[1], JSON.stringify(fonts))]);
    const script = new URL('./text_diagnostics.mjs', import.meta.url);
    const invoke = () => spawnSync(process.execPath, [script.pathname, ...files], { encoding: 'utf8' });
    const first = invoke();
    assert.equal(first.status, 0, first.stderr);
    assert.equal(first.stdout, invoke().stdout);
    const result = JSON.parse(first.stdout);
    assert.match(result.capture_basis, /^[a-f0-9]{64}$/);
    fonts.summary.truncated = true;
    await writeFile(files[1], JSON.stringify(fonts));
    assert.notEqual(JSON.parse(invoke().stdout).capture_basis, result.capture_basis);
    const bad = spawnSync(process.execPath, [script.pathname], { encoding: 'utf8' });
    assert.notEqual(bad.status, 0);
    assert.match(bad.stderr, /Usage:/);
  } finally { await rm(directory, { recursive: true, force: true }); }
});
