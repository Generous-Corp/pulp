// CSS-canonical `color` must fan out to the same setTextColor bridge
// call as the RN-canonical `textColor`. JSX styles authored by every
// HTML/Tailwind/Stitch/Figma export use `color` — the dispatch case
// was missing the alias, so 39 occurrences in Spectr's editor.html
// silently dropped at the prop-applier and no text picked up the
// authored colour.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
});

function makeInstance(id: string = 'k', type: string = 'Label'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function setTextColorCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'setTextColor');
}

describe('prop-applier color → textColor alias', () => {
    it('CSS `color` dispatches setTextColor', () => {
        applyChangedProps(makeInstance(), {}, { color: '#ff0000' });
        const calls = setTextColorCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', '#ff0000']);
    });

    it('RN `textColor` still dispatches setTextColor (no regression)', () => {
        applyChangedProps(makeInstance(), {}, { textColor: '#00ff00' });
        const calls = setTextColorCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', '#00ff00']);
    });

    it('color override (next render) re-dispatches with new value', () => {
        const inst = makeInstance();
        applyChangedProps(inst, {}, { color: 'red' });
        applyChangedProps(inst, { color: 'red' }, { color: 'blue' });
        const calls = setTextColorCalls(bridge);
        expect(calls).toHaveLength(2);
        expect(calls[1].args).toEqual(['k', 'blue']);
    });
});
