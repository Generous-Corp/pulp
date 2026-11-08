// System codec backend for Windows: Media Foundation's sink writer encodes
// AAC into MPEG-4 (.m4a), and its source reader decodes the compressed files
// Windows ships codecs for (.m4a, .aac, .wma) for FormatRegistry.

#include "decode_limits_detail.hpp"
#include "system_audio_codec_backend.hpp"

#include <pulp/runtime/log.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;
constexpr DWORD kFirstAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

// COM and Media Foundation for the duration of one call. COM already
// initialized in another apartment on this thread is fine for MF's
// free-threaded objects; it is left as it was.
class MediaFoundationScope {
  public:
    MediaFoundationScope() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_owned_ = SUCCEEDED(co);
        const bool com_usable = com_owned_ || co == RPC_E_CHANGED_MODE;
        mf_started_ = com_usable && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MediaFoundationScope() {
        if (mf_started_)
            MFShutdown();
        if (com_owned_)
            CoUninitialize();
    }
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;

    bool ok() const {
        return mf_started_;
    }

  private:
    bool com_owned_ = false;
    bool mf_started_ = false;
};

// Logs the failing stage and its HRESULT, so a false return can be told apart
// (an unsupported format from a full disk, for example).
bool succeeded(HRESULT hr, const char* stage) {
    if (SUCCEEDED(hr))
        return true;
    runtime::log_warn("System codec (Media Foundation): {} failed, HRESULT 0x{:08x}", stage,
                      static_cast<unsigned>(hr));
    return false;
}

std::wstring widen(const std::string& text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX))
        return {};
    const int length = static_cast<int>(text.size());
    const int wide =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (wide <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, out.data(), wide);
    return out;
}

// The AAC encoder accepts four average byte rates; pick the nearest.
UINT32 aac_bytes_per_second(std::uint32_t bitrate) {
    constexpr std::array<UINT32, 4> kRates = {12000, 16000, 20000, 24000};
    if (bitrate == 0)
        return 16000;
    const auto target = static_cast<long long>(bitrate / 8u);
    UINT32 best = kRates.front();
    for (auto rate : kRates)
        if (std::llabs(static_cast<long long>(rate) - target) <
            std::llabs(static_cast<long long>(best) - target))
            best = rate;
    return best;
}

std::uint64_t frames_for_duration(LONGLONG duration, UINT32 rate) {
    if (duration <= 0 || rate == 0)
        return 0;
    const auto whole = static_cast<std::uint64_t>(duration / kHundredNanosecondsPerSecond);
    const auto part = static_cast<std::uint64_t>(duration % kHundredNanosecondsPerSecond);
    return whole * rate + part * rate / static_cast<std::uint64_t>(kHundredNanosecondsPerSecond);
}

ComPtr<IMFSourceReader> open_source_reader(const std::string& path) {
    const auto wide = widen(path);
    if (wide.empty())
        return nullptr;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &reader)))
        return nullptr;
    return reader;
}

LONGLONG source_duration(IMFSourceReader* reader) {
    PROPVARIANT value;
    PropVariantInit(&value);
    LONGLONG duration = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &value)) &&
        value.vt == VT_UI8)
        duration = static_cast<LONGLONG>(value.uhVal.QuadPart);
    PropVariantClear(&value);
    return duration;
}

