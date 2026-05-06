// pulp #1499 follow-up — Codex P2: the package's `exports` map points
// `import` to `./dist/index.mjs`, but the original `build` script only
// ran `tsc` (which emits `.js`, not `.mjs`). Consumers importing
// @pulp/import-ir through package exports failed at runtime with a
// missing-module error.
//
// The fix extends the build script to also produce dist/index.mjs via
// esbuild. This test guards against the build script regressing — if
// dist/index.mjs disappears, downstream ESM consumers break silently
// at runtime.
//
// The test runs after `pnpm build`. CI invokes `pnpm build && pnpm
// test`, so the artifact is always present in CI; locally, run the
// build first.

import { describe, it, expect } from 'vitest';
import { existsSync, statSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const distMjs = resolve(__dirname, '..', 'dist', 'index.mjs');
const distJs = resolve(__dirname, '..', 'dist', 'index.js');
const distDts = resolve(__dirname, '..', 'dist', 'index.d.ts');

describe('@pulp/import-ir build output (#1499 follow-up)', () => {
    it('emits dist/index.mjs that the package exports map references', () => {
        expect(existsSync(distMjs)).toBe(true);
        const st = statSync(distMjs);
        // Sanity: bundle is non-trivial (the package exports several
        // modules' worth of code). 1 KB ceiling would be too tight,
        // 1 MB ceiling would mask accidental dep inlining.
        expect(st.size).toBeGreaterThan(1024);
        expect(st.size).toBeLessThan(500 * 1024);
    });

    it('emits dist/index.js (CommonJS-shaped tsc output) for `require` consumers', () => {
        expect(existsSync(distJs)).toBe(true);
    });

    it('emits dist/index.d.ts so TypeScript consumers see types', () => {
        expect(existsSync(distDts)).toBe(true);
    });

    it('dist/index.mjs is ESM-shaped (uses `export` declarations)', () => {
        const src = readFileSync(distMjs, 'utf8');
        // esbuild emits an ESM bundle with `export` statements at the
        // top-level. If anyone swapped it back to CJS the export map
        // would resolve to a malformed module.
        expect(src).toMatch(/\bexport\b/);
    });
});
