// Derived-style prop sets must not be erased by the removal walk.
//
// `normalizeHostProps` hoists `style` / `className` into flat visual props
// (`background`, `border*`, `textColor`). An element rendered WITHOUT a
// style source keeps none of those keys — they are ABSENT, not removed. If
// `applyChangedProps` reads that absence as a deletion it hard-resets the
// element to `setBackground(id,"transparent")` + `setBorderWidth(id,0)` +
// `setTextColor(id,"")`, and the whole box disappears in one frame. That is
// the "chip vanishes on hover" failure: a hover re-render emits the element
// with a plain prop set and the fill, border and text all go at once.
//
// The contract pinned here:
//   - derived (style/className) -> raw shape: derived visual keys are NOT
//     reset; non-visual removals (visible/opacity) still are
//   - derived -> derived with a key genuinely dropped: reset still fires,
//     so the conditional-spread contract in
//     prop-applier-disappearing-style.test.ts is preserved
//   - raw -> raw: unchanged legacy behaviour
//   - the shape marker itself never reaches a bridge setter

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import {
    applyAllProps,
    applyChangedProps,
    normalizeHostProps,
    setClassRulesProvider,
} from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
    setClassRulesProvider(null);
});

function makeInstance(
    id = 'chip',
    type = 'View',
    props: Record<string, unknown> = {},
): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props,
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

const destructive = (b: MockBridge) => b.calls.filter((c) =>
    (c.fn === 'setBackground' && c.args[1] === 'transparent')
    || (c.fn === 'setBorderWidth' && c.args[1] === 0)
    || (c.fn === 'setTextColor' && c.args[1] === '')
    || c.fn === 'setBorderSide');

describe('@pulp/react prop-applier — derived-style shape stability', () => {
    it('does not erase a chip when the re-render drops its style object', () => {
        // The whole defect in one assertion: same element, first rendered
        // with a style object, re-rendered without one.
        const oldN = normalizeHostProps('View', {
            style: {
                background: '#2b3a4a',
                borderColor: '#5f7fa0',
                borderWidth: 1,
                textColor: '#e8eef6',
            },
            'data-option': 'liquid',
        });
        const newN = normalizeHostProps('View', { 'data-option': 'liquid' });

        applyChangedProps(makeInstance('chip-liquid'), oldN, newN);

        expect(destructive(bridge)).toEqual([]);
    });

    it('does not erase when a className token stops resolving', () => {
        // The className path flattens too, so a resolvable -> unresolvable
        // token still leaves both sides derived and the reset still fires.
        // The vanish case is the token disappearing altogether.
        setClassRulesProvider((tok) =>
            tok === 'chip-active' ? { background: '#3a7', borderColor: '#fff' } : null);
        const oldN = normalizeHostProps('View', { className: 'chip-active' });
        const newN = normalizeHostProps('View', {});

        applyChangedProps(makeInstance('chip-cls'), oldN, newN);

        expect(destructive(bridge)).toEqual([]);
    });

    it('still resets non-visual removals across a shape change', () => {
        // The guard is scoped to derived VISUAL keys. `visible` / `opacity`
        // are real state and must still clear, or a hidden element stays
        // hidden forever.
        const oldN = normalizeHostProps('View', {
            style: { background: 'red' },
            visible: false,
            opacity: 0.2,
        });
        const newN = normalizeHostProps('View', {});

        applyChangedProps(makeInstance('n1'), oldN, newN);

        expect(bridge.calls.filter((c) => c.fn === 'setVisible')[0]?.args)
            .toEqual(['n1', true]);
        expect(bridge.calls.filter((c) => c.fn === 'setOpacity')[0]?.args)
            .toEqual(['n1', 1.0]);
        expect(destructive(bridge)).toEqual([]);
    });

    it('still resets when both sides are derived and a key is dropped', () => {
        // Regression guard for the conditional-spread contract:
        //   style={{ ...base, ...(active ? activeStyle : {}) }}
        // Both renders carry a style object, so the removal is authored.
        const oldN = normalizeHostProps('View', {
            style: { padding: 8, background: '#3a7', borderColor: '#fff' },
        });
        const newN = normalizeHostProps('View', { style: { padding: 8 } });

        applyChangedProps(makeInstance('chip-spread'), oldN, newN);

        const bg = bridge.calls.filter((c) => c.fn === 'setBackground');
        const bw = bridge.calls.filter((c) => c.fn === 'setBorderWidth');
        expect(bg.map((c) => c.args)).toEqual([['chip-spread', 'transparent']]);
        expect(bw.map((c) => c.args)).toEqual([['chip-spread', 0]]);
    });

    it('still resets when neither side came from a style source', () => {
        // Flat-prop callers (native intrinsics, hand-written JSX) are
        // unaffected by the marker.
        applyChangedProps(makeInstance('flat'), { background: 'red' }, {});
        expect(bridge.calls.filter((c) => c.fn === 'setBackground').map((c) => c.args))
            .toEqual([['flat', 'transparent']]);
    });

    it('keeps the shape marker out of key walks and bridge setters', () => {
        const out = normalizeHostProps('View', { style: { background: 'red' } });
        expect(Object.keys(out)).toEqual(['background']);
        expect(JSON.stringify(out)).toBe('{"background":"red"}');

        applyAllProps(makeInstance('m1', 'View', out));
        expect(bridge.calls.some((c) =>
            c.args.some((a) => String(a).includes('__pulpDerivedStyle')))).toBe(false);
    });
});
