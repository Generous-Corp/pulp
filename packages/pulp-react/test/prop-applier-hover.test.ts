// Test for pulp #1149 — prop-applier must call registerHover(id) when a
// hover-class event handler (onMouseEnter / onMouseLeave / pointer aliases)
// is set, otherwise the native bridge never fires the corresponding
// events even though the JS listener is installed.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

describe('@pulp/react prop-applier — hover registration (pulp #1149)', () => {
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
