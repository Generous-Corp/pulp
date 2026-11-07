// System codec backend for Apple platforms: ExtAudioFile from AudioToolbox
// encodes float input into AAC, ALAC or PCM in M4A, ADTS, CAF or AIFF.

#include "system_audio_codec_backend.hpp"

#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace pulp::audio::detail {

namespace {

AudioFileTypeID file_type(SystemAudioContainer container) {
    switch (container) {
    case SystemAudioContainer::M4A:
        return kAudioFileM4AType;
    case SystemAudioContainer::ADTS:
        return kAudioFileAAC_ADTSType;
    case SystemAudioContainer::CAF:
        return kAudioFileCAFType;
    case SystemAudioContainer::AIFF:
        return kAudioFileAIFFType;
    }
    return 0;
}

UInt32 alac_source_flag(std::uint32_t bit_depth) {
    switch (bit_depth) {
    case 20:
        return kAppleLosslessFormatFlag_20BitSourceData;
    case 24:
        return kAppleLosslessFormatFlag_24BitSourceData;
    case 32:
        return kAppleLosslessFormatFlag_32BitSourceData;
    default:
        return kAppleLosslessFormatFlag_16BitSourceData;
    }
}

// The stored format. Compressed formats leave the packet fields at zero for
// AudioFormat to complete; PCM fills every field.
bool file_format(const SystemAudioWriteOptions& options, Float64 rate, UInt32 channels,
                 AudioStreamBasicDescription& out) {
    out = {};
    out.mSampleRate = rate;
    out.mChannelsPerFrame = channels;
    switch (options.encoding) {
    case SystemAudioEncoding::AAC:
        out.mFormatID = kAudioFormatMPEG4AAC;
        out.mFormatFlags = kMPEG4Object_AAC_LC;
        break;
    case SystemAudioEncoding::ALAC:
        out.mFormatID = kAudioFormatAppleLossless;
        out.mFormatFlags = alac_source_flag(options.bit_depth);
        break;
    case SystemAudioEncoding::LinearPCM: {
        const UInt32 bits = options.float_pcm ? 32u : options.bit_depth;
        out.mFormatID = kAudioFormatLinearPCM;
        out.mFormatFlags =
            kAudioFormatFlagIsPacked |
            (options.float_pcm ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger);
        if (options.container == SystemAudioContainer::AIFF)
            out.mFormatFlags |= kAudioFormatFlagIsBigEndian;
        out.mBitsPerChannel = bits;
        out.mBytesPerFrame = bits / 8u * channels;
        out.mFramesPerPacket = 1;
        out.mBytesPerPacket = out.mBytesPerFrame;
        return true;
    }
    }
    UInt32 size = sizeof(out);
    return AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &out) ==
           noErr;
}

AudioStreamBasicDescription float_client_format(Float64 rate, UInt32 channels) {
    AudioStreamBasicDescription client{};
    client.mSampleRate = rate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mBitsPerChannel = 32;
    client.mChannelsPerFrame = channels;
    client.mBytesPerFrame = 4 * channels;
    client.mFramesPerPacket = 1;
    client.mBytesPerPacket = client.mBytesPerFrame;
    return client;
}

bool apply_bitrate(ExtAudioFileRef file, std::uint32_t bitrate) {
    AudioConverterRef converter = nullptr;
    UInt32 size = sizeof(converter);
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_AudioConverter, &size, &converter) !=
            noErr ||
        !converter)
        return false;
    UInt32 value = bitrate;
    if (AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(value), &value) !=
        noErr)
        return false;
    // A null config tells ExtAudioFile to pick up the converter's new state.
    CFArrayRef config = nullptr;
    return ExtAudioFileSetProperty(file, kExtAudioFileProperty_ConverterConfig, sizeof(config),
                                   &config) == noErr;
}

} // namespace

bool system_backend_can_write(SystemAudioContainer container, SystemAudioEncoding encoding) {
    switch (container) {
    case SystemAudioContainer::M4A:
        return encoding == SystemAudioEncoding::AAC || encoding == SystemAudioEncoding::ALAC;
    case SystemAudioContainer::ADTS:
        return encoding == SystemAudioEncoding::AAC;
    case SystemAudioContainer::CAF:
        return true;
    case SystemAudioContainer::AIFF:
        return encoding == SystemAudioEncoding::LinearPCM;
    }
    return false;
}

bool system_backend_write(const std::string& path, const AudioFileData& data,
                          const SystemAudioWriteOptions& options) {
    const auto channels = static_cast<UInt32>(data.num_channels());
    const auto rate = static_cast<Float64>(data.sample_rate);

    AudioStreamBasicDescription stored{};
    if (!file_format(options, rate, channels, stored))
        return false;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()),
        false);
    if (!url)
        return false;
    ExtAudioFileRef file = nullptr;
    const OSStatus created = ExtAudioFileCreateWithURL(url, file_type(options.container), &stored,
                                                       nullptr, kAudioFileFlags_EraseFile, &file);
    CFRelease(url);
    if (created != noErr || !file)
        return false;

    bool ok = true;
    const auto client = float_client_format(rate, channels);
    if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(client),
                                &client) != noErr)
        ok = false;
    if (ok && options.encoding == SystemAudioEncoding::AAC && options.aac_bitrate != 0)
        ok = apply_bitrate(file, options.aac_bitrate);

    constexpr std::size_t kChunkFrames = 4096;
    std::vector<float> interleaved(kChunkFrames * channels);
    const std::size_t total = static_cast<std::size_t>(data.num_frames());
    for (std::size_t start = 0; ok && start < total; start += kChunkFrames) {
        const std::size_t count = std::min(kChunkFrames, total - start);
        for (std::size_t f = 0; f < count; ++f)
            for (UInt32 c = 0; c < channels; ++c)
                interleaved[f * channels + c] = data.channels[c][start + f];
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0].mNumberChannels = channels;
        buffers.mBuffers[0].mDataByteSize = static_cast<UInt32>(count * channels * sizeof(float));
        buffers.mBuffers[0].mData = interleaved.data();
        ok = ExtAudioFileWrite(file, static_cast<UInt32>(count), &buffers) == noErr;
    }

    // Disposing flushes the encoder's final packets and writes the header.
    const bool closed = ExtAudioFileDispose(file) == noErr;
    if (!(ok && closed)) {
        std::remove(path.c_str());
        return false;
    }
    return true;
}

} // namespace pulp::audio::detail
