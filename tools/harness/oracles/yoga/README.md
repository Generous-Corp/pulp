# Yoga oracle

The yoga oracle is a static reference table sourced from Yoga upstream
(`facebook/yoga`, MIT) and `tools/import-design/catalogs/yoga.tsv`. We choose
"static reference" over "shell out to the live Yoga library" because:

1. Pulp already vendors `yogacore` via FetchContent; we know the pin (`v3.2.1`)
   and the property surface is stable across minor releases.
2. A static oracle keeps the harness portable — no extra build dependency at
   verifier-runtime.
3. Yoga's property surface is small (~50 properties, well-bounded enum values)
   and well-documented at <https://www.yogalayout.dev/docs/styling>.

## File: `yoga-supported.json`

Schema:

```json
{
  "version": "v3.2.1",
  "source": "tools/import-design/catalogs/yoga.tsv + Yoga upstream docs",
  "properties": {
    "<camelCase property name>": {
      "kind": "enum | number | length | length-or-percentage | length-or-percentage-or-auto | edge-set | string",
      "values": ["..."],          // for enum: the full Yoga-supported value list
      "default": "...",            // optional — Yoga default
      "notes": "..."               // optional
    }
  }
}
```

## Regeneration

The oracle is hand-maintained for now. To refresh:

1. Bump the Yoga pin in `tools/cmake/PulpDependencies.cmake` (both the
   `pulp_register_fetchcontent_source ... REF` and the `FetchContent_Declare`
   `GIT_TAG`).
2. Walk Yoga's `YGEnums.h` and `YGStyle.h` for new fields / values.
3. Mirror changes into `yoga-supported.json`, and restamp both `version` and the
   `facebook/yoga@<ref>` citation in `source`.
4. Run `python3 tools/harness/verifier.py --surface=yoga` and review the drift list.

Steps 1 and 3 have to move together. `tools/harness/adapters/yoga.py` reports
any property missing from this table as out-of-scope rather than as a gap, so a
table left behind a bumped pin hides real gaps behind an improving coverage
number. `python3 tools/scripts/check_yoga_oracle_pin.py` fails when the stamps
and the pin disagree; it runs in `gates.sh`, the pre-push hook, and the
`yoga-oracle-pin-lockstep` ctest. Restamping without re-reading `YGEnums.h`
satisfies the check and re-hides the gap — step 2 is the work.
