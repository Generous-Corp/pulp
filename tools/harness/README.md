# Pulp catalog harness

The catalog harness converts `compat.json` (the hand-edited 490-entry compatibility
catalog) into an automated, machine-derived coverage measurement.

> Strategic context: see `planning/approach-bounded-translator.md` and
> `planning/pulp-agent-prompt-harness-week1.md`.

## Layout

```
tools/harness/
├── README.md                       (this file)
├── verifier.py                     core verifier — walks compat.json, dispatches per surface
├── status.py                       status enum + scoring helpers
├── adapters/
│   ├── __init__.py
│   ├── base.py                     CatalogEntry / Result dataclasses + AdapterBase
│   └── yoga.py                     yoga/* surface adapter (Week 1)
└── oracles/
    └── yoga/
        ├── README.md               oracle definition + regeneration recipe
        └── yoga-supported.json     reference list of Yoga properties + supported values
```

## Surfaces

| Surface | Adapter           | Oracle                                       | Week |
| ------- | ----------------- | -------------------------------------------- | ---- |
| yoga    | `adapters/yoga.py`| `oracles/yoga/yoga-supported.json` (static)  | 1    |
| css     | _planned_         | Chromium headless snapshots                  | 2-3  |
| rn      | _planned_         | derived from CSS oracle + RN snapshot tests  | 3-4  |
| canvas2d| _planned_         | Chromium headless capture                    | 4-5  |
| html    | _planned_         | jsdom reference                              | 5-6  |

## Status taxonomy

| Status     | Meaning                                                                              |
| ---------- | ------------------------------------------------------------------------------------ |
| PASS       | Implemented, matches reference behavior on all enumerated supported values.          |
| DIVERGE    | Implemented but partial — some supported values match, others diverge or are missing.|
| NO-OP      | Accepted silently (sometimes intentional — e.g. shadow* pre-blur).                   |
| NOT-IMPL   | Falls through silently / has no implementation surface in pulp.                      |
| OOS        | Explicitly out of scope (catalog `status: wontfix`).                                 |

## CLI

```
pulp harness coverage --surface=yoga          # Run one surface
pulp harness coverage --all                   # Run every wired adapter
pulp harness coverage --diff main..HEAD       # Reserved for week 4
```

Outputs:

* `build/harness-coverage-<sha>.json`         machine-readable coverage report
* `build/harness-coverage.md`                 human-readable summary table
* `docs/reports/harness-coverage.md`          mirror committed for nightly trend tracking

## Drift list

The drift list is the diff between catalog `status` and the harness verdict. Today
the catalog is hand-edited; tomorrow the harness is the source of truth. During
Week 1 we report drift and file a follow-up issue but do **not** rewrite
`compat.json`. The optional `--update-compat` flag is reserved for Week 2+.

## Authoritative parents

* Umbrella: pulp #1387
* Children: #1391 (verifier core), #1392 (per-surface adapters), #1393 (oracles), #1394 (CLI + CI)
