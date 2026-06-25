// The prop applier forwards `clipPath`, `mask`, and `maskImage`
// values to the matching bridge functions without parsing them. The
// native bridge owns storage, clearing, and renderer-specific support
// for the forwarded syntax forms.

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

describe('clip-path and mask prop forwarding', () => {
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
