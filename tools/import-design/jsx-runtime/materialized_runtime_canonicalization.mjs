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

function tagEnd(html, start) {
  let quote = '';
  for (let i = start; i < html.length; ++i) {
    const ch = html[i];
    if (quote) {
      if (ch === quote) quote = '';
    } else if (ch === '"' || ch === "'") {
      quote = ch;
    } else if (ch === '>') {
      return i + 1;
    }
  }
  return -1;
}

function attribute(openTag, wanted) {
  let i = openTag.toLowerCase().indexOf('script') + 6;
  while (i < openTag.length) {
    while (/\s/.test(openTag[i])) ++i;
    if (i >= openTag.length || openTag[i] === '>' || openTag[i] === '/') break;
    const nameStart = i;
    while (i < openTag.length && !/[\s=/>]/.test(openTag[i])) ++i;
    const name = openTag.slice(nameStart, i).toLowerCase();
    while (/\s/.test(openTag[i])) ++i;
    let value = '';
    if (openTag[i] === '=') {
      ++i;
      while (/\s/.test(openTag[i])) ++i;
      const quote = openTag[i] === '"' || openTag[i] === "'" ? openTag[i++] : '';
      const valueStart = i;
      if (quote) while (i < openTag.length && openTag[i] !== quote) ++i;
      else while (i < openTag.length && !/[\s>]/.test(openTag[i])) ++i;
      value = openTag.slice(valueStart, i);
      if (quote && openTag[i] === quote) ++i;
    }
    if (name === wanted) return { value, start: nameStart, end: i };
  }
  return null;
}

function rewriteScripts(html, rewrite) {
  const lower = html.toLowerCase();
  let cursor = 0;
  let output = '';
  while (cursor < html.length) {
    const start = lower.indexOf('<script', cursor);
    if (start < 0) return output + html.slice(cursor);
    const boundary = lower[start + 7];
    if (boundary && !/[\s/>]/.test(boundary)) {
      output += html.slice(cursor, start + 7);
      cursor = start + 7;
      continue;
    }
    const contentStart = tagEnd(html, start + 7);
    if (contentStart < 0) return output + html.slice(cursor);
    let close = lower.indexOf('</script', contentStart);
    while (close >= 0 && !/[\s>]/.test(lower[close + 8] || '>'))
      close = lower.indexOf('</script', close + 8);
    if (close < 0) return output + html.slice(cursor);
    const closeEnd = tagEnd(html, close + 8);
    if (closeEnd < 0) return output + html.slice(cursor);
    output += html.slice(cursor, start) + rewrite({
      openTag: html.slice(start, contentStart),
      source: html.slice(contentStart, close),
      whole: html.slice(start, closeEnd),
    });
    cursor = closeEnd;
  }
  return output;
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
  let html = rewriteScripts(String(document.html || ''),
    ({ whole, openTag, source }) => {
      const typeAttribute = attribute(openTag, 'type');
      const type = typeAttribute?.value.toLowerCase();
      if (type !== 'text/babel' && type !== 'text/jsx')
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
      const javascriptOpenTag = openTag.slice(0, typeAttribute.start) +
        openTag.slice(typeAttribute.end);
      return `${javascriptOpenTag.slice(0, -1)} type="text/javascript">${compiled}</script>`;
    });

  // Babel is removable only after every JSX script has become ordinary JS.
  // React/ReactDOM are always redundant because the wrapper installs and
  // preserves the host reconciler before runtime import.
  const removableIds = new Set([...removable].filter(([, kind]) =>
    kind !== 'babel' || babelCount > 0).map(([id]) => id));
  html = rewriteScripts(html, ({ whole, openTag, source }) => {
    const src = attribute(openTag, 'src')?.value;
    return source === '' && src !== undefined && removableIds.has(src) ? '' : whole;
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
