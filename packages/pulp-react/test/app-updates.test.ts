// The JS side of pulp::format::add_app_update_handlers(): the shared editor
// code must read `available: false` in a plug-in (no updater installed, or an
// SDK without the bridge) and drive the standalone's updater otherwise.
import { describe, it, expect } from 'vitest';

import { appUpdatesClient, APP_UPDATES_UNAVAILABLE } from '../src/app-updates.js';

type Request = { type: string; payload: Record<string, unknown>; id: string };

const STANDALONE = {
    available: true, canCheckNow: true, automaticChecks: true, automaticInstall: false,
    stub: false, appName: 'Spectr', version: '1.0.7', build: '1.0.7',
    lastCheckUnixSeconds: 0, releasesUrl: 'https://github.com/danielraffel/spectr/releases',
    feedHost: 'github.com', installer: 'package', note: 'Updates download from …',
    versionText: 'Version 1.0.7', lastCheckText: 'Last checked: never',
};

function standaloneDispatch(log: Request[]) {
    const state = { ...STANDALONE };
    return (json: string) => {
        const req = JSON.parse(json) as Request;
        log.push(req);
        if (req.type === 'pulp_updates_get') return JSON.stringify({ ok: true, ...state });
        if (req.type === 'pulp_updates_check') {
            state.lastCheckUnixSeconds = 1790000000;
            return JSON.stringify({ ok: true, ...state, started: true });
        }
        if (req.type === 'pulp_updates_open_releases')
            return JSON.stringify({ ok: true, ...state, opened: true });
        if (req.type === 'pulp_updates_set_automatic') {
            state.automaticChecks = req.payload.on === true;
            return JSON.stringify({ ok: true, ...state, applied: true });
        }
        return JSON.stringify({ ok: false, error: 'unknown message type' });
    };
}

describe('appUpdatesClient', () => {
    it('reads unavailable when there is no dispatch at all', async () => {
        expect(await appUpdatesClient(undefined).get()).toEqual(APP_UPDATES_UNAVAILABLE);
    });

    it('reads unavailable from a plug-in (handler answers available:false)', async () => {
        const plugin = () => JSON.stringify({ ok: true, ...APP_UPDATES_UNAVAILABLE });
        const status = await appUpdatesClient(plugin).get();
        expect(status.available).toBe(false);
        const checked = await appUpdatesClient(plugin).check();
        expect(checked.started).toBe(false);
    });

    it('reads unavailable from an SDK without the bridge (unknown message type)', async () => {
        const old = () => JSON.stringify({ ok: false, error: 'unknown message type' });
        expect((await appUpdatesClient(old).get()).available).toBe(false);
    });

    it('never throws on a dispatch that throws or returns garbage', async () => {
        const throwing = () => { throw new Error('boom'); };
        expect((await appUpdatesClient(throwing).get()).available).toBe(false);
        expect((await appUpdatesClient(() => 'not json').get()).available).toBe(false);
    });

    it('drives the standalone: status, check, automatic toggle', async () => {
        const log: Request[] = [];
        const client = appUpdatesClient(standaloneDispatch(log));
        const status = await client.get();
        expect(status.available).toBe(true);
        expect(status.appName).toBe('Spectr');

        const checked = await client.check();
        expect(checked.started).toBe(true);
        expect(checked.lastCheckUnixSeconds).toBeGreaterThan(0);

        const off = await client.setAutomatic(false);
        expect(off.applied).toBe(true);
        expect(off.automaticChecks).toBe(false);

        expect((await client.openReleases()).opened).toBe(true);

        expect(log.map(r => r.type)).toEqual(['pulp_updates_get', 'pulp_updates_check',
            'pulp_updates_set_automatic', 'pulp_updates_open_releases']);
        expect(log[2].payload).toEqual({ on: false });
        expect(new Set(log.map(r => r.id)).size).toBe(4);
    });

    it('accepts an async dispatch', async () => {
        const log: Request[] = [];
        const sync = standaloneDispatch(log);
        const status = await appUpdatesClient(async json => sync(json)).get();
        expect(status.available).toBe(true);
    });
});
