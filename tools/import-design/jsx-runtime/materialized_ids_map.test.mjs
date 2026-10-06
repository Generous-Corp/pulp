// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  MATERIALIZED_IDS_MAP_SCHEMA,
  MATERIALIZED_IDS_MAP_VERSION,
  applyMaterializedIdsMap,
  assignMaterializedSourceIds,
  materializedSourceKey,
  normalizeMaterializedIdsMap,
  serializeMaterializedIdsMap,
} from './materialized_ids_map.mjs';

const HASH_A = 'a'.repeat(64);
const HASH_B = 'b'.repeat(64);
const source = (component, local_path, content_hash = HASH_A) => ({
  component, local_path, content_hash,
});

test('canonicalizes ids.map.json independent of entry and alias order', () => {
  const input = {
    version: MATERIALIZED_IDS_MAP_VERSION,
    schema: MATERIALIZED_IDS_MAP_SCHEMA,
    entries: [
      { ...source('Zed', 'z.tsx', HASH_B), id: 'pulp-source-z', aliases: [
        source('Zed', 'old.tsx', HASH_A),
      ] },
      { ...source('Alpha', 'a.tsx'), id: 'pulp-source-a' },
    ],
  };
  const shuffled = {
    ...input,
    entries: [
      { ...input.entries[0], aliases: [...input.entries[0].aliases].reverse() },
      input.entries[1],
    ],
  };
  assert.equal(serializeMaterializedIdsMap(input),
    serializeMaterializedIdsMap(shuffled));
  assert.equal(materializedSourceKey(source('Alpha', 'a.tsx')), JSON.stringify({
    component: 'Alpha', local_path: 'a.tsx', content_hash: HASH_A,
  }));
});

test('mints deterministic ids and carries them through path and content edits', () => {
  const first = assignMaterializedSourceIds([
    source('Toolbar', 'Button.tsx#0'),
    source('Toolbar', 'Button.tsx#1', HASH_B),
  ]);
  const reordered = assignMaterializedSourceIds([
    source('Toolbar', 'Button.tsx#2', HASH_A),
    source('Toolbar', 'Button.tsx#1', HASH_B),
  ], first.map);
  assert.equal(reordered.assignments[0].id, first.assignments[0].id);
  assert.equal(reordered.assignments[0].disposition, 'content-match');

  const edited = assignMaterializedSourceIds([
    source('Toolbar', 'Button.tsx#2', 'c'.repeat(64)),
  ], reordered.map);
  assert.equal(edited.assignments[0].id, first.assignments[0].id);
  assert.equal(edited.assignments[0].disposition, 'path-match');
  const editedEntry = edited.map.entries.find(entry => entry.id === first.assignments[0].id);
  assert.equal(editedEntry.aliases.length, 2);
  assert.equal(editedEntry.aliases[0].local_path, 'Button.tsx#0');
});

test('historical source keys can reactivate without duplicate aliases', () => {
  const first = assignMaterializedSourceIds([
    source('Panel', 'Panel.tsx#button'),
  ]);
  const edited = assignMaterializedSourceIds([
    source('Panel', 'Panel.tsx#renamed', HASH_A),
  ], first.map);
  const restored = assignMaterializedSourceIds([
    source('Panel', 'Panel.tsx#button'),
  ], edited.map);
  assert.equal(restored.assignments[0].id, first.assignments[0].id);
  assert.doesNotThrow(() => normalizeMaterializedIdsMap(restored.map));
  assert.equal(restored.map.entries[0].local_path, 'Panel.tsx#button');
  assert.equal(restored.map.entries[0].aliases.length, 1);
  assert.equal(restored.map.entries[0].aliases[0].local_path, 'Panel.tsx#renamed');
});

test('exact source keys reuse their prior id without minting or mutation', () => {
  const first = assignMaterializedSourceIds([
    source('Header', 'Header.tsx#title'),
  ]);
  const second = assignMaterializedSourceIds([
    source('Header', 'Header.tsx#title'),
  ], first.map);
  assert.equal(second.assignments[0].id, first.assignments[0].id);
  assert.equal(second.assignments[0].disposition, 'exact');
  assert.deepEqual(second.map, first.map);
  assert.equal(serializeMaterializedIdsMap(second.map),
    serializeMaterializedIdsMap(first.map));
});

test('same-signature candidates fail closed instead of guessing an identity', () => {
  const prior = assignMaterializedSourceIds([
    { ...source('Knob', 'left.tsx'), data_pulp_id: 'pulp-source-left' },
    { ...source('Knob', 'right.tsx'), data_pulp_id: 'pulp-source-right' },
  ]).map;
  assert.throws(() => assignMaterializedSourceIds([
    source('Knob', 'new.tsx'),
  ], prior), /matches multiple prior ids/);
});

test('conflicting path and content matches fail closed', () => {
  const first = assignMaterializedSourceIds([
    source('Meter', 'old.tsx', HASH_A),
    source('Meter', 'new.tsx', HASH_B),
  ]).map;
  assert.throws(() => assignMaterializedSourceIds([
    // The content belongs to old.tsx while the path belongs to new.tsx.
    source('Meter', 'new.tsx', HASH_A),
  ], first), /conflicting content and path matches/);
});

test('duplicate and malformed sidecar rows are rejected', () => {
  const base = { schema: MATERIALIZED_IDS_MAP_SCHEMA,
    version: MATERIALIZED_IDS_MAP_VERSION };
  assert.throws(() => normalizeMaterializedIdsMap({ ...base, entries: [
    { ...source('Button', 'button.tsx'), id: 'pulp-source-one' },
    { ...source('Button', 'button.tsx'), id: 'pulp-source-two' },
  ] }), /reuses a source key/);
  assert.throws(() => normalizeMaterializedIdsMap({ ...base, entries: [
    { ...source('Button', '../button.tsx'), id: 'pulp-source-one' },
  ] }), /safe relative path/);
  assert.throws(() => normalizeMaterializedIdsMap({ ...base, entries: [
    { ...source('Button', 'button.tsx'), id: 'Pulp-Source-One' },
  ] }), /id is invalid/);
});

test('applyMaterializedIdsMap updates only source-addressed bindings', () => {
  const result = applyMaterializedIdsMap({
    layout_bindings: [{
      component_name: 'Panel', local_path: 'Panel.tsx#button',
      content_hash: HASH_A, anchor: '#root', path: [], box: {},
    }, {
      // Missing source identity stays positional and is surfaced to the next
      // normalization stage rather than receiving a guessed id.
      anchor: '#root', path: [], box: {},
    }],
  });
  assert.match(result.document.layout_bindings[0].data_pulp_id,
    /^pulp-source-[0-9a-f]{64}$/);
  assert.equal(result.document.layout_bindings[0].id, undefined);
  assert.equal(result.document.layout_bindings[1].data_pulp_id, undefined);
  assert.equal(result.assignments.length, 1);
});

test('partial source metadata remains positional instead of aborting import', () => {
  const result = applyMaterializedIdsMap({
    layout_bindings: [{
      component_name: 'Panel', anchor: '#root', path: [], box: {},
    }, {
      local_path: 'Panel.tsx#button', content_hash: HASH_A,
      anchor: '#button', path: [], box: {},
    }],
  });
  assert.equal(result.document.layout_bindings[0].data_pulp_id, undefined);
  assert.equal(result.document.layout_bindings[1].data_pulp_id, undefined);
  assert.equal(result.assignments.length, 0);
});
