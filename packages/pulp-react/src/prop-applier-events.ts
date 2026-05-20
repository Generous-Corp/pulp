// prop-applier-events — overlay-claim / ARIA-driven interaction props
// (P5-NEW-A split of the former monolithic applyOne switch).
//
// `applyEventProp(id, key, value)` returns true if it handled the key,
// false otherwise. Behavior is byte-identical to the matching cases in
// the pre-split prop-applier switch — same bridge calls in the same
// order.
//
// Note: `on*` event handlers (onClick, onMouseEnter, …) are NOT routed
// here — they go through `applyEventHandler` in prop-applier.ts, which
// the host-config calls directly. This module covers the declarative
// props that influence event ROUTING (overlay claim, ARIA roles).

import { call } from './prop-applier-internal.js';

/// Apply an overlay / ARIA interaction prop. Returns true if handled.
export function applyEventProp(
    id: string,
    key: string,
    value: unknown,
): boolean {
    switch (key) {
        // pulp #1148 — generalized overlay-click routing. `overlay={true}`
        // claims the view as the active click-eligible overlay so React
        // popovers built on `<View position="absolute">` receive clicks
        // even though hit_test would otherwise resolve to a sibling. The
        // matching releaseOverlay is emitted by applyChangedProps when
        // the prop flips off, and by detach() at unmount.
        case 'overlay':
            if (value) { call('claimOverlay', id); return true; }
            call('releaseOverlay', id);
            return true;

        // pulp ARIA modal/popup auto-overlay — UX best-practice default.
        // When the JSX declares an ARIA role that semantically IS a
        // dismissable overlay (`role="dialog" | "alertdialog" | "menu" |
        // "listbox"`) or sets `aria-modal="true"`, claim the overlay so
        // Esc-dismiss + outside-click routing fire automatically. Pre-fix,
        // every consumer (Spectr's dom-adapter, etc.) had to opt in by
        // mirroring a position:absolute heuristic, which missed inset:0
        // full-screen modal backdrops (the most common modal pattern) and
        // every dropdown/menu authored without explicit positioning.
        //
        // Override semantics: an explicit `overlay={false}` still wins
        // because applyChangedProps emits that case AFTER the role case
        // (object iteration order is insertion order, and JSX collects
        // props left-to-right; `overlay` typically appears after `role`).
        // For defensive parity, an explicit overlay={true} is a no-op on
        // top of the auto-claim (idempotent on the bridge side).
        case 'role': {
            const r = typeof value === 'string' ? value.toLowerCase() : '';
            if (r === 'dialog' || r === 'alertdialog' || r === 'menu' || r === 'listbox') {
                call('claimOverlay', id);
                return true;
            }
            return true;
        }
        case 'aria-modal': {
            const truthy = value === true || value === 'true' || value === '';
            if (truthy) { call('claimOverlay', id); return true; }
            return true;
        }

        default:
            return false;
    }
}
