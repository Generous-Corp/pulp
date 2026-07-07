/// <reference path="./bridge-globals.generated.d.ts" />

import { bridgeSafeMockFunctionNames } from './bridge-mock-safe-functions.generated.js';

/// Test-only mock-bridge for unit tests. Replaces all the global
/// bridge functions with a recorder that captures calls to a log,
/// so we can assert that the host config emits the right setX
/// sequences without spinning up the full Pulp runtime.
export interface MockBridgeCall {
    fn: string;
    args: unknown[];
}

export interface MockBridge {
    calls: MockBridgeCall[];
    install(): void;
    uninstall(): void;
    reset(): void;
}

export function createMockBridge(functionNames: readonly string[] = bridgeSafeMockFunctionNames): MockBridge {
    const calls: MockBridgeCall[] = [];
    const fns = functionNames;
    const saved: Record<string, unknown> = {};
    return {
        calls,
        install() {
            for (const fn of fns) {
                saved[fn] = (globalThis as Record<string, unknown>)[fn];
                (globalThis as Record<string, unknown>)[fn] =
                    (...args: unknown[]) => { calls.push({ fn, args }); };
            }
        },
        uninstall() {
            for (const fn of fns) {
                (globalThis as Record<string, unknown>)[fn] = saved[fn];
            }
        },
        reset() { calls.length = 0; },
    };
}
