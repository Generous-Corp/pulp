#include <pulp/signal/spectral_frame_codec.hpp>

#include <algorithm>
#include <iostream>

int main() {
    using namespace pulp::signal;
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
    if (!decoded || decoded.value->frames[1].origin != -128 ||
        decoded.value->components[2] != 0.5f || decoded.value->components[3] != -0.25f ||
        decoded.value->components[782] != -1.25f)
        return 3;
    auto again = encode_spectral_stream(*decoded.value);
    if (!again || !std::ranges::equal(encoded.value->view(), again.value->view()))
        return 4;
    encoded.value->bytes[87] = 0x80;
    if (decode_spectral_stream(encoded.value->view()).error != SpectralCodecError::NoncanonicalZero)
        return 5;
    std::cout << "owned spectral codec: exact roundtrip and strict rejection passed\n";
}
