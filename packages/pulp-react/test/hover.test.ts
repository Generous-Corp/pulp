// hover.test.ts — registerHover wiring (pulp #1149).
//
// The bridge has a `registerHover(id)` opt-in that arms the
// `mouseenter`/`mouseleave` dispatch for a widget. Before #1149 the
// prop-applier registered `onMouseEnter`/`onMouseLeave` listeners via
// `on()` but never called `registerHover`, so the dispatch never fired
// and React-style hover handlers were silently dead.
//
// These tests assert that:
//   - `registerHover(id)` fires once when any hover-shaped handler
//     (onMouseEnter / onMouseLeave / onPointerEnter / onPointerLeave)
//     is set on a widget.
//   - It does NOT fire for widgets with no hover handlers.
//   - It is idempotent — setting both onMouseEnter and onPointerEnter
//     together arms the bridge once, not twice.
//   - applyChangedProps re-arms when a hover handler is added by a
//     later commit (handler attached conditionally).
//   - clearHoverFor (called from detach) drops bookkeeping so a
//     re-mount under the same id re-arms cleanly.

import { describe, it, expect, beforeEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import {
    applyAllProps,
    applyChangedProps,
    clearHoverFor,
} from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: PulpInstance['type'], props: Record<string, unknown>): PulpInstance {
    return {
        id,
        type,
        props,
        childIds: [],
        onBridge: false,
        pendingChildren: [],
    };
}

function hoverCalls(bridge: MockBridge): MockBridge['calls'] {
    return bridge.calls.filter((c) => c.fn === 'registerHover');
}

function onCalls(bridge: MockBridge): MockBridge['calls'] {
    return bridge.calls.filter((c) => c.fn === 'on');
}

