# Agent efficiency audits

Pulp keeps small, evidence-backed checks for recurring agent mistakes. Each
check has an owner, a regression test, an evidence record, and a review date.
The review asks whether the failure still occurs and whether the check catches
real defects without adding noise. Retire a rule when its evidence disappears;
do not preserve it merely because it is already installed.

The current shell portability checks cover two silent zsh hazards: unbraced
`$name:path` expansions, and Bash-only `${PIPESTATUS[...]}` in zsh sessions.
Run them with:

```sh
python3 tools/scripts/shell_portability_check.py tools/ci scripts tools/scripts
```

For a new recurring nuisance, record three independent root families, the
supported owner of the fix, a synthetic regression case, and a review date in
`tools/scripts/shell_portability_rules.json`. Keep the check narrow enough that
an agent can explain one actionable fix for every finding.

The recommended audit prompt is:

> Review a deduplicated, bounded sample of your recent development history.
> Record the owning system, evidence anchor, wasted calls/time, whether a
> supported route already existed, and the smallest prevention. Check for
> unnecessary full test runs, repeated configure/build work, SDK or CLI drift,
> merge-queue delays, permission loops, and false human blockers. Do not export
> transcript text or secrets. Recommend implementation only when the pattern
> appears in at least three independent root families and has a clear owner.
