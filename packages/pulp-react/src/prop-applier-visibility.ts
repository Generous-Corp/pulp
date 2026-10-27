// prop-applier-visibility — the one writer of a widget's visibility, shared
// by the author's props and by Suspense.
//
// A suspended <Suspense> boundary keeps its primary children mounted and asks
// the renderer to hide them; when it resolves it asks for them back. The
// author's own `visible` and `display` keep committing to those widgets in
// the meantime, and removing either key resets it. So two parties write the
// same native flag, and the boundary must win while it holds the widget
// without erasing what the author asked for.
//
// Every authored write lands in `writeVisibility`: it is remembered, and it
// reaches the bridge only while no boundary holds the widget. Revealing
// writes the remembered value back, so a widget the author hid stays hidden
// after its boundary resolves. Only widgets that are suspended or authored
// hidden keep a record; every other widget is visible by default.

import { call } from './prop-applier-internal.js';

interface VisibilityRecord {
    /// The value the author last asked for.
    authored: boolean;
    /// A suspended boundary is holding this widget hidden.
    suspended: boolean;
}

const records = new Map<string, VisibilityRecord>();

/// Apply the author's visibility for `id`, or remember it for the reveal
/// while a suspended boundary holds the widget hidden.
export function writeVisibility(id: string, authored: boolean): void {
    const record = records.get(id);
    if (record?.suspended) {
        record.authored = authored;
        return;
    }
    if (authored) records.delete(id);
    else records.set(id, { authored, suspended: false });
    call('setVisible', id, authored);
}

/// Hide `id` for a suspended boundary.
export function suspendVisibility(id: string): void {
    const record = records.get(id);
    if (record) record.suspended = true;
    else records.set(id, { authored: true, suspended: true });
    call('setVisible', id, false);
}

/// Reveal `id` after its boundary resolved, restoring the author's value.
export function resumeVisibility(id: string): void {
    const authored = records.get(id)?.authored ?? true;
    if (authored) records.delete(id);
    else records.set(id, { authored, suspended: false });
    call('setVisible', id, authored);
}

/// Drop the record for a widget that left the tree.
export function forgetVisibility(id: string): void {
    records.delete(id);
}
