import { defineConfig } from 'vitest/config';

// Vitest v8 coverage wired for pulp #1886 Phase 1 (measure-only).
// The native diff-coverage gate runs against a Cobertura XML, so v8
// provider + cobertura reporter keeps the JS/TS lane shape-compatible
// with the existing aggregation in tools/scripts/coverage_tier_check.py
// without changing the gate semantics. `text` keeps a human-readable
// summary in CI logs.
//
// Phase 1: no thresholds; the workflow uploads the artifact and Codecov
// flag `pulp-react`, nothing fails. Phase 2 will layer per-tier
// enforcement once a baseline is established.
export default defineConfig({
  test: {
    include: ['test/**/*.test.ts'],
    environment: 'node',
    coverage: {
      provider: 'v8',
      reporter: ['text', 'cobertura'],
      reportsDirectory: './coverage',
      include: ['src/**/*.{ts,tsx,js,jsx}'],
      exclude: [
        'dist/**',
        'node_modules/**',
        'test/**',
        '**/*.d.ts',
      ],
    },
  },
});
