# React compat

Reserved for React-specific features that aren't single-prop entries:
Suspense, ConcurrentMode, Profiler, error boundaries, refs, portals,
hooks edge cases (`useTransition`, `useDeferredValue`, `useId`,
`useSyncExternalStore`).

The single-prop surface (`fontSize`, `border`, `data`, etc.) is
captured under [`rn`](rn.md), which mirrors what
`packages/pulp-react/src/prop-applier.ts` actually dispatches.

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

## Status

As of 2026-05-04 the renderer is built on `react-reconciler@0.31` and
supports the basic function-component + `useState` + `useEffect` +
`useRef` + `useMemo` / `useCallback` set; concurrent features are not
exercised. The matrix entries here are currently empty pending a
dedicated end-to-end React-feature audit.

Follow-up: enumerate observed React features (concurrent rendering,
suspense boundaries, transition state, error boundaries, portal
targets) and populate per-feature entries.
