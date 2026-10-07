// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdtemp, readFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {
  canonicalJson,
  emitMaterializedRuntime,
} from './materialized_source_emitter.mjs';

const HASH = 'a'.repeat(64);
const base = (binding) => ({
  schema: 'pulp-materialized-browser-document-v2', version: 2,
  html: '<main>READY</main>',
  assets: [],
  semantic_bindings: [binding],
  layout_bindings: [], text_bindings: [], paint_bindings: [], canvas_bindings: [],
});

test('emits a byte-stable source tree and stable ids map', async () => {
  const first = await mkdtemp(path.join(os.tmpdir(), 'pulp-materialized-emitter-'));
  const second = await mkdtemp(path.join(os.tmpdir(), 'pulp-materialized-emitter-'));
  const document = base({
    component_name: 'Panel', local_path: 'Panel.tsx#ready', content_hash: HASH,
    anchor: '#root', kind: 'button', name: 'READY',
  });
  const a = await emitMaterializedRuntime({
    document, outputDirectory: first, idsMode: 'stable',
  });
  const b = await emitMaterializedRuntime({
    document, outputDirectory: second, idsMode: 'stable',
  });
  assert.deepEqual(a.files, b.files);
  for (const relative of a.files) {
    assert.equal(await readFile(path.join(first, relative), 'utf8'),
      await readFile(path.join(second, relative), 'utf8'), relative);
  }
  assert.match(await readFile(path.join(first, 'ids.map.json'), 'utf8'),
    /pulp-source-[0-9a-f]{64}/);
  assert.equal(a.document.bindings_by_id.semantic[Object.keys(
    a.document.bindings_by_id.semantic)[0]].data_pulp_id.startsWith('pulp-source-'), true);
});

test('stable mode rejects missing and partial source identity', async () => {
  const out = await mkdtemp(path.join(os.tmpdir(), 'pulp-materialized-emitter-'));
  await assert.rejects(() => emitMaterializedRuntime({
    document: base({ anchor: '#root', kind: 'button' }),
    outputDirectory: out, idsMode: 'stable',
  }), /missing source identity/);
  await assert.rejects(() => emitMaterializedRuntime({
    document: base({ component_name: 'Panel', anchor: '#root', kind: 'button' }),
    outputDirectory: out, idsMode: 'stable',
  }), /partial source identity/);
});

test('canonicalJson sorts object keys recursively', () => {
  assert.equal(canonicalJson({ z: 1, a: { y: 2, x: 1 } }),
    '{\n  "a": {\n    "x": 1,\n    "y": 2\n  },\n  "z": 1\n}\n');
});

test('stable mode rejects an ambiguous prior identity match', async () => {
  const out = await mkdtemp(path.join(os.tmpdir(), 'pulp-materialized-emitter-'));
  const previous = {
    schema: 'pulp-materialized-ids-map-v1', version: 1,
    entries: [
      { id: 'pulp-source-a', component: 'Panel', local_path: 'A.tsx', content_hash: HASH },
      { id: 'pulp-source-b', component: 'Panel', local_path: 'B.tsx', content_hash: HASH },
    ],
  };
  await assert.rejects(() => emitMaterializedRuntime({
    document: base({
      component_name: 'Panel', local_path: 'C.tsx', content_hash: HASH,
      anchor: '#root', kind: 'button',
    }),
    outputDirectory: out, idsMode: 'stable', idsMap: previous,
  }), /matches multiple prior ids/);
});
