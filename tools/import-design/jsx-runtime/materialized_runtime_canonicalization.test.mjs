import assert from 'node:assert/strict';
import test from 'node:test';

import { canonicalizeMaterializedRuntimeDocument } from
  './materialized_runtime_canonicalization.mjs';
import { trustedVendorPayload } from
  '../browser_capture/vendor_payload.mjs';

const asset = (id, source) => ({
  id, mime_type: 'text/javascript', byte_length: Buffer.byteLength(source),
  data_base64: Buffer.from(source).toString('base64'), sha256: '0'.repeat(64),
});
const vendorAsset = (id, source, vendor_kind) => ({
  ...asset(id, source), vendor_kind,
});
const trustedReact = () =>
  '/** @license React react.development.js */\n' +
  'ReactVersion createElement ' + ' '.repeat(32 * 1024);
const trustedReactDom = () =>
  '/** @license React react-dom.development.js */\n' +
  'ReactVersion createRoot ' + ' '.repeat(32 * 1024);
const trustedBabel = () =>
  `.Babel=${' '.repeat(1_000_000)}transformScriptTags registerPlugin`;
// Synthetic fixtures exercise canonicalizer behavior without checking in
// multi-megabyte browser bundles. Production uses the exact SHA-256 verifier;
// the strict path is covered by the padded-spoof control below.
const canonicalizeSynthetic = document => canonicalizeMaterializedRuntimeDocument(
  document, { vendorPayloadTrust: trustedVendorPayload });
test('precompiles captured JSX and removes redundant browser vendors', () => {
  const react = trustedReact();
  const reactDom = trustedReactDom();
  const babel = trustedBabel();
  const app = 'globalThis.keepMe = true;';
  const result = canonicalizeSynthetic({
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
  const result = canonicalizeSynthetic({
    html: '<script src="app"></script>',
    assets: [asset('app', 'globalThis.App = function App() {};')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="app"/);
});

test('parses script attributes without treating quoted markup as a tag end', () => {
  const result = canonicalizeSynthetic({
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
  const result = canonicalizeSynthetic({
    html: `<script type="text/babel">${source}</script>` +
      `<script type="text/jsx">${source}</script>`,
    assets: [],
  });
  assert.equal(result.runtime_canonicalization.jsx_scripts_compiled, 2);
  assert.equal((result.html.match(/React\.createElement\("span"/g) || []).length, 2);
});

test('does not remove a vendor-looking src from a script with authored body', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<script src="react">globalThis.authored = true;</script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.match(result.html, /globalThis\.authored/);
});

test('does not classify authored marker collisions as browser vendors', () => {
  const authored = '/** @license React react.development.js */\n' +
    'globalThis.Authored = true;';
  const result = canonicalizeSynthetic({
    html: '<script src="app.js?v=1"></script>',
    assets: [asset('app.js?v=1', authored)],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="app\.js\?v=1"/);
});

test('preserves legacy captures without explicit vendor metadata', () => {
  const react = '/** @license React react.development.js */';
  const result = canonicalizeSynthetic({
    version: 1,
    html: '<script src="react"></script>',
    assets: [asset('react', react)],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="react"/);
});

test('preserves a vendor asset when another reference has authored code', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<script src="react"></script><script src="react">globalThis.keep = 1;</script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /globalThis\.keep/);
});

test('accepts whitespace-only empty vendor script bodies', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<script src="react"> \n </script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 0);
  assert.doesNotMatch(result.html, /src="react"/);
});

test('ignores unknown vendor roles', () => {
  const result = canonicalizeSynthetic({
    html: '<script src="mystery"></script>',
    assets: [vendorAsset('mystery', 'globalThis.keep = true;', 'maybe-react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="mystery"/);
});

test('does not remove a marked asset whose payload is not a trusted vendor', () => {
  const result = canonicalizeSynthetic({
    html: '<script src="react"></script>',
    assets: [vendorAsset('react', '/* data-pulp-vendor=react */ window.keep = true;', 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="react"/);
});

test('strict canonicalization rejects a padded vendor marker spoof', () => {
  const spoof = trustedReact() + 'globalThis.authored = true;';
  const result = canonicalizeMaterializedRuntimeDocument({
    html: '<script src="react"></script>',
    assets: [vendorAsset('react', spoof, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src="react"/);
});

test('canonicalizes application/javascript vendor assets consistently', () => {
  const source = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<script src="react"></script>',
    assets: [{ ...vendorAsset('react', source, 'react'),
      mime_type: 'application/javascript; charset=utf-8' }],
  });
  assert.equal(result.assets.length, 0);
  assert.doesNotMatch(result.html, /src="react"/);
});

test('preserves vendor assets referenced outside script tags', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<link rel="preload" href="react"><script src="react"></script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /href="react"/);
});

test('preserves vendor assets referenced by inline script bodies', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<script>globalThis.loadVendor("react");</script>' +
      '<script src="react"></script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /loadVendor\("react"\)/);
  assert.match(result.html, /src="react"/);
});

test('preserves vendor assets in unquoted and CSS references', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<img src=react><script src="react"></script>' +
      '<style>.x{background:url(react)}</style>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src=react/);
  assert.match(result.html, /url\(react\)/);
});

test('preserves vendor assets in an unquoted attribute reference alone', () => {
  const react = trustedReact();
  const result = canonicalizeSynthetic({
    html: '<img src=react><script src="react"></script>',
    assets: [vendorAsset('react', react, 'react')],
  });
  assert.equal(result.assets.length, 1);
  assert.match(result.html, /src=react/);
});