describe('@pulp/react hover wiring (#1149)', () => {
    let bridge: MockBridge;

    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
        // Each test starts with no widgets armed. Bridge bookkeeping is
        // module-level state in prop-applier; clear any leftovers from
        // prior tests via the public clear helper.
        clearHoverFor('btn1');
        clearHoverFor('btn2');
        clearHoverFor('btn3');
        clearHoverFor('btn4');
    });

    it('arms registerHover once when onMouseEnter is set on mount', () => {
        const inst = instance('btn1', 'Button', {
            text: 'Hover me',
            onMouseEnter: () => { /* noop */ },
        });
        applyAllProps(inst);

        const arms = hoverCalls(bridge);
        expect(arms.length).toBe(1);
        expect(arms[0].args).toEqual(['btn1']);
    });

    it('arms registerHover once when onMouseLeave alone is set', () => {
        const inst = instance('btn2', 'Button', {
            text: 'Leave me',
            onMouseLeave: () => { /* noop */ },
        });
        applyAllProps(inst);

        const arms = hoverCalls(bridge);
        expect(arms.length).toBe(1);
        expect(arms[0].args).toEqual(['btn2']);
    });

    it('does NOT arm registerHover when no hover handlers are present', () => {
        const inst = instance('btn3', 'Button', {
            text: 'Click me',
            onClick: () => { /* noop */ },
        });
        applyAllProps(inst);

        expect(hoverCalls(bridge).length).toBe(0);
    });

    it('arms registerHover exactly once when both onMouseEnter and onPointerEnter are set together', () => {
        const inst = instance('btn4', 'Button', {
            text: 'Both',
            onMouseEnter: () => { /* noop */ },
            onPointerEnter: () => { /* noop */ },
        });
        applyAllProps(inst);

        const arms = hoverCalls(bridge);
        expect(arms.length).toBe(1);
        expect(arms[0].args).toEqual(['btn4']);
    });

    it('arms registerHover once even when all four hover-shaped handlers are set', () => {
        const inst = instance('btn1', 'Button', {
            text: 'All four',
            onMouseEnter: () => { /* noop */ },
            onMouseLeave: () => { /* noop */ },
            onPointerEnter: () => { /* noop */ },
            onPointerLeave: () => { /* noop */ },
        });
        applyAllProps(inst);

        expect(hoverCalls(bridge).length).toBe(1);
    });

    it('re-arms via applyChangedProps when onMouseEnter is added by a later commit', () => {
        const inst = instance('btn2', 'Button', { text: 'Conditional' });
        applyAllProps(inst);
        // No hover handler at mount → nothing armed.
        expect(hoverCalls(bridge).length).toBe(0);

        // A later commit attaches onMouseEnter. applyChangedProps should
        // detect the new hover-shaped key and call registerHover.
        const newProps = { text: 'Conditional', onMouseEnter: () => { /* noop */ } };
        applyChangedProps(inst, inst.props, newProps);

        const arms = hoverCalls(bridge);
        expect(arms.length).toBe(1);
        expect(arms[0].args).toEqual(['btn2']);
    });

    it('does NOT re-arm registerHover on subsequent prop updates if already armed', () => {
        const handler1 = () => { /* noop */ };
        const handler2 = () => { /* noop */ };
        const inst = instance('btn3', 'Button', {
            text: 'X',
            onMouseEnter: handler1,
        });
        applyAllProps(inst);
        expect(hoverCalls(bridge).length).toBe(1);

        // Change the handler reference. applyChangedProps re-applies
        // the event, but registerHover should NOT fire a second time.
        applyChangedProps(
            inst,
            { text: 'X', onMouseEnter: handler1 },
            { text: 'X', onMouseEnter: handler2 },
        );
        expect(hoverCalls(bridge).length).toBe(1);
    });

    it('aliases onPointerEnter to the mouseenter event name', () => {
        // Native registerHover dispatches `mouseenter` / `mouseleave`
        // only — never `pointerenter` / `pointerleave`. The prop-applier
        // must alias the React-style pointer prop names to mouse event
        // names so the listener registered via `on()` matches the
        // dispatch key the bridge actually sends (pulp #1149, Codex P1).
        const inst = instance('btn1', 'Button', {
            text: 'Pointer',
            onPointerEnter: () => { /* noop */ },
            onPointerLeave: () => { /* noop */ },
        });
        applyAllProps(inst);

        const ons = onCalls(bridge);
        const eventNames = ons.map((c) => c.args[1]);
        // The two pointer-shaped handlers should register under the
        // mouseenter / mouseleave keys, NOT pointerenter / pointerleave.
        expect(eventNames).toContain('mouseenter');
        expect(eventNames).toContain('mouseleave');
        expect(eventNames).not.toContain('pointerenter');
        expect(eventNames).not.toContain('pointerleave');
    });

    it('mouseenter+pointerenter together collapse to a single mouseenter listener', () => {
        // When a consumer sets BOTH onMouseEnter and onPointerEnter,
        // both alias to the `mouseenter` event name. The second `on()`
        // call simply overwrites the first listener — only the latest
        // one wins (last-write semantics on the bridge's __callbacks__
        // map). registerHover stays armed exactly once.
        const inst = instance('btn2', 'Button', {
            text: 'Both pointer + mouse',
            onMouseEnter: () => { /* noop */ },
            onPointerEnter: () => { /* noop */ },
        });
        applyAllProps(inst);

        expect(hoverCalls(bridge).length).toBe(1);
        const mouseenterRegs = onCalls(bridge).filter((c) => c.args[1] === 'mouseenter');
        // Both handlers are routed to the mouseenter key — that's two
        // on() calls, both targeting the same dispatch slot.
        expect(mouseenterRegs.length).toBe(2);
    });

    it('clearHoverFor lets a re-mount under the same id re-arm cleanly', () => {
        const inst1 = instance('btn4', 'Button', {
            text: 'First',
            onMouseEnter: () => { /* noop */ },
        });
        applyAllProps(inst1);
        expect(hoverCalls(bridge).length).toBe(1);

        // Simulate the host config's detach() path clearing bookkeeping
        // when the widget is removed.
        clearHoverFor('btn4');

        // Re-mount under the same id (rare but possible — id reuse,
        // hot-reload, certain test harnesses).
        const inst2 = instance('btn4', 'Button', {
            text: 'Second',
            onMouseEnter: () => { /* noop */ },
        });
        applyAllProps(inst2);

        // Should have armed twice across the two mounts.
        expect(hoverCalls(bridge).length).toBe(2);
    });
});
