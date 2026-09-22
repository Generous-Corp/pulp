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

/// The overlay this view DECLARES it stacks on, as the widget id
/// `claimOverlay` speaks, or undefined when nothing was declared.
///
/// `View::claim_overlay()` nests a claim only when it descends from the open
/// overlay. A submenu placed to escape its menu's box is lifted out of that
/// menu's subtree — `position: fixed`, a portal, a returned fragment — so it is
/// a SIBLING of the menu it belongs to, the parent-chain walk cannot see the
/// relationship, and the menu underneath is dismissed as a rival together with
/// the submenu's own rows. Neither `overlay` nor `role` can supply the missing
/// fact: both are equally true of the menu and of its submenu. So it is
/// declared, and read here from the WHOLE prop bag rather than from one key, so
/// JSX key order cannot decide whether the declaration reaches the claim — the
/// same reason `applySvgPathStrokeState` treats stroke + gradient as one
/// compound state.
///
/// Never inferred. An inferred parent would be precisely the "nest on whatever
/// happened to be open" bypass the descendant rule exists to prevent; and a
/// declaration that does not name a live, currently-open overlay under this
/// view's own root is ignored natively, so a stale name degrades to the
/// ordinary claim.
function declaredOverlayParent(
    props?: Record<string, unknown>,
): string | undefined {
    if (!props) return undefined;
    const parent = props.overlayParent;
    if (typeof parent !== 'string' || parent === '') return undefined;
    return parent;
}

/// `claimOverlay(id, consume)`, plus the declared parent when there is one.
/// The third argument is omitted rather than passed empty so an undeclared
/// claim emits the exact call it always has.
function emitOverlayClaim(id: string, props?: Record<string, unknown>): void {
    const parent = declaredOverlayParent(props);
    if (parent === undefined) call('claimOverlay', id, true);
    else call('claimOverlay', id, true, parent);
}

/// Does this prop bag claim an overlay at all? `overlayParent` on its own
/// declares a relationship for a view that is not an overlay, which is nothing
/// — so a change to it re-emits the claim only for a view that has one.
function claimsOverlay(props?: Record<string, unknown>): boolean {
    if (!props) return false;
    if (props.overlay) return true;
    const r = typeof props.role === 'string' ? props.role.toLowerCase() : '';
    if (r === 'dialog' || r === 'alertdialog' || r === 'menu' || r === 'listbox')
        return true;
    const modal = props['aria-modal'];
    return modal === true || modal === 'true' || modal === '';
}

/// Apply an overlay / ARIA interaction prop. Returns true if handled.
export function applyEventProp(
    id: string,
    key: string,
    value: unknown,
    props?: Record<string, unknown>,
): boolean {
    switch (key) {
        // Generalized overlay-click routing. `overlay={true}` claims the
        // view as the active click-eligible overlay so React popovers built
        // on `<View position="absolute">` receive clicks even though
        // hit_test would otherwise resolve to a sibling. The matching
        // releaseOverlay is emitted by applyChangedProps when the prop flips
        // off, and by detach() at unmount.
        case 'overlay':
            if (value) { emitOverlayClaim(id, props); return true; }
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
                emitOverlayClaim(id, props);
                return true;
            }
            return true;
        }
        case 'aria-modal': {
            const truthy = value === true || value === 'true' || value === '';
            if (truthy) { emitOverlayClaim(id, props); return true; }
            return true;
        }

        // Declares the overlay a lifted submenu stacks on. Handled, but it
        // emits nothing of its own: it QUALIFIES the claim the arms above make
        // and is read from the prop bag by all of them, so claiming here too
        // would send the same claim twice for every mount. The update path,
        // where the declaration can move without any claiming key moving with
        // it, is `reclaimOverlayForMovedParent` below.
        case 'overlayParent':
            return true;

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

/// Re-emit an overlay claim when ONLY its declared parent moved.
///
/// `commitUpdate` applies the keys whose values changed. When `overlay`,
/// `role`, or `aria-modal` is one of them the claim is re-emitted anyway and
/// carries the current declaration; when an author re-points a lifted submenu
/// at a different menu and changes nothing else, nothing would, and the native
/// side would keep nesting on the menu that is no longer the parent.
///
/// Returns whether it emitted, so the caller can report a mutation.
export function reclaimOverlayForMovedParent(
    id: string,
    oldProps: Record<string, unknown>,
    newProps: Record<string, unknown>,
): boolean {
    if (oldProps.overlayParent === newProps.overlayParent) return false;
    // A view that claims no overlay has no claim for a declaration to qualify.
    if (!claimsOverlay(newProps)) return false;
    // Each claiming arm re-emits on its own when its OWN key moved, so acting
    // here as well would double the claim — and when `overlay` moved to false
    // that arm releases, which this must not undo.
    if (oldProps.overlay !== newProps.overlay) return false;
    if (oldProps.role !== newProps.role) return false;
    if (oldProps['aria-modal'] !== newProps['aria-modal']) return false;
    emitOverlayClaim(id, newProps);
    return true;
}
