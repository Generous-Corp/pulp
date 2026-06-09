// pulp parity-found (framework-importer #18) — the @pulp/react
// prop-applier created the Image widget (host-config createImage) and
// declared the bridge surface, but never dispatched the `src` prop to
// setImageSource. The emitted bundle had createImage calls and ZERO
// setImageSource calls, so every <img>/<Image> rendered as the empty
// "IMG" placeholder (in the design-import path AND the framework
// importer). This mirrors the non-React fix in
// core/view/js/web-compat-element.js (pulp #1658).
//
// These tests assert:
//   1. applyAllProps(<Image src=…>) dispatches setImageSource with the
//      resolved (verbatim, absolute) path.
//   2. Changing `src` re-dispatches setImageSource with the new path.
//   3. Removing `src` clears the ImageView (empty-string sentinel).
//   4. The path is forwarded verbatim — absolute paths (design-import /
//      importer convention) pass through untouched.
//   5. A stray `src` on a non-Image widget does NOT reach the
//      ImageView-only setter (the `type === 'Image'` gate).

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

function imageSourceCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'setImageSource');
}

describe('@pulp/react prop-applier — <Image src> → setImageSource (#18)', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('applyAllProps dispatches setImageSource for an Image with src', () => {
        applyAllProps(instance('img1', 'Image', { src: '/abs/path/logo.png' }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['img1', '/abs/path/logo.png']);
    });

    it('forwards an absolute path verbatim (design-import / importer convention)', () => {
        const abs = '/Volumes/Workshop/assets/cover.png';
        applyAllProps(instance('img2', 'Image', { src: abs }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['img2', abs]);
    });

    it('forwards file:// and bare-path forms unchanged', () => {
        applyAllProps(instance('img3', 'Image', { src: 'file:///x/y.png' }));
        applyAllProps(instance('img4', 'Image', { src: 'assets/icon.png' }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(2);
        expect(calls[0].args).toEqual(['img3', 'file:///x/y.png']);
        expect(calls[1].args).toEqual(['img4', 'assets/icon.png']);
    });

    it('re-dispatches setImageSource when src changes (commitUpdate)', () => {
        const inst = instance('img5', 'Image', {});
        const mutated = applyChangedProps(
            inst,
            { src: '/abs/old.png' },
            { src: '/abs/new.png' },
        );
        expect(mutated).toBe(true);
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['img5', '/abs/new.png']);
    });

    it('does not re-dispatch when src is unchanged', () => {
        applyChangedProps(
            instance('img6', 'Image', {}),
            { src: '/abs/same.png' },
            { src: '/abs/same.png' },
        );
        expect(imageSourceCalls(bridge)).toHaveLength(0);
    });

    it('clears the ImageView (empty-string sentinel) when src is removed', () => {
        const mutated = applyChangedProps(
            instance('img7', 'Image', {}),
            { src: '/abs/gone.png' },
            {},
        );
        expect(mutated).toBe(true);
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['img7', '']);
    });

    it('ignores src on a non-Image widget (type gate)', () => {
        applyAllProps(instance('btn1', 'Button', { src: '/abs/should-not-fire.png' }));
        expect(imageSourceCalls(bridge)).toHaveLength(0);
    });
});
