// An application that links pulp::view-audio without the build-tree shim must
// register the audio displays itself, and one explicit call is enough.

namespace pulp::test {
int check_explicit_audio_registration();
}

int main() {
    return pulp::test::check_explicit_audio_registration();
}
