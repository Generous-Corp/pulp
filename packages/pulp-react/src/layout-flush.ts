type Flush = () => void;

let suppressDepth = 0;
let deferredFlush: Flush | undefined;
let flushScheduled = false;

function enqueueDeferredFlush(): void {
    if (flushScheduled) return;
    flushScheduled = true;
    const run = () => {
        flushScheduled = false;
        if (suppressDepth > 0) {
            enqueueDeferredFlush();
            return;
        }
        const flush = deferredFlush;
        deferredFlush = undefined;
        flush?.();
    };
    const raf = (globalThis as { requestAnimationFrame?: (fn: () => void) => unknown }).requestAnimationFrame;
    if (typeof raf === 'function') {
        raf(run);
    } else if (typeof setTimeout === 'function') {
        setTimeout(run, 1);
    } else {
        flushScheduled = false;
    }
}

export function requestLayoutFlush(flush: Flush): void {
    if (suppressDepth > 0) {
        deferredFlush = flush;
        enqueueDeferredFlush();
        return;
    }
    flush();
}

export function withSuppressedLayoutFlush<T>(fn: () => T): T {
    ++suppressDepth;
    try {
        return fn();
    } finally {
        --suppressDepth;
    }
}
