# React compat

Reserved for React-specific features that aren't single-prop entries:
Suspense, ConcurrentMode, Profiler, error boundaries, refs, portals,
hooks edge cases (`useTransition`, `useDeferredValue`, `useId`,
`useSyncExternalStore`).

The single-prop surface (`fontSize`, `border`, `data`, etc.) is
captured under [`rn`](rn.md), which mirrors what
`packages/pulp-react/src/prop-applier.ts` actually dispatches.

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

## Status

As of 2026-05-04 the renderer is built on `react-reconciler@0.31` and
supports the basic function-component + `useState` + `useEffect` +
`useRef` + `useMemo` / `useCallback` set; concurrent features are not
exercised. The matrix entries here are currently empty pending a
dedicated end-to-end React-feature audit.

Platform hosts deliver global key events through the core view
script-events hook when `pulp::view-script` is linked. `WidgetBridge`
registers the React/runtime fan-out from the keyboard-input translation
unit, so `window.addEventListener('keydown', ...)` and
`registerShortcut(...)` continue to work for React apps without forcing
core-only `pulp::view-core` consumers to link the JS runtime.

The global-key translation also preserves the W3C punctuation value for the
standard settings chord: Command+Comma on macOS and Control+Comma elsewhere
arrive with `key === ","` (or `"<"` when Shift is held). Applications may
therefore use the same global listener or shortcut registration in native and
browser-backed React surfaces.

The renderer exposes a typed `<VirtualList>` intrinsic for large rich-row
lists. It lowers to the native `VirtualList` widget, keeps a bounded recycled
row pool, and routes `rowCount`, fixed `rowHeight`, `overscan`,
`selectionMode`, `selected`, `scrollToRow`, and `renderRow` through the
bridge. Lowercase `virtual-list` / `virtuallist` DOM tags reach the same native
widget for web-compat consumers.

Follow-up: enumerate observed React features (concurrent rendering,
suspense boundaries, transition state, error boundaries, portal
targets) and populate per-feature entries.

## Removed style keys

A style key that disappears from a commit, or whose value becomes `null` or
`undefined`, hands its native slots back to the widget's starting value, so
the conditional spread `style={{...base, ...(active ? activeStyle : {})}}`
and a whole conditional `style={hidden ? s : undefined}` both undo what the
active style painted. Layout slots reset through `resetStyleProperty`, which
restores the value the widget had before its first layout write; other keys
reset through their own setter. A surviving key that shares a slot with a
removed one, and a default the host config gave the widget when it created
it, are applied again afterwards. A render that omits its `style` prop
entirely is read as "no style source", not as a removal, and resets nothing.

`packages/pulp-react/src/prop-applier-reset.ts` holds the decision for every
key. Known gaps, each recorded there as `gap`:

- `top`, `right`, `bottom`, `left`, `inset*`, `start` and `end` are not
  reset, so a hover lift written `{position: 'relative', top: -2}` stays
  lifted after the hover ends. View has no way to clear an inset once set.
- Text colour, and border, outline and decoration colours, have no unset
  argument; text colour stays painted, and borders and outlines are hidden
  by width instead.
- Label typography (font size, weight, letter spacing, alignment) resets to
  the Label's constructed value as its own value, so it stops inheriting
  that property from an ancestor.

## Hit-test-only props

`hitSlop` is applied by `prop-applier-paint.ts` and reaches
`View::set_hit_slop` without entering layout or paint. The applier accepts
every form the style adapter does — number, 1-4-number shorthand string, and
RN object — so the two authoring surfaces resolve identical input to
identical edges. Emitting only the number form is a silent half-fix: a
string falls through to a slop of zero with no error anywhere, which reads
as "the fix did not work" rather than "the value was not understood". See
`rn/hitSlop` for the grammar.

## Arming: a handler prop is not a subscription

`prop-applier` turns every `on*` prop into `on(id, <lowercased name>, cb)`, but
`on()` is what decides whether native dispatch is ARMED for that name. A name it
does not route reaches `__callbacks__` and nothing else: the handler is
installed, looks correct in the tree, and is never called. `onContextMenu` sat in
that state — the whole `contextmenu` group was missing from `on()` and from
`__ensureNativeRegistered__`, so `registerContextMenu` was never issued and
`View::on_context_menu` stayed null. The routed groups are `click`, `hover`,
`pointer`, `gesture`, `wheel` and `contextmenu`; a prop outside them is inert.

`onDoubleClick` is still in that state, and is the reason to check rather than
assume: `eventNameFor` produces `doubleclick`, which is neither the DOM's
`dblclick` nor the gesture registrar's `doubletap`, so nothing produces it. The
test for "is this prop live" is whether `on()` routes its name to a registrar —
not whether the prop applies without error.

`onContextMenu` receives a DOM-shaped payload: `clientX`/`clientY` in the space
`getBoundingClientRect()` reports (so `e.clientX - rect.left` cancels exactly),
widget-local `offsetX`/`offsetY`, and `button`/`buttons` of 2. `contextmenu`
bubbles, so a listener on a wrapper still fires when the press lands on a child
that has no listener of its own — the common case of a canvas drawn over its
container.

## Delegated event delivery

React's delegated root listener now receives each native wheel tick once.
`__dispatch__` owns the element fan-out and normalizes legacy positional wheel
deltas there; the per-event registration callback does not dispatch again.
Handler exceptions stay inside the bridge dispatch boundary so one component
error cannot terminate requestAnimationFrame rescheduling.
