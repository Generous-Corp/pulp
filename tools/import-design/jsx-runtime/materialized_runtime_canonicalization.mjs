import { transformSync } from 'esbuild';

function assetText(asset) {
  if (!asset || asset.mime_type !== 'text/javascript' ||
      typeof asset.data_base64 !== 'string') return '';
  return Buffer.from(asset.data_base64, 'base64').toString('utf8');
}
function nativeVendorKind(asset) {
  const text = assetText(asset);
  if (text.includes('@license React') && text.includes('react.development.js'))
    return 'react';
  if (text.includes('@license React') && text.includes('react-dom.development.js'))
    return 'react-dom';
  if (text.length > 1_000_000 && text.slice(0, 1000).includes('.Babel=') &&
      text.includes('transform')) return 'babel';
  return '';
}

// Runtime imports already install @pulp/react as React/ReactDOM. Compile the
// captured JSX at build time, then remove browser-only development React and
// Babel payloads rather than parsing several megabytes on every editor open.
export function canonicalizeMaterializedRuntimeDocument(document) {
  const assets = Array.isArray(document.assets) ? document.assets : [];
  const removable = new Map();
  for (const asset of assets) {
    const kind = nativeVendorKind(asset);
    if (kind) removable.set(String(asset.id), kind);
  }

  let babelCount = 0;
  let html = String(document.html || '').replace(
    /<script\b([^>]*)>([\s\S]*?)<\/script\s*>/gi,
    (whole, attributes, source) => {
      if (!/\btype\s*=\s*(["'])text\/(?:babel|jsx)\1/i.test(attributes))
        return whole;
      const compiled = transformSync(source, {
        loader: 'jsx',
        target: 'es2020',
        jsx: 'transform',
        jsxFactory: 'React.createElement',
        jsxFragment: 'React.Fragment',
        legalComments: 'none',
      }).code.replace(/<\/script/gi, '<\\/script');
      ++babelCount;
      const javascriptAttributes = attributes.replace(
        /\s*\btype\s*=\s*(["'])text\/(?:babel|jsx)\1/i, '');
      return `<script${javascriptAttributes} type="text/javascript">${compiled}</script>`;
    });

  // Babel is removable only after every JSX script has become ordinary JS.
  // React/ReactDOM are always redundant because the wrapper installs and
  // preserves the host reconciler before runtime import.
  const removableIds = new Set([...removable].filter(([, kind]) =>
    kind !== 'babel' || babelCount > 0).map(([id]) => id));
  html = html.replace(/<script\b([^>]*)><\/script\s*>/gi, (whole, attributes) => {
    const match = attributes.match(/\bsrc\s*=\s*(["'])([^"']+)\1/i);
    return match && removableIds.has(match[2]) ? '' : whole;
  });

  return {
    ...document,
    html,
    assets: assets.filter((asset) => !removableIds.has(String(asset.id))),
    runtime_canonicalization: {
      jsx_scripts_compiled: babelCount,
      browser_vendor_assets_removed: removableIds.size,
    },
  };
}
