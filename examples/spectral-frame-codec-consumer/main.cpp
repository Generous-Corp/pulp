#include <pulp/signal/spectral_frame_codec.hpp>

#include <algorithm>
#include <iostream>
#include <string_view>
#include <charconv>
#include <cstdint>
#include <system_error>

namespace {
constexpr std::string_view kP0ProtocolAttestation = "pulp.signal.spectral-frame-codec-consumer.v1";

std::uint32_t requested_partition(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--partition") {
            std::uint32_t value = 0;
            const auto text = std::string_view(argv[i + 1]);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
                (value == 64 || value == 128))
                return value;
            return 0;
        }
    }
    return 0;
}
}

int main(int argc, char** argv) {
    using namespace pulp::signal;
    const auto partition = requested_partition(argc, argv);
    if (argc > 1 && partition == 0 && std::string_view(argv[1]) == "--partition")
        return 5;
    auto stream = make_spectral_stream({256, 64, 2, 48000}, 2);
    if (!stream)
        return 1;
    stream.value->components[2] = 0.5f;
    stream.value->components[3] = -0.25f;
    stream.value->components[516 + 258 + 8] = -1.25f;
    auto encoded = encode_spectral_stream(*stream.value);
    if (!encoded || encoded.value->size != 4224)
        return 2;
    auto decoded = decode_spectral_stream(encoded.value->view());
    if (!decoded || decoded.value->frame_count != 2 || decoded.value->frames[0].sequence != 0 ||
        decoded.value->frames[0].origin != -192 || decoded.value->frames[1].sequence != 1 ||
        decoded.value->frames[1].origin != -128 || decoded.value->components[2] != 0.5f ||
        decoded.value->components[3] != -0.25f || decoded.value->components[782] != -1.25f)
        return 3;
    auto again = encode_spectral_stream(*decoded.value);
    if (!again || !std::ranges::equal(encoded.value->view(), again.value->view()))
        return 4;
    if (argc > 1 && std::string_view(argv[1]) == "--planted-unknown-profile") {
        // The CTest entry is WILL_FAIL: a correct decoder rejects this
        // production-byte mutation, so the deliberately inverted exit status
        // makes a guard regression fail the package-level control.
        encoded.value->bytes[12] = 0x02;
        return decode_spectral_stream(encoded.value->view()) ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--planted-frame-chronology") {
        // A consumer must reject metadata that no longer names the canonical
        // analyzed-frame sequence.
        encoded.value->bytes[64] = 0x01;
        return decode_spectral_stream(encoded.value->view()).error ==
                       SpectralCodecError::InvalidChronology
                   ? 1
                   : 0;
    }
    std::cout << "owned spectral codec: analyzed frames=2 origins=[-192,-128] "
                 "exact roundtrip and strict rejection passed\n";
    // Bind the evidence receipt to this named installed consumer. This is
    // fixture output metadata and does not change the public/runtime contract.
    std::cout << kP0ProtocolAttestation;
    if (partition != 0)
        std::cout << " partition_frames=" << partition;
    std::cout << '\n';
}
