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
  is not an acceptable standing credential;
- Pulp requires code-owner review with stale-review dismissal for the control
  and candidate paths in `.github/CODEOWNERS`;
- Pulp requires both `Vellum freeze` and the head commit status
  `Vellum trusted freeze` without removing its existing required checks;
- the filtered Vellum history seed has a pending immutable authority commit
  containing the exact counterpart record verified by the trusted gate;
- the selected Vellum commit has zero unclassified/forbidden dependency debt
  and its exact transferred paths match Pulp's generated ownership projection;
- the activation protocol has a reviewed generator mode that derives an active
  projection without accepting arbitrary ownership metadata (the prepared-only
  generator intentionally makes activation impossible today);
- no `unresolved` manifest row is transferred; every transferred row is
  positively classified and passes the audio/plugin neutrality scan;
- the trusted gate revalidates the exact merge result with base-branch controls
  and private counterpart access, or activation is kept out of multi-PR merge
  groups until that property is proven;
- Pulp branch protection requires strict/up-to-date heads (currently reported
  `strict: false`) or an equivalent trusted exact-merge-result gate, refreshes
  the trusted status whenever the PR base advances, and binds the required
  context to the expected GitHub App/status creator;
- later slice transfers and any ownership reversal have an explicit,
  counterpart-verified protocol; active metadata and untransferred slice
  definitions cannot mutate silently;
- an initial observatory cursor and reconciliation report exist in Vellum.

Activation is a two-phase handshake: Vellum first publishes the immutable
pending record; Pulp then merges one authority-transition outbox event and the
matching active projection; Vellum finally records the landed Pulp SHA and
advances its cursor. A failed or missing check leaves the projection prepared.
