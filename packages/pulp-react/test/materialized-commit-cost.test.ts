// Per-commit cost of a materialized import, counted in native bridge calls.
//
// Each case drives a React commit through the real host config against the
// real generated importer runtime (see materialized-runtime-rig.ts) and counts
// what that commit made the bridge do. Counts, not timings, are asserted: a
// wall-clock budget flakes on a shared runner and cannot say which cost moved.
// The timing is printed beside the counts as a benchmark reading only.

import { afterEach, describe, expect, it } from 'vitest';
import { createRig, type Rig } from './materialized-runtime-rig.js';

let rig: Rig | null = null;
afterEach(() => { rig?.teardown(); rig = null; });

const HOVER_OUT = { style: { backgroundColor: '#222', borderColor: '#333',
    color: '#aaa', cursor: 'default' }, 'data-hover': 'false', children: 'Btn' };
const HOVER_IN = { style: { backgroundColor: '#444', borderColor: '#666',
    color: '#fff', cursor: 'pointer' }, 'data-hover': 'true', children: 'Btn' };

interface Reading {
    calls: number; layoutReads: number; typography: number;
    childReads: number; epochBumps: number; usPerCommit: number;
}

// Mean over `rounds` hover-in/hover-out pairs, per commit.
function measure(r: Rig, commitPair: () => void, rounds = 100): Reading {
    commitPair(); // settle any first-commit work out of the reading
    r.reset();
    const epoch0 = r.epoch() ?? 0;
    const start = process.hrtime.bigint();
    for (let i = 0; i < rounds; ++i) commitPair();
    const elapsed = Number(process.hrtime.bigint() - start) / 1000;
    const commits = rounds * 2;
    const per = (n: number) => Math.round((n / commits) * 10) / 10;
    return {
        calls: per(r.count()),
        layoutReads: per(r.count('getLayoutBoxMetrics')),
        typography: per(r.count('setFontFamily') + r.count('setFontSize')
            + r.count('setFontWeight') + r.count('setFontStyle')
            + r.count('setLetterSpacing')),
        childReads: per(r.childReads()),
        epochBumps: per((r.epoch() ?? 0) - epoch0),
        usPerCommit: Math.round(elapsed / commits),
    };
}

function report(label: string, reading: Reading): Reading {
    console.log(`[materialized-commit-cost] ${label} ${JSON.stringify(reading)}`);
    return reading;
}

const hoverPair = (r: Rig, id: string) => () => {
    r.commit(id, 'button', HOVER_OUT, HOVER_IN);
    r.commit(id, 'button', HOVER_IN, HOVER_OUT);
};

describe('materialized per-commit cost', () => {
    it('a button hover that recolours text, border and fill is paint-only', () => {
        rig = createRig();
        const reading = report('hover(unowned button)', measure(rig, hoverPair(rig, 'btn5')));
        // React's own setters only: background, border colour, text colour,
        // cursor. No captured-metadata pass and no epoch bump.
        expect(reading.layoutReads).toBe(0);
        expect(reading.typography).toBe(0);
        expect(reading.childReads).toBe(0);
        expect(reading.epochBumps).toBe(0);
        expect(reading.calls).toBeLessThanOrEqual(5);
    });

    it('a hover on a button whose colour the capture owns reconciles one property', () => {
        rig = createRig();
        const reading = report('hover(owned button)', measure(rig, hoverPair(rig, 'btn3')));
        expect(reading.layoutReads).toBe(0);
        expect(reading.typography).toBe(0);
        expect(reading.epochBumps).toBe(0);
        // React's four setters plus exactly one captured-colour write.
        expect(reading.calls).toBeLessThanOrEqual(6);
    });

    it('an icon fill change the capture owns reconciles only that channel', () => {
        rig = createRig();
        const r = rig;
        const reading = report('icon fill(owned)', measure(r, () => {
            r.commit('icon5', 'path', { fill: '#111' }, { fill: '#999' });
            r.commit('icon5', 'path', { fill: '#999' }, { fill: '#111' });
        }));
        expect(reading.layoutReads).toBe(0);
        expect(reading.epochBumps).toBe(0);
        expect(reading.calls).toBeLessThanOrEqual(2);
    });

    it('a data-* change no selector reads is paint-only', () => {
        rig = createRig({ states: [{ id: 'menu', match: {
            selector: '[data-menu-open="true"]' } }] });
        const r = rig;
        const reading = report('data-hover only', measure(r, () => {
            r.commit('btn5', 'button', { 'data-hover': 'false', children: 'Btn' },
                { 'data-hover': 'true', children: 'Btn' });
            r.commit('btn5', 'button', { 'data-hover': 'true', children: 'Btn' },
                { 'data-hover': 'false', children: 'Btn' });
        }));
        expect(reading.layoutReads).toBe(0);
        expect(reading.epochBumps).toBe(0);
        expect(reading.calls).toBe(0);
    });

    it('a geometric change still re-applies its scope, walking each node once', () => {
        rig = createRig();
        const r = rig;
        const reading = report('width(scoped pass)', measure(r, () => {
            r.commit('btn5', 'button', { width: 30, children: 'Btn' },
                { width: 40, children: 'Btn' });
            r.commit('btn5', 'button', { width: 40, children: 'Btn' },
                { width: 30, children: 'Btn' });
        }, 50));
        // CONTROL: the same instrument must see a pass here. A zero would
        // mean the counters are broken, not that the gate works.
        expect(reading.epochBumps).toBe(1);
        expect(reading.layoutReads).toBeGreaterThan(0);
        // 1 root + 24 rows + 24*3 row children + 24 svg children = 121 nodes.
        expect(reading.childReads).toBeLessThanOrEqual(121);
    });

    it('a full application walks each registry node at most once', () => {
        rig = createRig();
        const r = rig;
        r.reset();
        (r.entry.__pulpApplyMaterializedImportMetadata__ as (s: unknown) => number)(null);
        const childReads = r.childReads();
        const typography = r.count('setFontFamily');
        console.log('[materialized-commit-cost] full pass ' + JSON.stringify({
            childReads, calls: r.count(), typography }));
        expect(childReads).toBeLessThanOrEqual(121);
    });
});
