// SPDX-License-Identifier: MIT

// A capture that stalls inside one never-resolving browser call would otherwise
// reach its deadline reporting nothing but "timed out". The tracker keeps the
// coarse phase plus the individual steps in flight, so an expired deadline can
// name the last thing that finished and the thing that never did.
export function createCaptureProgress(initialPhase = "startup") {
  let phase = initialPhase;
  let completed;
  const inFlight = new Map();
  let nextToken = 1;

  const describeStep = (step, elapsedMs) => `${step} (${elapsedMs}ms)`;

  return {
    get phase() {
      return phase;
    },
    enterPhase(next) {
      phase = next;
    },
    // Returns a settle callback. A step that gave up rather than answering is
    // settled with `false` so it leaves the in-flight set without being
    // reported as the last thing that completed.
    begin(step) {
      const token = nextToken++;
      inFlight.set(token, { step, startedAt: Date.now() });
      return (didComplete = true) => {
        const entry = inFlight.get(token);
        if (!entry) return;
        inFlight.delete(token);
        if (didComplete) {
          completed = { step: entry.step, finishedAt: Date.now() };
        }
      };
    },
    describe(now = Date.now()) {
      const parts = [`phase=${phase}`];
      parts.push(completed
        ? `last-completed=${
          describeStep(completed.step, now - completed.finishedAt)} ago`
        : "last-completed=none");
      // Concurrent calls are rare outside page configuration; report a bounded
      // set so one wedged step is never buried under a burst of siblings.
      const stalled = [...inFlight.values()]
        .sort((left, right) => left.startedAt - right.startedAt)
        .slice(0, 3)
        .map((entry) => describeStep(entry.step, now - entry.startedAt));
      if (stalled.length > 0) parts.push(`stalled=${stalled.join(", ")}`);
      return parts.join(" ");
    },
  };
}

export function armCleanupDeadline({
  timeoutMs,
  cleanup,
  onExpired,
  // Cleanup tears down the connection the stalled work was waiting on, which
  // erases the evidence of what stalled. Anything that has to observe the live
  // state of an expired capture runs here, before cleanup.
  onExpiring = () => {},
}) {
  if (!Number.isInteger(timeoutMs) || timeoutMs <= 0) {
    throw new Error("cleanup deadline must be a positive integer");
  }
  if (typeof cleanup !== "function" || typeof onExpired !== "function") {
    throw new Error("cleanup deadline requires cleanup and expiry callbacks");
  }

  let state = "armed";
  const timer = setTimeout(async () => {
    if (state !== "armed") return;
    state = "expired";
    onExpiring();
    let cleanupError;
    try {
      await cleanup();
    } catch (error) {
      cleanupError = error;
    }
    await onExpired(cleanupError);
  }, timeoutMs);

  return () => {
    if (state !== "armed") return false;
    state = "cancelled";
    clearTimeout(timer);
    return true;
  };
}
