import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import { mkdtempSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';

import {
  DEFAULT_FINGERPRINT_PATH,
  loadRuntimeFingerprint,
  runtimeFingerprintBanner,
} from './runtime_fingerprint.mjs';
import { buildMaterializedRuntimeEntry } from './materialized_runtime_entry.mjs';

test('the banner carries the manifest revision the staleness checks parse', () => {
  const manifest = loadRuntimeFingerprint();
  const banner = runtimeFingerprintBanner(manifest);
  const match = /@pulp\/react runtime revision (\d+)/.exec(banner);
  assert.ok(match, banner);
  assert.equal(Number(match[1]), manifest.revision);
});

test('every fix names a revision no newer than the manifest', () => {
  const manifest = loadRuntimeFingerprint();
  assert.ok(manifest.fixes.length > 0);
  for (const fix of manifest.fixes) {
    assert.ok(Number.isInteger(fix.revision) && fix.revision >= 1, fix.id);
    assert.ok(fix.revision <= manifest.revision, fix.id);
    assert.ok(fix.summary && fix.id, JSON.stringify(fix));
  }
});

test('the banner runs ahead of the bundle and publishes the revision', () => {
  const sandbox = { globalThis: {} };
  sandbox.globalThis = sandbox;
  vm.runInNewContext(runtimeFingerprintBanner({ revision: 7 }), sandbox);
  assert.equal(sandbox.__PULP_REACT_RUNTIME_REVISION__, 7);
});

test('an invalid revision is refused rather than stamped', () => {
  const path = join(mkdtempSync(join(tmpdir(), 'pulp-fp-')), 'fp.json');
  writeFileSync(path, JSON.stringify({ schema: 1, revision: 0, fixes: [] }));
  assert.throws(() => loadRuntimeFingerprint(path), /positive integer/);
});

// A signature the SDK runtime does not itself contain would mark every
// bundle stale for that fix forever, or -- worse -- a common identifier would
// mark every bundle current. Each one must name code that ships: the generated
// materialized entry or the @pulp/react source compiled into the same bundle.
test('every fix signature names code the SDK runtime ships', () => {
  const entry = buildMaterializedRuntimeEntry({
    capturedCssVariables: {}, presentationTime: 0, requestedState: '',
    textBindings: [], layoutBindings: [], paintBindings: [],
    runtimeDocumentAsset: null, sidecar: null, productPrelude: '',
    surfaceBackground: null, authoredLeft: 0, authoredTop: 0, authoredWidth: 1,
    authoredHeight: 1, authoredTransform: null, visualAuthority: null,
    stateAtlas: [], visualWidth: 1, visualHeight: 1, canvasBindings: [],
    behaviorCanvasAnchors: [], capturedPaintAuthorityAnchors: [],
  });
  const srcDir = resolve(dirname(DEFAULT_FINGERPRINT_PATH), 'src');
  const reactSource = readdirSync(srcDir).filter(name => name.endsWith('.ts'))
    .map(name => readFileSync(resolve(srcDir, name), 'utf8')).join('\n');
  const manifest = loadRuntimeFingerprint();
  assert.ok(manifest.fixes.length > 0);
  for (const fix of manifest.fixes) {
    if (!fix.signature) continue;
    assert.ok(entry.includes(fix.signature) || reactSource.includes(fix.signature),
      `${fix.id}: signature ${fix.signature} is not in the SDK runtime`);
  }
});
