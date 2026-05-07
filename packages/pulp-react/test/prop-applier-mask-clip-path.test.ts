// pulp #1515 — CSS clip-path + mask cluster. Verify @pulp/react
// prop-applier forwards `clipPath` / `mask` / `maskImage` to the
// matching bridge fns. The bridge stores the value on the View;
// Skia paint side honors the `path("...")` form via
// SkPath::FromSVGString. URL refs / named shape forms (circle / inset
// / polygon) and the saveLayer + SkBlendMode::kDstIn shader composite
// for mask painting are deferred — at the prop-applier layer we just
// confirm the dispatch lands at the right bridge fn.

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

function callOf(b: MockBridge, fn: string) {
    return b.calls.find((c) => c.fn === fn);
}

describe('clip-path / mask cluster (pulp #1515)', () => {
    it('clipPath path() form forwards verbatim to setClipPath', () => {
        applyChangedProps(makeInstance(), {}, {
            clipPath: 'path("M 0 0 L 100 0 L 100 100 Z")',
        });
        expect(callOf(bridge, 'setClipPath')?.args).toEqual([
            'k',
            'path("M 0 0 L 100 0 L 100 100 Z")',
        ]);
    });

    it('clipPath none forwards verbatim (bridge clears slot)', () => {
        applyChangedProps(makeInstance(), {}, { clipPath: 'none' });
        expect(callOf(bridge, 'setClipPath')?.args).toEqual(['k', 'none']);
    });

    it('clipPath deferred forms forward verbatim (bridge ignores)', () => {
        applyChangedProps(makeInstance(), {}, { clipPath: 'circle(50%)' });
        expect(callOf(bridge, 'setClipPath')?.args).toEqual(['k', 'circle(50%)']);
    });

    it('mask shorthand forwards verbatim to setMask', () => {
        applyChangedProps(makeInstance(), {}, { mask: 'url(#m) repeat' });
        expect(callOf(bridge, 'setMask')?.args).toEqual(['k', 'url(#m) repeat']);
    });

    it('maskImage forwards verbatim to setMaskImage', () => {
        applyChangedProps(makeInstance(), {}, { maskImage: 'url(#mask)' });
        expect(callOf(bridge, 'setMaskImage')?.args).toEqual(['k', 'url(#mask)']);
    });

    it('maskImage none forwards verbatim (bridge clears slot)', () => {
        applyChangedProps(makeInstance(), {}, { maskImage: 'none' });
        expect(callOf(bridge, 'setMaskImage')?.args).toEqual(['k', 'none']);
    });
});
