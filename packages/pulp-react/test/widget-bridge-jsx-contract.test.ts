// JSX-reachability contract.
//
// The C++ test_widget_bridge_api_contracts.cpp checks that every native
// bridge fn in core/view/src/widget_bridge_api_manifest.tsv is registered
// in a registrar source — but it does NOT check the @pulp/react side. That
// gap is exactly how `setSvgFillGradient` shipped half-wired: a real C++
// bridge fn + widget slot that no JSX prop could reach.
//
// This test closes the loop for the manifest's `jsx`-tagged rows: every fn
// tagged `prop:Type.name` must be reachable from JSX (generated mock allowlist +
// a prop-applier `call('<fn>')` dispatch + a typed prop in types.ts), and
// every `factory:`/`geometry:` fn must at least be on the allowlist. The
// reverse is checked too: every `call('setSvg…' | 'createSvg…')` the
// prop-applier emits must exist in the manifest.
//
// Today only the `svg` surface is annotated; the contract extends to other
// surfaces as their rows gain `jsx` tags. Untagged rows are skipped.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '../../..');
const read = (rel: string) => readFileSync(resolve(repoRoot, rel), 'utf8');

const manifestText = read('core/view/src/widget_bridge_api_manifest.tsv');
const propApplier = read('packages/pulp-react/src/prop-applier.ts');
const bridgeMockFunctions = read('packages/pulp-react/src/bridge-mock-functions.generated.ts');
const types = read('packages/pulp-react/src/types.ts');

interface Row { name: string; category: string; jsx: string; }

const rows: Row[] = manifestText
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0 && !l.startsWith('#'))
    .map((l) => l.split('\t'))
    .filter((f) => f[0] !== 'name') // header
    .map((f) => ({ name: f[0], category: f[1], jsx: f[4] ?? '' }));

const tagged = rows.filter((r) => r.jsx.length > 0);

// Quote either spelling: call('fn' ... or call("fn" ...
const allowlisted = (fn: string) => bridgeMockFunctions.includes(`'${fn}'`) || bridgeMockFunctions.includes(`"${fn}"`);
const dispatched = (fn: string) => propApplier.includes(`'${fn}'`) || propApplier.includes(`"${fn}"`);

describe('WidgetBridge ↔ @pulp/react JSX-reachability contract', () => {
    it('has jsx-tagged rows to check (sanity)', () => {
        expect(tagged.length).toBeGreaterThan(0);
    });

    describe('prop: tags are fully reachable from JSX', () => {
        for (const r of tagged.filter((t) => t.jsx.startsWith('prop:'))) {
            it(`${r.name} (${r.jsx})`, () => {
                // The generated mock-allowlist must expose the fn.
                expect(allowlisted(r.name), `${r.name} missing from generated mock allowlist`).toBe(true);
                // prop-applier must dispatch to it
                expect(dispatched(r.name), `${r.name} has no prop-applier call()`).toBe(true);
                // types.ts must declare the prop on a Props interface
                const prop = r.jsx.split('.')[1];
                expect(prop, `malformed prop tag ${r.jsx}`).toBeTruthy();
                expect(
                    types.includes(`${prop}?:`),
                    `prop '${prop}' (for ${r.name}) not declared in types.ts`,
                ).toBe(true);
            });
        }
    });

    describe('factory:/geometry: tags are on the bridge allowlist', () => {
        for (const r of tagged.filter((t) => t.jsx.startsWith('factory:') || t.jsx.startsWith('geometry:'))) {
            it(`${r.name} (${r.jsx})`, () => {
                expect(allowlisted(r.name), `${r.name} missing from generated mock allowlist`).toBe(true);
            });
        }
    });

    it('every svg call() the prop-applier emits exists in the manifest', () => {
        const manifestNames = new Set(rows.map((r) => r.name));
        const called = new Set<string>();
        const re = /call\(\s*['"]((?:setSvg|createSvg)\w+)['"]/g;
        for (let m = re.exec(propApplier); m; m = re.exec(propApplier)) {
            called.add(m[1]);
        }
        expect(called.size).toBeGreaterThan(0);
        for (const fn of called) {
            expect(manifestNames.has(fn), `prop-applier calls '${fn}' which is not in the manifest`).toBe(true);
        }
    });
});