class MediaFoundationReader : public FormatReader {
  public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        MediaFoundationScope scope;
        if (!scope.ok())
            return std::nullopt;
        auto reader = open_source_reader(path);
        if (!reader)
            return std::nullopt;
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(kFirstAudioStream, 0, &native)))
            return std::nullopt;
        UINT32 rate = 0;
        UINT32 channels = 0;
        UINT32 bits = 0;
        native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (rate == 0 || channels == 0)
            return std::nullopt;
        const auto duration = source_duration(reader.Get());

        AudioFileInfo info;
        info.sample_rate = rate;
        info.num_channels = channels;
        info.bits_per_sample = bits;
        info.num_frames = frames_for_duration(duration, rate);
        info.duration_seconds =
            static_cast<double>(duration) / static_cast<double>(kHundredNanosecondsPerSecond);
        info.format = "Media Foundation";
        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        return read(path, DecodeLimits{});
    }

    std::optional<AudioFileData> read(const std::string& path,
                                      const DecodeLimits& limits) override {
        MediaFoundationScope scope;
        if (!scope.ok())
            return std::nullopt;
        auto reader = open_source_reader(path);
        if (!reader)
            return std::nullopt;

        // Ask the decoder for 32-bit float at the stream's own rate and layout.
        ComPtr<IMFMediaType> wanted;
        if (FAILED(MFCreateMediaType(&wanted)) ||
            FAILED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
            FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),
                                              FALSE)) ||
            FAILED(reader->SetStreamSelection(kFirstAudioStream, TRUE)) ||
            FAILED(reader->SetCurrentMediaType(kFirstAudioStream, nullptr, wanted.Get())))
            return std::nullopt;
        ComPtr<IMFMediaType> current;
        if (FAILED(reader->GetCurrentMediaType(kFirstAudioStream, &current)))
            return std::nullopt;
        UINT32 rate = 0;
        UINT32 channels = 0;
        current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        if (rate == 0 || channels == 0)
            return std::nullopt;

        // The container's duration is an untrusted size claim: refuse it
        // before reserving, then keep checking the decoded count.
        const auto claimed = frames_for_duration(source_duration(reader.Get()), rate);
        if (!detail::within_decode_limits("Media Foundation", claimed, channels, limits))
            return std::nullopt;

        AudioFileData data;
        data.sample_rate = rate;
        data.channels.resize(channels);
        detail::reserve_planar(data.channels, claimed);
        std::uint64_t decoded = 0;

        for (;;) {
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(kFirstAudioStream, 0, nullptr, &flags, nullptr, &sample)))
                return std::nullopt;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                return std::nullopt;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                break;
            if (!sample)
                continue;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
                return std::nullopt;
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&bytes, nullptr, &length)))
                return std::nullopt;
            const std::uint64_t frames = length / (sizeof(float) * channels);
            if (!detail::within_decode_limits("Media Foundation", decoded + frames, channels,
                                              limits)) {
                buffer->Unlock();
                return std::nullopt;
            }
            const auto* samples = reinterpret_cast<const float*>(bytes);
            for (UINT32 c = 0; c < channels; ++c) {
                auto& out = data.channels[c];
                const auto base = out.size();
                out.resize(base + static_cast<std::size_t>(frames));
                for (std::uint64_t f = 0; f < frames; ++f)
                    out[base + static_cast<std::size_t>(f)] =
                        samples[static_cast<std::size_t>(f) * channels + c];
            }
            buffer->Unlock();
            decoded += frames;
        }
        if (decoded == 0)
            return std::nullopt;
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".m4a" || ext == ".aac" || ext == ".wma";
    }

    std::string format_name() const override {
        return "Media Foundation";
    }

    FormatCapabilities capabilities() const override {
        FormatCapabilities caps;
        caps.format_name = format_name();
        caps.extensions = {".m4a", ".aac", ".wma"};
        caps.float_samples = true;
        caps.compressed = true;
        caps.lossless = false;
        return caps;
    }
};

} // namespace

std::unique_ptr<FormatReader> create_media_foundation_reader() {
    return std::make_unique<MediaFoundationReader>();
}

