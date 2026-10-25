// SPDX-License-Identifier: MIT

// A capture that stalls inside one never-resolving browser call would otherwise
// reach its deadline reporting nothing but "timed out". The tracker keeps the
// coarse phase plus the individual steps in flight, so an expired deadline can
// name the last thing that finished and the thing that never did.
//
// The same transitions are the capture's clock. Every millisecond from the
// tracker's creation belongs to exactly one phase, so the phase totals sum to
// the elapsed time and no stage of a slow capture goes unattributed. A phase
// entered more than once accumulates. Steps are timed too, but they overlap
// each other and nest inside phases, so their totals are not additive.
export function createCaptureProgress(
  initialPhase = "startup", now = () => performance.now()) {
  let phase = initialPhase;
  let completed;
  const inFlight = new Map();
  let nextToken = 1;
  const startedAt = now();
  let phaseStartedAt = startedAt;
  const phaseTotals = new Map();
  const stepTotals = new Map();

  const closePhase = (at) => {
    phaseTotals.set(phase, (phaseTotals.get(phase) ?? 0) + (at - phaseStartedAt));
    phaseStartedAt = at;
  };
  const roundMs = (value) => Math.round(value * 10) / 10;

  const describeStep = (step, elapsedMs) => `${step} (${elapsedMs}ms)`;

  // Returns a settle callback. A step that gave up rather than answering is
  // settled with `false` so it leaves the in-flight set without being
  // reported as the last thing that completed.
  const begin = (step) => {
    const token = nextToken++;
    inFlight.set(token, { step, startedAt: Date.now(), clockAt: now() });
    return (didComplete = true) => {
      const entry = inFlight.get(token);
      if (!entry) return;
      inFlight.delete(token);
      const total = stepTotals.get(entry.step) ?? { count: 0, ms: 0 };
      total.count += 1;
      total.ms += now() - entry.clockAt;
      stepTotals.set(entry.step, total);
      if (didComplete) {
        completed = { step: entry.step, finishedAt: Date.now() };
      }
    };
  };

  return {
    get phase() {
      return phase;
    },
    enterPhase(next) {
      closePhase(now());
      phase = next;
    },
    begin,
    // Times a unit of work under its own step name. Use it for work no single
    // protocol call names, such as a multi-call evaluation or a file write.
    async time(step, work) {
      const settle = begin(step);
      try {
        const result = await work();
        settle();
        return result;
      } catch (error) {
        settle(false);
        throw error;
      }
    },
    // Closes the running phase at the moment of the call. The current phase
    // keeps running, so a later call reports its further time.
    timings() {
      const at = now();
      const phases = {};
      for (const [name, ms] of phaseTotals) phases[name] = ms;
      phases[phase] = (phases[phase] ?? 0) + (at - phaseStartedAt);
      const steps = {};
      for (const [name, total] of [...stepTotals].sort(
        (left, right) => right[1].ms - left[1].ms)) {
        steps[name] = { count: total.count, ms: roundMs(total.ms) };
      }
      return {
        schema: "pulp-browser-capture-timings-v1",
        total_ms: roundMs(at - startedAt),
        phases: Object.fromEntries(
          Object.entries(phases).map(([name, ms]) => [name, roundMs(ms)])),
        steps,
      };
    },
    describe(at = Date.now()) {
      const parts = [`phase=${phase}`];
      parts.push(completed
        ? `last-completed=${
          describeStep(completed.step, at - completed.finishedAt)} ago`
        : "last-completed=none");
      // Concurrent calls are rare outside page configuration; report a bounded
      // set so one wedged step is never buried under a burst of siblings.
      const stalled = [...inFlight.values()]
        .sort((left, right) => left.startedAt - right.startedAt)
        .slice(0, 3)
        .map((entry) => describeStep(entry.step, at - entry.startedAt));
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
