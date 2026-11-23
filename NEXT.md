# Interaction bridge lane evidence

The required first test commit is `c964172ce7` (`test: cover Vellum interaction adapter boundary`).
On `origin/main`, `integrations/pulp-vellum/include/pulp/vellum/interaction_adapter.hpp`
does not exist (`git cat-file -e origin/main:...` returned 128), so the new test's
include fails before the adapter fix. A local syntax probe also returned compiler
status 1 while the Vellum SDK include path was unavailable in the bare probe.
The normal `~/bin/leased-build --wait-secs 3600 -- pulp build` attempt reached the
shared CHOC cache lock and was interrupted after no progress. Re-run the targeted
Vellum editor test once the shared build lease/cache is available.
