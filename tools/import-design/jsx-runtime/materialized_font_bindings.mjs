// SPDX-License-Identifier: MIT

const familyHead = (value) => String(value || '').split(',')[0]
  .trim().replace(/^(['"])(.*)\1$/, '$2');

export function parseUnicodeRange(value) {
  const ranges = [];
  for (const raw of String(value || '').split(',')) {
    const token = raw.trim().toUpperCase();
    if (!token) continue;
    const wildcard = token.match(/^U\+([0-9A-F]{1,5})(\?{1,5})$/);
    if (wildcard) {
      const lo = Number.parseInt(wildcard[1] + '0'.repeat(wildcard[2].length), 16);
      const hi = Number.parseInt(wildcard[1] + 'F'.repeat(wildcard[2].length), 16);
      if (!Number.isSafeInteger(lo) || !Number.isSafeInteger(hi) || hi > 0x10ffff)
        return null;
      ranges.push([lo, hi]);
      continue;
    }
    const explicit = token.match(/^U\+([0-9A-F]{1,6})(?:-([0-9A-F]{1,6}))?$/);
    if (!explicit) return null;
    const lo = Number.parseInt(explicit[1], 16);
    const hi = Number.parseInt(explicit[2] || explicit[1], 16);
    if (!Number.isSafeInteger(lo) || !Number.isSafeInteger(hi) ||
        lo > hi || hi > 0x10ffff) return null;
    ranges.push([lo, hi]);
  }
  return ranges;
}

function weightCovers(value, requested) {
  const numbers = String(value || 'normal').match(/\d+(?:\.\d+)?/g)?.map(Number) || [];
  if (numbers.length === 0) return String(value || '').trim().toLowerCase() === 'normal'
    ? requested === 400 : true;
  if (numbers.length === 1) return Math.round(numbers[0]) === requested;
  return requested >= numbers[0] && requested <= numbers[1];
}

export function normalizeMaterializedFontBindings(bindings) {
  if (!Array.isArray(bindings)) return [];
  return bindings.map((binding, index) => {
    if (!binding || typeof binding !== 'object' ||
        typeof binding.family !== 'string' || !binding.family ||
        typeof binding.asset_id !== 'string' || !binding.asset_id ||
        typeof binding.weight !== 'string' ||
        typeof binding.style !== 'string' ||
        (binding.runtime_family !== undefined &&
         (typeof binding.runtime_family !== 'string' || !binding.runtime_family)) ||
        (binding.unicode_range !== undefined &&
         typeof binding.unicode_range !== 'string')) {
      throw new Error(`materialized font binding ${index} is invalid`);
    }
    // Captures written before the private-family/font-subset contract already
    // contain the content-addressed face asset. Upgrade those sidecars at the
    // consumer boundary instead of requiring Chromium to be run again: the
    // alias is a pure function of captured data, and an omitted CSS
    // unicode-range means the face covers the complete Unicode space.
    const runtimeFamily = binding.runtime_family ||
      `${binding.family} [${binding.asset_id}]`;
    const unicodeRange = binding.unicode_range ?? '';
    const unicodeRanges = parseUnicodeRange(unicodeRange);
    if (unicodeRanges === null) {
      throw new Error(`materialized font binding ${index} unicode range is invalid`);
    }
    return { family: binding.family, runtime_family: runtimeFamily,
      weight: binding.weight, style: binding.style,
      unicode_ranges: unicodeRanges };
  });
}

export function runtimeFamilyForText(fontBindings, textBinding) {
  const requested = textBinding.basis.requested;
  const wantedFamily = familyHead(requested.font_family).toLowerCase();
  const wantedStyle = requested.font_slant === 0 ? 'normal'
    : requested.font_slant === 1 ? 'italic' : 'oblique';
  const codepoints = [...textBinding.text].map((character) => character.codePointAt(0));
  const face = fontBindings.find((candidate) =>
    candidate.family.toLowerCase() === wantedFamily &&
    (candidate.style.toLowerCase() === wantedStyle ||
     (wantedStyle === 'oblique' && candidate.style.toLowerCase() === 'italic')) &&
    weightCovers(candidate.weight, requested.font_weight) &&
    (candidate.unicode_ranges.length === 0 || codepoints.every((point) =>
      candidate.unicode_ranges.some(([lo, hi]) => point >= lo && point <= hi))));
  return face?.runtime_family || '';
}
