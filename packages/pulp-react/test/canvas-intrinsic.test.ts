import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import React, { act } from 'react';
import { Canvas } from '../src/intrinsics.js';
import { createRoot, render, unmount } from '../src/index.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';

describe('@pulp/react Canvas intrinsic', () => {
    let bridge: MockBridge;
    let nextFrame = 1;
    let queued: ((time: number) => void) | undefined;
    const cancel = vi.fn();

    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
        nextFrame = 1;
        queued = undefined;
        cancel.mockReset();
        (globalThis as unknown as { requestAnimationFrame: (cb: (time: number) => void) => number }).requestAnimationFrame = (cb) => {
            queued = cb;
            return nextFrame++;
        };
        (globalThis as unknown as { cancelAnimationFrame: typeof cancel }).cancelAnimationFrame = cancel;
    });

    afterEach(() => bridge.uninstall());

    it('runs the imperative frame callback and cancels it on unmount', () => {
        const onFrame = vi.fn((ctx: { drawSdf: (...args: unknown[]) => unknown }) => {
            ctx.drawSdf({ shape: 'circle', x: 0, y: 0, w: 12, h: 12 });
        });
        const root = createRoot('canvas-test-root');
        act(() => render(React.createElement(Canvas, { width: 64, height: 32, onFrame }), root));

        expect(typeof queued).toBe('function');
        act(() => queued?.(1000));
        expect(onFrame).toHaveBeenCalledWith(expect.objectContaining({ drawSdf: expect.any(Function) }), {
            time: 1,
            width: 64,
            height: 32,
        });
        expect(bridge.calls).toContainEqual({
            fn: 'canvasDrawSdf',
            args: [expect.stringMatching(/^canvas_/), { shape: 'circle', x: 0, y: 0, w: 12, h: 12 }, undefined, undefined],
        });

        act(() => unmount(root));
        expect(cancel).toHaveBeenCalled();
    });
});
