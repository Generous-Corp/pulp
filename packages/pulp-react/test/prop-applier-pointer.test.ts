// Test for pulp #1381 — prop-applier must call registerPointer(id) when a
// pointer-class event handler (onPointerDown / Move / Up / Cancel / Wheel)
// is set, parallel to the existing registerHover call for hover events.
//
// Without registerPointer, the bridge keeps the JS listener in its dispatch
// table but the View's on_pointer_event callback is never armed by the
// native side, so clicks never fire the React handler. Spectr's FilterBank
// band drag was the canonical repro — see spectr #32 + import-design
// SKILL.md gotcha #8.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

describe('@pulp/react prop-applier — pointer registration (pulp #1381)', () => {
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
        // registerPointer lambda filters out is_wheel events. See pulp
        // #1387 gap #4 (Spectr's zoom doesn't fire).
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
        // Spectr FilterBank shape: onPointerDown for band drag + onWheel
        // for zoom. Both lambdas must be wired since each filters on
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

    it('routes style.overflow through setOverflow (pulp #1387 gap #1)', () => {
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
        // Spectr FilterBank wrap shape: hover-driven cursor + drag-driven gain
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
});
