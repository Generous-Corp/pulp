// The positive control: an ordinary consumer of the installed SDK.
//
// If this fails, the harness is broken and NO verdict from the minimal
// consumer beside it may be reported, because a staging, configure or link
// fault would make that consumer fail too and look exactly like a finding.
// So it exercises the same three hops the minimal consumer depends on
// (headers resolve, the export set links, the result runs) over a wider and
// deliberately source-bearing slice: pulp::state is a real archive, so
// reaching it proves the installed export set resolves compiled libraries and
// not just header include paths.

#include <pulp/audio/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cstdio>
#include <vector>

int main() {
    // audio: a non-owning view over per-channel pointers.
    std::vector<float> left(64, 0.5f);
    std::vector<float> right(64, -0.5f);
    float* channels[2] = {left.data(), right.data()};
    pulp::audio::BufferView<float> buffer(channels, 2, 64);
    if (buffer.num_channels() != 2 || buffer.num_samples() != 64) {
        std::fprintf(stderr, "buffer view shape disagreed\n");
        return 1;
    }
    if (buffer.channel(0)[0] != 0.5f) {
        std::fprintf(stderr, "buffer view content disagreed\n");
        return 2;
    }

    // state: the parameter store, a linked module rather than a header.
    pulp::state::StateStore store;
    pulp::state::ParamInfo info;
    info.id = 1;
    info.name = "Gain";
    info.range = pulp::state::ParamRange::linear(0.0f, 1.0f, 0.5f);
    store.add_parameter(info);

    if (store.get_value(1) != 0.5f) {
        std::fprintf(stderr, "parameter default disagreed\n");
        return 3;
    }
    store.set_value(1, 0.25f);
    if (store.get_value(1) != 0.25f) {
        std::fprintf(stderr, "parameter round-trip disagreed\n");
        return 4;
    }

    std::printf("full consumer ok\n");
    return 0;
}
