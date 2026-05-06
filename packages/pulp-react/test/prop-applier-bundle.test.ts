// pulp #1434 small-wins bundle (Triage #7 + #14) — verify @pulp/react
// prop-applier forwards the new keyword vocabularies on the cursor and
// flexWrap surfaces.

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

function makeInstance(id: string = 'k', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

describe('flexWrap reverse (Triage #14)', () => {
    it('forwards "wrap-reverse" string verbatim', () => {
        applyChangedProps(makeInstance(), {}, { flexWrap: 'wrap-reverse' });
        const c = bridge.calls.find((x) => x.fn === 'setFlex' && x.args[1] === 'flex_wrap');
        expect(c?.args).toEqual(['k', 'flex_wrap', 'wrap-reverse']);
    });

    it('forwards "wrap" string verbatim', () => {
        applyChangedProps(makeInstance(), {}, { flexWrap: 'wrap' });
        const c = bridge.calls.find((x) => x.fn === 'setFlex' && x.args[1] === 'flex_wrap');
        expect(c?.args).toEqual(['k', 'flex_wrap', 'wrap']);
    });

    it('legacy boolean true → 1', () => {
        applyChangedProps(makeInstance(), {}, { flexWrap: true });
        const c = bridge.calls.find((x) => x.fn === 'setFlex' && x.args[1] === 'flex_wrap');
        expect(c?.args).toEqual(['k', 'flex_wrap', 1]);
    });

    it('legacy boolean false → 0', () => {
        applyChangedProps(makeInstance(), {}, { flexWrap: false });
        const c = bridge.calls.find((x) => x.fn === 'setFlex' && x.args[1] === 'flex_wrap');
        expect(c?.args).toEqual(['k', 'flex_wrap', 0]);
    });
});
