// prop-applier-controlled — a controlled value control shows the committed
// prop, not the edit its handler declined.
//
// A Knob, Fader, slider or Toggle moves itself before it reports the edit:
// the native widget already shows the new value when `change` or `toggle`
// reaches the handler. A React control with a `value` (or `checked`) prop is
// controlled, so the widget must end up showing that prop. When the handler
// accepts the edit the commit writes the new prop and nothing else is needed;
// when it clamps it, the commit writes the clamped value. When it rejects the
// edit outright — no setState, or a setState to the same value — nothing
// commits, and the widget would keep showing a value the application never
// took. React DOM restores a controlled input in that case; this module does
// the same for the native controls.
//
// The trampoline records the payload an armed control delivered. After the
// outermost dispatch's batch has committed, the bridge calls
// `__pulpAfterDiscreteDispatch__`, and every recorded control whose committed
// prop differs from the payload, and that no commit wrote since the edit, gets
// the committed prop back through `setValue`. That write does not dispatch a
// change event, so the restore cannot re-enter a handler.
//
// A widget bound to a parameter with `bindWidgetToParam` is never armed: the
// binding re-asserts the store's value on the next frame, and a second write
// from here would race it.

import { call } from './prop-applier-internal.js';

type AnyFn = (...args: unknown[]) => unknown;
const g = globalThis as unknown as Record<string, AnyFn | undefined>;

/// The discrete events after which a controlled control is checked.
const DISCRETE_EDIT_EVENTS = new Set(['change', 'toggle', 'input']);

interface ControlledRecord {
    /// The committed prop, in the domain `setValue` takes.
    value: number;
    /// Bumped whenever the applier writes the controlled prop to the widget.
    writes: number;
}

interface PendingEdit {
    /// The value the widget reported, and therefore shows.
    payload: number;
    /// `ControlledRecord.writes` when the edit arrived.
    writesAtEdit: number;
}

const controlled = new Map<string, ControlledRecord>();
const pending = new Map<string, PendingEdit>();

/// The committed controlled value of a prop bag, or undefined when the host
/// is uncontrolled. `value` wins over `checked`; `checked` maps to the 0/1 a
/// Toggle or Checkbox reports.
function controlledValueOf(props: Record<string, unknown>): number | undefined {
    if (typeof props.value === 'number') return props.value;
    if (typeof props.checked === 'boolean') return props.checked ? 1 : 0;
    return undefined;
}

/// Record the committed props of `id` after a full or changed-prop apply.
export function noteControlledProps(id: string, props: Record<string, unknown>): void {
    const value = controlledValueOf(props);
    if (value === undefined) {
        controlled.delete(id);
        pending.delete(id);
        return;
    }
    const record = controlled.get(id);
    if (record) record.value = value;
    else controlled.set(id, { value, writes: 0 });
}

/// Write the controlled prop of `id` to its widget. Every applier write of
/// `value` or `checked` goes through here, so a restore can tell that a
/// commit already wrote the widget after the edit.
export function writeControlledValue(id: string, value: number): void {
    const record = controlled.get(id);
    if (record) record.writes += 1;
    call('setValue', id, value);
}

/// Called by the event trampoline before the handler runs.
export function noteDiscreteEdit(id: string, eventName: string, payload: unknown): void {
    if (!DISCRETE_EDIT_EVENTS.has(eventName)) return;
    if (typeof payload !== 'number' || !Number.isFinite(payload)) return;
    const record = controlled.get(id);
    if (!record) return;
    const bound = g.isWidgetParamBound;
    if (typeof bound === 'function' && bound(id) === true) return;
    pending.set(id, { payload, writesAtEdit: record.writes });
}

/// Put back the committed prop of every control whose edit was not taken.
export function restoreControlledValues(): void {
    if (pending.size === 0) return;
    const edits = Array.from(pending);
    pending.clear();
    for (const [id, edit] of edits) {
        const record = controlled.get(id);
        // Unmounted or turned uncontrolled while the handler ran.
        if (!record) continue;
        // The commit wrote the widget after the edit: it shows the prop.
        if (record.writes !== edit.writesAtEdit) continue;
        if (Object.is(record.value, edit.payload)) continue;
        writeControlledValue(id, record.value);
    }
}

/// Drop the record of a widget that left the tree.
export function forgetControlled(id: string): void {
    controlled.delete(id);
    pending.delete(id);
}

(globalThis as { __pulpAfterDiscreteDispatch__?: unknown }).__pulpAfterDiscreteDispatch__ =
    restoreControlledValues;
