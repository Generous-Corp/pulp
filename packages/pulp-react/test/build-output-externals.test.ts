// Regression test for pulp #1292 — the React-dispatcher-null crash.
//
// Root cause: when @pulp/react was published with React (and friends)
// inlined inside dist/index.mjs, downstream consumers (esbuild bundling
// Spectr's editor.js) ended up with TWO copies of React in the final
// IIFE — one inside the pre-bundled @pulp/react.mjs and one resolved
// from the consumer's npm-installed `react` package via host-shims.
//
// React 18's dispatcher lives on a per-React-module singleton
// (`__SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED.ReactCurrentDispatcher`),
// so two React instances => two dispatchers. The reconciler set the
// dispatcher on its (bundled) React, but the user's App() called
// `useState` against the *other* React's dispatcher, which was still
// `null` => `cannot read property 'useState' of null` at boot.
//
// Fix: the pulp-react build externalizes `react`, `react-reconciler`,
// `react-reconciler/constants.js`, and `scheduler`. The published
// `dist/index.mjs` keeps `import` declarations for those packages, so
// downstream esbuild can dedupe them against the consumer's own
// `node_modules/react` resolution. One React in the final bundle =>
// one dispatcher => no crash.
//
// This test guards the build output: if any of the four packages get
// inlined back into the bundle (because someone removed an
// `--external:` flag from the build script), the test fails before
// the regression hits Spectr.

import { describe, it, expect } from 'vitest';
import { readFileSync, statSync } from 'node:fs';
import { resolve } from 'node:path';

const distMjs = resolve(__dirname, '..', 'dist', 'index.mjs');

describe('@pulp/react build output (pulp #1292)', () => {
  it('dist/index.mjs exists and is small (no React inlined)', () => {
    const st = statSync(distMjs);
    // After externalizing react / react-reconciler / scheduler the bundle
    // is ~27 KB. If anyone re-inlines react it jumps to ~950 KB. A 100 KB
    // ceiling gives us comfortable headroom for the non-React surface to
    // grow without letting React back in.
    expect(st.size).toBeLessThan(100 * 1024);
  });

  it('keeps `react` as an external ESM import (not inlined)', () => {
    const src = readFileSync(distMjs, 'utf8');
    // No copy of react.production.min.js shipped inside the bundle.
    expect(src).not.toMatch(/react\.production\.min\.js/);
    // No ReactCurrentDispatcher object literal — that's the React
    // module-internal singleton that pulp #1292 was creating two of.
    expect(src).not.toMatch(/ReactCurrentDispatcher\s*[:=]\s*\{/);
  });

  it('keeps `react-reconciler` as an external ESM import (not inlined)', () => {
    const src = readFileSync(distMjs, 'utf8');
    // Verify the import declaration survives the build.
    expect(src).toMatch(/from\s+["']react-reconciler["']/);
    expect(src).toMatch(/from\s+["']react-reconciler\/constants\.js["']/);
    // No copy of react-reconciler's source bundled inline.
    expect(src).not.toMatch(/react-reconciler\.production\.min\.js/);
    expect(src).not.toMatch(/react-reconciler\.development\.js/);
  });

  it('keeps `scheduler` as an external import (not inlined)', () => {
    const src = readFileSync(distMjs, 'utf8');
    // scheduler is pulled in transitively by react-reconciler. If it
    // gets inlined here, downstream esbuild has to dedup it manually.
    expect(src).not.toMatch(/scheduler\.production\.min\.js/);
    expect(src).not.toMatch(/scheduler\.development\.js/);
  });
});
