#include "test_pulp_sampler_support.hpp"

TEST_CASE("Sampler resident mip pyramid preserves DC and octave coordinates", "[sampler][mip]") {
    SamplerResidentMipStore store;
    REQUIRE(store.prepare());
    std::vector<float> source(4097, 1.0f);
    REQUIRE(store.stage_mono(source.data(), source.size(), 48000.0, 0));

    const auto view = store.staged_view();
    REQUIRE(view.level_count > 1);
    const auto* level_one = view.level(1);
    REQUIRE(level_one != nullptr);
    REQUIRE(level_one->frames == 2049);
    REQUIRE_THAT(level_one->sample_rate, WithinAbs(24000.0, 1e-9));
    for (std::uint64_t frame = 0; frame < level_one->frames; ++frame) {
        REQUIRE_THAT(level_one->channels[0][frame], WithinAbs(1.0, 2e-6));
    }

    REQUIRE(sampler_exact_mip_octave(1.0) == 0);
    REQUIRE(sampler_exact_mip_octave(1.0001) == 0);
    REQUIRE(sampler_exact_mip_octave(2.0) == 1);
    REQUIRE(sampler_exact_mip_octave(2.0001) == 0);
}

TEST_CASE("Sampler resident mip rejects first-octave aliases", "[sampler][mip][quality]") {
    constexpr std::size_t frames = 32768;
    constexpr double pi = 3.14159265358979323846;
    auto render_tone = [&](double cycles_per_frame) {
        std::vector<float> source(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            source[frame] = static_cast<float>(
                std::sin(2.0 * pi * cycles_per_frame * static_cast<double>(frame)));
        }
        SamplerResidentMipStore store;
        REQUIRE(store.prepare());
        REQUIRE(store.stage_mono(source.data(), source.size(), 48000.0, 0));
        const auto view = store.staged_view();
        const auto* level = view.level(1);
        REQUIRE(level != nullptr);
        double energy = 0.0;
        std::size_t count = 0;
        for (std::size_t frame = 512; frame + 512 < level->frames; ++frame) {
            const auto sample = static_cast<double>(level->channels[0][frame]);
            energy += sample * sample;
            ++count;
        }
        return std::sqrt(energy / static_cast<double>(count));
    };

    const auto passband_rms = render_tone(0.10);
    const auto stopband_rms = render_tone(0.35);
    REQUIRE_THAT(passband_rms * std::sqrt(2.0), WithinAbs(1.0, 1e-3));
    REQUIRE(stopband_rms < 1e-6);
}

TEST_CASE("Streamed mip sidecar validates source and payload identity",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_valid", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    RetainedSamplerFile opened(source.path);
    const auto& base = opened.reader;
    REQUIRE(base.valid);

    auto loaded = load_sampler_stream_mip_sidecar(source.path, base, opened.retained);
    REQUIRE(loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(loaded.level_count == 2);
    REQUIRE(loaded.levels[0].reader.total_frames == 2048);
    REQUIRE(loaded.levels[0].sample_rate == 24000);
    REQUIRE(loaded.levels[1].reader.total_frames == 1024);
    REQUIRE(loaded.levels[1].sample_rate == 12000);
    REQUIRE(loaded.levels[0].reader.content_sha256 != loaded.levels[1].reader.content_sha256);
    for (std::size_t index = 0; index < sidecar.payload_paths.size(); ++index) {
        const auto payload_hex =
            runtime::hex_encode(loaded.levels[index].reader.content_sha256.data(),
                                16);
        REQUIRE(sidecar.payload_paths[index].find(payload_hex) != std::string::npos);
        REQUIRE(std::filesystem::path(sidecar.payload_paths[index]).filename().string().size() <=
                96);
    }

    auto rebuilt = audio::build_sample_mip_sidecar(source.path);
    REQUIRE(rebuilt.ok);
    REQUIRE(rebuilt.payload_paths == sidecar.payload_paths);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, base, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Valid);

    audio::SampleMipBuildOptions bounded;
    bounded.maximum_source_bytes = base.mapped_byte_size - 1;
    auto rejected = audio::build_sample_mip_sidecar(source.path, bounded);
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, base, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Valid);
}

