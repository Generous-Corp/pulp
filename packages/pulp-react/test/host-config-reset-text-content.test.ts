// pulp #1840 P1 follow-up — React's mutation reconciler calls
// resetTextContent(instance) when shouldSetTextContent flips from
// true → false on an existing TEXT_BEARING node. Without this host
// hook the reconciler throws or leaves stale text on the node.
// Reproduce the transition by calling the hook directly through the
// PulpHostConfig surface and asserting setText('') fires on the bridge.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { PulpHostConfig } from '../src/host-config.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
});

function makeInstance(id: string, type: PulpInstance['type'] = 'Label'): PulpInstance {
    return {
        id,
        type,
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

const callsOf = (b: MockBridge, fn: string) =>
    b.calls.filter((c) => c.fn === fn);

describe('host-config resetTextContent (pulp #1840 P1 Codex follow-up)', () => {
    const fn = PulpHostConfig.resetTextContent as
        ((instance: PulpInstance) => void) | undefined;

    it('hook exists (required by React mutation reconciler when transitioning text→non-text)', () => {
        expect(typeof fn).toBe('function');
    });

    it('clears Label text by dispatching setText(id, "")', () => {
        const inst = makeInstance('k', 'Label');
        fn!(inst);
        const c = callsOf(bridge, 'setText');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', '']);
    });

    it('clears HTML-alias <span> text by dispatching setText(id, "")', () => {
        // The transition that motivated the hook: <span>hi</span> →
        // <span><em>hi</em></span>. React first calls resetTextContent
        // on the span; we must emit setText('') so the bridge clears
        // the stale "hi" before <em> mounts as a child.
        const inst = makeInstance('s1', 'span' as PulpInstance['type']);
        fn!(inst);
        const c = callsOf(bridge, 'setText');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['s1', '']);
    });

    it('calling on multiple instances dispatches setText for each', () => {
        fn!(makeInstance('a', 'span' as PulpInstance['type']));
        fn!(makeInstance('b', 'p' as PulpInstance['type']));
        fn!(makeInstance('c', 'Button'));
        const c = callsOf(bridge, 'setText');
        expect(c).toHaveLength(3);
        expect(c.map((x) => x.args)).toEqual([
            ['a', ''], ['b', ''], ['c', ''],
        ]);
    });

    it('no setText call if bridge does not expose setText (graceful degrade)', () => {
        // Remove the function from the global stub mid-test. The hook
        // must not throw — bridges without text capability should be
        // safe to attach to a text-bearing reconciler.
        const g = globalThis as Record<string, unknown>;
        const saved = g.setText;
        delete g.setText;
        try {
            expect(() => fn!(makeInstance('k', 'Label'))).not.toThrow();
        } finally {
            g.setText = saved;
        }
    });
});
