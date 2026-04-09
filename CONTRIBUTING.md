# Contributing to Pulp

We welcome contributions. Here's how to get started.

## Contributor expectations

Pulp is currently a **single-maintainer project**. Contributors should expect:

- **Every PR runs CI on macOS, Linux, and Windows.** Your PR cannot merge until all three platform builds + tests are green. CI runs automatically on every PR via GitHub Actions; the maintainer also runs local CI on dedicated VMs.
- **`main` is protected.** No force pushes, no deletions, no direct commits — every change goes through a PR.
- **Reviews are not required (yet).** Solo project — the maintainer cannot review their own PRs, and there are no other reviewers. As Pulp grows, this expectation will change. Watch the README for updates.
- **Release tags are immutable.** Once a `v*` tag is published, it cannot be force-pushed, deleted, or updated. This guarantees the cryptographic identity of every published artifact.
- **The maintainer signs releases.** macOS plugin bundles use Developer ID code signing; release artifacts will gain Sigstore attestations in a future release for cryptographic provenance.

Pulp's CI controller is migrating from `tools/local-ci/local_ci.py` to [Shipyard](https://github.com/danielraffel/Shipyard) — the reusable CI tool extracted from `local_ci.py`. Both tools accept the same workflows during the transition.

If you're interested in the rationale behind these rules, see [Astral's open-source security post](https://astral.sh/blog/open-source-security-at-astral) — Pulp adopts several of the practices documented there.

## How to submit a PR

1. Fork the repo (or create a feature branch if you have write access).
2. Make your changes on a branch named `feature/short-description` or `fix/short-description`.
3. **Pre-PR validation** (optional but recommended): `shipyard run` (preferred, when shipyard is on PATH) or `python3 tools/local-ci/local_ci.py run` (fallback). This validates on macOS + Linux + Windows without creating a PR.
4. Open the PR with `git push` + `gh pr create`, or use `shipyard ship` / `python3 tools/local-ci/local_ci.py ship` to push, open the PR, validate, and merge on green automatically.
5. CI will run automatically on macOS, Linux, and Windows.
6. The maintainer will merge the PR after CI passes. If anything is unclear, leave a comment.

## Developer Certificate of Origin

All contributions must be signed off under the [Developer Certificate of Origin](https://developercertificate.org/) (DCO). This certifies that you have the right to submit the code under the project's MIT license.

Add `Signed-off-by: Your Name <your@email.com>` to your commit messages, or use `git commit -s`.

## Code Standards

- **C++20** — use modern features where they improve clarity
- **Original naming** — all names must be original to Pulp
- **Platform isolation** — platform-specific code goes in `platform/` subdirectories
- **Tests required** — every feature needs tests
- **Clean commits** — imperative mood, explain why not just what

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

## Dependencies

Before adding any dependency:
1. Check its license (MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0 only)
2. Add it to `DEPENDENCIES.md` (alphabetical order)
3. Add its license text to `NOTICE.md` (alphabetical order)
