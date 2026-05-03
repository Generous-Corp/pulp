// pulp #1148 — generalized overlay-click routing.
//
// `<View overlay>` opts a JSX View in as the active click-eligible
// overlay so the platform window host (window_host_mac.mm and siblings)
// short-circuits hit-testing for clicks landing inside the view's
// bounds. Without this, clicks fall through to whatever sibling /
// ancestor pixel sits behind an absolutely-positioned popover.
//
// The prop-applier contract this test pins:
//   - `overlay: true`  → call claimOverlay(id)
//   - `overlay: false` → call releaseOverlay(id)
//   - prop removed     → call releaseOverlay(id) on commitUpdate
//   - prop never set   → no claim/release call

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
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

function makeInstance(id: string = 'popover', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

describe('@pulp/react prop-applier — overlay routing (pulp #1148)', () => {
    it('applyAllProps calls claimOverlay when overlay=true', () => {
        applyAllProps({ ...makeInstance('p1'), props: { overlay: true } });
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['p1']);
        // releaseOverlay must NOT fire on the mount-claim path.
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });

    it('applyAllProps calls releaseOverlay when overlay=false', () => {
        applyAllProps({ ...makeInstance('p2'), props: { overlay: false } });
        const releases = bridge.calls.filter((c) => c.fn === 'releaseOverlay');
        expect(releases.length).toBe(1);
        expect(releases[0].args).toEqual(['p2']);
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
    });

    it('does not touch overlay APIs when prop is absent', () => {
        applyAllProps({ ...makeInstance('p3'), props: { background: '#000' } });
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });

    it('commitUpdate flips overlay false → true via claimOverlay', () => {
        applyChangedProps(
            makeInstance('p4'),
            { overlay: false },
            { overlay: true },
        );
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['p4']);
    });

    it('commitUpdate flips overlay true → false via releaseOverlay', () => {
        applyChangedProps(
            makeInstance('p5'),
            { overlay: true },
            { overlay: false },
        );
        const releases = bridge.calls.filter((c) => c.fn === 'releaseOverlay');
        expect(releases.length).toBe(1);
        expect(releases[0].args).toEqual(['p5']);
    });

    it('commitUpdate releases on prop removal (oldProps had overlay=true)', () => {
        // The "prop disappeared" path — equivalent to React unmounting just
        // the overlay attribute without unmounting the whole view.
        applyChangedProps(
            makeInstance('p6'),
            { overlay: true },
            {},
        );
        const releases = bridge.calls.filter((c) => c.fn === 'releaseOverlay');
        expect(releases.length).toBe(1);
        expect(releases[0].args).toEqual(['p6']);
    });

    it('commitUpdate is a no-op when overlay was already truthy and stays truthy', () => {
        applyChangedProps(
            makeInstance('p7'),
            { overlay: true },
            { overlay: true, background: '#fff' },
        );
        // applyChangedProps only re-emits when value !==. Same-true → no
        // additional claim/release; only the background change should fire.
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });
});