TEST_CASE("Streamed mip sidecar preserves fractional logical sample rates",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_fractional_rate", 4096, 0.5f, 22050);
    TempSamplerMipSidecar sidecar(source);
    RetainedSamplerFile opened(source.path);
    const auto& base = opened.reader;
    REQUIRE(base.valid);

    auto loaded = load_sampler_stream_mip_sidecar(source.path, base, opened.retained);
    REQUIRE(loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(loaded.levels[0].sample_rate == 11025.0);
    REQUIRE(loaded.levels[0].reader.sample_rate == 11025);
    REQUIRE(loaded.levels[1].sample_rate == 5512.5);
    REQUIRE(loaded.levels[1].reader.sample_rate == 5513);
}

TEST_CASE("Version 5 mip manifest bytes match the independently assembled wire contract",
          "[sampler][mip][stream][sidecar][contract]") {
    TempSamplerWav source("mip_sidecar_v5_contract", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    RetainedSamplerFile opened(source.path);
    REQUIRE(opened.reader.valid);
    REQUIRE(opened.retained);

    constexpr std::uint16_t golden_version = 5;
    constexpr std::uint16_t golden_header_bytes = 116;
    constexpr std::uint32_t golden_builder_revision = 5;
    constexpr std::size_t golden_record_bytes = 80;
    const std::array<std::uint8_t, 8> golden_magic{'P', 'U', 'L', 'P', 'M', 'I', 'P', 0};
    std::vector<std::uint8_t> golden;
    golden.insert(golden.end(), golden_magic.begin(), golden_magic.end());
    append_unsigned_le(golden, golden_version);
    append_unsigned_le(golden, golden_header_bytes);
    append_unsigned_le(golden, std::uint32_t{2});
    golden.insert(golden.end(), opened.reader.content_sha256.begin(),
                  opened.reader.content_sha256.end());
    append_unsigned_le(golden, opened.reader.mapped_byte_size);
    append_unsigned_le(golden, opened.reader.channels);
    append_unsigned_le(golden, opened.reader.total_frames);
    append_unsigned_le(golden, opened.reader.sample_rate);
    append_unsigned_le(golden, golden_builder_revision);
    const auto source_identity = opened.retained->opened_file_identity();
    REQUIRE(source_identity.valid);
    append_unsigned_le(golden, source_identity.volume);
    append_unsigned_le(golden, source_identity.file);
    append_unsigned_le(golden, source_identity.generation);

    std::error_code path_error;
    const auto source_path = std::filesystem::path(source.path);
    const auto canonical_parent =
        std::filesystem::weakly_canonical(source_path.parent_path(), path_error);
    REQUIRE_FALSE(path_error);
    const auto source_spelling =
        (canonical_parent / source_path.filename()).lexically_normal().generic_string();
    const auto namespace_digest = runtime::sha256(
        reinterpret_cast<const std::uint8_t*>(source_spelling.data()), source_spelling.size());
    REQUIRE(namespace_digest.size() == 32);
    golden.insert(golden.end(), namespace_digest.begin(), namespace_digest.begin() + 16);

    std::uint64_t previous_frames = opened.reader.total_frames;
    for (std::uint32_t index = 0; index < 2; ++index) {
        RetainedSamplerFile payload(sidecar.payload_paths[index]);
        REQUIRE(payload.reader.valid);
        const auto octave = index + 1;
        const auto decimation = std::uint32_t{1} << octave;
        const auto frames = (previous_frames + 1) / 2;
        REQUIRE(payload.reader.total_frames == frames);
        append_unsigned_le(golden, octave);
        append_unsigned_le(golden, decimation);
        append_unsigned_le(golden, frames);
        append_unsigned_le(golden, opened.reader.sample_rate);
        append_unsigned_le(golden, decimation);
        append_unsigned_le(golden, payload.reader.mapped_byte_size);
        golden.insert(golden.end(), payload.reader.content_sha256.begin(),
                      payload.reader.content_sha256.end());
        golden.insert(golden.end(), 16, 0);
        previous_frames = frames;
    }

    REQUIRE(golden.size() == golden_header_bytes + 2 * golden_record_bytes);
    REQUIRE(read_binary_file(sidecar.manifest_path) == golden);
    {
        std::ofstream output(sidecar.manifest_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(golden.data()),
                     static_cast<std::streamsize>(golden.size()));
        REQUIRE(output.good());
    }
    auto loaded = load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained);
    REQUIRE(loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(loaded.level_count == 2);
    REQUIRE(loaded.levels[0].sample_rate == 24000.0);
    REQUIRE(loaded.levels[1].sample_rate == 12000.0);

    constexpr std::size_t first_record_reserved_offset = 180;
    golden[first_record_reserved_offset] = 1;
    {
        std::ofstream output(sidecar.manifest_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(golden.data()),
                     static_cast<std::streamsize>(golden.size()));
        REQUIRE(output.good());
    }
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Invalid);
}

TEST_CASE("Persisted mip WAVs preserve stereo DC and passband channel independence",
          "[sampler][mip][stream][sidecar][quality]") {
    constexpr std::uint32_t source_rate = 48000;
    constexpr std::size_t source_frames = 65536;
    std::vector<float> left(source_frames);
    std::vector<float> right(source_frames);
    for (std::size_t index = 0; index < source_frames; ++index) {
        const auto time = static_cast<double>(index) / source_rate;
        left[index] = static_cast<float>(0.20 + 0.25 * std::sin(2.0 * std::acos(-1.0) * 375.0 * time));
        right[index] = static_cast<float>(-0.10 + 0.20 * std::cos(2.0 * std::acos(-1.0) * 750.0 * time));
    }
    TempSamplerWav source("mip_sidecar_stereo_quality", {left, right}, source_rate);
    TempSamplerMipSidecar sidecar(source);

    for (std::size_t level = 0; level < sidecar.payload_paths.size(); ++level) {
        const auto decoded = audio::read_audio_file(sidecar.payload_paths[level]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->num_channels() == 2);
        const auto decimation = std::uint32_t{1} << (level + 1);
        REQUIRE(decoded->sample_rate == source_rate / decimation);
        REQUIRE(decoded->num_frames() == (level == 0 ? 32768 : 16384));
        constexpr std::size_t trim = 2048;
        REQUIRE_THAT(measured_dc(decoded->channels[0], trim), WithinAbs(0.20, 2.0e-5));
        REQUIRE_THAT(measured_dc(decoded->channels[1], trim), WithinAbs(-0.10, 2.0e-5));
        REQUIRE_THAT(measured_tone_amplitude(decoded->channels[0], 375.0,
                                             decoded->sample_rate, trim),
                     WithinAbs(0.25, 5.0e-4));
        REQUIRE_THAT(measured_tone_amplitude(decoded->channels[1], 750.0,
                                             decoded->sample_rate, trim),
                     WithinAbs(0.20, 5.0e-4));
        REQUIRE(measured_tone_amplitude(decoded->channels[0], 750.0,
                                        decoded->sample_rate, trim) < 1.0e-5);
        REQUIRE(measured_tone_amplitude(decoded->channels[1], 375.0,
                                        decoded->sample_rate, trim) < 1.0e-5);
    }
}

TEST_CASE("Persisted mono mip WAVs reject source stopband energy",
          "[sampler][mip][stream][sidecar][quality]") {
    constexpr std::uint32_t source_rate = 48000;
    constexpr std::size_t source_frames = 65536;
    std::vector<float> stopband(source_frames);
    for (std::size_t index = 0; index < source_frames; ++index) {
        stopband[index] = static_cast<float>(
            0.5 * std::sin(2.0 * std::acos(-1.0) * 18000.0 * index / source_rate));
    }
    REQUIRE(measured_rms(stopband, 2048) > 0.35);
    TempSamplerWav source("mip_sidecar_mono_stopband", {stopband}, source_rate);
    TempSamplerMipSidecar sidecar(source);
    for (const auto& payload_path : sidecar.payload_paths) {
        const auto decoded = audio::read_audio_file(payload_path);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->num_channels() == 1);
        REQUIRE(measured_rms(decoded->channels[0], 2048) < 2.0e-5);
    }
}

