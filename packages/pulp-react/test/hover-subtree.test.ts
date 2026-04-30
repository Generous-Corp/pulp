// hover-subtree.test.ts — recursive hover cleanup on subtree removal
// (pulp #1149, Codex P1 review).
//
// React's commitDeletion only calls removeChild on the ROOT of a
// deleted subtree — descendants are NOT torn down via per-child
// host-config calls. But the native bridge's removeWidget(parent)
// recursively destroys child Views. So when a parent with hover-armed
// descendants unmounts, the host config must walk its own instance
// graph and clear hover bookkeeping for every descendant.
//
// Without this walk, a re-mount that re-uses any descendant id would
// skip registerHover (because hoverArmed still has the stale entry)
// and that re-mounted widget's hover handlers would silently never
// fire — exactly the original #1149 bug shape, just at a different
// trigger point.

import { describe, it, expect, beforeEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';
import { applyAllProps, clearHoverFor } from '../src/prop-applier.js';
import type { PulpContainer, PulpInstance } from '../src/types.js';

function hoverCalls(bridge: MockBridge): MockBridge['calls'] {
    return bridge.calls.filter((c) => c.fn === 'registerHover');
}

describe('@pulp/react hover subtree cleanup (#1149)', () => {
    let bridge: MockBridge;

    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
        for (const id of ['parent1', 'leaf1', 'leaf2', 'leaf3', 'mid1']) {
            clearHoverFor(id);
        }
    });

    it('removeChild on a parent walks the subtree and clears descendants', () => {
        const container: PulpContainer = { rootId: '', nextId: 0 };

        // Build a small tree with hover-armed leaves at multiple depths:
        //   parent1
        //     ├── mid1
        //     │     └── leaf1   (onMouseEnter)
        //     ├── leaf2         (onMouseEnter)
        //     └── leaf3         (no hover handler)
        const parent: PulpInstance = (PulpHostConfig.createInstance!(
            'View',
            { id: 'parent1' },
            container,
            {},
            null,
        ) as unknown) as PulpInstance;

        const mid: PulpInstance = (PulpHostConfig.createInstance!(
            'View',
            { id: 'mid1' },
            container,
            {},
            null,
        ) as unknown) as PulpInstance;

        const leaf1: PulpInstance = (PulpHostConfig.createInstance!(
            'Button',
            { id: 'leaf1', text: 'L1', onMouseEnter: () => { /* noop */ } },
            container,
            {},
            null,
        ) as unknown) as PulpInstance;

        const leaf2: PulpInstance = (PulpHostConfig.createInstance!(
            'Button',
            { id: 'leaf2', text: 'L2', onMouseEnter: () => { /* noop */ } },
            container,
            {},
            null,
        ) as unknown) as PulpInstance;

        const leaf3: PulpInstance = (PulpHostConfig.createInstance!(
            'Button',
            { id: 'leaf3', text: 'L3', onClick: () => { /* noop */ } },
            container,
            {},
            null,
        ) as unknown) as PulpInstance;

        // Wire the React-side parent/child relationship by hand,
        // mirroring what attach() does. We don't go through React's
        // appendChild because we want to test removeChild's cleanup
        // in isolation, not the full mount path.
        parent.childIds = ['mid1', 'leaf2', 'leaf3'];
        mid.childIds = ['leaf1'];

        // Arm hover on the two hover-shaped leaves.
        applyAllProps(leaf1);
        applyAllProps(leaf2);
        applyAllProps(leaf3);
        // leaf1 + leaf2 should each have armed registerHover once.
        expect(hoverCalls(bridge).length).toBe(2);

        // Now React unmounts the parent — single removeChild call,
        // not one per descendant. The host config must walk the
        // subtree itself and clear hoverArmed for every descendant.
        PulpHostConfig.removeChild!(parent as never, parent as never);

        // Re-mount a fresh widget under the SAME id as a former
        // descendant. If subtree cleanup ran, registerHover should
        // fire again (third time total). If cleanup was only single-
        // level, hoverArmed still contains 'leaf1' / 'leaf2' and the
        // re-mount silently skips arming, leaving hover dead.
        const reMounted: PulpInstance = (PulpHostConfig.createInstance!(
            'Button',
            { id: 'leaf1', text: 'NEW', onMouseEnter: () => { /* noop */ } },
            container,
            {},
            null,
        ) as unknown) as PulpInstance;
        applyAllProps(reMounted);

        // 2 from initial mount + 1 from the post-cleanup re-mount = 3.
        expect(hoverCalls(bridge).length).toBe(3);
    });
});
