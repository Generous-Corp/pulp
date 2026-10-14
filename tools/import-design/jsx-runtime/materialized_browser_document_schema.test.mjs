// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  MATERIALIZED_BINDING_KINDS,
  MATERIALIZED_BROWSER_DOCUMENT_V1,
  MATERIALIZED_BROWSER_DOCUMENT_V2,
  isMaterializedBrowserDocumentSchema,
  normalizeMaterializedBindingDocument,
} from './materialized_binding_contract.mjs';
import { normalizeMaterializedMetadata } from './materialized_metadata_contract.mjs';

const schema = JSON.parse(await readFile(fileURLToPath(new URL(
  './materialized_browser_document.schema.json', import.meta.url)), 'utf8'));

// This is deliberately a small, dependency-free boundary probe rather than a
// second JSON-Schema implementation. The production parser owns value
// validation; this helper checks the schema's version/profile policy and root
// surface so a field can never be added to the document silently.
function assertDocumentSurface(document) {
  assert.equal(typeof document, 'object');
  for (const key of Object.keys(document)) {
    assert.equal(Object.hasOwn(schema.properties, key), true,
      `unknown top-level materialized field: ${key}`);
  }
  assert.equal(isMaterializedBrowserDocumentSchema(document), true);
  if (document.schema === MATERIALIZED_BROWSER_DOCUMENT_V1) {
    assert.equal(document.version, 1);
    assert.equal(Object.hasOwn(document, 'bindings_by_id'), false,
      'v1 must not carry the v2 id-addressed projection');
  } else {
    assert.equal(document.schema, MATERIALIZED_BROWSER_DOCUMENT_V2);
    assert.equal(document.version, 2);
    if (!document.bindings_by_id)
      throw new Error('v2 requires bindings_by_id id-addressed projection');
    assert.deepEqual(Object.keys(document.bindings_by_id).sort(),
      [...MATERIALIZED_BINDING_KINDS].sort());
    for (const kind of MATERIALIZED_BINDING_KINDS) {
      for (const [id, binding] of Object.entries(document.bindings_by_id[kind])) {
        assert.match(id, /^[a-z][a-z0-9._:-]{1,127}$/);
        assert.equal(binding.id, id);
      }
    }
  }
}

function baseDocument(schemaName, version) {
  return {
    schema: schemaName,
    version,
    html: '<!doctype html><html><body></body></html>',
    mime_type: 'text/html',
    assets: [],
    font_bindings: [],
    layout_bindings: [],
    text_bindings: [],
    paint_bindings: [],
    semantic_bindings: [],
    canvas_bindings: [],
  };
}

test('published materialized document schema declares v1/v2 migration and bounds', () => {
  assert.equal(schema.$schema, 'https://json-schema.org/draft/2020-12/schema');
  assert.equal(schema.additionalProperties, false);
  assert.deepEqual(schema.properties.schema.enum, [
    MATERIALIZED_BROWSER_DOCUMENT_V1,
    MATERIALIZED_BROWSER_DOCUMENT_V2,
  ]);
  assert.equal(schema.properties.version.enum.includes(1), true);
  assert.equal(schema.properties.version.enum.includes(2), true);
  assert.equal(schema.$defs.bindingList.maxItems, 16384);
  assert.equal(schema.$defs.path.maxItems, 256);
  assert.deepEqual([...schema.$defs.bindingsById.required].sort(),
    [...MATERIALIZED_BINDING_KINDS].sort());
  assert.match(schema.$comment, /Unknown top-level fields are rejected/);
  assert.match(schema.description, /metadata may omit html and assets/);
});

test('v1 positional and v2 id-addressed documents have explicit profiles', () => {
  const v1 = baseDocument(MATERIALIZED_BROWSER_DOCUMENT_V1, 1);
  assertDocumentSurface(v1);
  assert.doesNotThrow(() => normalizeMaterializedMetadata(v1));

  const v2 = normalizeMaterializedBindingDocument(
    baseDocument(MATERIALIZED_BROWSER_DOCUMENT_V1, 1), { upgradeSchema: true });
  assertDocumentSurface(v2);
  assert.doesNotThrow(() => normalizeMaterializedMetadata(v2));
});

test('schema/version and planted unknown-field mutations fail closed', () => {
  const v1 = baseDocument(MATERIALIZED_BROWSER_DOCUMENT_V1, 1);
  assert.throws(() => assertDocumentSurface({ ...v1, generated_by: 'agent' }),
    /unknown top-level materialized field/);
  assert.equal(isMaterializedBrowserDocumentSchema({
    ...v1, schema: 'pulp-materialized-browser-document-v3', version: 3,
  }), false);

  const v2 = normalizeMaterializedBindingDocument(v1, { upgradeSchema: true });
  assert.throws(() => assertDocumentSurface({
    ...v2, schema: MATERIALIZED_BROWSER_DOCUMENT_V1, version: 1,
  }), /v1 must not carry/);
  const missingMap = { ...v2 };
  delete missingMap.bindings_by_id;
  assert.throws(() => assertDocumentSurface(missingMap), /bindings_by_id/);
});