#ifndef _WIN32
TEST_CASE("Streamed mip publication preserves the source access policy",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_permissions", 4096, 0.5f, 48000);
    constexpr auto intended = std::filesystem::perms::owner_read |
                              std::filesystem::perms::owner_write |
                              std::filesystem::perms::group_read;
    std::error_code error;
    std::filesystem::permissions(source.path, intended, std::filesystem::perm_options::replace,
                                 error);
    REQUIRE_FALSE(error);
    TempSamplerMipSidecar sidecar(source);

    const auto mask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                      std::filesystem::perms::others_all;
    REQUIRE((std::filesystem::status(sidecar.manifest_path).permissions() & mask) == intended);
    for (const auto& payload : sidecar.payload_paths) {
        REQUIRE((std::filesystem::status(payload).permissions() & mask) == intended);
    }

    constexpr auto tightened =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(source.path, tightened, std::filesystem::perm_options::replace,
                                 error);
    REQUIRE_FALSE(error);
    auto rebuilt = audio::build_sample_mip_sidecar(source.path);
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE(rebuilt.payload_paths == sidecar.payload_paths);
    REQUIRE((std::filesystem::status(rebuilt.manifest_path).permissions() & mask) == tightened);
    for (const auto& payload : rebuilt.payload_paths) {
        REQUIRE((std::filesystem::status(payload).permissions() & mask) == tightened);
    }
}

TEST_CASE("Mip payload policy reconciliation does not mutate hardlink peers",
          "[sampler][mip][stream][sidecar][security]") {
    TempSamplerWav source("mip_sidecar_hardlink_policy", 4096, 0.5f, 48000);
    constexpr auto original_mode = std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read;
    constexpr auto tightened_mode = std::filesystem::perms::owner_read |
                                    std::filesystem::perms::owner_write;
    std::error_code error;
    std::filesystem::permissions(source.path, original_mode,
                                 std::filesystem::perm_options::replace, error);
    REQUIRE_FALSE(error);
    TempSamplerMipSidecar sidecar(source, 1);
    const auto victim = sidecar.payload_paths.front() + ".hardlink-victim";
    std::filesystem::create_hard_link(sidecar.payload_paths.front(), victim, error);
    REQUIRE_FALSE(error);
    auto cleanup = runtime::make_scope_guard([&] { std::filesystem::remove(victim, error); });
    REQUIRE(std::filesystem::equivalent(sidecar.payload_paths.front(), victim));
    const auto victim_identity = runtime::file_identity(victim);
    REQUIRE(victim_identity.valid);
    const auto victim_bytes = [&] {
        std::ifstream input(victim, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input), {});
    }();

    std::filesystem::permissions(source.path, tightened_mode,
                                 std::filesystem::perm_options::replace, error);
    REQUIRE_FALSE(error);
    audio::SampleMipBuildOptions one_level;
    one_level.level_count = 1;
    auto rebuilt = audio::build_sample_mip_sidecar(source.path, one_level);
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);

    const auto mask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                      std::filesystem::perms::others_all;
    REQUIRE_FALSE(std::filesystem::equivalent(rebuilt.payload_paths.front(), victim));
    REQUIRE((std::filesystem::status(rebuilt.payload_paths.front()).permissions() & mask) ==
            tightened_mode);
    REQUIRE((std::filesystem::status(victim).permissions() & mask) == original_mode);
    const auto victim_identity_after = runtime::file_identity(victim);
    REQUIRE(victim_identity_after.valid);
    REQUIRE(victim_identity_after.volume == victim_identity.volume);
    REQUIRE(victim_identity_after.file == victim_identity.file);
    std::ifstream victim_after(victim, std::ios::binary);
    REQUIRE(std::vector<char>(std::istreambuf_iterator<char>(victim_after), {}) == victim_bytes);
}

