// Test for pulp #1352 — prop-applier must wrap JSX event handlers in a
// synthetic-event factory so consumers see a React-DOM-shaped event
// object (with `currentTarget`, `target`, `preventDefault`, etc.) and
// event-type-specific fields (`clientX/Y`, `e.target.value`, `key`)
// instead of the bridge's raw positional args.
//
// The bridge dispatches `__dispatch__('btn1', 'mouseenter', 0)` for
// hover; idiomatic JSX handlers (`e => e.currentTarget.style.background = ...`)
// expect an event object — getting `e === 0` crashes them.

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps } from '../src/prop-applier.js';
import { makeSyntheticEvent } from '../src/synthetic-event.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

/// Pull the wrapper that prop-applier registered with the bridge's `on()`
/// for a given (id, eventName) and invoke it with the supplied raw bridge
/// args. Mirrors what the C++ side does in `__dispatch__`.
function dispatch(bridge: MockBridge, id: string, eventName: string, ...rawArgs: unknown[]): unknown {
    const onCall = bridge.calls.find(
        (c) => c.fn === 'on' && c.args[0] === id && c.args[1] === eventName,
    );
    if (!onCall) throw new Error(`no on() registration for ${id}/${eventName}`);
    const wrapper = onCall.args[2] as (...a: unknown[]) => unknown;
    return wrapper(...rawArgs);
}

