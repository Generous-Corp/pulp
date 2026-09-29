// Correctness of the paint-only commit path against the real importer runtime.
//
// A colour/opacity/fill/stroke-only or unread data-* commit no longer runs the
// captured-metadata pass. These cases prove the native end state is still the
// one the pass would have produced: captured values where the capture owns the
// channel, React's values everywhere else, and a real pass (plus epoch bump)
// whenever a captured-state selector could observe the change.

import { afterEach, describe, expect, it } from 'vitest';
import {
    CAPTURED_ICON, CAPTURED_OWNED_BUTTON_COLOR, OWNED_BUTTON_ROW, ROWS,
    createRig, type Rig,
} from './materialized-runtime-rig.js';

let rig: Rig | null = null;
afterEach(() => { rig?.teardown(); rig = null; });

const OWNED = `btn${OWNED_BUTTON_ROW}`;
const PAINT_VERBS = ['setTextColor', 'setOpacity', 'setSvgFill', 'setSvgStroke'];

const button = (color: string, extra: Record<string, unknown> = {}) => ({
    style: { color, borderColor: color, backgroundColor: '#111' },
    children: 'Btn', ...extra,
});

// The last value each paint verb left on every id: the native paint state.
function paintState(r: Rig): Map<string, unknown> {
    const state = new Map<string, unknown>();
    for (const call of r.calls) {
        if (!PAINT_VERBS.includes(call.verb)) continue;
        state.set(`${call.verb}:${String(call.args[0])}`, call.args[1]);
    }
    return state;
}

// What the full metadata pass leaves behind after the same commits. Paint
// state from the reconciled path must already equal it on every channel the
// pass owns -- otherwise skipping the pass changed what is on screen.
function expectFullPassAgrees(r: Rig): void {
    const before = paintState(r);
    const fullPass = r.entry.__pulpApplyMaterializedImportMetadata__ as
        (scope: unknown) => number;
    const mark = r.calls.length;
    fullPass(null);
    const written = new Map<string, unknown>();
    for (const call of r.calls.slice(mark)) {
        if (!PAINT_VERBS.includes(call.verb)) continue;
        written.set(`${call.verb}:${String(call.args[0])}`, call.args[1]);
    }
    expect(written.size).toBeGreaterThan(0);
    for (const [key, value] of written) expect(before.get(key)).toEqual(value);
}