TEST_CASE("Identical mip sources at different paths do not share access policy",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav private_source("mip_sidecar_private_instance", 4096, 0.5f, 48000);
    TempSamplerWav public_source("mip_sidecar_public_instance", 4096, 0.5f, 48000);
    constexpr auto private_mode =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    constexpr auto public_mode =
        private_mode | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
    std::filesystem::permissions(private_source.path, private_mode,
                                 std::filesystem::perm_options::replace);
    std::filesystem::permissions(public_source.path, public_mode,
                                 std::filesystem::perm_options::replace);

    TempSamplerMipSidecar private_sidecar(private_source);
    TempSamplerMipSidecar public_sidecar(public_source);
    REQUIRE(private_sidecar.payload_paths != public_sidecar.payload_paths);
    const auto mask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                      std::filesystem::perms::others_all;
    for (const auto& payload : private_sidecar.payload_paths)
        REQUIRE((std::filesystem::status(payload).permissions() & mask) == private_mode);
    for (const auto& payload : public_sidecar.payload_paths)
        REQUIRE((std::filesystem::status(payload).permissions() & mask) == public_mode);
}

TEST_CASE("Mip garbage collection is isolated across source aliases",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_alias_gc", 4096, 0.5f, 48000);
    TempSamplerMipSidecar direct_sidecar(source);
    const auto alias = source.path + ".alias.wav";
    std::filesystem::create_symlink(source.path, alias);
    std::vector<std::string> alias_payloads;
    auto cleanup = runtime::make_scope_guard([&] {
        std::error_code error;
        std::filesystem::remove(alias + ".pulpmip", error);
        std::filesystem::remove(alias, error);
        for (const auto& payload : alias_payloads)
            std::filesystem::remove(payload, error);
    });
    auto alias_build = audio::build_sample_mip_sidecar(alias);
    INFO(alias_build.error);
    REQUIRE(alias_build.ok);
    alias_payloads = alias_build.payload_paths;
    REQUIRE(alias_payloads != direct_sidecar.payload_paths);

    audio::SampleMipBuildOptions one_level;
    one_level.level_count = 1;
    auto shortened = audio::build_sample_mip_sidecar(alias, one_level);
    INFO(shortened.error);
    REQUIRE(shortened.ok);
    for (const auto& payload : direct_sidecar.payload_paths)
        REQUIRE(std::filesystem::exists(payload));

    RetainedSamplerFile direct_source(source.path);
    REQUIRE(direct_source.reader.valid);
    REQUIRE(
        load_sampler_stream_mip_sidecar(source.path, direct_source.reader, direct_source.retained)
            .status == SamplerStreamMipSidecarStatus::Valid);
}

