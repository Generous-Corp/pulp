#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_heritage_bus_dsp.hpp>
#include <pulp/audio/sample_heritage_pitch.hpp>
#include <pulp/audio/sample_heritage_record_commit.hpp>
#include <pulp/audio/sample_heritage_voice_dsp.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kFrames = 16384;
constexpr double kPi = 3.14159265358979323846;

bool write_mono(const std::filesystem::path& path,
                const pulp::audio::Buffer<float>& buffer) {
    pulp::audio::AudioFileData file;
    file.sample_rate = kSampleRate;
    file.channels = {std::vector<float>(buffer.channel(0).begin(),
                                        buffer.channel(0).end())};
    return pulp::audio::write_wav_file(path.string(), file,
                                       pulp::audio::WavBitDepth::Float32);
}

pulp::audio::Buffer<float> tone(std::size_t frames, double amplitude,
                                double frequency) {
    pulp::audio::Buffer<float> result(1, frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        result.channel(0)[frame] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * frame / kSampleRate));
    return result;
}

bool render_c1(const std::filesystem::path& out) {
    auto loaded = tone(kFrames, 0.0001, 375.0);
    pulp::audio::SampleHeritageProfile profile;
    profile.profile_id = "neutral.c1-calibration";
    profile.host_sample_rate = kSampleRate;
    profile.record_commit.push_back({
        pulp::audio::SampleHeritageBlockDomain::RecordCommit, false,
        pulp::audio::SampleHeritageRecordInputDriveClipBlock{0.5f, 1.0f}});
    const auto recorded = pulp::audio::commit_sample_heritage_recording(
        profile, static_cast<const pulp::audio::Buffer<float>&>(loaded).view(),
        kSampleRate,
        {.source_id = "fixture:c1-tone",
         .capture_method = "synthetic-product-render",
         .evidence_id = "heritage:c1-c5-bootstrap"});
    return recorded.valid() && write_mono(out / "c1-loaded.wav", loaded) &&
           write_mono(out / "c1-recorded.wav", recorded.asset->audio());
}

bool render_c2(const std::filesystem::path& out) {
    pulp::audio::SampleHeritagePitchProcessor processor;
    if (processor.prepare(pulp::audio::SampleHeritagePitchFamily::EarlyLinear,
                          2.0, 1) != pulp::audio::SampleHeritagePitchStatus::Ok)
        return false;
    const auto plan = processor.plan(kFrames);
    if (!plan.valid()) return false;
    auto source = tone(plan.input_frames, 0.4, 375.0);
    pulp::audio::Buffer<float> reference(1, kFrames);
    std::copy_n(source.channel(0).begin(), kFrames, reference.channel(0).begin());
    pulp::audio::Buffer<float> capture(1, kFrames);
    if (processor.process(
            static_cast<const pulp::audio::Buffer<float>&>(source).view(),
            capture.view()) != pulp::audio::SampleHeritagePitchStatus::Ok)
        return false;
    return write_mono(out / "c2-source.wav", reference) &&
           write_mono(out / "c2-p12.wav", capture);
}

bool render_c3(const std::filesystem::path& out) {
    pulp::audio::SampleHeritageProfile profile;
    profile.profile_id = "neutral.c3-calibration";
    profile.host_sample_rate = kSampleRate;
    profile.voice.push_back({
        pulp::audio::SampleHeritageBlockDomain::Voice, false,
        pulp::audio::SampleHeritageVoiceConverterBlock{
            pulp::audio::SampleHeritageConverterFamily::LinearPcm,
            8.0f, 0.0f, 0.0f, 0,
            pulp::audio::SampleHeritageSeedPolicy::RestartFromProfileSeed}});
    const auto validated = pulp::audio::validate_sample_heritage_profile(profile);
    pulp::audio::SampleHeritageVoiceDsp dsp;
    if (!validated.valid() || !dsp.prepare(validated.profile, kSampleRate))
        return false;
    auto reference = tone(kFrames, 0.8, 375.0);
    dsp.process(reference.view());
    dsp.reset();
    auto lower = tone(kFrames, 0.08, 375.0);
    dsp.process(lower.view());
    return write_mono(out / "c3-reference.wav", reference) &&
           write_mono(out / "c3-lower.wav", lower);
}

