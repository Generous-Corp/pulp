// SPDX-License-Identifier: MIT

export function armCleanupDeadline({
  timeoutMs,
  cleanup,
  onExpired,
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