describe('@pulp/react prop-applier — synthetic event factory (pulp #1352)', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('onMouseEnter handler receives an event object (not literal 0)', () => {
        const handler = vi.fn();
        applyAllProps(instance('btn1', 'Button', { onMouseEnter: handler }));
        // Bridge fires __dispatch__('btn1', 'mouseenter', 0) — see
        // widget_bridge.cpp:1364.
        dispatch(bridge, 'btn1', 'mouseenter', 0);
        expect(handler).toHaveBeenCalledTimes(1);
        const evt = handler.mock.calls[0][0];
        expect(evt).not.toBe(0);
        expect(typeof evt).toBe('object');
        expect(evt.currentTarget).toBeDefined();
        expect(evt.target).toBeDefined();
        expect(evt.type).toBe('mouseenter');
    });

    it('e.currentTarget.style.background = "..." routes to setBackground', () => {
        const handler = vi.fn((e: { currentTarget: { style: Record<string, unknown> } }) => {
            e.currentTarget.style.background = 'rgba(120,180,255,0.14)';
        });
        applyAllProps(instance('btn2', 'Button', { onMouseEnter: handler }));
        dispatch(bridge, 'btn2', 'mouseenter', 0);
        expect(handler).toHaveBeenCalledTimes(1);
        const setBg = bridge.calls.filter((c) => c.fn === 'setBackground');
        expect(setBg.length).toBe(1);
        expect(setBg[0].args).toEqual(['btn2', 'rgba(120,180,255,0.14)']);
    });

    it('synthetic event has type === "click" for onClick', () => {
        const handler = vi.fn();
        applyAllProps(instance('btn3', 'Button', { onClick: handler }));
        dispatch(bridge, 'btn3', 'click', 0);
        expect(handler).toHaveBeenCalledTimes(1);
        const evt = handler.mock.calls[0][0];
        expect(evt.type).toBe('click');
        expect(evt.currentTarget.id).toBe('btn3');
    });

    it('onChange exposes e.target.value from the bridge string arg', () => {
        const handler = vi.fn();
        applyAllProps(instance('te1', 'TextEditor', { onChange: handler }));
        // Bridge fires __dispatch__('te1', 'change', 'hello') — see
        // widget_bridge.cpp:1997.
        dispatch(bridge, 'te1', 'change', 'hello');
        expect(handler).toHaveBeenCalledTimes(1);
        const evt = handler.mock.calls[0][0];
        expect(evt.target.value).toBe('hello');
        expect(evt.type).toBe('change');
    });

    it('onPointerDown lifts clientX/clientY from the bridge data object', () => {
        const handler = vi.fn();
        applyAllProps(instance('p1', 'View', { onPointerDown: handler }));
        // Bridge data shape from widget_bridge.cpp:1485-1501.
        dispatch(bridge, 'p1', 'pointerdown', {
            clientX: 142,
            clientY: 87,
            offsetX: 12,
            offsetY: 5,
            button: 0,
            pointerId: 1,
            pointerType: 'mouse',
            isPrimary: true,
            pressure: 0.5,
            ctrlKey: false,
            shiftKey: false,
            altKey: false,
            metaKey: false,
        });
        expect(handler).toHaveBeenCalledTimes(1);
        const evt = handler.mock.calls[0][0];
        expect(evt.clientX).toBe(142);
        expect(evt.clientY).toBe(87);
        expect(evt.offsetX).toBe(12);
        expect(evt.offsetY).toBe(5);
        expect(evt.pointerId).toBe(1);
        expect(evt.pointerType).toBe('mouse');
    });

    it('preventDefault marks defaultPrevented without throwing', () => {
        const handler = vi.fn((e: { preventDefault: () => void; defaultPrevented: boolean }) => {
            e.preventDefault();
            expect(e.defaultPrevented).toBe(true);
        });
        applyAllProps(instance('btn4', 'Button', { onClick: handler }));
        dispatch(bridge, 'btn4', 'click', 0);
        expect(handler).toHaveBeenCalledTimes(1);
    });

    it('stopPropagation is callable as a no-op', () => {
        const handler = vi.fn((e: { stopPropagation: () => void }) => {
            // Should not throw — JSX consumers may call it reflexively even
            // though @pulp/react has no bubble chain on this dispatch lane.
            e.stopPropagation();
        });
        applyAllProps(instance('btn5', 'Button', { onClick: handler }));
        dispatch(bridge, 'btn5', 'click', 0);
        expect(handler).toHaveBeenCalledTimes(1);
    });

    it('nativeEvent.rawArgs preserves the original bridge args (debug escape hatch)', () => {
        const handler = vi.fn();
        applyAllProps(instance('p2', 'View', { onPointerMove: handler }));
        const rawData = { clientX: 5, clientY: 7, pointerId: 2, pointerType: 'pen' };
        dispatch(bridge, 'p2', 'pointermove', rawData);
        const evt = handler.mock.calls[0][0];
        expect(evt.nativeEvent.rawArgs).toEqual([rawData]);
    });

    it('onMouseLeave receives a synthetic event with currentTarget set', () => {
        const handler = vi.fn();
        applyAllProps(instance('btn6', 'Button', { onMouseLeave: handler }));
        dispatch(bridge, 'btn6', 'mouseleave', 0);
        const evt = handler.mock.calls[0][0];
        expect(evt.type).toBe('mouseleave');
        expect(evt.currentTarget.id).toBe('btn6');
    });

    it('keydown lifts key/keyCode from the bridge data object', () => {
        // The current bridge keydown path dispatches to '__global__' not
        // a per-element handler, but the synthetic-event factory should
        // still lift key fields from any handler that does receive a
        // keydown-shaped object. Test the factory directly.
        const evt = makeSyntheticEvent('btn7', 'keydown', [{ key: 'Enter', keyCode: 13 }]);
        expect(evt.key).toBe('Enter');
        expect(evt.keyCode).toBe(13);
    });

    it('toggle event exposes e.target.checked from the numeric bridge arg', () => {
        const evt = makeSyntheticEvent('tg1', 'toggle', [1]);
        expect((evt.target as unknown as { checked: boolean }).checked).toBe(true);
        const evtOff = makeSyntheticEvent('tg1', 'toggle', [0]);
        expect((evtOff.target as unknown as { checked: boolean }).checked).toBe(false);
    });

    it('multiple style writes route to the matching bridge setters', () => {
        const handler = vi.fn((e: { currentTarget: { style: Record<string, unknown> } }) => {
            e.currentTarget.style.background = '#112233';
            e.currentTarget.style.opacity = 0.5;
            e.currentTarget.style.borderRadius = 8;
        });
        applyAllProps(instance('v1', 'View', { onClick: handler }));
        dispatch(bridge, 'v1', 'click', 0);
        const fns = bridge.calls.map((c) => c.fn);
        expect(fns).toContain('setBackground');
        expect(fns).toContain('setOpacity');
        expect(fns).toContain('setBorderRadius');
        const bg = bridge.calls.find((c) => c.fn === 'setBackground');
        expect(bg?.args).toEqual(['v1', '#112233']);
        const op = bridge.calls.find((c) => c.fn === 'setOpacity');
        expect(op?.args).toEqual(['v1', 0.5]);
        const br = bridge.calls.find((c) => c.fn === 'setBorderRadius');
        expect(br?.args).toEqual(['v1', 8]);
    });

    it('currentTarget exposes setAttribute / getAttribute as DOM-shaped no-ops', () => {
        const handler = vi.fn((e: {
            currentTarget: {
                setAttribute: (n: string, v: string) => void;
                getAttribute: (n: string) => string | null;
            };
        }) => {
            // Should not throw — DOM-shaped code may touch these.
            e.currentTarget.setAttribute('aria-pressed', 'true');
            expect(e.currentTarget.getAttribute('aria-pressed')).toBeNull();
        });
        applyAllProps(instance('btn8', 'Button', { onClick: handler }));
        dispatch(bridge, 'btn8', 'click', 0);
        expect(handler).toHaveBeenCalledTimes(1);
    });

    it('pointerType from a stylus dispatch is preserved on the synthetic event', () => {
        const handler = vi.fn();
        applyAllProps(instance('p3', 'View', { onPointerDown: handler }));
        dispatch(bridge, 'p3', 'pointerdown', {
            clientX: 0, clientY: 0,
            pointerId: 99, pointerType: 'pen',
            pressure: 0.92, isPrimary: true,
        });
        const evt = handler.mock.calls[0][0];
        expect(evt.pointerType).toBe('pen');
        expect(evt.pressure).toBe(0.92);
    });
});
