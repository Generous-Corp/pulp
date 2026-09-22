#!/usr/bin/env node
// SPDX-License-Identifier: MIT
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
import { buildTextDiagnostics } from './text_diagnostics.mjs';
import { compareNativeSelection } from './native_text_diagnostics.mjs';

const execute = promisify(execFile);
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const write = (file, value) => writeFile(file, JSON.stringify(value, null, 2) + '\n');
const [observerArg, browserArg, outputArg] = process.argv.slice(2);
if (!observerArg || !browserArg || !outputArg || process.argv.length !== 5)
  throw new Error('Usage: node text_diagnostics_native_proof.mjs <observer> <chromium> <output-directory>');
const observer = path.resolve(observerArg), browser = path.resolve(browserArg), output = path.resolve(outputArg);
await mkdir(output, { recursive: true });
const source = path.join(output, 'source');
await mkdir(source, { recursive: true });
const text = 'AVATAR office width';
const html = `<!doctype html><meta charset="utf-8"><style>html,body{margin:0;width:320px;height:120px;background:white}#sample{position:absolute;left:24px;top:24px;width:260px;height:22px;font:16px/22px Arial;white-space:nowrap;color:black}</style><div id="sample">${text}</div><script>
// This measures the whole string in a separate Canvas context. It does not
// locate the DOM baseline or expose per-glyph ink.
globalThis.__pulpCaptureReady=document.fonts.ready.then(()=>{
 const element=document.getElementById('sample'), style=getComputedStyle(element);
 const context=document.createElement('canvas').getContext('2d');
 context.font=style.font; context.textBaseline='alphabetic'; context.textAlign='left';
 const metrics=context.measureText(element.textContent);
 const values={}; for(const key of ['width','actualBoundingBoxLeft','actualBoundingBoxRight','actualBoundingBoxAscent','actualBoundingBoxDescent','fontBoundingBoxAscent','fontBoundingBoxDescent']) values[key]=Number.isFinite(metrics[key])?metrics[key]:null;
 element.setAttribute('data-text-metric-probe',JSON.stringify({source:'independent Canvas2D whole-string measureText; not DOM layout',font:context.font,values}));
});</script>`;
await writeFile(path.join(source, 'index.html'), html);
const capture = path.join(output, 'capture');
await execute(process.execPath, [fileURLToPath(new URL('./capture.mjs', import.meta.url)),
  'capture', '--browser', browser, '--input', path.join(source, 'index.html'), '--root', source,
  '--output', capture, '--width', '320', '--initial-height', '120', '--dpr', '2', '--timeout-ms', '20000'],
  { maxBuffer: 4 * 1024 * 1024 });
const snapshotBytes = await readFile(path.join(capture, 'dom-snapshot.json'));
const fontBytes = await readFile(path.join(capture, 'platform-fonts.json'));
const snapshot = JSON.parse(snapshotBytes);
const basis = hash(hash(snapshotBytes) + hash(fontBytes));
const reference = buildTextDiagnostics(snapshot, JSON.parse(fontBytes), basis);
const matching = reference.runs.filter(run => run.text === text);
assert.equal(matching.length, 1);
const strings = snapshot.strings;
const owner = snapshot.documents[0].nodes.attributes[matching[0].owner_node_index];
const attributes = Object.fromEntries(Array.from({length: owner.length / 2}, (_, i) => [strings[owner[2*i]], strings[owner[2*i+1]]]));
assert.equal(attributes.id, 'sample');
const browserProbe = JSON.parse(attributes['data-text-metric-probe']);
assert.ok(browserProbe.values.width > 0);
await write(path.join(output, 'browser-metric-probe.json'), browserProbe);
await write(path.join(output, 'browser-text.json'), reference);
const observerHash = hash(await readFile(observer));
const artifactHashes = {};
const results = {};
for (const [name, family, left] of [['reference', 'Arial', 24], ['font-control', 'Courier New', 24], ['geometry-control', 'Arial', 36]]) {
  const input = path.join(output, `${name}.ir.json`);
  const ir = { version: 1, source: 'jsx', root: { type: 'frame', name: 'root',
    style: { width: 320, height: 120, backgroundColor: '#ffffff' }, children: [{
      type: 'text', name: 'sample', stable_anchor_id: 'sample', content: text,
      style: { position: 'absolute', left, top: 24, width: 260, height: 22,
        fontFamily: family, fontSize: 16, fontWeight: 400, lineHeight: 22,
        whiteSpace: 'nowrap', color: '#000000', verticalAlign: 'top' },
    }] } };
  await write(input, ir);
  const nativeFile = path.join(output, `${name}.native.json`);
  const { stderr } = await execute(observer, ['--input', input, '--render', path.join(output, `${name}.png`),
    '--layout', path.join(output, `${name}.layout.json`), '--text-diagnostics', nativeFile,
    '--width', '320', '--height', '120', '--scale', '2'], { maxBuffer: 4 * 1024 * 1024 });
  await writeFile(path.join(output, `${name}.stderr.txt`), stderr);
  const native = JSON.parse(await readFile(nativeFile));
  const join = { capture_basis: basis, native_input: input, native_origin_in_browser: [0, 0],
    proof: 'fixture owns identical 320x120 untransformed root; sample DOM id joins sample IR anchor; whole ASCII text verified',
    runs: [{ browser_run: matching[0].id, native_anchor: 'sample', layout_contract: 'untransformed-horizontal-ltr-single-line' }] };
  await write(path.join(output, `${name}.join.json`), join);
  const comparison = compareNativeSelection(reference, native, join, 0.5);
  await write(path.join(output, `${name}.comparison.json`), comparison);
  assert.equal(comparison.rows[0].status, 'compared', JSON.stringify(comparison));
  results[name] = comparison.rows[0];
  artifactHashes[name] = { input_sha256: hash(await readFile(input)),
    observation_sha256: hash(await readFile(nativeFile)),
    screenshot_sha256: hash(await readFile(path.join(output, `${name}.png`))) };
  assert.equal(hash(await readFile(observer)), observerHash, 'observer executable changed during proof');
}
assert.ok(['match', 'mismatch'].includes(results.reference.horizontal_range.status));
assert.equal(results.reference.paint_font_request.status, 'match');
assert.equal(results['font-control'].paint_font_request.status, 'mismatch');
assert.ok(Math.abs(results['font-control'].horizontal_range.candidate[1] - results.reference.horizontal_range.candidate[1]) > 0.5, 'font intervention must change measured native geometry');
assert.equal(results['geometry-control'].paint_font_request.status, 'match');
for (const [i, delta] of results['geometry-control'].horizontal_range.delta.entries())
  assert.ok(Math.abs(delta - results.reference.horizontal_range.delta[i] - 12) <= 0.5);
const receipt = { schema: 'pulp-native-text-proof-v1', observer, observer_sha256: observerHash, artifact_hashes: artifactHashes,
  source_html_sha256: hash(html), snapshot_sha256: hash(snapshotBytes), platform_fonts_sha256: hash(fontBytes),
  assertions: 'controls prove sensitivity; reference mismatch is retained, not forced to pass parity',
  evidence_scope: 'Real browser and native Skia renders of explicitly paired fixtures, not arbitrary importer parity',
  unavailable: ['native resolved faces', 'DOM baselines', 'per-glyph ink', 'CSS line boxes'], results };
await write(path.join(output, 'receipt.json'), receipt);
console.log(JSON.stringify({passed: true, receipt: path.join(output, 'receipt.json')}));
