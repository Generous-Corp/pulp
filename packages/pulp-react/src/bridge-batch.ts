// Commit-scoped coalescing for idempotent widget setters.
// Imported UIs can rewrite one captured property more than once in a commit.
// Keep the first ordering slot and send only the last value. Reads and calls
// with other semantics remain synchronous.
type BridgeFn = (...args: unknown[]) => unknown;
type PendingCall = { name: string; fn: BridgeFn; args: unknown[] };

// Only setters whose contract is "replace the value for this widget slot" may
// be coalesced. A name prefix is too broad: future setters may trigger an
// ordered side effect, animation, or host command.
const COALESCEABLE_BRIDGE_SETTERS = new Set([
    'setWidth', 'setHeight', 'setFlex', 'setBackground', 'setBackgroundGradient',
    'setOpacity', 'setVisible', 'setBorderColor', 'setBorderWidth',
    'setBorderRadius', 'setTextColor', 'setFontSize', 'setFontFamily',
    'setWhiteSpace', 'setTextOverflow', 'setDirection', 'setVerticalAlign',
    'setTextDecorationColor', 'setTextDecorationStyle', 'setText',
    'setTextAlign', 'setFontWeight', 'setFontStyle', 'setLetterSpacing',
    'setLineHeight', 'setLineClamp', 'setTextDecoration', 'setShadowColor',
    'setShadowOffset', 'setShadowRadius', 'setTextTransform', 'setFontVariant',
    'setTranslate', 'setRotation', 'setScale', 'setSkew', 'setTransformOrigin',
]);

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
    let firstError: unknown;
    for (const call of calls) {
        try {
            call.fn(...call.args);
        } catch (error) {
            // A failed setter must not strand later independent updates. The
            // first error remains observable after the queue is fully drained.
            firstError ??= error;
        }
    }
    if (firstError !== undefined) throw firstError;
}

export function queueBridgeCall(name: string, fn: BridgeFn, args: unknown[]): boolean {
    if (!active || !COALESCEABLE_BRIDGE_SETTERS.has(name) ||
        typeof args[0] !== 'string') return false;
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