bool render_c4(const std::filesystem::path& out) {
    pulp::audio::SampleHeritageProfile profile;
    profile.profile_id = "neutral.c4-calibration";
    profile.host_sample_rate = kSampleRate;
    profile.bus.push_back({
        pulp::audio::SampleHeritageBlockDomain::Bus, false,
        pulp::audio::SampleHeritageBusNoiseIdleBlock{
            .noise_amplitude = 0.01f,
            .idle_amplitude = 0.001f,
            .tilt_db_per_octave = 0.0f,
            .gate = pulp::audio::SampleHeritageNoiseGate::VoiceActive,
            .seed = UINT64_C(0x68431)}});
    const auto validated = pulp::audio::validate_sample_heritage_profile(profile);
    pulp::audio::SampleHeritageBusDsp dsp;
    if (!validated.valid() ||
        dsp.prepare(validated.profile, kSampleRate, 1) !=
            pulp::audio::SampleHeritageBusDspStatus::Ok)
        return false;
    pulp::audio::Buffer<float> active(1, kFrames);
    if (dsp.process(active.view(), true) != pulp::audio::SampleHeritageBusDspStatus::Ok)
        return false;
    dsp.reset();
    pulp::audio::Buffer<float> idle(1, kFrames);
    if (dsp.process(idle.view(), false) != pulp::audio::SampleHeritageBusDspStatus::Ok)
        return false;
    return write_mono(out / "c4-active.wav", active) &&
           write_mono(out / "c4-idle.wav", idle);
}

bool render_c5(const std::filesystem::path& out) {
    pulp::audio::SampleHeritageProfile profile;
    profile.profile_id = "neutral.c5-calibration";
    profile.host_sample_rate = kSampleRate;
    profile.voice.push_back({
        pulp::audio::SampleHeritageBlockDomain::Voice, false,
        pulp::audio::SampleHeritageVoiceHoldDroopBlock{
            pulp::audio::SampleHeritageHoldMode::ZeroOrder, 4, 0.1f}});
    const auto validated = pulp::audio::validate_sample_heritage_profile(profile);
    pulp::audio::SampleHeritageVoiceDsp dsp;
    if (!validated.valid() || !dsp.prepare(validated.profile, kSampleRate))
        return false;
    pulp::audio::Buffer<float> impulse(1, kFrames);
    impulse.channel(0)[8] = 1.0f;
    dsp.process(impulse.view());
    dsp.reset();
    pulp::audio::Buffer<float> step(1, kFrames);
    std::fill(step.channel(0).begin() + 8, step.channel(0).end(), 1.0f);
    dsp.process(step.view());
    return write_mono(out / "c5-impulse.wav", impulse) &&
           write_mono(out / "c5-step.wav", step);
}

void usage(const char* executable) {
    std::fprintf(stderr, "usage: %s --out DIR\n", executable);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path out;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--out") == 0 && index + 1 < argc) {
            out = argv[++index];
        } else if (std::strcmp(argv[index], "--help") == 0 ||
                   std::strcmp(argv[index], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    std::error_code error;
    if (out.empty() || (!std::filesystem::exists(out) &&
                        !std::filesystem::create_directories(out, error)) || error) {
        usage(argv[0]);
        return 2;
    }
    if (!render_c1(out) || !render_c2(out) || !render_c3(out) ||
        !render_c4(out) || !render_c5(out)) {
        std::fprintf(stderr, "C1-C5 calibration renderer failed\n");
        return 1;
    }
    return 0;
}
