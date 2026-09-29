// The @pulp/react runtime revision an emitted bundle was built from.
//
// Materialized and JSX imports vendor a whole @pulp/react bundle into the app
// that ships them, and nothing rebuilds it when the SDK improves: an app keeps
// the runtime it was imported with until someone re-runs the transform. The
// banner written here is how that app (and `check_vendored_runtime.py` /
// `pulp_check_vendored_react_runtime()`) can tell its copy is older than the
// SDK it now builds against. The revision and the fixes it names live in
// packages/pulp-react/runtime-fingerprint.json; bump both there when a runtime
// change is worth a refresh.

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

export const DEFAULT_FINGERPRINT_PATH = resolve(
  here, '..', '..', '..', 'packages', 'pulp-react', 'runtime-fingerprint.json');

export function loadRuntimeFingerprint(path = DEFAULT_FINGERPRINT_PATH) {
  const manifest = JSON.parse(readFileSync(path, 'utf8'));
  if (!Number.isInteger(manifest.revision) || manifest.revision < 1) {
    throw new Error(`${path}: revision must be a positive integer`);
  }
  return manifest;
}

// One comment line the checkers parse, plus a global a live realm can report.
// Emitted as an esbuild banner, so it precedes the bundle's IIFE wrapper.
export function runtimeFingerprintBanner(manifest = loadRuntimeFingerprint()) {
  const revision = manifest.revision;
  return `/* @pulp/react runtime revision ${revision} */\n` +
    `globalThis.__PULP_REACT_RUNTIME_REVISION__ = ${revision};`;
}
