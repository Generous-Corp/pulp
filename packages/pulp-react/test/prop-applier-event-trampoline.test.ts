// Each (widget, event) gets one native listener: a trampoline that reads the
// handler registry at dispatch time. A re-render that only hands React a new
// closure — every inline arrow function — is a registry write with no bridge
// call; arming and installation happen on the absent-to-present edge, and the
// inert callback on present-to-absent.

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import {
    applyAllProps,
    applyChangedProps,
    clearMaterializedEventCallbacks,
} from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    delete (globalThis as Record<string, unknown>).__pulpReactEventCallbacks__;
    bridge.uninstall();
});

/// The most recent native listener installed for (id, event), as the bridge's
/// replace-only listener table would hold it.
function listener(id: string, eventName: string): (...a: unknown[]) => unknown {
    const calls = bridge.calls.filter(
        (c) => c.fn === 'on' && c.args[0] === id && c.args[1] === eventName);
    if (calls.length === 0) throw new Error(`no listener for ${id}/${eventName}`);
    return calls[calls.length - 1].args[2] as (...a: unknown[]) => unknown;
}

describe('stable event trampoline', () => {
    it('a new closure on re-render costs no bridge call and still dispatches', () => {
        const first = vi.fn();
        const second = vi.fn();
        const item = instance('btn', 'Button', { onClick: first });
        applyAllProps(item);
        // CONTROL: the mount armed and installed the listener.
        expect(bridge.calls.map((c) => c.fn)).toEqual(
            expect.arrayContaining(['registerClick', 'on']));

        const before = bridge.calls.length;
        applyChangedProps(item, { onClick: first }, { onClick: second });
        expect(bridge.calls.slice(before)).toEqual([]);

        listener('btn', 'click')(0);
        expect(first).not.toHaveBeenCalled();
        expect(second).toHaveBeenCalledTimes(1);
    });

    it('re-renders of a component with many inline handlers stay bridge-silent', () => {
        const handlers = () => ({
            onClick: () => {}, onPointerDown: () => {}, onPointerMove: () => {},
            onWheel: () => {}, onMouseEnter: () => {}, onMouseLeave: () => {},
        });
        let props = handlers();
        const item = instance('pad', 'View', props);
        applyAllProps(item);
        const mounted = bridge.calls.length;
        expect(mounted).toBeGreaterThan(0);
        for (let render = 0; render < 10; ++render) {
            const next = handlers();
            applyChangedProps(item, props, next);
            props = next;
        }
        expect(bridge.calls.length - mounted).toBe(0);
    });

    it('removing a handler installs the inert callback once and stops dispatch', () => {
        const handler = vi.fn();
        const item = instance('opt', 'Button', { onClick: handler });
        applyAllProps(item);
        const trampoline = listener('opt', 'click');

        const before = bridge.calls.length;
        applyChangedProps(item, { onClick: handler }, {});
        const removal = bridge.calls.slice(before);
        expect(removal.map((c) => c.fn)).toEqual(['on']);
        expect(listener('opt', 'click')()).toEqual({ __pulpEventPropagation: 0 });
        // A trampoline the native side still held reads the cleared registry.
        expect(trampoline(0)).toEqual({ __pulpEventPropagation: 0 });
        expect(handler).not.toHaveBeenCalled();

        // Removing again is a no-op: nothing is installed to deactivate.
        const afterRemoval = bridge.calls.length;
        applyChangedProps(item, { onClick: undefined }, {});
        expect(bridge.calls.length).toBe(afterRemoval);
    });

    it('a handler that returns after removal is armed and installed again', () => {
        const back = vi.fn();
        const item = instance('toggle', 'Button', { onClick: () => {} });
        applyAllProps(item);
        applyChangedProps(item, { onClick: item.props.onClick }, {});
        const before = bridge.calls.length;
        applyChangedProps(item, {}, { onClick: back });
        expect(bridge.calls.slice(before).map((c) => c.fn)).toEqual(['registerClick', 'on']);
        listener('toggle', 'click')(0);
        expect(back).toHaveBeenCalledTimes(1);
    });

    it('teardown forgets the listener so a reused id is installed afresh', () => {
        const stale = vi.fn();
        const fresh = vi.fn();
        applyAllProps(instance('row', 'Button', { onClick: stale }));
        clearMaterializedEventCallbacks('row');
        const registry = (globalThis as Record<string, unknown>)
            .__pulpReactEventCallbacks__ as Map<string, unknown>;
        expect(registry.has('row:click')).toBe(false);

        // An update (not a first attach) reaching the reused id must still
        // install, or the new widget would have no native listener at all.
        const before = bridge.calls.length;
        applyChangedProps(instance('row', 'Button', {}), {}, { onClick: fresh });
        expect(bridge.calls.slice(before).map((c) => c.fn)).toEqual(['registerClick', 'on']);
        listener('row', 'click')(0);
        expect(stale).not.toHaveBeenCalled();
        expect(fresh).toHaveBeenCalledTimes(1);
    });

    it('a widget recreated under the same id is armed again on first attach', () => {
        const handler = vi.fn();
        applyAllProps(instance('knob', 'Button', { onClick: handler }));
        const before = bridge.calls.length;
        applyAllProps(instance('knob', 'Button', { onClick: handler }));
        expect(bridge.calls.slice(before).map((c) => c.fn)).toEqual(['registerClick', 'on']);
    });

    it('hover spellings share one native listener through every change', () => {
        const enter = vi.fn();
        const pointerEnter = vi.fn();
        const item = instance('chip', 'View', { onMouseEnter: enter });
        applyAllProps(item);
        const before = bridge.calls.length;
        applyChangedProps(item, { onMouseEnter: enter },
            { onMouseEnter: () => {}, onPointerEnter: pointerEnter });
        expect(bridge.calls.slice(before)).toEqual([]);
        listener('chip', 'mouseenter')(0);
        expect(pointerEnter).toHaveBeenCalledTimes(1);

        // Dropping one spelling leaves the other live with no bridge call.
        const kept = vi.fn();
        const mid = bridge.calls.length;
        applyChangedProps(item, { onMouseEnter: enter, onPointerEnter: pointerEnter },
            { onMouseEnter: kept });
        expect(bridge.calls.slice(mid)).toEqual([]);
        listener('chip', 'mouseenter')(0);
        expect(kept).toHaveBeenCalledTimes(1);
        expect(pointerEnter).toHaveBeenCalledTimes(1);

        // Dropping the last spelling deactivates the native listener.
        applyChangedProps(item, { onMouseEnter: kept }, {});
        expect(listener('chip', 'mouseenter')(0)).toEqual({ __pulpEventPropagation: 0 });
        expect(kept).toHaveBeenCalledTimes(1);
    });

    it('a replaced bridge starts the installed record over', () => {
        const handler = vi.fn();
        const item = instance('swap', 'Button', { onClick: handler });
        applyAllProps(item);
        bridge.uninstall();
        bridge = createMockBridge();
        bridge.install();
        applyChangedProps(item, { onClick: handler }, { onClick: () => {} });
        expect(bridge.calls.map((c) => c.fn)).toEqual(['registerClick', 'on']);
    });
});