namespace detail {

bool system_backend_can_write(SystemAudioContainer container, SystemAudioEncoding encoding) {
    return container == SystemAudioContainer::M4A && encoding == SystemAudioEncoding::AAC;
}

std::vector<std::uint32_t> system_backend_aac_sample_rates() {
    return {44100, 48000};
}

bool system_backend_write(const std::string& path, const AudioFileData& data,
                          const SystemAudioWriteOptions& options) {
    const auto channels = static_cast<UINT32>(data.num_channels());
    const UINT32 rate = data.sample_rate;
    // The Windows AAC encoder accepts these rates. Its four average byte
    // rates below are documented for mono and stereo; 5.1 uses other values
    // and is not offered until it can be tested on a Windows host.
    if ((rate != 44100 && rate != 48000) || (channels != 1 && channels != 2))
        return false;

    MediaFoundationScope scope;
    if (!scope.ok())
        return false;
    const auto wide = widen(path);
    if (wide.empty())
        return false;

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1)) ||
        FAILED(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4)))
        return false;
    ComPtr<IMFSinkWriter> writer;
    if (!succeeded(MFCreateSinkWriterFromURL(wide.c_str(), nullptr, attributes.Get(), &writer),
                   "creating the sink writer"))
        return false;

    bool ok = true;
    ComPtr<IMFMediaType> stored;
    ok = SUCCEEDED(MFCreateMediaType(&stored)) &&
         SUCCEEDED(stored->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) &&
         SUCCEEDED(stored->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC)) &&
         SUCCEEDED(stored->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) &&
         SUCCEEDED(stored->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate)) &&
         SUCCEEDED(stored->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels)) &&
         SUCCEEDED(stored->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                     aac_bytes_per_second(options.aac_bitrate)));
    if (!ok)
        runtime::log_warn("System codec (Media Foundation): describing the AAC output failed");
    DWORD stream = 0;
    ok = ok && succeeded(writer->AddStream(stored.Get(), &stream), "adding the AAC stream");

    // The encoder takes 16-bit PCM input.
    const UINT32 block_align = 2u * channels;
    ComPtr<IMFMediaType> input;
    ok = ok && SUCCEEDED(MFCreateMediaType(&input)) &&
         SUCCEEDED(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) &&
         SUCCEEDED(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) &&
         SUCCEEDED(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) &&
         SUCCEEDED(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate)) &&
         SUCCEEDED(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels)) &&
         SUCCEEDED(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align)) &&
         SUCCEEDED(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_align * rate)) &&
         succeeded(writer->SetInputMediaType(stream, input.Get(), nullptr),
                   "setting the PCM input format") &&
         succeeded(writer->BeginWriting(), "starting to write");

    constexpr std::size_t kChunkFrames = 4096;
    const std::size_t total = static_cast<std::size_t>(data.num_frames());
    for (std::size_t start = 0; ok && start < total; start += kChunkFrames) {
        const std::size_t count = std::min(kChunkFrames, total - start);
        const auto bytes = static_cast<DWORD>(count * block_align);
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* destination = nullptr;
        if (FAILED(MFCreateMemoryBuffer(bytes, &buffer)) ||
            FAILED(buffer->Lock(&destination, nullptr, nullptr))) {
            ok = false;
            break;
        }
        auto* pcm = reinterpret_cast<std::int16_t*>(destination);
        for (std::size_t f = 0; f < count; ++f)
            for (UINT32 c = 0; c < channels; ++c) {
                const float clamped = std::clamp(data.channels[c][start + f], -1.0f, 1.0f);
                pcm[f * channels + c] = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
            }
        buffer->Unlock();
        ComPtr<IMFSample> sample;
        const auto time = static_cast<LONGLONG>(start) * kHundredNanosecondsPerSecond / rate;
        const auto duration = static_cast<LONGLONG>(count) * kHundredNanosecondsPerSecond / rate;
        ok = SUCCEEDED(buffer->SetCurrentLength(bytes)) && SUCCEEDED(MFCreateSample(&sample)) &&
             SUCCEEDED(sample->AddBuffer(buffer.Get())) && SUCCEEDED(sample->SetSampleTime(time)) &&
             SUCCEEDED(sample->SetSampleDuration(duration)) &&
             succeeded(writer->WriteSample(stream, sample.Get()), "encoding");
    }
    ok = ok && succeeded(writer->Finalize(), "finishing the file");
    writer.Reset();
    if (!ok)
        DeleteFileW(wide.c_str());
    return ok;
}

} // namespace detail

} // namespace pulp::audio
