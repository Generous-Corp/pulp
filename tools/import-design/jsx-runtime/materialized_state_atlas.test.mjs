import assert from 'node:assert/strict';
import { mkdtempSync, realpathSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

import { loadMaterializedStateAtlas } from './materialized_state_atlas.mjs';

function fixture(atlas) {
  const root = mkdtempSync(join(tmpdir(), 'pulp-materialized-atlas-'));
  const image = join(root, 'settings.png');
  writeFileSync(image, 'png');
  const atlasPath = join(root, 'atlas.json');
  writeFileSync(atlasPath, JSON.stringify(atlas));
  return { atlasPath, image };
}

test('normalizes a captured dropdown or modal state contract', () => {
  const { atlasPath, image } = fixture({
    schema: 'pulp-materialized-state-atlas-v1',
    version: 1,
    states: [{
      id: 'settings',
      image: 'settings.png',
      match: { selector: '[aria-label="Settings"]', ancestor: '#panel' },
      activate: [{ selector: 'button[title="Settings"]' }],
    }],
  });
  assert.deepEqual(loadMaterializedStateAtlas(atlasPath), [{
    id: 'settings', image: realpathSync(image),
    match: { selector: '[aria-label="Settings"]', ancestor: '#panel' },
    activate: [{ selector: 'button[title="Settings"]', event: 'click', data: null }],
  }]);
});

test('emits package-relative state paint for a portable runtime', () => {
  const { atlasPath } = fixture({
    schema: 'pulp-materialized-state-atlas-v1',
    version: 1,
    states: [{ id: 'settings', image: 'settings.png' }],
  });
  assert.equal(loadMaterializedStateAtlas(atlasPath, {
    runtimeBase: realpathSync(join(atlasPath, '..')),
  })[0].image, 'settings.png');
});

test('native visual authority validates behavior without embedding screenshots', () => {
  const { atlasPath } = fixture({
    schema: 'pulp-materialized-state-atlas-v1', version: 1,
    states: [{
      id: 'context-menu', image: 'missing.png',
      activate: [{ selector: '#canvas', event: 'contextmenu', data: { button: 2 } }],
    }],
  });
  assert.deepEqual(loadMaterializedStateAtlas(
    atlasPath, { visualAuthority: 'native' })[0], {
    id: 'context-menu', image: '', match: null,
    activate: [{ selector: '#canvas', event: 'contextmenu', data: { button: 2 } }],
  });
});

test('rejects missing paint, duplicate ids, and malformed activation atomically', () => {
  const base = {
    schema: 'pulp-materialized-state-atlas-v1', version: 1,
  };
  const missing = fixture({ ...base, states: [{ id: 'open', image: 'absent.png' }] });
  assert.throws(() => loadMaterializedStateAtlas(missing.atlasPath),
    /image does not exist/);

  const duplicate = fixture({ ...base, states: [
    { id: 'open', image: 'settings.png' },
    { id: 'open', image: 'settings.png' },
  ] });
  assert.throws(() => loadMaterializedStateAtlas(duplicate.atlasPath),
    /invalid or duplicate id/);

  const malformed = fixture({ ...base, states: [{
    id: 'open', image: 'settings.png', activate: [{ selector: '' }],
  }] });
  assert.throws(() => loadMaterializedStateAtlas(malformed.atlasPath),
    /activation step 0 is invalid/);
});

test('bounds state count, selectors, activation programs, and event data', () => {
  const base = { schema: 'pulp-materialized-state-atlas-v1', version: 1 };
  const tooMany = fixture({ ...base, states: Array.from({ length: 65 }, (_, i) => ({
    id: `state-${i}`, image: 'settings.png',
  })) });
  assert.throws(() => loadMaterializedStateAtlas(tooMany.atlasPath), /1-64/);

  const selector = fixture({ ...base, states: [{
    id: 'open', image: 'settings.png', match: { selector: `#${'x'.repeat(4096)}` },
  }] });
  assert.throws(() => loadMaterializedStateAtlas(selector.atlasPath),
    /invalid match contract/);

  const program = fixture({ ...base, states: [{
    id: 'open', image: 'settings.png', activate: Array.from({ length: 33 }, () => ({
      selector: '#open', event: 'click',
    })),
  }] });
  assert.throws(() => loadMaterializedStateAtlas(program.atlasPath),
    /invalid activation contract/);

  const data = fixture({ ...base, states: [{
    id: 'open', image: 'settings.png', activate: [{
      selector: '#open', data: { payload: 'x'.repeat(64 * 1024) },
    }],
  }] });
  assert.throws(() => loadMaterializedStateAtlas(data.atlasPath),
    /data is too large/);
});

test('reference paint must be a regular file contained by the atlas directory', () => {
  const base = { schema: 'pulp-materialized-state-atlas-v1', version: 1 };
  const outsideRoot = mkdtempSync(join(tmpdir(), 'pulp-materialized-outside-'));
  const outsideImage = join(outsideRoot, 'outside.png');
  writeFileSync(outsideImage, 'png');

  const escaped = fixture({ ...base, states: [{
    id: 'open', image: 'settings.png',
  }] });
  const atlasRoot = join(escaped.atlasPath, '..');
  symlinkSync(outsideImage, join(atlasRoot, 'escaped.png'));
  writeFileSync(escaped.atlasPath, JSON.stringify({ ...base, states: [{
    id: 'open', image: 'escaped.png',
  }] }));
  assert.throws(() => loadMaterializedStateAtlas(escaped.atlasPath),
    /escapes the atlas directory/);
});

test('portable state paint cannot escape the packaged runtime directory', () => {
  const { atlasPath } = fixture({
    schema: 'pulp-materialized-state-atlas-v1', version: 1,
    states: [{ id: 'settings', image: 'settings.png' }],
  });
  const outsideRoot = mkdtempSync(join(tmpdir(), 'pulp-materialized-runtime-'));
  assert.throws(() => loadMaterializedStateAtlas(atlasPath, {
    runtimeBase: outsideRoot,
  }), /escapes the portable runtime directory/);
});
