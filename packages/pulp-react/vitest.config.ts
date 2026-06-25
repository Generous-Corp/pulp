import { defineConfig } from 'vitest/config';

// Vitest v8 coverage emits Cobertura XML for the native diff-coverage
// aggregation in tools/scripts/coverage_tier_check.py. `text` keeps a
// human-readable summary in CI logs.
//
// Thresholds intentionally live outside this package config; the workflow
// uploads the `pulp-react` artifact and Codecov flag, while repository-level
// gates decide whether a coverage result is enforceable.
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
