import assert from 'node:assert/strict';
import { mkdtempSync, realpathSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test, { after } from 'node:test';

import {
  canonicalizeCanvasAuthorityMatches,
  loadMaterializedStateAtlas,
} from './materialized_state_atlas.mjs';

// Every fixture lives under one suite root that is removed when the suite
// finishes, so a run leaves nothing behind in the shared temp directory.
const suiteRoot = mkdtempSync(join(tmpdir(), 'pulp-materialized-suite-'));
after(() => rmSync(suiteRoot, { recursive: true, force: true }));

function scratch(prefix) {
  return mkdtempSync(join(suiteRoot, prefix));
}

function fixture(atlas) {
  const root = scratch('pulp-materialized-atlas-');
  const image = join(root, 'settings.png');
  writeFileSync(image, 'png');
  const atlasPath = join(root, 'atlas.json');
  writeFileSync(atlasPath, JSON.stringify(atlas));
  return { atlasPath, image };
}

function metadataFixture(atlas) {
  const result = fixture(atlas);
  writeFileSync(join(result.atlasPath, '..', 'settings.materialized.json'),
    JSON.stringify({
      schema: 'pulp-materialized-browser-document-v1', version: 1,
      font_bindings: [],
      layout_bindings: [{
        anchor: '#root', path: [{ tag: 'div', index: 0 }],
        box: { left: 10, top: 20, width: 300, height: 200 },
      }],
      text_bindings: [],
    }));
  return result;
}

test('canonicalizes opt-in captured canvas authority to the registered canvas', () => {
  const states = canonicalizeCanvasAuthorityMatches(
    [{ id: 'home', image: 'home.png', match: null }],
    [{ anchor: 'chromium:backend-node:111' }],
    { enabled: true });
  assert.deepEqual(states[0].match, { selector: 'canvas', ancestor: '' });
  assert.equal(states[0].canvas_index, 0);
  assert.deepEqual(canonicalizeCanvasAuthorityMatches(
    [{ id: 'menu', canvas_index: 1, match: { selector: 'canvas' } }],
    [{ anchor: 'chromium:backend-node:111' },
      { anchor: 'chromium:backend-node:222' }],
    { enabled: true })[0].canvas_index, 1);
});

test('captured canvas authority fails closed without valid canvas metadata', () => {
  assert.throws(() => canonicalizeCanvasAuthorityMatches(
    [{ id: 'home' }], [], { enabled: true }), /one or more canvas bindings/);
  assert.throws(() => canonicalizeCanvasAuthorityMatches(
    [{ id: 'home' }, { id: 'other' }],
    [{ anchor: 'chromium:backend-node:111' }], { enabled: true }),
  /one implicit state or explicit canvas matches/);
  assert.throws(() => canonicalizeCanvasAuthorityMatches(
    [{ id: 'home' }], [{ anchor: '#root' }], { enabled: true }),
  /canvas authority binding 0 is invalid/);
  assert.throws(() => canonicalizeCanvasAuthorityMatches(
    [{ id: 'home', match: { selector: '#root' } }],
    [{ anchor: 'chromium:backend-node:111' }], { enabled: true }),
  /must use the registered canvas selector/);
  assert.throws(() => canonicalizeCanvasAuthorityMatches(
    [{ id: 'home', canvas_index: 1, match: { selector: 'canvas' } }],
    [{ anchor: 'chromium:backend-node:111' }], { enabled: true }),
  /out-of-range canvas index/);
  assert.deepEqual(canonicalizeCanvasAuthorityMatches(
    [{ id: 'home' }], [], { enabled: false }), [{ id: 'home' }]);
});

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
    metadata: null,
  }]);
});

test('loads captured state geometry independently from captured paint', () => {
  const { atlasPath } = metadataFixture({
    schema: 'pulp-materialized-state-atlas-v1', version: 1,
    states: [{
      id: 'settings', image: 'settings.png',
      materialized_document: 'settings.materialized.json',
    }],
  });
  const state = loadMaterializedStateAtlas(
    atlasPath, { visualAuthority: 'native' })[0];
  assert.equal(state.image, '');
  assert.deepEqual(state.metadata.layout_bindings, [{
    id: 'pulp-layout-01bebc6698495063',
    anchor: '#root', path: [{ tag: 'div', index: 0 }],
    box: { left: 10, top: 20, width: 300, height: 200 },
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
    metadata: null,
  });
});

test('native authority retains semantic state metadata without paint planes', () => {
  const temp = scratch('pulp-native-state-metadata-');
  writeFileSync(join(temp, 'state.json'), JSON.stringify({
    schema: 'pulp-materialized-browser-document-v1', version: 1,
    font_bindings: [],
    layout_bindings: [{ anchor: '#root', path: [{ tag: 'div', index: 0 }],
      box: { left: 4, top: 5, width: 20, height: 10 } }],
    text_bindings: [],
  }));
  const atlasPath = join(temp, 'atlas.json');
  writeFileSync(atlasPath, JSON.stringify({
    schema: 'pulp-materialized-state-atlas-v1', version: 1,
    states: [{ id: 'open', image: 'unused.png',
      materialized_document: 'state.json',
      match: { selector: '[data-open]' } }],
  }));
  const [state] = loadMaterializedStateAtlas(atlasPath,
    { visualAuthority: 'native' });
  assert.equal(state.image, '');
  assert.equal(state.metadata.layout_bindings.length, 1);
});

test('rejects missing, escaped, and malformed captured state metadata', () => {
  const base = { schema: 'pulp-materialized-state-atlas-v1', version: 1 };
  const missing = fixture({ ...base, states: [{
    id: 'settings', image: 'settings.png',
    materialized_document: 'missing.json',
  }] });
  assert.throws(() => loadMaterializedStateAtlas(missing.atlasPath),
    /metadata does not exist/);

  const escaped = fixture({ ...base, states: [{
    id: 'settings', image: 'settings.png',
    materialized_document: 'escaped.json',
  }] });
  const outsideRoot = scratch('pulp-materialized-metadata-');
  const outsideDocument = join(outsideRoot, 'outside.json');
  writeFileSync(outsideDocument, JSON.stringify({ schema: 'wrong', version: 1 }));
  symlinkSync(outsideDocument, join(escaped.atlasPath, '..', 'escaped.json'));
  assert.throws(() => loadMaterializedStateAtlas(escaped.atlasPath),
    /metadata escapes/);

  const malformed = fixture({ ...base, states: [{
    id: 'settings', image: 'settings.png',
    materialized_document: 'malformed.json',
  }] });
  writeFileSync(join(malformed.atlasPath, '..', 'malformed.json'),
    JSON.stringify({ schema: 'wrong', version: 1 }));
  assert.throws(() => loadMaterializedStateAtlas(malformed.atlasPath),
    /metadata document is invalid/);

  const oversized = fixture({ ...base, states: [{
    id: 'settings', image: 'settings.png',
    materialized_document: 'oversized.json',
  }] });
  writeFileSync(join(oversized.atlasPath, '..', 'oversized.json'),
    Buffer.alloc(8 * 1024 * 1024 + 1, 0x20));
  assert.throws(() => loadMaterializedStateAtlas(oversized.atlasPath),
    /metadata is too large/);
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
  const outsideRoot = scratch('pulp-materialized-outside-');
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
  const outsideRoot = scratch('pulp-materialized-runtime-');
  assert.throws(() => loadMaterializedStateAtlas(atlasPath, {
    runtimeBase: outsideRoot,
  }), /escapes the portable runtime directory/);
});
