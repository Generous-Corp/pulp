// Clean control for the release-artifact scanner: a program that carries none
// of the forbidden markers, so a scan over every gate must report zero hits.
// Its generated sibling (release_artifact_forbidden_fixture.cpp, written at
// configure time from tools/cmake/release_artifact_forbidden.json) carries all
// of them and must be flagged for each.
int main() {
    return 0;
}
