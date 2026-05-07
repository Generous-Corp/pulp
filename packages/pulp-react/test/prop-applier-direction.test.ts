// pulp #1434 Phase A2-3 — verify @pulp/react prop-applier forwards
// `writingDirection` keyword strings verbatim to the setDirection
// bridge fn. The CSS spec name `direction` already routes through
// FlexProps in this codebase (`FlexDirection` shorthand) — JSX
// surfaces only `writingDirection` to avoid the type-name conflict.
// The CSS-string-form path (`style.direction = 'rtl'`) goes through
// the el.style adapter and reaches setDirection separately.

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

describe('prop-applier writingDirection (pulp #1434 Phase A2-3)', () => {
    it('writingDirection "ltr" forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { writingDirection: 'ltr' });
        expect(bridge.calls.find((x) => x.fn === 'setDirection')?.args).toEqual(['k', 'ltr']);
    });

    it('writingDirection "rtl" forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { writingDirection: 'rtl' });
        expect(bridge.calls.find((x) => x.fn === 'setDirection')?.args).toEqual(['k', 'rtl']);
    });

    it('writingDirection "inherit" forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { writingDirection: 'inherit' });
        expect(bridge.calls.find((x) => x.fn === 'setDirection')?.args).toEqual(['k', 'inherit']);
    });

    it('writingDirection accepts RN-spec "auto" keyword', () => {
        applyChangedProps(makeInstance(), {}, { writingDirection: 'auto' });
        expect(bridge.calls.find((x) => x.fn === 'setDirection')?.args).toEqual(['k', 'auto']);
    });

    it('does NOT shadow FlexProps `direction` — that prop still routes to setFlex', () => {
        // FlexProps.direction (FlexDirection: 'row' | 'col') was wired
        // long before A2-3. Confirm it still goes to setFlex(direction)
        // and is NOT misrouted to setDirection.
        applyChangedProps(makeInstance(), {}, { direction: 'row' });
        expect(
            bridge.calls.find((x) => x.fn === 'setFlex' && x.args[1] === 'direction')?.args,
        ).toEqual(['k', 'direction', 'row']);
        expect(bridge.calls.find((x) => x.fn === 'setDirection')).toBeUndefined();
    });
});
