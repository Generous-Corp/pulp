// Serialization + child-process launch for the MF-3 denormal null reference.
//
// Deliberately contains NO filter code, so it is safe to compile into both the
// snap-enabled test binary and the snap-disabled refgen binary. See
// denormal_null_reference.hpp for why the reference cannot simply be another
// TU inside the test executable.

#include "denormal_null_reference.hpp"

#include <cstdio>
#include <vector>

namespace denormal_null {
namespace {

// The 11 AllOutputs channels, in one fixed wire order shared by both sides.
template <typename Outputs, typename Fn>
void for_each_channel(Outputs& o, Fn&& fn) {
    fn(o.biquad);
    fn(o.svf);
    fn(o.ladder);
    fn(o.ballistics);
    fn(o.dc_blocker);
    fn(o.tpt);
    fn(o.reverb);
    fn(o.phaser);
    fn(o.noise_gate);
    fn(o.compressor);
    fn(o.limiter);
}

bool write_pod(std::FILE* f, const void* p, std::size_t n) {
    return std::fwrite(p, 1, n, f) == n;
}

bool read_pod(std::FILE* f, void* p, std::size_t n) {
    return std::fread(p, 1, n, f) == n;
}

}  // namespace

bool write_reference(const char* path, const AllOutputs& outputs,
                     const TailReport& tail, bool flushed) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;

    bool ok = write_pod(f, &kRefMagic, sizeof(kRefMagic));

    const std::uint8_t flags[6] = {
        static_cast<std::uint8_t>(flushed),
        static_cast<std::uint8_t>(tail.dc_blocker),
        static_cast<std::uint8_t>(tail.ballistics),
        static_cast<std::uint8_t>(tail.svf),
        static_cast<std::uint8_t>(tail.ladder),
        static_cast<std::uint8_t>(tail.reverb),
    };
    ok = ok && write_pod(f, flags, sizeof(flags));

    for_each_channel(outputs, [&](const std::vector<float>& v) {
        const auto n = static_cast<std::uint32_t>(v.size());
        ok = ok && write_pod(f, &n, sizeof(n));
        ok = ok && (n == 0u || write_pod(f, v.data(), n * sizeof(float)));
    });

    const bool closed = std::fclose(f) == 0;
    return ok && closed;
}

bool read_reference(const char* path, Reference& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    std::uint32_t magic = 0u;
    bool ok = read_pod(f, &magic, sizeof(magic)) && magic == kRefMagic;

    std::uint8_t flags[6] = {};
    ok = ok && read_pod(f, flags, sizeof(flags));
    if (ok) {
        out.denormals_flushed = flags[0] != 0u;
        out.tail.dc_blocker = flags[1] != 0u;
        out.tail.ballistics = flags[2] != 0u;
        out.tail.svf = flags[3] != 0u;
        out.tail.ladder = flags[4] != 0u;
        out.tail.reverb = flags[5] != 0u;
    }

    for_each_channel(out.outputs, [&](std::vector<float>& v) {
        std::uint32_t n = 0u;
        ok = ok && read_pod(f, &n, sizeof(n));
        if (!ok) return;
        v.resize(n);
        ok = n == 0u || read_pod(f, v.data(), n * sizeof(float));
    });

    std::fclose(f);
    return ok;
}

}  // namespace denormal_null
