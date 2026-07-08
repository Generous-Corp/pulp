---
description: Freeze a graph into a signed .pulpbake artifact (or verify one)
---

Run the Pulp bake command with the user's arguments: `$ARGUMENTS`

`pulp bake <input.pulpgraph> -o <out.pulpbake> --sign-key <key>` freezes a graph
into a signed, distributable `.pulpbake` artifact; `pulp bake verify <artifact>
--trust <key>` checks the signature + bounded parse and prints ACCEPTED/REJECTED
without executing the plan. A non-bakeable graph (a hosted plugin node, a
MIDI/automation/sidechain lane, or a non-opted-in Custom type) is refused with the
specific reason and a non-zero exit. Report the command's output and exit status.
