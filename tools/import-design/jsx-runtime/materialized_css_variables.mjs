// SPDX-License-Identifier: MIT

// Convert the browser-capture token envelope back into the CSS custom-property
// namespace consumed by executable React styles. DesignIR deliberately keys
// tokens as `css/name`; sourceIdentity retains the original `--name`, which is
// the authoritative spelling for var(--name) resolution.
export function materializedCssVariables(tokens) {
  const result = Object.create(null);
  if (!tokens || typeof tokens !== 'object') return result;

  const identities = tokens.sourceIdentity ?? tokens.source_identity ?? {};
  const categories = [
    ['colors', (value) => String(value)],
    ['dimensions', (value) => Number.isFinite(Number(value))
      ? `${Number(value)}px` : ''],
    ['strings', (value) => String(value)],
  ];
  for (const [category, encode] of categories) {
    const values = tokens[category];
    if (!values || typeof values !== 'object') continue;
    for (const [tokenName, rawValue] of Object.entries(values)) {
      const identity = identities[tokenName];
      const sourceId = typeof identity?.sourceId === 'string'
        ? identity.sourceId
        : typeof identity?.source_id === 'string'
          ? identity.source_id : '';
      const fallbackName = tokenName.startsWith('css/')
        ? `--${tokenName.slice(4)}` : '';
      const cssName = sourceId.startsWith('--') ? sourceId : fallbackName;
      if (!/^--[A-Za-z0-9_-]+$/.test(cssName)) continue;
      const value = encode(rawValue).trim();
      if (!value || value.length > 4096) continue;
      result[cssName.slice(2)] = value;
    }
  }
  return result;
}
