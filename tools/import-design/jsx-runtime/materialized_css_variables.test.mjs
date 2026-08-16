// SPDX-License-Identifier: MIT

import test from 'node:test';
import assert from 'node:assert/strict';
import { materializedCssVariables } from './materialized_css_variables.mjs';

test('materialized runtime restores captured CSS custom properties', () => {
  const vars = materializedCssVariables({
    colors: { 'css/accent': '#42d3ff' },
    dimensions: { 'css/radius': 12 },
    strings: {
      'css/mono': "'JetBrains Mono', ui-monospace, monospace",
      'css/sans': "'Inter', system-ui, sans-serif",
    },
    sourceIdentity: {
      'css/accent': { sourceId: '--accent' },
      'css/radius': { sourceId: '--radius' },
      'css/mono': { sourceId: '--mono' },
      'css/sans': { sourceId: '--sans' },
    },
  });
  assert.equal(Object.getPrototypeOf(vars), null);
  assert.deepEqual({ ...vars }, {
    accent: '#42d3ff',
    radius: '12px',
    mono: "'JetBrains Mono', ui-monospace, monospace",
    sans: "'Inter', system-ui, sans-serif",
  });
});

test('materialized CSS variables fall back to css/ keys and reject unsafe data', () => {
  const vars = materializedCssVariables({
    strings: {
      'css/valid-name': 'value',
      '__proto__': 'poison',
      'other/name': 'ignored',
      'css/oversized': 'x'.repeat(4097),
    },
  });
  assert.deepEqual({ ...vars }, { 'valid-name': 'value' });
  assert.equal(vars.__proto__, undefined);
});
