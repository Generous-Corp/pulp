// prop-applier must call registerHover(id) when a hover-class event handler
// (onMouseEnter / onMouseLeave / pointer aliases) is set, otherwise the
// native bridge never fires the corresponding events even though the JS
// listener is installed.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

describe('@pulp/react prop-applier — hover registration', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('calls registerHover when onMouseEnter is set', () => {
        applyAllProps(instance('btn1', 'Button', {
            onMouseEnter: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerHover');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['btn1']);
    });

    it('calls registerHover when onMouseLeave is set', () => {
        applyAllProps(instance('btn2', 'Button', {
            onMouseLeave: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerHover');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['btn2']);
    });

    it('calls registerHover for pointerenter/pointerleave too', () => {
        applyAllProps(instance('btn3', 'Button', {
            onPointerEnter: () => {},
            onPointerLeave: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerHover');
        // One call per hover-class handler installed; idempotent on the bridge.
        expect(reg.length).toBe(2);
        expect(reg.every((c) => c.args[0] === 'btn3')).toBe(true);
    });

    it('does NOT call registerHover when only onClick is set', () => {
        applyAllProps(instance('btn4', 'Button', {
            onClick: () => {},
        }));
        const reg = bridge.calls.filter((c) => c.fn === 'registerHover');
        expect(reg.length).toBe(0);
    });

    it('still installs the on() listener alongside registerHover', () => {
        applyAllProps(instance('btn5', 'Button', {
            onMouseEnter: () => {},
        }));
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.length).toBe(1);
        expect(on[0].args[0]).toBe('btn5');
        expect(on[0].args[1]).toBe('mouseenter');
    });

    // The bridge only ever dispatches `mouseenter` / `mouseleave` for hover.
    // Registering a pointer-spelled listener under `pointerenter` installs a
    // listener for a name the C++ side never emits, so the handler is dead —
    // while registerHover still succeeds, which is why the arming assertions
    // above all passed while onPointerEnter did nothing.
    it('registers onPointerEnter under the native mouseenter dispatch name', () => {
        applyAllProps(instance('ptr1', 'Button', {
            onPointerEnter: () => {},
        }));
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.length).toBe(1);
        expect(on[0].args[0]).toBe('ptr1');
        expect(on[0].args[1]).toBe('mouseenter');
    });

    it('registers onPointerLeave under the native mouseleave dispatch name', () => {
        applyAllProps(instance('ptr2', 'Button', {
            onPointerLeave: () => {},
        }));
        const on = bridge.calls.filter((c) => c.fn === 'on');
        expect(on.length).toBe(1);
        expect(on[0].args[1]).toBe('mouseleave');
    });

    it('invokes the onPointerEnter handler when the bridge dispatches mouseenter', () => {
        let entered = 0;
        applyAllProps(instance('ptr3', 'Button', {
            onPointerEnter: () => { entered += 1; },
        }));
        const on = bridge.calls.filter((c) => c.fn === 'on' && c.args[1] === 'mouseenter');
        expect(on.length).toBe(1);
        const listener = on[0].args[2] as (...a: unknown[]) => unknown;
        listener('0');
        expect(entered).toBe(1);
    });

    // Both spellings on one node share a single native dispatch name, and the
    // bridge's listener table is replace-only — so a naive alias would drop
    // one of them. Both handlers must run.
    it('fans one native mouseenter out to both hover spellings', () => {
        const seen: string[] = [];
        applyAllProps(instance('ptr4', 'Button', {
            onMouseEnter: () => { seen.push('mouse'); },
            onPointerEnter: () => { seen.push('pointer'); },
        }));
        const on = bridge.calls.filter((c) => c.fn === 'on' && c.args[1] === 'mouseenter');
        expect(on.length).toBeGreaterThan(0);
        const listener = on[on.length - 1].args[2] as (...a: unknown[]) => unknown;
        listener('0');
        expect(seen).toEqual(['mouse', 'pointer']);
    });

    it('keeps the surviving spelling live when the sibling handler is removed', () => {
        const seen: string[] = [];
        const mouse = () => { seen.push('mouse'); };
        const pointer = () => { seen.push('pointer'); };
        applyAllProps(instance('ptr5', 'Button', {
            onMouseEnter: mouse, onPointerEnter: pointer,
        }));
        applyChangedProps(
            instance('ptr5', 'Button', { onMouseEnter: mouse }),
            { onMouseEnter: mouse, onPointerEnter: pointer },
            { onMouseEnter: mouse },
        );
        const on = bridge.calls.filter((c) => c.fn === 'on' && c.args[1] === 'mouseenter');
        expect(on.length).toBeGreaterThan(0);
        const listener = on[on.length - 1].args[2] as (...a: unknown[]) => unknown;
        listener('0');
        expect(seen).toEqual(['mouse']);
    });

    it('calls registerHover on commitUpdate when adding hover handler', () => {
        applyChangedProps(
            instance('btn6', 'Button', {}),
            { onClick: () => {} },
            { onClick: () => {}, onMouseEnter: () => {} },
        );
        const reg = bridge.calls.filter((c) => c.fn === 'registerHover');
        expect(reg.length).toBe(1);
        expect(reg[0].args).toEqual(['btn6']);
    });
});
