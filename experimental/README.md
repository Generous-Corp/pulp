# Experimental — status decisions

This directory holds in-tree prototypes and evaluation tracks that are
not (yet) part of Pulp's supported surface. Each subdirectory has its
own README; this top-level file records the **current status decision**
for each entry, so contributors don't have to reverse-engineer "is this
shipping?" from per-subdirectory docs that may have drifted.

Maintenance rule: when a subdirectory's status changes (prototype →
production cutover, archival, removal), update the table below in the
same PR.

---

## Current entries

### `pulp-rs/` — NOT experimental anymore

**Status:** Production CLI.

**How to confirm:**

- `CLAUDE.md` documents the cutover: *"source builds produce
  `./build/pulp` as the user-facing CLI and
  `./build/tools/cli/pulp-cpp` as the C++ delegate for commands that
  still live in C++"*.
- `experimental/pulp-rs/CMakeLists.txt` is wired into the root build.
- `tools/cli/` produces the C++ delegate; `experimental/pulp-rs/`
  produces the user-facing `pulp` binary.
- `experimental/pulp-rs/UPSTREAM_SYNC.md` actively tracks 30+ C++
  source files to mirror.

**Where `pulp-rs/README.md` is out of date:**

The per-subdirectory README still describes the crate as a prototype
that *"will never merge to `main`"* and is *"not shipping. Not
user-facing. Not wired into any Pulp build."* Those claims no longer
match the build. They are kept here as a note so a maintainer can
refresh the per-subdirectory README during the relocation PR
described below.

**Recommendation: option (a) — relocate to `tools/cli-rs/`.**

Three options considered:

| Option | Pros | Cons |
|---|---|---|
| **(a) Move to `tools/cli-rs/`** | Matches reality; co-locates with `tools/cli/` C++ delegate; removes "experimental" misnomer; clarifies status for new contributors. | Cost: `CMakeLists.txt` updates, CI workflow updates, doc updates, possibly skill updates. Need to refresh `pulp-rs/README.md` and `UPSTREAM_SYNC.md` accordingly. |
| **(b) Keep in place with corrected README** | Lowest effort. | Misleading location signals "unstable" to readers; the README contradicts the build system. |
| **(c) Archive** | N/A — the code is in active production use. | Wrong call. |

**Why (a):** The cost is bounded (one PR to move the directory + update
CMake/CI/docs), and the benefit is durable (no more "is this real?"
question for every new contributor). Option (b) keeps a permanent
papering-over of the layout-vs-reality mismatch.

**Implementation as a follow-up PR (not this one):**

1. `git mv experimental/pulp-rs tools/cli-rs`.
2. Update `tools/cli-rs/UPSTREAM_SYNC.md` (paths only — references
   remain valid).
3. Refresh the renamed `tools/cli-rs/README.md` head: remove the
   "experimental", "not shipping", "explore branch" framing; describe
   it as the user-facing `pulp` CLI.
4. Update root `CMakeLists.txt` references to the renamed path.
5. Update any CI workflows that reference `experimental/pulp-rs/`.
6. Update `CLAUDE.md` to reference the new path.
7. Update this top-level `experimental/README.md` to remove the
   `pulp-rs/` row (this README continues to document any future
   experimental tracks).
8. Update the relevant skill (cli-maintenance, or a new rust-cli
   skill) per the Skill Maintenance Rule in `CLAUDE.md`.

This file is the **decision artifact**. The physical move is a
separate PR with its own diff-coverage gate and version bump.

---

## Future experimental entries

When adding a new entry under `experimental/`:

1. Create the subdirectory.
2. Add a row to the "Current entries" section above.
3. State explicitly: what's the evaluation criteria? When will this be
   promoted or archived?
4. Link the GitHub issue tracking the evaluation.

The implicit rule: **`experimental/` is for prototypes with a stated
exit criterion.** Anything that's quietly shipping should not live here.
