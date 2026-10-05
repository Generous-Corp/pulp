# Spectr / Forge / Magenta metadata audit

Audited against `origin/main` at
`0fd5c3830fc54d5a19b8aa0bdc577b7308ef67fd` in the isolated metadata-audit
worktree.

The paced-generation receipt remains blocked and metadata-only. Its prior
parent stamp (`8b10d5ada01466d39de2d519811e7c74bc5b0340`) was stale and is now
updated to the protected-head SHA. The Forge catalog still has zero
`spectr.gpu_nam` rows.

The named GPU-NAM checkout exists at
`/Users/danielraffel/Code/pulp-gpu-nam`, revision
`014f24325a7f4211ee5052993badd9c90151b414`; it is an external CPU-oracle/plugin
source and is not a Forge registration. The Magenta model store paths
`/Users/danielraffel/.pulp/magenta` and
`/Users/danielraffel/.pulp/magenta/models` are absent.

The current MLX execution receipt's 100,000-block run is explicitly synthetic,
has no product-model/transport/fallback evidence, and says **gate not claimed**.
The current neural status row keeps Spectr/Forge/Magenta at metadata-only
closure. No sustained Magenta generation or shipped product claim is made.

Promotion gates remain: a real named Magenta model artifact, fresh paced
producer receipt, public neural adapter, validated model loading/fallback and
generation fencing, and a named Forge consumer registration.
