import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;
beforeEach(() => { bridge = createMockBridge(); bridge.install(); });
afterEach(() => { bridge.uninstall(); });

function makeInstance(type: string): PulpInstance {
    return { id: 's', type: type as PulpInstance['type'], props: {},
             childIds: [], onBridge: true, pendingChildren: [] };
}

function scrollCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'scrollTo').map((c) => c.args);
}

describe('prop-applier ScrollView offset', () => {
    it('forwards scrollTop as a vertical offset', () => {
        applyChangedProps(makeInstance('ScrollView'), {}, { scrollTop: 618 });
        expect(scrollCalls(bridge)).toEqual([['s', 0, 618]]);
    });

    it('forwards scrollLeft as a horizontal offset', () => {
        applyChangedProps(makeInstance('ScrollView'), {}, { scrollLeft: 40 });
        expect(scrollCalls(bridge)).toEqual([['s', 40, 0]]);
    });

    it('accepts a numeric string, as the DOM property does', () => {
        applyChangedProps(makeInstance('ScrollView'), {}, { scrollTop: '120' });
        expect(scrollCalls(bridge)).toEqual([['s', 0, 120]]);
    });

    // Zero is the value a "scroll back to the top" control sends, and a
    // falsy-guard would swallow exactly that one.
    it('forwards zero rather than treating it as absent', () => {
        applyChangedProps(makeInstance('ScrollView'), {}, { scrollTop: 0 });
        expect(scrollCalls(bridge)).toEqual([['s', 0, 0]]);
    });

    // The prop is type-dispatched: only a ScrollView can scroll, and silently
    // calling scrollTo on a Panel would move whatever ScrollView happened to
    // share that id in the wrapper map.
    it('ignores the prop on a widget that cannot scroll', () => {
        applyChangedProps(makeInstance('Panel'), {}, { scrollTop: 618 });
        expect(scrollCalls(bridge)).toEqual([]);
    });
});
