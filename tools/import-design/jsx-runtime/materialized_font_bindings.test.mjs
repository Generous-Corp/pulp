// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  normalizeMaterializedFontBindings,
  parseUnicodeRange,
  runtimeFamilyForText,
} from './materialized_font_bindings.mjs';

const faces = normalizeMaterializedFontBindings([
  { family: 'Captured Mono', runtime_family: 'Captured Mono [cyrillic]',
    asset_id: 'cyrillic', weight: '300 700', style: 'normal',
    unicode_range: 'U+0400-04FF' },
  { family: 'Captured Mono', runtime_family: 'Captured Mono [latin]',
    asset_id: 'latin', weight: '300 700', style: 'normal',
    unicode_range: 'U+0000-00FF, U+20AC' },
]);

const text = (value, weight = 500) => ({ text: value, basis: { requested: {
  font_family: "'Captured Mono', monospace", font_weight: weight, font_slant: 0,
} } });

test('selects the exact packaged unicode subset Chromium shaped', () => {
  assert.equal(runtimeFamilyForText(faces, text('Settings')),
    '"Captured Mono [latin]"');
  assert.equal(runtimeFamilyForText(faces, text('Настройки')),
    '"Captured Mono [cyrillic]"');
});

test('builds a captured subset stack for mixed-script text', () => {
  assert.equal(runtimeFamilyForText(faces, text('AЖ')),
    '"Captured Mono [latin]", "Captured Mono [cyrillic]"');
  assert.equal(runtimeFamilyForText(faces, text('Settings', 800)), '');
});

test('deduplicates subset aliases while preserving first-glyph order', () => {
  assert.equal(runtimeFamilyForText(faces, text('ЖAБZ')),
    '"Captured Mono [cyrillic]", "Captured Mono [latin]"');
});

test('preserves the exact Chromium platform fallback for uncovered glyphs', () => {
  const mixed = text('PRESETS ▾');
  mixed.basis.resolved_faces = [
    { family_name: 'Captured Mono', post_script_name: 'CapturedMono-Regular',
      is_custom_font: true, glyph_count: 8 },
    { family_name: 'Menlo', post_script_name: 'Menlo-Regular',
      is_custom_font: false, glyph_count: 1 },
  ];
  assert.equal(runtimeFamilyForText(faces, mixed),
    '"Captured Mono [latin]", "Menlo"');

  const unresolved = text('PRESETS ▾');
  assert.equal(runtimeFamilyForText(faces, unresolved), '');
});

test('parses explicit and wildcard CSS unicode ranges fail closed', () => {
  assert.deepEqual(parseUnicodeRange('U+0000-00FF, U+4??'),
    [[0x0000, 0x00ff], [0x400, 0x4ff]]);
  assert.equal(parseUnicodeRange('not-a-range'), null);
  assert.equal(parseUnicodeRange('U+110000'), null);
});

test('upgrades pre-subset captures from their content-addressed font asset', () => {
  const legacy = normalizeMaterializedFontBindings([
    { family: 'Inter', asset_id: 'pulp-materialized-asset-deadbeef',
      weight: '400', style: 'normal' },
  ]);
  assert.equal(legacy[0].runtime_family,
    'Inter [pulp-materialized-asset-deadbeef]');
  assert.deepEqual(legacy[0].unicode_ranges, []);
  const legacyText = { text: 'Spectr', basis: { requested: {
    font_family: 'Inter, sans-serif', font_weight: 400, font_slant: 0,
  } } };
  assert.equal(runtimeFamilyForText(legacy, legacyText),
    '"Inter [pulp-materialized-asset-deadbeef]"');
});
