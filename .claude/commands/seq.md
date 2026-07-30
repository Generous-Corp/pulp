---
name: seq
description: Inspect, validate, edit, explain, import, or consent-gated export of a Pulp timeline project
---

Work with a Pulp timeline project through the typed `pulp seq` surface. Read
`.agents/skills/timeline/SKILL.md` before changing a project; canonical project
JSON is immutable document state and must not be edited by hand.

Choose the operation from the user's request:

```bash
pulp seq schema
pulp seq validate <project.json>
pulp seq explain <project.json> [--sample-rate <hz>]
pulp seq apply <project.json> <commands.json> [--out <project.json>]
pulp seq export <project.json> --format <smf|dawproject> --plan
pulp seq export <project.json> --format <smf|dawproject> --out <new-directory> \
  [--accept-loss <concept-id>]...
pulp seq import <file.mid|unpacked/project.xml> --format <smf|dawproject> \
  --out <new-directory>
```

If `$ARGUMENTS` names an operation or paths, use it to form the invocation. If
the requested mutation is not already represented by a typed command envelope,
inspect `pulp seq schema` and prepare the command JSON before applying it.

For export, always run `--plan` first and show the reported loss manifest to the
user. Continue only with the exact repeated `--accept-loss <concept-id>` values
the user accepts. There is no wildcard, force, or accept-all path, and accepting
a loss never removes it from the successful result's manifest.

Treat refusal as useful output: report `required_consent` exactly so it can be
reviewed and supplied on a later call. Do not invent concept IDs. Import and
export destinations must be new paths; do not delete or replace an existing
destination to make the command succeed. DAWproject currently uses an unpacked
`project.xml` directory rather than packing or unpacking a `.dawproject` ZIP.

After `apply` or `import`, validate the emitted project. Use `explain` when the
user needs the lowered playback plan, and render separately only when an audio
artifact is actually required.
