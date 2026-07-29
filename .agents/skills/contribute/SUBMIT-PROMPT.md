# Ready-to-submit prompt

Paste the block below into Claude Code (or Codex) from your Pulp checkout when
you have work you think is done. It is deliberately one message: everything it
needs is either in the repo or something it will ask you for.

Nothing here needs Shipyard, Tart, a VM, or write access.

---

```
Use the `contribute` skill in this repo (.agents/skills/contribute/SKILL.md).

I have work I think is ready to hand off. Take it from "it works on my machine"
to "a maintainer can land this without asking me questions."

Do this in order, and tell me what you find at each step rather than fixing
everything silently:

1. Show me the diff against origin/main, grouped by concern. If it contains more
   than one unrelated change, propose a split into separate patches in dependency
   order, and say which are independent.

2. Check the routing: does anything here belong in Forge rather than Pulp, or
   vice versa? Flag it if so.

3. For every behaviour change, confirm a test covers it. For each such test,
   revert the fix, rebuild, and show me the test failing — then restore. If you
   cannot do that for one of them, say which and why. Do not skip this and do not
   assert it without running it.

4. Run: tools/scripts/contributor_check.sh <my test targets>
   Fix what it reports as FAIL. For anything it reports as SKIP, keep the exact
   text — it goes in the handoff.

5. Review the change the way a maintainer would before writing the handoff:
   anything over ~1000 lines in a file, duplicated logic that should be shared,
   a new core/ file with no test, naming that will not survive review. Tell me
   what you would push back on. Fix the clear-cut ones; ask me about the rest.

6. Write the handoff doc in the format the skill specifies. Be specific in
   "Verification" (name the suites and the results) and honest in "What I could
   not do" — that section is the most valuable one, so do not leave it empty or
   pad it.

7. Tell me exactly how to deliver it, based on what access I have.

Ask me anything you need instead of guessing. If something looks wrong with what
I built, say so before writing the handoff.
```

---

## Why it is shaped this way

- **"tell me what you find rather than fixing everything silently"** — the point
  is a change you can explain, not one an agent rewrote past you.
- **Step 3 is the one that gets skipped.** Asserting a test would fail is not the
  same as watching it fail, and the difference is where real bugs hide.
- **Step 5 is the review that keeps a contribution landable** — the difference
  between "works" and "a maintainer will merge it".
- **"What I could not do" is the most valuable section.** A contribution that
  fixes one thing and lists three unverified things is far more useful than one
  that implies everything was checked.