TEST_CASE("Mip manifests retain their payload namespace across parent aliases",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_parent_alias", 4096, 0.5f, 48000);
    TempSamplerMipSidecar direct_sidecar(source);
    const auto source_path = std::filesystem::path(source.path);
    const auto alias_parent =
        source_path.parent_path() / (source_path.filename().string() + ".parent-alias");
    std::filesystem::create_directory_symlink(source_path.parent_path(), alias_parent);
    auto cleanup = runtime::make_scope_guard([&] {
        std::error_code error;
        std::filesystem::remove(alias_parent, error);
    });
    const auto alias_source = alias_parent / source_path.filename();

    audio::SampleMipBuildOptions one_level;
    one_level.level_count = 1;
    auto rebuilt = audio::build_sample_mip_sidecar(alias_source.string(), one_level);
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE(rebuilt.manifest_path == direct_sidecar.manifest_path);
    REQUIRE(std::filesystem::equivalent(rebuilt.manifest_path, direct_sidecar.manifest_path));
    REQUIRE(rebuilt.payload_paths.front() == direct_sidecar.payload_paths.front());

    RetainedSamplerFile direct_source(source.path);
    RetainedSamplerFile alias_opened(alias_source.string());
    REQUIRE(direct_source.reader.valid);
    REQUIRE(alias_opened.reader.valid);
    const auto direct_loaded =
        load_sampler_stream_mip_sidecar(source.path, direct_source.reader, direct_source.retained);
    const auto alias_loaded = load_sampler_stream_mip_sidecar(
        alias_source.string(), alias_opened.reader, alias_opened.retained);
    REQUIRE(direct_loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(alias_loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(direct_loaded.level_count == 1);
    REQUIRE(alias_loaded.level_count == 1);
}
#endif

TEST_CASE("Mip rebuilds reject namespaces copied from another source",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav first_source("mip_sidecar_namespace_owner_a", 4096, 0.25f, 48000);
    TempSamplerWav second_source("mip_sidecar_namespace_owner_b", 4096, 0.5f, 48000);
    TempSamplerMipSidecar first_sidecar(first_source);
    TempSamplerMipSidecar second_sidecar(second_source);
    constexpr std::streamoff manifest_namespace_offset = 100;
    std::array<char, 16> transplanted_namespace{};
    {
        std::ifstream first_manifest(first_sidecar.manifest_path, std::ios::binary);
        first_manifest.seekg(manifest_namespace_offset);
        first_manifest.read(transplanted_namespace.data(), transplanted_namespace.size());
        REQUIRE(first_manifest.good());
    }
    {
        std::fstream second_manifest(second_sidecar.manifest_path,
                                     std::ios::binary | std::ios::in | std::ios::out);
        second_manifest.seekp(manifest_namespace_offset);
        second_manifest.write(transplanted_namespace.data(), transplanted_namespace.size());
        second_manifest.flush();
        REQUIRE(second_manifest.good());
    }

    auto rebuilt = audio::build_sample_mip_sidecar(second_source.path);
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE(rebuilt.payload_paths == second_sidecar.payload_paths);
    for (const auto& payload : first_sidecar.payload_paths)
        REQUIRE(std::filesystem::exists(payload));

    RetainedSamplerFile first_opened(first_source.path);
    REQUIRE(first_opened.reader.valid);
    REQUIRE(load_sampler_stream_mip_sidecar(first_source.path, first_opened.reader,
                                            first_opened.retained)
                .status == SamplerStreamMipSidecarStatus::Valid);
}

TEST_CASE("Mip publication failure cuts restore the previous bundle",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_publication_rollback", 4096, 0.5f, 48000);
    TempSamplerMipSidecar previous(source, 1);
    RetainedSamplerFile opened(source.path);
    REQUIRE(opened.reader.valid);
    const auto owner_prefix =
        mip_owner_filename_prefix(source.path, opened.reader.content_sha256);

    const auto payload_inventory = [&] {
        std::set<std::string> names;
        const auto parent = std::filesystem::path(source.path).parent_path();
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(parent, error), end;
             !error && iterator != end; iterator.increment(error)) {
            const auto name = iterator->path().filename().string();
            if (name.starts_with(owner_prefix) && iterator->path().extension() == ".wav")
                names.insert(name);
        }
        REQUIRE_FALSE(error);
        return names;
    };
    const auto baseline_payloads = payload_inventory();
    const std::array faults{
        audio::detail::SampleMipBuildFaultForTesting::ManifestPolicyFinalization,
        audio::detail::SampleMipBuildFaultForTesting::SourceChangedAfterManifestPublication,
        audio::detail::SampleMipBuildFaultForTesting::PublishedManifestVerification,
    };
    auto reset_fault = runtime::make_scope_guard([] {
        audio::detail::set_sample_mip_build_fault_for_testing(
            audio::detail::SampleMipBuildFaultForTesting::None);
    });
    for (const auto fault : faults) {
        audio::detail::set_sample_mip_build_fault_for_testing(fault);
        auto failed = audio::build_sample_mip_sidecar(source.path);
        INFO(failed.error);
        REQUIRE_FALSE(failed.ok);
        REQUIRE(failed.payload_paths.empty());
        REQUIRE(payload_inventory() == baseline_payloads);
        const auto restored = load_sampler_stream_mip_sidecar(
            source.path, opened.reader, opened.retained);
        REQUIRE(restored.status == SamplerStreamMipSidecarStatus::Valid);
        REQUIRE(restored.level_count == 1);
    }

    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::PublishedManifestVerificationException);
    REQUIRE_THROWS_AS(audio::build_sample_mip_sidecar(source.path), std::runtime_error);
    REQUIRE(payload_inventory() == baseline_payloads);
    const auto restored =
        load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained);
    REQUIRE(restored.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(restored.level_count == 1);
}

TEST_CASE("Mip payload publication exceptions remove unpublished owner payloads",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_payload_exception", 4096, 0.5f, 48000);
    RetainedSamplerFile opened(source.path);
    REQUIRE(opened.reader.valid);
    const auto owner_prefix =
        mip_owner_filename_prefix(source.path, opened.reader.content_sha256);
    const auto payload_inventory = [&] {
        std::set<std::string> names;
        const auto parent = std::filesystem::path(source.path).parent_path();
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(parent, error), end;
             !error && iterator != end; iterator.increment(error)) {
            const auto name = iterator->path().filename().string();
            if (name.starts_with(owner_prefix) && iterator->path().extension() == ".wav")
                names.insert(name);
        }
        REQUIRE_FALSE(error);
        return names;
    };
    const auto baseline_payloads = payload_inventory();
    auto reset_fault = runtime::make_scope_guard([] {
        audio::detail::set_sample_mip_build_fault_for_testing(
            audio::detail::SampleMipBuildFaultForTesting::None);
    });
    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::PayloadPublicationException);
    REQUIRE_THROWS_AS(audio::build_sample_mip_sidecar(source.path), std::runtime_error);
    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::None);
    REQUIRE_FALSE(std::filesystem::exists(source.path + ".pulpmip"));
    REQUIRE(payload_inventory() == baseline_payloads);
}

