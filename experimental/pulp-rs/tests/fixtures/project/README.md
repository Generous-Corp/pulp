# `tests/fixtures/project/` — Phase 6b project-bump fixtures

Each subdirectory contains one `CMakeLists.txt` shape exercising a
different pin kind. The `project_parity_test` integration test
copies the fixture tree into a tempdir before invoking `pulp-rs
project bump`, so the originals remain immutable.

| Fixture                | Pin kind                  | Expected bump behaviour              |
|------------------------|---------------------------|--------------------------------------|
| `fetch_content_pin`    | `FetchContentGitTag`      | Rewrites `v0.30.0` → `v<target>`     |
| `pulp_add_project_pin` | `PulpAddProject`          | Rewrites `0.30.0` → `<target>`       |
| `project_version_pin`  | `ProjectVersion`          | Rewrites `0.30.0` → `<target>`       |
| `dynamic_branch_pin`   | `FetchContentGitTag(main)`| Skipped as "dynamic pin — leave alone" |
| `no_pin`               | `Unknown`                 | Skipped as "no recognizable Pulp pin"  |
