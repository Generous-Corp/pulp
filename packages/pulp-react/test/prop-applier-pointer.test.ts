// prop-applier must call registerPointer(id) when a pointer-class event handler
// (onPointerDown / Move / Up / Cancel / Wheel) is set, parallel to the existing
// registerHover call for hover events.
//
// Without registerPointer, the bridge keeps the JS listener in its dispatch
// table but the View's on_pointer_event callback is never armed by the
// native side, so clicks never fire the React handler. Canvas band-drag
// handlers with wheel zoom are the representative regression shape.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

describe('@pulp/react prop-applier — pointer registration', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('calls registerPointer when onPointerDown is set', () => {
        applyAllProps(instance('w1', 'View', {
            onPointerDown: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['w1']);
    });

    it('calls registerPointer when onPointerMove is set', () => {
        applyAllProps(instance('w2', 'View', {
            onPointerMove: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['w2']);
    });

    it('calls registerPointer when onPointerUp is set', () => {
        applyAllProps(instance('w3', 'View', {
            onPointerUp: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['w3']);
    });

    it('calls registerPointer when onPointerCancel is set', () => {
        applyAllProps(instance('w4', 'View', {
            onPointerCancel: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['w4']);
    });

    it('calls registerWheel (NOT registerPointer) when onWheel is set', () => {
        // Wheel goes through a separate bridge call because the
        // registerPointer lambda filters out is_wheel events.
        applyAllProps(instance('w5', 'View', {
            onWheel: () => {},
        }));
        const regPointer = bridge.calls.filter((c) => c.fn === 'registerPointer');
        const regWheel = bridge.calls.filter((c) => c.fn === 'registerWheel');
        expect(regPointer.length).toBe(0);
        expect(regWheel.length).toBe(1);
        expect(regWheel[0].args).toEqual(['w5']);
    });

    it('calls both registerPointer and registerWheel when both pointer + wheel handlers present', () => {
        // Canvas band-drag shape: onPointerDown for drag + onWheel for zoom.
        // Both lambdas must be wired since each filters on
        // me.is_wheel inversely.
        applyAllProps(instance('canvas2', 'Canvas', {
            onPointerDown: () => {},
            onWheel: () => {},
        }));
        const regPointer = bridge.calls.filter((c) => c.fn === 'registerPointer');
        const regWheel = bridge.calls.filter((c) => c.fn === 'registerWheel');
        expect(regPointer.length).toBe(1);
        expect(regWheel.length).toBe(1);
    });

    it('calls registerPointer once per pointer-class handler installed', () => {
        applyAllProps(instance('canvas1', 'Canvas', {
            onPointerDown: () => {},
            onPointerMove: () => {},
            onPointerUp: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        // One call per pointer-class handler installed; idempotent on the
        // bridge side (replaces the lambda each time).
        expect(reg.length).toBe(3);
        expect(reg.every((c) => c.args[0] === 'canvas1')).toBe(true);
    });

    it('does NOT call registerPointer for hover-only props', () => {
        applyAllProps(instance('w6', 'View', {
            onMouseEnter: () => {},
            onMouseLeave: () => {},
        }));
        const regPointer = bridge.calls.filter((c) => c.fn === 'registerPointer');
        const regHover = bridge.calls.filter((c) => c.fn === 'registerHover');
        // Hover events still go through registerHover, NOT registerPointer.
        expect(regPointer.length).toBe(0);
        expect(regHover.length).toBe(2);
    });

    it('does NOT call registerPointer when only onClick is set', () => {
        applyAllProps(instance('btn1', 'Button', {
            onClick: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(0);
    });

    it('still installs the on() listener alongside registerPointer', () => {
        applyAllProps(instance('w7', 'View', {
            onPointerDown: () => {},
        }));
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.length).toBe(1);
        expect(on[0].args[0]).toBe('w7');
        expect(on[0].args[1]).toBe('pointerdown');
    });

    it('calls registerPointer on commitUpdate when adding pointer handler', () => {
        applyChangedProps(
            instance('w8', 'View', {}),
            { onClick: () => {} },
            { onClick: () => {}, onPointerDown: () => {} },
        );
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['w8']);
    });

    it('does NOT call registerPointer when pointer handler is removed', () => {
        // Removing a pointer handler should not re-arm — it's the consumer's
        // job to clean up. The bridge's on_pointer_event lambda stays
        // wired, but the JS listener is gone, so the lambda dispatches to
        // a no-op handler.
        applyChangedProps(
            instance('w9', 'View', {}),
            { onPointerDown: () => {} },
            {},
        );
        const reg = bridge.calls.filter((c) => c.fn === 'registerPointer');
        expect(reg.length).toBe(0);
    });

    it('routes style.overflow through setOverflow', () => {
        applyAllProps(instance('row1', 'View', {
            overflow: 'hidden',
        }));
        const set = bridge.calls.filter((c) => c.fn === 'setOverflow');
        expect(set.length).toBe(1);
        expect(set[0].args).toEqual(['row1', 'hidden']);
    });

    it('routes style.overflow=visible through setOverflow', () => {
        applyAllProps(instance('row2', 'View', {
            overflow: 'visible',
        }));
        const set = bridge.calls.filter((c) => c.fn === 'setOverflow');
        expect(set.length).toBe(1);
        expect(set[0].args).toEqual(['row2', 'visible']);
    });

    it('arms registerPointer + registerHover when both pointer and hover handlers present', () => {
        // Canvas interaction wrapper shape: hover-driven cursor + drag-driven gain
        // edits both attached to the same widget.
        applyAllProps(instance('wrap', 'View', {
            onPointerDown: () => {},
            onPointerMove: () => {},
            onPointerUp: () => {},
            onMouseEnter: () => {},
            onMouseLeave: () => {},
        }));
        const regPointer = bridge.calls.filter((c) => c.fn === 'registerPointer');
        const regHover = bridge.calls.filter((c) => c.fn === 'registerHover');
        expect(regPointer.length).toBe(3);
        expect(regHover.length).toBe(2);
    });

    it('arms tap and double-tap native recognizers from React props', () => {
        applyAllProps(instance('gestures1', 'View', {
            onTap: () => {},
            onDoubleTap: () => {},
        }));
        expect(bridge.calls.filter((c) => c.fn === 'registerTapGesture')[0].args)
            .toEqual(['gestures1']);
        expect(bridge.calls.filter((c) => c.fn === 'registerDoubleTapGesture')[0].args)
            .toEqual(['gestures1']);
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.map((c) => c.args[1])).toEqual(['tap', 'doubletap']);
    });

    it('arms pan recognizer once per pan phase handler at the prop-applier layer', () => {
        applyAllProps(instance('gestures2', 'View', {
            onPanStart: () => {},
            onPanChange: () => {},
            onPanEnd: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerPanGesture');
        expect(reg.length).toBe(3);
        expect(reg.every((c) => c.args[0] === 'gestures2')).toBe(true);
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.map((c) => c.args[1])).toEqual(['panstart', 'panchange', 'panend']);
    });

    it('arms multi-touch native recognizers from pinch and rotate props', () => {
        applyAllProps(instance('gestures3', 'View', {
            onPinchStart: () => {},
            onPinchChange: () => {},
            onRotateStart: () => {},
            onRotateEnd: () => {},
        }));
        expect(bridge.calls.filter((c) => c.fn === 'registerPinchGesture').length).toBe(2);
        expect(bridge.calls.filter((c) => c.fn === 'registerRotateGesture').length).toBe(2);
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.map((c) => c.args[1])).toEqual([
            'pinchstart',
            'pinchchange',
            'rotatestart',
            'rotateend',
        ]);
    });

    it('maps continuous gesture aliases to change events', () => {
        applyAllProps(instance('gestures5', 'View', {
            onPan: () => {},
            onPinch: () => {},
            onRotate: () => {},
        }));
        expect(bridge.calls.filter((c) => c.fn === 'registerPanGesture').length).toBe(1);
        expect(bridge.calls.filter((c) => c.fn === 'registerPinchGesture').length).toBe(1);
        expect(bridge.calls.filter((c) => c.fn === 'registerRotateGesture').length).toBe(1);
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.map((c) => c.args[1])).toEqual([
            'panchange',
            'pinchchange',
            'rotatechange',
        ]);
    });

    it('arms long-press, swipe, and fling native recognizers from React props', () => {
        applyAllProps(instance('gestures4', 'View', {
            onLongPress: () => {},
            onSwipe: () => {},
            onFling: () => {},
        }));
        expect(bridge.calls.filter((c) => c.fn === 'registerLongPressGesture')[0].args)
            .toEqual(['gestures4']);
        expect(bridge.calls.filter((c) => c.fn === 'registerSwipeGesture')[0].args)
            .toEqual(['gestures4']);
        expect(bridge.calls.filter((c) => c.fn === 'registerFlingGesture')[0].args)
            .toEqual(['gestures4']);
    });
});
