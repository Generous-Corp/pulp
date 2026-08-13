// SPDX-License-Identifier: MIT
import assert from 'node:assert/strict';
import test from 'node:test';
import { normalizeRequestedTypography } from './materialized_text_contract.mjs';

test('upgrades legacy Chromium CSS typography evidence deterministically', () => {
  assert.deepEqual(normalizeRequestedTypography({
    font_family: '"JetBrains Mono", monospace', font_size: '11px',
    font_weight: '600', font_style: 'normal',
  }), { font_family: '"JetBrains Mono", monospace', font_size: 11,
    font_weight: 600, font_slant: 0 });
});

test('preserves normalized typography and rejects ambiguous CSS values', () => {
  assert.deepEqual(normalizeRequestedTypography({ font_family: 'Inter',
    font_size: 12, font_weight: 400, font_slant: 1 }),
    { font_family: 'Inter', font_size: 12, font_weight: 400, font_slant: 1 });
  assert.equal(normalizeRequestedTypography({ font_family: 'Inter',
    font_size: 'medium', font_weight: 'bolder', font_style: 'normal' }), null);
});
