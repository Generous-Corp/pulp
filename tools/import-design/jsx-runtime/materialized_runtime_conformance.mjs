#!/usr/bin/env node
// Small, dependency-light conformance probe for the materialized runtime
// canonicalizer. It is intentionally usable both from ctest and by humans:
// repeated runs report a stable digest and the negative control proves that the
// same assertions reject a planted regression.

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { canonicalizeMaterializedRuntimeDocument } from
  './materialized_runtime_canonicalization.mjs';

const args = process.argv.slice(2);
const value = (name, fallback) => {
  const index = args.indexOf(name);
  return index < 0 ? fallback : args[index + 1];
};
const runs = Math.max(2, Number.parseInt(value('--runs', '5'), 10));
if (!Number.isInteger(runs) || runs < 2) {
  console.error('--runs must be an integer >= 2');
  process.exit(2);
}
const jsonOutput = args.includes('--json');

const asset = (id, source, vendor_kind) => ({
const asset = (id, source) => ({
  id,
  mime_type: 'text/javascript',
  byte_length: Buffer.byteLength(source),
  data_base64: Buffer.from(source).toString('base64'),
  sha256: '0'.repeat(64),
  ...(vendor_kind ? { vendor_kind } : {}),
});

function fixture() {
  // Keep the fixture payloads representative of the capture adapter's trust
  // gate.  Marker-only payloads must remain in the document and would make
  // this conformance probe fail before it can exercise canonicalization.
  const react = '/** @license React react.development.js */\n' +
    'ReactVersion createElement ' + ' '.repeat(32 * 1024);
  const reactDom = '/** @license React react-dom.development.js */\n' +
    'ReactVersion createRoot ' + ' '.repeat(32 * 1024);
  const babel = `.Babel=${' '.repeat(1_000_000)}` +
    'transformScriptTags registerPlugin';
  return {
    schema: 'pulp-materialized-browser-document-v1',
    version: 1,
    html: '<!doctype html><head><script src="react"></script>' +
      '<script src="react-dom"></script><script src="babel"></script>' +
      '<script src="app"></script></head><body>' +
      '<script type="text/babel">globalThis.node = <button>OK</button>;</script>' +
      '</body>',
    assets: [asset('react', react, 'react'),
      asset('react-dom', reactDom, 'react-dom'), asset('babel', babel, 'babel'),
      asset('app', 'globalThis.keepMe = true;')],
    assets: [asset('react', react), asset('react-dom', reactDom),
      asset('babel', babel), asset('app', 'globalThis.keepMe = true;')],
  };
}

function canonicalBytes(document) {
  return Buffer.from(JSON.stringify(document));
}

function digest(bytes) {
  return createHash('sha256').update(bytes).digest('hex');
}

function assertConformant(document) {
  assert.equal(document.runtime_canonicalization.jsx_scripts_compiled, 1);
  assert.equal(document.runtime_canonicalization.browser_vendor_assets_removed, 3);
  assert.deepEqual(document.assets.map(({ id }) => id), ['app']);
  assert.doesNotMatch(document.html, /text\/(?:babel|jsx)/i);
  assert.doesNotMatch(document.html, /src=["'](?:react|react-dom|babel)["']/i);
  assert.match(document.html, /React\.createElement\("button"/);
  assert.match(document.html, /src="app"/);
}

const kExpectedCanonicalSha256 =
  '4e784cc969ad0826b5733af1b610076cd00511cf77acbb56ba266b64459edd21';

const input = fixture();
const samples = [];
let first;
for (let index = 0; index < runs; ++index) {
  const start = performance.now();
  const output = canonicalizeMaterializedRuntimeDocument(input);
  const bytes = canonicalBytes(output);
  assertConformant(output);
  samples.push(performance.now() - start);
  if (!first) {
    first = { output, bytes, sha256: digest(bytes) };
    assert.equal(first.sha256, kExpectedCanonicalSha256,
      'canonical fixture digest changed; review the canonicalization contract');
  } else {
    assert.equal(digest(bytes), first.sha256,
      'canonicalization must be byte-identical across repeated runs');
    assert.deepEqual(bytes, first.bytes,
      'canonicalization must preserve exact output bytes across repeated runs');
  }
}

// A planted vendor tag is the negative control: a detector that only checks
// determinism would pass this, while the conformance contract must reject it.
const negative = structuredClone(first.output);
negative.html = negative.html.replace('</head>',
  '<script src="react"></script></head>');
assert.throws(() => assertConformant(negative), /src=/,
  'negative control must be rejected by the conformance assertions');

const sorted = [...samples].sort((a, b) => a - b);
const report = {
  schema: 'pulp-design-import-conformance-v1',
  fixture: 'messy-materialized-runtime',
  runs,
  input_bytes: canonicalBytes(input).byteLength,
  output_bytes: first.bytes.byteLength,
  canonical_sha256: first.sha256,
  elapsed_ms_median: sorted[Math.floor(sorted.length / 2)],
  negative_control: 'passed',
};
if (jsonOutput) console.log(JSON.stringify(report));
else {
  console.log(`canonical_sha256=${report.canonical_sha256}`);
  console.log(`runs=${runs} output_bytes=${report.output_bytes} ` +
    `elapsed_ms_median=${report.elapsed_ms_median.toFixed(3)}`);
  console.log('negative_control=passed');
}