describe('materialized paint-only commits', () => {
    it('hover in and out on an unowned button leaves React\'s colour', () => {
        rig = createRig();
        rig.commit('btn5', 'button', button('#aaa'), button('#fff'));
        expect(rig.last('setTextColor', 'btn5')).toBe('#fff');
        rig.commit('btn5', 'button', button('#fff'), button('#aaa'));
        expect(rig.last('setTextColor', 'btn5')).toBe('#aaa');
        expectFullPassAgrees(rig);
    });

    it('hover in and out on a button whose colour the capture owns keeps the capture', () => {
        rig = createRig();
        rig.commit(OWNED, 'button', button('#aaa'), button('#fff'));
        expect(rig.last('setTextColor', OWNED)).toBe(CAPTURED_OWNED_BUTTON_COLOR);
        // React's border colour is not a captured channel; it stands.
        expect(rig.last('setBorderColor', OWNED)).toBe('#fff');
        rig.commit(OWNED, 'button', button('#fff'), button('#aaa'));
        expect(rig.last('setTextColor', OWNED)).toBe(CAPTURED_OWNED_BUTTON_COLOR);
        expectFullPassAgrees(rig);
    });

    it('restores only the property that changed, on only that node', () => {
        rig = createRig();
        rig.reset();
        rig.commit('icon7', 'path', { fill: '#123' }, { fill: '#456' });
        const writes = rig.calls.map(call => `${call.verb}:${String(call.args[0])}`);
        // React's fill, then the captured fill. No opacity/colour/stroke
        // rewrite, nothing on any other node.
        expect(writes).toEqual(['setSvgFill:icon7', 'setSvgFill:icon7']);
        expect(rig.last('setSvgFill', 'icon7')).toBe(CAPTURED_ICON.fill);
    });

    it('restores captured opacity when React removes its own', () => {
        rig = createRig();
        rig.commit('icon2', 'path', { opacity: 0.2 }, {});
        expect(rig.last('setOpacity', 'icon2')).toBe(CAPTURED_ICON.opacity);
    });

    it('a theme switch across every row ends where a full pass would', () => {
        rig = createRig();
        const r = rig;
        const epoch = r.epoch();
        for (let row = 0; row < ROWS; ++row) {
            r.commit(`btn${row}`, 'button', button('#aaa'), button('#0f0'));
            r.commit(`label${row}`, 'span', { color: '#aaa', children: 'L' },
                { color: '#0f0', children: 'L' });
            r.commit(`icon${row}`, 'path', { fill: '#aaa', stroke: '#aaa', opacity: 1 },
                { fill: '#0f0', stroke: '#0f0', opacity: 0.5 });
        }
        expect(r.epoch()).toBe(epoch);
        expect(r.last('setTextColor', 'label4')).toBe('#0f0');
        expect(r.last('setSvgStroke', 'icon4')).toBe(CAPTURED_ICON.stroke);
        expect(r.last('setOpacity', 'icon4')).toBe(CAPTURED_ICON.opacity);
        expectFullPassAgrees(r);
    });

    it('a data-* change a captured-state selector reads still flips the state', () => {
        rig = createRig({ states: [{ id: 'menu', match: {
            selector: '[data-menu-open="true"]' } }] });
        const r = rig;
        const epoch = r.epoch() ?? 0;
        const refresh = r.entry.__pulpRefreshMaterializedState__ as () => string;
        expect(refresh()).toBe('');
        r.commit('btn5', 'button', { 'data-menu-open': 'false', children: 'Btn' },
            { 'data-menu-open': 'true', children: 'Btn' });
        expect(r.epoch()).toBe(epoch + 1);
        expect(refresh()).toBe('menu');
        r.commit('btn5', 'button', { 'data-menu-open': 'true', children: 'Btn' },
            { 'data-menu-open': 'false', children: 'Btn' });
        expect(refresh()).toBe('');
    });

    it('treats an attribute a runtime selector named later as observable', () => {
        rig = createRig();
        const r = rig;
        const find = r.entry.__pulpFindMaterializedElement__ as (s: string) => unknown;
        const epoch = r.epoch() ?? 0;
        r.commit('btn5', 'button', { 'data-late': 'a', children: 'Btn' },
            { 'data-late': 'b', children: 'Btn' });
        expect(r.epoch()).toBe(epoch);
        // A miss on this selector is now cached against the current epoch...
        expect(find('[data-late="c"]')).toBeNull();
        r.commit('btn5', 'button', { 'data-late': 'b', children: 'Btn' },
            { 'data-late': 'c', children: 'Btn' });
        // ...so this change must invalidate it, and the node must be found.
        expect(r.epoch()).toBe(epoch + 1);
        expect(find('[data-late="c"]')).toBe(r.node('btn5'));
    });

    it('a colour a selector names is not paint-only', () => {
        rig = createRig({ states: [{ id: 'lit', match: { selector: 'path[fill]' } }] });
        const r = rig;
        const epoch = r.epoch() ?? 0;
        r.commit('icon5', 'path', { fill: '#111' }, { fill: '#222' });
        expect(r.epoch()).toBe(epoch + 1);
    });

    it('a geometric change on an owned node re-applies its captured paint', () => {
        rig = createRig();
        const r = rig;
        r.commit(OWNED, 'button', button('#aaa', { width: 30 }),
            button('#fff', { width: 40 }));
        expect(r.last('setTextColor', OWNED)).toBe(CAPTURED_OWNED_BUTTON_COLOR);
    });
});
