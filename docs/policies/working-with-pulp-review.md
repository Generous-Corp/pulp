# Working with Pulp's review process (for contributor agents)

*A short contract for AI agents opening pull requests against Pulp. Read this before
your first PR. It tells you how review works here and how to be maximally useful — so
your change lands with the fewest round-trips.*

Pulp welcomes agent-authored contributions. Contributions are validated by automation and
**merged by a human maintainer.** Automation advises; a person decides.

---

## How review works here

1. **You open a PR** against `main`. Every revision you push is validated **in isolation**
   (a first PR waits for a maintainer to wave validation through — see *When first
   contributing*). Contributor familiarity affects *communication*, not *revision
   validation* — including for a contributor whose earlier work was merged. Do not expect
   scrutiny to relax on a later push; every revision is validated on its own merits.
2. **Automation and the review workflow run** builds, tests, and review against your exact
   head. Findings come back as **normal GitHub review comments.**
3. **You own the substantive fixes** — design, code, tests, and provenance. Address findings
   and push a new head; it is revalidated. **Maintainers normally handle the mechanical
   integration** — rebases onto a moving base, version collisions, generated-file markers,
   and final merge prep — so you don't have to fight the repository's plumbing. A "changes
   requested" is not a rejection.
4. **A human merges.** No automation merges your PR. When validation is clean, a maintainer
   makes the final call.

**Access to Pulp's review and execution infrastructure is never part of contributing.** Your
changes are validated in an isolated environment with no access to repository secrets or
credentials. A contribution must be **self-contained** — it must not depend on credentials,
private network access, or undeclared downloads to build or pass its tests. Don't design
around the environment, request access to it, or probe it; doing so ends the review, not just
the run. Every external revision is handled the same way.

---

## How to be maximally useful (this is the efficient path)

**One focused change per PR.** A PR should do exactly what its title says and touch only the
files that change requires. A diff that wanders outside its stated purpose is the single most
common reason a PR stalls — it is hard to review, easy to get wrong, and it fights the merge
process. If you find two things to fix, open two PRs.

**Say what and why, briefly.** A tight description — the problem, the approach, the blast
radius — lets review focus on the code instead of reconstructing intent. Link the issue if
there is one.

**Ship the test with the change, and build it locally first.** A fix without a test that
would have caught it will be asked for one; DSP or behavior changes need golden-file or unit
coverage. Every PR runs CI on macOS, Linux, and Windows and cannot merge until all three are
green — so build and run the tests locally before you push. See
[Agent Contribution Rules](agent-contribution-rules.md) for exactly when tests, docs, and
status manifests are required.

**Declare provenance, and sign off.** Every commit must carry a DCO sign-off
(`Signed-off-by: Name <email>`, or `git commit -s`) — this certifies you can submit the code
under Pulp's MIT license. For adapted code, also state the source and its license in the PR
description (`original work`, or `adapted from <source> (<license>)`). Pulp cannot take
copyleft or unattributed third-party code; a vendored snippet without a clear, compatible
license blocks the PR.

**Mind the title.** A `fix:` or `feat:` title raises Pulp's version-bump requirement. You do
not need to author the version bump yourself — that is mechanical integration a maintainer
normally handles — but a mislabeled title creates avoidable churn, so title the PR for what
the change actually is.

**Keep the diff clean.** No unrelated reformatting, no bundled version bumps, no vendored
blobs, no secrets or tokens in the diff, no generated artifacts checked in by accident.

**Satisfy the checks; report gate friction.** Pulp's checks (such as spelling, size limits,
and licensing) encode real past incidents — satisfy them rather than bypass them. If a gate
looks repo-specific or stale, say so in the PR; maintainers own mechanical integration and
will sort it. Green gates are the fast path.

**Leave security, CI, and release plumbing alone unless it's in agreed scope.** Changes to
signing, workflows, branch protection, or release automation need a maintainer to have
explicitly agreed they're in scope first. An unsolicited PR touching those will be held
regardless of how clean it is.

---

## What to expect back

- **Findings as GitHub review comments** — actionable, tied to specific lines. Address them
  and push; the new head re-validates.
- **A material blocker or an ambiguous decision** may pause review while a maintainer weighs
  in. That is normal for anything with a real trade-off.
- **The merge decision is a human's.** No automation merges; even a fully green PR waits for
  a maintainer.

The fastest merges are small, well-described, well-tested, cleanly-scoped changes with clear
provenance that leave the plumbing alone. Everything above is in service of that.

---

## Before you push — checklist

- [ ] **One focused change** — the diff does only what the title says
- [ ] **Title fits the change** (`fix:` / `feat:` raise a maintainer-handled version bump)
- [ ] **Tests included**, and they pass in a local build
- [ ] **Provenance declared** + every commit `Signed-off-by:` (DCO)
- [ ] **Clean diff** — no unrelated reformatting, vendored blobs, secrets, or stray artifacts
- [ ] **Plumbing untouched** — no signing / CI / branch-protection / release changes unless agreed

---

## When first contributing

A first-time contributor's PR may receive additional maintainer triage. This is routine and
not a judgment of you; it is how any first contact is handled. Nothing is required from you
except a clean, well-scoped PR.

---

*This is a living document, updated as Pulp's review automation gains capabilities. If
something here does not match what you actually experience on your PR, that is a doc bug —
say so on the PR.*
