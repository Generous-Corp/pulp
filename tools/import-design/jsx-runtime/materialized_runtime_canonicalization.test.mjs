import assert from 'node:assert/strict';
import test from 'node:test';

import { canonicalizeMaterializedRuntimeDocument } from
  './materialized_runtime_canonicalization.mjs';

const asset = (id, source) => ({
  id, mime_type: 'text/javascript', byte_length: Buffer.byteLength(source),
  data_base64: Buffer.from(source).toString('base64'), sha256: '0'.repeat(64),
});
const vendorAsset = (id, source, vendor_kind) => ({
  ...asset(id, source), vendor_kind,
});
test('precompiles captured JSX and removes redundant browser vendors', () => {
  const react = '/** @license React react.development.js */';
  const reactDom = '/** @license React react-dom.development.js */';
  const babel = `.Babel=${' '.repeat(1_000_000)}transform`;
  const app = 'globalThis.keepMe = true;';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="react"></script><script src="react-dom"></script>' +
      '<script src="babel"></script><script src="app"></script>' +
      '<script type="text/babel">globalThis.node = <span>OK</span>;</script>',
    assets: [vendorAsset('react', react, 'react'),
      vendorAsset('react-dom', reactDom, 'react-dom'),
      vendorAsset('babel', babel, 'babel'), asset('app', app)],
  });

  assert.equal(result.runtime_canonicalization.jsx_scripts_compiled, 1);
  assert.equal(result.runtime_canonicalization.browser_vendor_assets_removed, 3);
  assert.deepEqual(result.assets.map(({ id }) => id), ['app']);
  assert.doesNotMatch(result.html, /text\/babel|src="react|src="babel/);
  assert.match(result.html, /React\.createElement\("span"/);
  assert.match(result.html, /src="app"/);
});

test('does not strip an unrelated external JavaScript asset', () => {
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="app"></script>',
    assets: [asset('app', 'globalThis.App = function App() {};')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="app"/);
});

test('parses script attributes without treating quoted markup as a tag end', () => {
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script data-note="> bait" TYPE="text/jsx">' +
      'globalThis.node = <span title="</scriptx>">OK</span>;</script>',
    assets: [],
  });
  assert.equal(result.runtime_canonicalization.jsx_scripts_compiled, 1);
  assert.match(result.html, /type="text\/javascript"/);
  assert.match(result.html, /React\.createElement\("span"/);
});

test('reuses the compiled form for repeated inline JSX programs', () => {
  const source = 'globalThis.node = <span>OK</span>;';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: `<script type="text/babel">${source}</script>` +
      `<script type="text/jsx">${source}</script>`,
    assets: [],
  });
  assert.equal(result.runtime_canonicalization.jsx_scripts_compiled, 2);
  assert.equal((result.html.match(/React\.createElement\("span"/g) || []).length, 2);
});

test('does not remove a vendor-looking src from a script with authored body', () => {
  const react = '/** @license React react.development.js */';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="react">globalThis.authored = true;</script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.match(result.html, /globalThis\.authored/);
});

test('does not classify authored marker collisions as browser vendors', () => {
  const authored = '/** @license React react.development.js */\n' +
    'globalThis.Authored = true;';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="app.js?v=1"></script>',
    assets: [asset('app.js?v=1', authored)],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="app\.js\?v=1"/);
});

test('preserves legacy captures without explicit vendor metadata', () => {
  const react = '/** @license React react.development.js */';
  const result = canonicalizeMaterializedRuntimeDocument({
    version: 1,
    html: '<script src="react"></script>',
    assets: [asset('react', react)],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="react"/);
});

test('preserves a vendor asset when another reference has authored code', () => {
  const react = '/** @license React react.development.js */';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="react"></script><script src="react">globalThis.keep = 1;</script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /globalThis\.keep/);
});

test('accepts whitespace-only empty vendor script bodies', () => {
  const react = '/** @license React react.development.js */';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="react"> \n </script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 0);
  assert.doesNotMatch(result.html, /src="react"/);
});

test('ignores unknown vendor roles', () => {
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="mystery"></script>',
    assets: [vendorAsset('mystery', 'globalThis.keep = true;', 'maybe-react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="mystery"/);
});
