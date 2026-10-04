// Commit-scoped coalescing for idempotent widget setters.
// Imported UIs can rewrite one captured property more than once in a commit.
// Keep the first ordering slot and send only the last value. Reads and calls
// with other semantics remain synchronous.
type BridgeFn = (...args: unknown[]) => unknown;
type PendingCall = { name: string; fn: BridgeFn; args: unknown[] };

let active = false;
let pending: PendingCall[] = [];
const byKey = new Map<string, number>();

export function beginBridgeBatch(): void {
    if (active) throw new Error('@pulp/react: nested bridge batch');
    active = true;
    pending = [];
    byKey.clear();
}

export function endBridgeBatch(): void {
    if (!active) return;
    const calls = pending;
    pending = [];
    byKey.clear();
    active = false;
    for (const call of calls) call.fn(...call.args);
}

export function queueBridgeCall(name: string, fn: BridgeFn, args: unknown[]): boolean {
    if (!active || !name.startsWith('set') || typeof args[0] !== 'string') return false;
    const key = `${name}\u0000${args[0]}`;
    const existing = byKey.get(key);
    if (existing === undefined) {
        byKey.set(key, pending.length);
        pending.push({ name, fn, args: [...args] });
    } else {
        pending[existing] = { name, fn, args: [...args] };
    }
    return true;
}
