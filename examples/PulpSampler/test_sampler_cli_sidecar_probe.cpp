#include "pulp_sampler.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <system_error>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fputs("usage: pulp-sampler-cli-sidecar-probe SOURCE EXPECTED_LEVELS\n",
                   stderr);
        return 2;
    }

    std::uint32_t expected_levels = 0;
    const std::string_view levels_text(argv[2]);
    const auto parsed = std::from_chars(
        levels_text.data(), levels_text.data() + levels_text.size(), expected_levels);
    if (parsed.ec != std::errc{} || parsed.ptr != levels_text.data() + levels_text.size() ||
        expected_levels == 0) {
        std::fputs("invalid expected level count\n", stderr);
        return 2;
    }

    pulp::state::StateStore state;
    pulp::examples::PulpSamplerProcessor processor;
    processor.set_state_store(&state);
    processor.define_parameters(state);

    pulp::format::PrepareContext context;
    context.sample_rate = 48000.0;
    context.max_buffer_size = 512;
    context.input_channels = 0;
    context.output_channels = 2;
    processor.prepare(context);

    const auto result = processor.load_sample_file_result(argv[1]);
    if (result.status != pulp::examples::PulpSamplerLoadStatus::Ok ||
        result.sidecar_status != pulp::examples::PulpSamplerSidecarStatus::Loaded ||
        result.sidecar_level_count != expected_levels) {
        std::fprintf(stderr,
                     "PulpSampler rejected CLI sidecar: status=%u sidecar=%u levels=%u "
                     "expected=%u\n",
                     static_cast<unsigned>(result.status),
                     static_cast<unsigned>(result.sidecar_status),
                     result.sidecar_level_count, expected_levels);
        return 1;
    }

    std::printf("PulpSampler loaded CLI sidecar levels=%u\n", expected_levels);
    return 0;
}