#ifndef _WIN32
TEST_CASE("Mip builders reject staging in a shared non-sticky parent",
          "[sampler][mip][stream][sidecar][security]") {
    TempSamplerWav source("mip_sidecar_unsafe_parent_source", 4096, 0.5f, 48000);
    const auto unsafe_parent = std::filesystem::path(source.path + ".unsafe-parent");
    const auto copied_source = unsafe_parent / "tone.wav";
    std::filesystem::create_directory(unsafe_parent);
    std::filesystem::copy_file(source.path, copied_source);
    std::error_code error;
    std::filesystem::permissions(unsafe_parent, std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace, error);
    REQUIRE_FALSE(error);
    auto cleanup = runtime::make_scope_guard([&] {
        std::filesystem::permissions(unsafe_parent, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(unsafe_parent, error);
    });

    const auto rejected = audio::build_sample_mip_sidecar(copied_source.string());
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(rejected.error == "failed to create a private temporary directory");
    REQUIRE_FALSE(std::filesystem::exists(copied_source.string() + ".pulpmip"));
}

TEST_CASE("Mip rebuild replaces a symlink manifest without touching its target",
          "[sampler][mip][stream][sidecar][security]") {
    TempSamplerWav source("mip_sidecar_manifest_symlink", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source, 1);
    const auto sentinel = source.path + ".manifest-sentinel";
    const std::string sentinel_bytes = "external sentinel must not be rewritten";
    {
        std::ofstream output(sentinel, std::ios::binary);
        output.write(sentinel_bytes.data(), static_cast<std::streamsize>(sentinel_bytes.size()));
        REQUIRE(output.good());
    }
    std::error_code error;
    auto cleanup = runtime::make_scope_guard([&] { std::filesystem::remove(sentinel, error); });
    std::filesystem::remove(sidecar.manifest_path, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_symlink(sentinel, sidecar.manifest_path, error);
    REQUIRE_FALSE(error);

    audio::SampleMipBuildOptions one_level;
    one_level.level_count = 1;
    auto rebuilt = audio::build_sample_mip_sidecar(source.path, one_level);
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE_FALSE(std::filesystem::is_symlink(std::filesystem::symlink_status(
        rebuilt.manifest_path)));
    std::ifstream sentinel_after(sentinel, std::ios::binary);
    REQUIRE(std::string(std::istreambuf_iterator<char>(sentinel_after), {}) == sentinel_bytes);
    RetainedSamplerFile opened(source.path);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Valid);
}
#endif

TEST_CASE("Case-equivalent mip builders serialize one manifest",
          "[sampler][mip][stream][sidecar]") {
    REQUIRE(audio::detail::sample_mip_coordination_key_for_manifest_path(
                "/tmp/Kick.wav.pulpmip", false) !=
            audio::detail::sample_mip_coordination_key_for_manifest_path(
                "/tmp/kick.wav.pulpmip", false));
    REQUIRE(audio::detail::sample_mip_coordination_key_for_manifest_path(
                "/tmp/Kick.wav.pulpmip", true) ==
            audio::detail::sample_mip_coordination_key_for_manifest_path(
                "/tmp/kick.wav.pulpmip", true));
    TempSamplerWav source("mip_sidecar_case_alias", 262144, 0.5f, 48000);
    TempSamplerMipSidecar initial(source);
    auto alias_path = std::filesystem::path(source.path);
    auto alias_name = alias_path.filename().string();
    alias_name.front() = alias_name.front() == 'p' ? 'P' : 'p';
    alias_path = alias_path.parent_path() / alias_name;
    std::error_code equivalent_error;
    if (!std::filesystem::equivalent(source.path, alias_path, equivalent_error) ||
        equivalent_error) {
        RetainedSamplerFile direct_identity(source.path);
        REQUIRE(direct_identity.reader.valid);
        REQUIRE(audio::detail::sample_mip_coordination_key(
                    source.path, direct_identity.retained->opened_file_identity()) !=
                audio::detail::sample_mip_coordination_key(
                    alias_path.string(), direct_identity.retained->opened_file_identity()));
        return;
    }
    RetainedSamplerFile direct_identity(source.path);
    RetainedSamplerFile alias_identity(alias_path.string());
    REQUIRE(direct_identity.reader.valid);
    REQUIRE(alias_identity.reader.valid);
    REQUIRE(audio::detail::sample_mip_coordination_key(
                source.path, direct_identity.retained->opened_file_identity()) ==
            audio::detail::sample_mip_coordination_key(
                alias_path.string(), alias_identity.retained->opened_file_identity()));

    for (int round = 0; round < 8; ++round) {
        audio::SampleMipBuildOptions one_level;
        one_level.level_count = 1;
        audio::SampleMipBuildOptions two_levels;
        two_levels.level_count = 2;
        audio::SampleMipBuildResult direct;
        audio::SampleMipBuildResult alias;
        std::atomic<bool> start{false};
        std::thread direct_builder([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            direct = audio::build_sample_mip_sidecar(source.path, one_level);
        });
        std::thread alias_builder([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            alias = audio::build_sample_mip_sidecar(alias_path.string(), two_levels);
        });
        start.store(true, std::memory_order_release);
        direct_builder.join();
        alias_builder.join();
        INFO(direct.error);
        INFO(alias.error);
        REQUIRE(direct.ok);
        REQUIRE(alias.ok);

        RetainedSamplerFile direct_opened(source.path);
        RetainedSamplerFile alias_opened(alias_path.string());
        REQUIRE(load_sampler_stream_mip_sidecar(source.path, direct_opened.reader,
                                                direct_opened.retained)
                    .status == SamplerStreamMipSidecarStatus::Valid);
        REQUIRE(load_sampler_stream_mip_sidecar(alias_path.string(), alias_opened.reader,
                                                alias_opened.retained)
                    .status == SamplerStreamMipSidecarStatus::Valid);
    }
}

TEST_CASE("Concurrent mip rebuilds publish one coherent sidecar",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_concurrent", 4096, 0.5f, 48000);
    TempSamplerMipSidecar initial(source);
    audio::SampleMipBuildResult first;
    audio::SampleMipBuildResult second;
    std::thread first_builder([&] { first = audio::build_sample_mip_sidecar(source.path); });
    std::thread second_builder([&] { second = audio::build_sample_mip_sidecar(source.path); });
    first_builder.join();
    second_builder.join();
    INFO(first.error);
    INFO(second.error);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    REQUIRE(first.payload_paths == second.payload_paths);

    RetainedSamplerFile opened(source.path);
    const auto& base = opened.reader;
    REQUIRE(base.valid);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, base, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Valid);
}

