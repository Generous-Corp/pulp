# Pulp catalog harness

The catalog harness converts `compat.json` into an automated,
machine-derived coverage measurement.

## Layout

```
tools/harness/
├── README.md                       (this file)
├── verifier.py                     core verifier — walks compat.json, dispatches per surface
├── status.py                       status enum + scoring helpers
├── adapters/
│   ├── __init__.py
│   ├── base.py                     CatalogEntry / Result dataclasses + AdapterBase
│   ├── canvas2d.py                 canvas2d/* surface adapter
│   ├── css.py                      css/* surface adapter
│   ├── html.py                     html/* surface adapter
│   ├── rn.py                       rn/* surface adapter
│   └── yoga.py                     yoga/* surface adapter
└── oracles/
    ├── canvas2d/
    │   ├── README.md               oracle definition + regeneration recipe
    │   └── canvas2d-supported.json reference table of HTML5 Canvas2D + bridge mapping
    ├── css/
    │   ├── README.md               oracle definition + regeneration recipe
    │   └── css-supported.json      CSS enum/property reference table
    ├── html/
    │   ├── README.md               oracle definition + regeneration recipe
    │   └── html-supported.json     DOM-lite / HTML reference table
    ├── rn/
    │   ├── README.md               oracle definition + regeneration recipe
    │   └── rn-viewstyle.json       React Native ViewStyle reference table
    ├── yoga/
    │   ├── README.md               oracle definition + regeneration recipe
    │   └── yoga-supported.json     reference list of Yoga properties + supported values
```

## Surfaces

| Surface  | Adapter                 | Oracle                                                |
| -------- | ----------------------- | ----------------------------------------------------- |
| canvas2d | `adapters/canvas2d.py`  | `oracles/canvas2d/canvas2d-supported.json` (static)   |
| css      | `adapters/css.py`       | `oracles/css/css-supported.json` (static)             |
| html     | `adapters/html.py`      | `oracles/html/html-supported.json` (static)           |
| rn       | `adapters/rn.py`        | `oracles/rn/rn-viewstyle.json` (static)               |
| yoga     | `adapters/yoga.py`      | `oracles/yoga/yoga-supported.json` (static)           |

## Status taxonomy

| Harness status | Meaning                                                                              | Catalog `status` mapping |
| -------------- | ------------------------------------------------------------------------------------ | ------------------------ |
| PASS           | Implemented, matches reference behavior on all enumerated supported values.          | `supported`              |
| DIVERGE        | Implemented but partial — some supported values match, others diverge or are missing.| `partial`                |
| NO-OP          | Accepted silently (intentional stub — bridge registration exists, body is empty).    | `noop`                   |
| NOT-IMPL       | Falls through silently / has no implementation surface in pulp.                      | `missing`                |
| OOS            | Explicitly out of scope.                                                             | `wontfix`                |

The `noop` catalog status was added in pulp #1475 to close the
vocabulary gap discovered while triaging css/animation* and
css/touchAction during #1474. Without it, every intentional NO-OP
registers as drift regardless of what the catalog claims.

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

The drift list is the diff between catalog `status` and the harness verdict.
The catalog is still hand-edited, so the harness reports drift without
rewriting `compat.json` by default. A future `--update-compat` mode is reserved
for explicit catalog-maintenance passes.
