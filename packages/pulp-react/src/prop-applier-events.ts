// prop-applier-events — overlay-claim / ARIA-driven interaction props.
//
// `applyEventProp(id, key, value)` returns true if it handled the key,
// false otherwise. Each handled prop emits the bridge calls expected by
// the corresponding event-routing contract.
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
        // Generalized overlay-click routing. `overlay={true}` claims the
        // view as the active click-eligible overlay so React popovers built
        // on `<View position="absolute">` receive clicks even though
        // hit_test would otherwise resolve to a sibling. The matching
        // releaseOverlay is emitted by applyChangedProps when the prop flips
        // off, and by detach() at unmount.
        case 'overlay':
            if (value) { call('claimOverlay', id, true); return true; }
            call('releaseOverlay', id);
            return true;

        // ARIA modal/popup auto-overlay. This makes semantically modal
        // controls dismissable by default without each consumer duplicating
        // overlay-position heuristics.
        // When the JSX declares an ARIA role that semantically IS a
        // dismissable overlay (`role="dialog" | "alertdialog" | "menu" |
        // "listbox"`) or sets `aria-modal="true"`, claim the overlay so
        // Esc-dismiss + outside-click routing fire automatically.
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
                call('claimOverlay', id, true);
                return true;
            }
            return true;
        }
        case 'aria-modal': {
            const truthy = value === true || value === 'true' || value === '';
            if (truthy) { call('claimOverlay', id, true); return true; }
            return true;
        }

        // The counterpart of the two arms above. They say "this element IS a
        // dismissable overlay"; `aria-haspopup` says "this control OPENS one".
        // The dismissal policy needs both: it claims a popover with
        // consume=true so a press outside it cannot also operate the underlay,
        // and `View::overlay_trigger()` is the exception that keeps that rule
        // from taxing the press the user actually meant. Without the mark, a
        // press on a second dropdown's trigger while the first is open is
        // spent entirely on the dismissal, so switching menus costs two
        // presses instead of one.
        //
        // Reading it from ARIA rather than from a Pulp-specific prop is the
        // point: an app that has already described its own menus for assistive
        // technology has said everything the policy needs, and until now Pulp
        // honoured only the half of that description that made presses
        // disappear. `data-overlay-trigger` (web-compat) and an explicit
        // `overlayTrigger` prop remain available for documents that do not use
        // ARIA.
        //
        // Scoped to triggers deliberately -- ordinary content stays consumed,
        // or clicking away from a menu would also operate whatever sits under
        // the click. Any ARIA token other than absent/"false" marks
        // (true|menu|listbox|tree|grid|dialog); "false" unmarks, so a control
        // that stops offering a popup stops being a trigger.
        case 'aria-haspopup': {
            const token = typeof value === 'string' ? value.toLowerCase() : value;
            const isTrigger = token === true
                || (typeof token === 'string' && token !== '' && token !== 'false');
            call('setOverlayTrigger', id, isTrigger);
            return true;
        }

        // An explicit opt-in for documents that do not author ARIA. Same
        // effect, stated in Pulp's own vocabulary.
        case 'overlayTrigger':
            call('setOverlayTrigger', id, !!value);
            return true;

        default:
            return false;
    }
}