TEST_CASE("Mip rebuild construction keeps the previous sidecar readable",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_read_during_rebuild", 262144, 0.5f, 48000);
    TempSamplerMipSidecar initial(source);
    RetainedSamplerFile opened(source.path);
    REQUIRE(opened.reader.valid);

    std::array<audio::SampleMipBuildResult, 2> rebuilds;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::thread builder([&] {
        started.store(true, std::memory_order_release);
        for (auto& rebuilt : rebuilds)
            rebuilt = audio::build_sample_mip_sidecar(source.path);
        finished.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();

    std::uint32_t successful_loads = 0;
    std::uint32_t invalid_loads = 0;
    auto first_invalid_status = SamplerStreamMipSidecarStatus::Valid;
    while (!finished.load(std::memory_order_acquire)) {
        const auto loaded =
            load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained);
        if (loaded.status != SamplerStreamMipSidecarStatus::Valid) {
            first_invalid_status = loaded.status;
            ++invalid_loads;
            break;
        }
        ++successful_loads;
    }
    builder.join();
    INFO("sidecar status=" << static_cast<unsigned>(first_invalid_status));
    REQUIRE(invalid_loads == 0);
    REQUIRE(successful_loads > 0);
    for (const auto& rebuilt : rebuilds) {
        INFO(rebuilt.error);
        REQUIRE(rebuilt.ok);
    }
}

TEST_CASE("Mip sidecars reject byte-identical source replacements",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_replaced_identity", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    const auto retained = source.path + ".old-identity";
    std::filesystem::rename(source.path, retained);
    auto cleanup = runtime::make_scope_guard([&] {
        std::error_code error;
        std::filesystem::remove(retained, error);
    });
    std::filesystem::copy_file(retained, source.path);

    RetainedSamplerFile opened_replacement(source.path);
    const auto& replacement = opened_replacement.reader;
    REQUIRE(replacement.valid);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, replacement, opened_replacement.retained)
                .status == SamplerStreamMipSidecarStatus::Invalid);
}

TEST_CASE("Mip coordination survives source generation replacement",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_generation_lock", 4096, 0.5f, 48000);
    RetainedSamplerFile original(source.path);
    REQUIRE(original.reader.valid);
    const auto original_key = audio::detail::sample_mip_coordination_key(
        source.path, original.retained->opened_file_identity());

    const auto retained = source.path + ".old-generation";
    std::filesystem::rename(source.path, retained);
    auto cleanup = runtime::make_scope_guard([&] {
        std::error_code error;
        std::filesystem::remove(retained, error);
    });
    std::filesystem::copy_file(retained, source.path);
    RetainedSamplerFile replacement(source.path);
    REQUIRE(replacement.reader.valid);
    REQUIRE(replacement.retained->opened_file_identity() !=
            original.retained->opened_file_identity());
    REQUIRE(audio::detail::sample_mip_coordination_key(
                source.path, replacement.retained->opened_file_identity()) == original_key);
}

TEST_CASE("Failed mip builds do not report rolled-back payloads",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_failure_result", 4096, 0.5f, 48000);
    audio::SampleMipBuildOptions options;
    options.level_count = 1;
    options.maximum_output_bytes = 2048 * sizeof(float);
    auto rejected = audio::build_sample_mip_sidecar(source.path, options);
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(rejected.error == "mip payloads exceed the configured byte limit");
    REQUIRE(rejected.payload_paths.empty());
}

TEST_CASE("Mip payload naming normalizes equivalent source spellings",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_normalized_path", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    const auto path = std::filesystem::path(source.path);
    const auto equivalent = path.parent_path() / "." / path.filename();
    auto rebuilt = audio::build_sample_mip_sidecar(equivalent.string());
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE(rebuilt.payload_paths == sidecar.payload_paths);
}

TEST_CASE("Successful mip rebuilds reclaim superseded instance payloads",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_gc", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    auto orphan = std::filesystem::path(sidecar.payload_paths.front());
    auto orphan_name = orphan.filename().string();
    orphan_name[orphan_name.size() - 5] = orphan_name[orphan_name.size() - 5] == '0' ? '1' : '0';
    orphan = orphan.parent_path() / orphan_name;
    std::filesystem::copy_file(sidecar.payload_paths.front(), orphan);
    REQUIRE(std::filesystem::exists(orphan));

    auto rebuilt = audio::build_sample_mip_sidecar(source.path);
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE_FALSE(std::filesystem::exists(orphan));
}

