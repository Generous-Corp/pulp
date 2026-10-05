// In-app updates for a JS editor: the client side of the EditorBridge
// messages registered by `pulp::format::add_app_update_handlers()`
// (pulp/format/app_updates_bridge.hpp).
//
// The editor code is shared by every format. In the standalone app the
// status comes back `available: true`; in a plug-in (AU, VST3, CLAP, AAX) it
// comes back `available: false`, and the editor should render no update
// controls at all.
//
//   const updates = appUpdatesClient(globalThis.__myEditorDispatch);
//   const status = await updates.get();
//   if (status.available) { ...render a toggle, a button and status.note... }
//
// Or in a component:
//
//   const [status, actions] = useAppUpdates(dispatch);
//   if (!status?.available) return null;

import { useCallback, useEffect, useState } from 'react';

export type AppUpdateInstaller = 'package' | 'app' | 'unknown';

export interface AppUpdateStatus {
    available: boolean;
    canCheckNow: boolean;
    automaticChecks: boolean;
    automaticInstall: boolean;
    /** A development stub that never contacts a feed. */
    stub: boolean;
    appName: string;
    version: string;
    build: string;
    /** Unix seconds of the last completed check; 0 = never. */
    lastCheckUnixSeconds: number;
    releasesUrl: string;
    feedHost: string;
    installer: AppUpdateInstaller;
    /** Explanatory note built natively from the app's declared update facts. */
    note: string;
    /** "Version 1.0.7" or "Version 1.0.7 (1.0.7.2)". */
    versionText: string;
    /** "Last checked: never" / "Last checked: 2026-10-04 14:03". */
    lastCheckText: string;
}

/** The editor's native dispatch: takes `{type, payload, id}` JSON, returns the response JSON. */
export type EditorDispatch = (requestJson: string) => string | Promise<string>;

export const APP_UPDATES_UNAVAILABLE: AppUpdateStatus = Object.freeze({
    available: false,
    canCheckNow: false,
    automaticChecks: false,
    automaticInstall: false,
    stub: false,
    appName: '',
    version: '',
    build: '',
    lastCheckUnixSeconds: 0,
    releasesUrl: '',
    feedHost: '',
    installer: 'unknown',
    note: '',
    versionText: '',
    lastCheckText: '',
}) as AppUpdateStatus;

export interface AppUpdatesClient {
    get(): Promise<AppUpdateStatus>;
    /** User-initiated check; resolves with `started` and the refreshed status. */
    check(): Promise<AppUpdateStatus & { started: boolean }>;
    setAutomatic(on: boolean): Promise<AppUpdateStatus & { applied: boolean }>;
    /** Open `releasesUrl` in the user's browser (native side). */
    openReleases(): Promise<AppUpdateStatus & { opened: boolean }>;
}

function normalize(response: unknown): AppUpdateStatus {
    if (!response || typeof response !== 'object') return APP_UPDATES_UNAVAILABLE;
    const r = response as Record<string, unknown>;
    // An editor bundled with an SDK older than the update bridge answers
    // "unknown message type"; treat every failure as "no updater here".
    if (r.ok !== true) return APP_UPDATES_UNAVAILABLE;
    const out = { ...APP_UPDATES_UNAVAILABLE, ...(r as Partial<AppUpdateStatus>) };
    out.available = r.available === true;
    return out;
}

/** Wraps an editor dispatch function. Never throws: a missing or failing
 *  dispatch reads as `available: false`. */
export function appUpdatesClient(dispatch: EditorDispatch | null | undefined): AppUpdatesClient {
    let sequence = 0;
    const send = async (type: string, payload: Record<string, unknown> = {}) => {
        if (typeof dispatch !== 'function') return null;
        try {
            const raw = await dispatch(JSON.stringify({ type, payload, id: `pulp-updates-${++sequence}` }));
            return JSON.parse(raw) as Record<string, unknown>;
        } catch {
            return null;
        }
    };
    return {
        async get() {
            return normalize(await send('pulp_updates_get'));
        },
        async check() {
            const raw = await send('pulp_updates_check');
            return { ...normalize(raw), started: raw?.started === true };
        },
        async setAutomatic(on: boolean) {
            const raw = await send('pulp_updates_set_automatic', { on: on === true });
            return { ...normalize(raw), applied: raw?.applied === true };
        },
        async openReleases() {
            const raw = await send('pulp_updates_open_releases');
            return { ...normalize(raw), opened: raw?.opened === true };
        },
    };
}

export interface AppUpdatesActions {
    refresh(): Promise<void>;
    check(): Promise<void>;
    setAutomatic(on: boolean): Promise<void>;
    openReleases(): Promise<void>;
}

/** React hook over appUpdatesClient. `status` is null until the first read
 *  resolves; render nothing until `status?.available` is true. */
export function useAppUpdates(dispatch: EditorDispatch | null | undefined,
                              pollMs = 0): [AppUpdateStatus | null, AppUpdatesActions] {
    const [status, setStatus] = useState<AppUpdateStatus | null>(null);
    const client = appUpdatesClient(dispatch);
    const refresh = useCallback(async () => { setStatus(await client.get()); },
        // eslint-disable-next-line react-hooks/exhaustive-deps
        [dispatch]);
    const check = useCallback(async () => { setStatus(await client.check()); },
        // eslint-disable-next-line react-hooks/exhaustive-deps
        [dispatch]);
    const setAutomatic = useCallback(async (on: boolean) => {
        setStatus(await client.setAutomatic(on));
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [dispatch]);
    const openReleases = useCallback(async () => { await client.openReleases(); },
        // eslint-disable-next-line react-hooks/exhaustive-deps
        [dispatch]);
    useEffect(() => {
        void refresh();
        if (!(pollMs > 0)) return undefined;
        const timer = setInterval(() => { void refresh(); }, pollMs);
        return () => clearInterval(timer);
    }, [refresh, pollMs]);
    return [status, { refresh, check, setAutomatic, openReleases }];
}
