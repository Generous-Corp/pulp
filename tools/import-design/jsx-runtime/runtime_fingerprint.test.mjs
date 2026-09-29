import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import {
  loadRuntimeFingerprint,
  runtimeFingerprintBanner,
} from './runtime_fingerprint.mjs';

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