TEST_CASE("Post-commit mip garbage collection exceptions preserve the published bundle",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_post_commit_gc_exception", 4096, 0.5f, 48000);
    TempSamplerMipSidecar previous(source);
    REQUIRE(previous.payload_paths.size() == 2);
    const auto superseded_payload = previous.payload_paths.back();
    auto reset_fault = runtime::make_scope_guard([] {
        audio::detail::set_sample_mip_build_fault_for_testing(
            audio::detail::SampleMipBuildFaultForTesting::None);
    });

    audio::SampleMipBuildOptions one_level;
    one_level.level_count = 1;
    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::PostCommitGarbageCollectionException);
    auto committed = audio::build_sample_mip_sidecar(source.path, one_level);
    INFO(committed.error);
    REQUIRE(committed.ok);
    REQUIRE(committed.payload_paths.size() == 1);
    REQUIRE(std::filesystem::exists(superseded_payload));

    RetainedSamplerFile opened(source.path);
    REQUIRE(opened.reader.valid);
    const auto loaded =
        load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained);
    REQUIRE(loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(loaded.level_count == 1);

    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::None);
    auto retried = audio::build_sample_mip_sidecar(source.path, one_level);
    INFO(retried.error);
    REQUIRE(retried.ok);
    REQUIRE_FALSE(std::filesystem::exists(superseded_payload));
}

TEST_CASE("Mip rebuilds reclaim payloads from the previous source revision",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_source_revision_gc", 4096, 0.25f, 48000);
    TempSamplerMipSidecar previous(source);
    const auto previous_payloads = previous.payload_paths;

    audio::AudioFileData replacement;
    replacement.sample_rate = 48000;
    replacement.channels = {std::vector<float>(8192, -0.5f)};
    REQUIRE(audio::write_wav_file(source.path, replacement, audio::WavBitDepth::Float32));
    const auto rebuilt = audio::build_sample_mip_sidecar(source.path);
    auto cleanup_rebuilt = runtime::make_scope_guard([&] {
        std::error_code error;
        for (const auto& payload : rebuilt.payload_paths)
            std::filesystem::remove(payload, error);
    });
    INFO(rebuilt.error);
    REQUIRE(rebuilt.ok);
    REQUIRE(rebuilt.payload_paths != previous_payloads);
    for (const auto& payload : previous_payloads)
        REQUIRE_FALSE(std::filesystem::exists(payload));

    RetainedSamplerFile opened(source.path);
    REQUIRE(opened.reader.valid);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, opened.reader, opened.retained).status ==
            SamplerStreamMipSidecarStatus::Valid);
}

#ifndef _WIN32
TEST_CASE("Mip rebuilds through parent aliases reclaim every old source revision",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_parent_alias_revision_gc", 4096, 0.25f, 48000);
    TempSamplerMipSidecar first_revision(source);
    const auto first_payloads = first_revision.payload_paths;

    audio::AudioFileData second_data;
    second_data.sample_rate = 48000;
    second_data.channels = {std::vector<float>(8192, -0.25f)};
    REQUIRE(audio::write_wav_file(source.path, second_data, audio::WavBitDepth::Float32));
    auto reset_fault = runtime::make_scope_guard([] {
        audio::detail::set_sample_mip_build_fault_for_testing(
            audio::detail::SampleMipBuildFaultForTesting::None);
    });
    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::PostCommitGarbageCollectionException);
    auto second_revision = audio::build_sample_mip_sidecar(source.path);
    INFO(second_revision.error);
    REQUIRE(second_revision.ok);
    const auto second_payloads = second_revision.payload_paths;
    REQUIRE(second_payloads != first_payloads);
    for (const auto& payload : first_payloads)
        REQUIRE(std::filesystem::exists(payload));
    audio::detail::set_sample_mip_build_fault_for_testing(
        audio::detail::SampleMipBuildFaultForTesting::None);

    const auto source_path = std::filesystem::path(source.path);
    const auto alias_parent =
        source_path.parent_path() / (source_path.filename().string() + ".revision-parent-alias");
    std::filesystem::create_directory_symlink(source_path.parent_path(), alias_parent);
    std::vector<std::string> third_payloads;
    auto cleanup = runtime::make_scope_guard([&] {
        std::error_code error;
        std::filesystem::remove(alias_parent, error);
        for (const auto& payload : second_payloads)
            std::filesystem::remove(payload, error);
        for (const auto& payload : third_payloads)
            std::filesystem::remove(payload, error);
    });

    audio::AudioFileData third_data;
    third_data.sample_rate = 48000;
    third_data.channels = {std::vector<float>(16384, 0.75f)};
    REQUIRE(audio::write_wav_file(source.path, third_data, audio::WavBitDepth::Float32));
    const auto alias_source = alias_parent / source_path.filename();
    auto third_revision = audio::build_sample_mip_sidecar(alias_source.string());
    INFO(third_revision.error);
    REQUIRE(third_revision.ok);
    third_payloads = third_revision.payload_paths;
    REQUIRE(third_revision.payload_paths != second_payloads);
    for (const auto& payload : first_payloads)
        REQUIRE_FALSE(std::filesystem::exists(payload));
    for (const auto& payload : second_payloads)
        REQUIRE_FALSE(std::filesystem::exists(payload));
    for (const auto& payload : third_revision.payload_paths)
        REQUIRE(std::filesystem::exists(payload));

    RetainedSamplerFile direct_opened(source.path);
    RetainedSamplerFile alias_opened(alias_source.string());
    REQUIRE(direct_opened.reader.valid);
    REQUIRE(alias_opened.reader.valid);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, direct_opened.reader,
                                            direct_opened.retained)
                .status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(load_sampler_stream_mip_sidecar(alias_source.string(), alias_opened.reader,
                                            alias_opened.retained)
                .status == SamplerStreamMipSidecarStatus::Valid);
}
#endif
