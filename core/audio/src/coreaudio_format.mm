// CoreAudioFormat — macOS AAC/ALAC/CAF reader via ExtAudioFile API.
// Supports any format that CoreAudio can decode (AAC, ALAC, Apple Lossless, CAF).

#include <pulp/audio/format_registry.hpp>

#ifdef __APPLE__
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

namespace pulp::audio {

class CoreAudioReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
            static_cast<CFIndex>(path.size()), false);
        if (!url) return std::nullopt;

        ExtAudioFileRef file = nullptr;
        OSStatus status = ExtAudioFileOpenURL(url, &file);
        CFRelease(url);
        if (status != noErr || !file) return std::nullopt;

        // Get file format
        AudioStreamBasicDescription fileFormat{};
        UInt32 size = sizeof(fileFormat);
        ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat);

        // Get frame count
        SInt64 frameCount = 0;
        size = sizeof(frameCount);
        ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames, &size, &frameCount);

        ExtAudioFileDispose(file);

        AudioFileInfo info;
        info.sample_rate = static_cast<uint32_t>(fileFormat.mSampleRate);
        info.num_channels = fileFormat.mChannelsPerFrame;
        info.num_frames = static_cast<uint64_t>(frameCount);
        info.bits_per_sample = fileFormat.mBitsPerChannel;
        info.duration_seconds = static_cast<double>(frameCount) / fileFormat.mSampleRate;

        // Determine format name
        if (fileFormat.mFormatID == kAudioFormatMPEG4AAC)
            info.format = "AAC";
        else if (fileFormat.mFormatID == kAudioFormatAppleLossless)
            info.format = "ALAC";
        else if (fileFormat.mFormatID == 'caff')
            info.format = "CAF";
        else
            info.format = "CoreAudio";

        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
            static_cast<CFIndex>(path.size()), false);
        if (!url) return std::nullopt;

        ExtAudioFileRef file = nullptr;
        OSStatus status = ExtAudioFileOpenURL(url, &file);
        CFRelease(url);
        if (status != noErr || !file) return std::nullopt;

        // Get source format
        AudioStreamBasicDescription srcFormat{};
        UInt32 size = sizeof(srcFormat);
        ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &srcFormat);

        // Get frame count
        SInt64 frameCount = 0;
        size = sizeof(frameCount);
        ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames, &size, &frameCount);

        // Set client format to float32 interleaved
        AudioStreamBasicDescription clientFormat{};
        clientFormat.mSampleRate = srcFormat.mSampleRate;
        clientFormat.mFormatID = kAudioFormatLinearPCM;
        clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        clientFormat.mBitsPerChannel = 32;
        clientFormat.mChannelsPerFrame = srcFormat.mChannelsPerFrame;
        clientFormat.mBytesPerFrame = 4 * srcFormat.mChannelsPerFrame;
        clientFormat.mFramesPerPacket = 1;
        clientFormat.mBytesPerPacket = clientFormat.mBytesPerFrame;

        ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                                sizeof(clientFormat), &clientFormat);

        // Read all frames
        uint32_t channels = srcFormat.mChannelsPerFrame;
        auto totalFrames = static_cast<uint32_t>(frameCount);

        std::vector<float> interleaved(static_cast<size_t>(totalFrames) * channels);

        AudioBufferList bufferList;
        bufferList.mNumberBuffers = 1;
        bufferList.mBuffers[0].mNumberChannels = channels;
        bufferList.mBuffers[0].mDataByteSize = static_cast<UInt32>(interleaved.size() * sizeof(float));
        bufferList.mBuffers[0].mData = interleaved.data();

        UInt32 framesToRead = totalFrames;
        status = ExtAudioFileRead(file, &framesToRead, &bufferList);
        ExtAudioFileDispose(file);

        if (status != noErr) return std::nullopt;

        // Deinterleave
        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(srcFormat.mSampleRate);
        data.channels.resize(channels);
        for (auto& ch : data.channels)
            ch.resize(framesToRead);

        for (uint32_t f = 0; f < framesToRead; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                data.channels[c][f] = interleaved[static_cast<size_t>(f) * channels + c];

        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".m4a" || ext == ".aac" || ext == ".caf" || ext == ".alac";
    }

    std::string format_name() const override { return "CoreAudio"; }
};

// Auto-register on macOS
namespace {
    struct CoreAudioRegistrar {
        CoreAudioRegistrar() {
            FormatRegistry::instance().register_reader(std::make_unique<CoreAudioReader>());
        }
    };
    static CoreAudioRegistrar coreaudio_registrar;
}

}  // namespace pulp::audio

#endif  // __APPLE__
