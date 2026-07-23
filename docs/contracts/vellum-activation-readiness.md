# Vellum authority activation readiness

The files in this change establish a **prepared** control plane. They do not
transfer source authority, push the filtered source seed, or make Pulp consume
Vellum. `.github/vellum-ownership.json` remains `prepared`, and every candidate
slice remains `pulp-authoritative-untransferred`.

Authority activation is blocked until all of the following are true:

- the older `danielraffel/burl` experiment is recorded at an immutable commit
  as `superseded-frozen`, mechanically disjoint, or the chosen extraction seed;
- the private Vellum repository can enforce required checks (the current
  Generous-Corp GitHub Free plan cannot enforce rulesets on private repos);
- dedicated, least-privilege GitHub App credentials back
  `VELLUM_READER_TOKEN` and `VELLUM_OBSERVATORY_TOKEN`; a personal broad token
  is not an acceptable standing credential, and the dedicated reader App ID
  must replace the deliberately null fail-closed pin in the trusted validator;
- Pulp requires code-owner review with stale-review dismissal for the control
  and candidate paths in `.github/CODEOWNERS`;
- Pulp requires both `Vellum freeze` and the head commit status
  `Vellum trusted freeze` without removing its existing required checks;
- the filtered Vellum history seed has a pending immutable authority commit
  containing the exact counterpart record verified by the trusted gate;
- that exact Vellum authority-record commit has strict protected-ref checks
  named `forbidden-deps`, `provenance-verify`, and `sterile-consumer`, each
  produced by its pinned GitHub App; the current workflows do not yet expose
  all three checks on an authority ref;
- the selected Vellum commit has zero unclassified/forbidden dependency debt
  and its exact transferred paths match Pulp's generated ownership projection;
- the activation generator and schema-v2 freeze controls receive review
  alongside an exact Vellum schema-v2 record; the generator derives all active
  and per-slice authority metadata from one append-only transition event rather
  than accepting independent values;
- no `unresolved` manifest row is transferred; every transferred row is
  positively classified and passes the audio/plugin neutrality scan;
- the trusted gate revalidates the exact merge result with base-branch controls
  and private counterpart access, or activation is kept out of multi-PR merge
  groups until that property is proven;
- Pulp branch protection requires strict/up-to-date heads (currently reported
  `strict: false`) or an equivalent trusted exact-merge-result gate, refreshes
  the trusted status whenever the PR base advances, and binds the required
  context to the expected GitHub App/status creator;
- any later slice transfer or ownership reversal gets a separately reviewed,
  counterpart-verified protocol; schema v2 intentionally rejects later slice
  transfers instead of pretending the initial-event metadata can describe
  them;
- an initial observatory cursor and reconciliation report exist in Vellum.

Activation is a two-phase handshake: Vellum first publishes the immutable
pending record; Pulp then merges one authority-transition outbox event and the
matching active projection; Vellum finally records the landed Pulp SHA and
advances its cursor. A failed or missing check leaves the projection prepared.

The trusted merge-group workflow has a one-time bootstrap constraint: GitHub
loads a `merge_group` workflow from the current base branch, not from the
synthetic merge result. The first commit that introduces
`tools/scripts/github_app_jwt.py` therefore cannot repair the already-loaded
base workflow from inside that workflow. For that bootstrap only, the
non-required `Vellum trusted gate` workflow is disabled while the exact
reviewed head passes the repository's normal required PR and merge-queue
checks, then re-enabled immediately after landing. No authority transfer is
allowed in that bootstrap commit. The next prepared-ownership merge group must
run the re-enabled workflow from the new base and pass `Vellum trusted freeze`;
that successful strict merge-group run is the retained proof that activation
work may continue.
