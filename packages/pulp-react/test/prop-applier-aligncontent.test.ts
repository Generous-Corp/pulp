// pulp #1434 (sub-agent #12 follow-up) — verify the @pulp/react
// prop-applier forwards `alignContent` to the bridge.
//
// align-content is multi-line flex cross-axis distribution. Yoga
// supports it natively via YGNodeStyleSetAlignContent; before this
// batch the bridge had no `align_content` case in setFlex and the
// type didn't expose `alignContent` on FlexProps. RN snippets
// emitting `style={{ flexWrap: 'wrap', alignContent: 'space-between'
// }}` silently dropped the alignment entirely.
//
// The TS surface accepts both bare (`start`/`end`) and prefixed
// (`flex-start`/`flex-end`) spellings plus the three space-* values;
// the bridge does the same mapping. This test asserts that every
// value flows through verbatim — string normalization happens in C++,
// not at the JSX boundary.

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

function flexCalls(b: MockBridge, slot: string) {
    return b.calls.filter((c) => c.fn === 'setFlex' && c.args[1] === slot);
}

describe('prop-applier alignContent (pulp #1434 sub-agent #12 follow-up)', () => {
    it.each([
        'start',
        'flex-start',
        'end',
        'flex-end',
        'center',
        'stretch',
        'space-between',
        'space-around',
        'space-evenly',
    ])('forwards %s verbatim to setFlex(align_content)', (value) => {
        applyChangedProps(makeInstance(), {}, { alignContent: value as never });
        const calls = flexCalls(bridge, 'align_content');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', 'align_content', value]);
    });
});
