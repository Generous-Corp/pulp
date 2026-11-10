// libFuzzer entry point over the audio container readers that ship their own
// decoder: WAV (dr_wav behind Pulp's chunk walk), AIFF (in-house), FLAC
// (dr_flac), MP3 (dr_mp3) and Ogg Vorbis (stb_vorbis). Each input reaches
// exactly one reader.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored
//
// Routing: the container magic is sniffed first, so a plain seed file (and
// every mutation that keeps its header) goes to the reader that owns it.
// When no magic matches, the first byte selects a format and the rest is the
// payload, which lets the fuzzer reach a reader with a header it invented.
// The platform CoreAudio reader is never selected: it is system
// code, and the deterministic Catch2 corpus replay covers it on Apple.
//
// Readers take paths, so the input is written to one scratch file per call.
// The oracle shared with the Catch2 lane decides what a finding is; here a
// finding traps so libFuzzer records the input.

#include "../support/unique_temp_dir.hpp"
#include "audio_decoder_fuzz_oracle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

const pulp::test::ScopedTempDir& scratch_dir() {
    static const pulp::test::ScopedTempDir dir("pulp-audio-decoder-fuzz");
    return dir;
}

bool starts_with(const uint8_t* data, size_t size, std::string_view magic, size_t offset = 0) {
    return size >= offset + magic.size() &&
           std::memcmp(data + offset, magic.data(), magic.size()) == 0;
}

// Container magic, checked in an order where no two prefixes overlap.
std::string_view sniff_extension(const uint8_t* data, size_t size) {
    if (starts_with(data, size, "RIFF") && starts_with(data, size, "WAVE", 8))
        return ".wav";
    if (starts_with(data, size, "RF64") && starts_with(data, size, "WAVE", 8))
        return ".wav";
    if (starts_with(data, size, "FORM") && starts_with(data, size, "AIFF", 8))
        return ".aiff";
    if (starts_with(data, size, "FORM") && starts_with(data, size, "AIFC", 8))
        return ".aifc";
    if (starts_with(data, size, "fLaC"))
        return ".flac";
    if (starts_with(data, size, "OggS"))
        return ".ogg";
    if (starts_with(data, size, "ID3"))
        return ".mp3";
    // MPEG audio frame sync: eleven set bits.
    if (size >= 2 && data[0] == 0xFFu && (data[1] & 0xE0u) == 0xE0u)
        return ".mp3";
    return {};
}

bool write_input(const fs::path& path, const uint8_t* data, size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good())
        return false;
    if (size > 0)
        output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return output.good();
}

// abort() rather than __builtin_trap(): libFuzzer handles SIGABRT and writes
// the crash-* reproducer, while the trap is SIGTRAP on arm64 and ends the
// process with no artifact. Static destructors do not run either way, so the
// scratch directory is removed here first.
void trap_on_failure(const pulp_fuzz::audio_decoder::OracleFailure& failure) {
    std::fprintf(stderr, "audio decoder oracle: %s: %s: %s\n", failure.path.c_str(),
                 failure.field.c_str(), failure.detail.c_str());
    std::fflush(stderr);
    std::error_code ec;
    fs::remove_all(scratch_dir().path, ec);
    std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0)
        return 0;

    std::string_view extension = sniff_extension(data, size);
    const uint8_t* payload = data;
    size_t payload_size = size;
    if (extension.empty()) {
        extension = pulp_fuzz::audio_decoder::kFuzzableFormats
                        [data[0] % pulp_fuzz::audio_decoder::kFuzzableFormats.size()]
                            .extensions[0];
        payload = data + 1;
        payload_size = size - 1;
    }

    const auto path = scratch_dir().path / ("input" + std::string(extension));
    if (!write_input(path, payload, payload_size))
        return 0;

    (void)pulp_fuzz::audio_decoder::check_registry(
        path.string(), pulp_fuzz::audio_decoder::fuzz_limits(), trap_on_failure);
    std::error_code ec;
    fs::remove(path, ec);
    return 0;
}
