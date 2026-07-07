// Verify that Image src props reach the ImageView bridge surface.
//
// These tests assert:
//   1. applyAllProps(<Image src=...>) dispatches setImageSource with the
//      resolved (verbatim, absolute) path.
//   2. Changing `src` re-dispatches setImageSource with the new path.
//   3. Removing `src` clears the ImageView (empty-string sentinel).
//   4. The path is forwarded verbatim: absolute paths from import flows pass
//      through untouched.
//   5. A stray `src` on a non-Image widget does not reach the ImageView-only
//      setter.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { bridgeMockFunctionNames } from '../src/bridge-mock-functions.generated.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

function imageSourceCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'setImageSource');
}

describe('@pulp/react prop-applier - <Image src> to setImageSource', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge(bridgeMockFunctionNames);
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('applyAllProps dispatches setImageSource for an Image with src', () => {
        applyAllProps(instance('img1', 'img', { src: '/abs/path/logo.png' }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['img1', '/abs/path/logo.png']);
    });

    it('forwards an absolute path verbatim (design-import / importer convention)', () => {
        const abs = '/Volumes/Workshop/assets/cover.png';
        applyAllProps(instance('img2', 'img', { src: abs }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['img2', abs]);
    });

    it('forwards file:// and bare-path forms unchanged', () => {
        applyAllProps(instance('img3', 'img', { src: 'file:///x/y.png' }));
        applyAllProps(instance('img4', 'img', { src: 'assets/icon.png' }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(2);
        expect(calls[0].args).toEqual(['img3', 'file:///x/y.png']);
        expect(calls[1].args).toEqual(['img4', 'assets/icon.png']);
    });

    it('re-dispatches setImageSource when src changes (commitUpdate)', () => {
        const inst = instance('img5', 'img', {});
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
            instance('img6', 'img', {}),
            { src: '/abs/same.png' },
            { src: '/abs/same.png' },
        );
        expect(imageSourceCalls(bridge)).toHaveLength(0);
    });

    it('clears the ImageView (empty-string sentinel) when src is removed', () => {
        const mutated = applyChangedProps(
            instance('img7', 'img', {}),
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

    // host-config maps both the lowercase `<img>` intrinsic and the `<Image>`
    // component to createImage, so the prop-applier must accept both element
    // types. Gating only on `'Image'` would drop real `<img>` elements because
    // their runtime type is `'img'`.
    it('dispatches setImageSource for both the img intrinsic and the Image component', () => {
        applyAllProps(instance('imgLower', 'img', { src: '/abs/a.png' }));
        applyAllProps(instance('imgUpper', 'Image', { src: '/abs/b.png' }));
        const calls = imageSourceCalls(bridge);
        expect(calls).toHaveLength(2);
        expect(calls[0].args).toEqual(['imgLower', '/abs/a.png']);
        expect(calls[1].args).toEqual(['imgUpper', '/abs/b.png']);
    });
});
