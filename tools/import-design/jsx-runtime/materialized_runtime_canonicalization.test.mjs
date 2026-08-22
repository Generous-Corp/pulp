import assert from 'node:assert/strict';
import test from 'node:test';

import { canonicalizeMaterializedRuntimeDocument } from
  './materialized_runtime_canonicalization.mjs';

const asset = (id, source) => ({
  id, mime_type: 'text/javascript', byte_length: Buffer.byteLength(source),
  data_base64: Buffer.from(source).toString('base64'), sha256: '0'.repeat(64),
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
    assets: [asset('react', react), asset('react-dom', reactDom),
      asset('babel', babel), asset('app', app)],
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

test('does not remove a vendor-looking src from a script with authored body', () => {
  const react = '/** @license React react.development.js */';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="react">globalThis.authored = true;</script>',
    assets: [asset('react', react)],
  });
  assert.match(result.html, /globalThis\.authored/);
});
