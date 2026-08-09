#include <pulp/audio/wavetable_authoring.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/sinc_resampler.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::audio {
namespace {

constexpr std::uint32_t kMinimumTableLength = 256;
constexpr std::uint32_t kMaximumTableLength = 8192;
constexpr std::uint32_t kMaximumBands = 32;
constexpr std::uint32_t kMaximumGuardHarmonics = 8;
constexpr std::size_t kMaximumProvenanceTextBytes = 4096;
// One work item is one source-projection or destination-synthesis harmonic
// term. This keeps arbitrary explicit cycles from turning the offline exact
// Fourier downsampler into unbounded quadratic work.
constexpr std::uint64_t kMaximumFourierResampleWork = 1u << 25;

bool is_power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool valid_text_size(std::string_view text) noexcept {
    return text.size() <= kMaximumProvenanceTextBytes;
}

bool valid_recipe(const WavetableAuthoringRecipe& recipe) noexcept {
    const bool automatic_cycle_valid = recipe.explicit_cycle.has_value() ||
                                       (recipe.automatic_cycle.minimum_cycle_samples >= 1 &&
                                        recipe.automatic_cycle.minimum_cycle_samples <=
                                            recipe.automatic_cycle.maximum_cycle_samples &&
                                        std::isfinite(recipe.automatic_cycle.minimum_correlation) &&
                                        recipe.automatic_cycle.minimum_correlation >= 0.0 &&
                                        recipe.automatic_cycle.minimum_correlation <= 1.0);
    return recipe.schema_version == kWavetableAuthoringRecipeSchemaVersion &&
           is_power_of_two(recipe.table_length) && recipe.table_length >= kMinimumTableLength &&
           recipe.table_length <= kMaximumTableLength && recipe.num_bands >= 1 &&
           recipe.num_bands <= kMaximumBands && std::isfinite(recipe.reference_sample_rate) &&
           recipe.reference_sample_rate >= 22050.0 && recipe.reference_sample_rate <= 192000.0 &&
           recipe.guard_harmonics <= kMaximumGuardHarmonics && automatic_cycle_valid &&
           std::isfinite(recipe.normalize_target_dbfs) && recipe.normalize_target_dbfs >= -6.0 &&
           recipe.normalize_target_dbfs <= 0.0 && std::isfinite(recipe.maximum_seam_error) &&
           recipe.maximum_seam_error >= 0.0 && recipe.maximum_seam_error <= 1.0;
}

bool valid_provenance(const WavetableAuthoringProvenance& provenance) noexcept {
    return valid_text_size(provenance.source.source_id) &&
           valid_text_size(provenance.source.capture_method) &&
           valid_text_size(provenance.source.evidence_id) &&
           valid_text_size(provenance.license_id) && valid_text_size(provenance.rights_note);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_float(std::vector<std::uint8_t>& bytes, float value) {
    if (value == 0.0f)
        value = 0.0f;
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_double(std::vector<std::uint8_t>& bytes, double value) {
    if (value == 0.0)
        value = 0.0;
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_domain(std::vector<std::uint8_t>& bytes, std::string_view domain) {
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    bytes.push_back(0);
}

std::string source_hash(BufferView<const float> source, double sample_rate) {
    std::vector<std::uint8_t> bytes;
    append_domain(bytes, "pulp.wavetable-authoring.source.v1");
    append_double(bytes, sample_rate);
    append_u32(bytes, static_cast<std::uint32_t>(source.num_channels()));
    append_u64(bytes, static_cast<std::uint64_t>(source.num_samples()));
    for (std::size_t channel = 0; channel < source.num_channels(); ++channel)
        for (const float sample : source.channel(channel))
            append_float(bytes, sample);
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

std::string materialized_hash(const WavetableCompileResult& result) {
    std::vector<std::uint8_t> bytes;
    append_domain(bytes, "pulp.wavetable-authoring.materialized.v1");
    append_u64(bytes, static_cast<std::uint64_t>(result.bands.size()));
    for (const auto& band : result.bands) {
        append_float(bytes, band.max_frequency_hz);
        append_u64(bytes, static_cast<std::uint64_t>(band.samples.size()));
        for (const float sample : band.samples)
            append_float(bytes, sample);
    }
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

WavetableCompileStatus map_cycle_status(SampleHeritageAutoCycleStatus status) {
    switch (status) {
    case SampleHeritageAutoCycleStatus::Ok:
        return WavetableCompileStatus::Ok;
    case SampleHeritageAutoCycleStatus::InsufficientSignal:
        return WavetableCompileStatus::InsufficientSignal;
    case SampleHeritageAutoCycleStatus::NoReliableCycle:
        return WavetableCompileStatus::NoReliableCycle;
    case SampleHeritageAutoCycleStatus::InvalidSource:
        return WavetableCompileStatus::InvalidSource;
    case SampleHeritageAutoCycleStatus::InvalidRange:
        return WavetableCompileStatus::InvalidRecipe;
    }
    return WavetableCompileStatus::InvalidRecipe;
}

struct SeamMeasurement {
    std::uint64_t start = 0;
    double level = 0.0;
    double slope = 0.0;
    double normalized = std::numeric_limits<double>::infinity();
};

SeamMeasurement measure_seam(const std::span<const float> samples, std::uint64_t start,
                             std::uint32_t length, double scale) {
    SeamMeasurement result;
    result.start = start;
    const auto s = static_cast<std::size_t>(start);
    const auto next = s + static_cast<std::size_t>(length);
    result.level = std::abs(static_cast<double>(samples[s]) - samples[next]);
    const double left_slope = static_cast<double>(samples[s + 1]) - samples[s];
    const double right_slope = static_cast<double>(samples[next + 1]) - samples[next];
    result.slope = std::abs(left_slope - right_slope);
    scale = std::max(scale, 1.0e-12);
    result.normalized = std::max(result.level / scale, result.slope / (2.0 * scale));
    return result;
}

std::optional<SeamMeasurement> select_automatic_seam(const std::span<const float> samples,
                                                     const SampleHeritageAutoCycleOptions& options,
                                                     std::uint32_t length) {
    const std::uint64_t begin = options.analysis_start_frame;
    const std::uint64_t end = options.analysis_end_frame == 0
                                  ? static_cast<std::uint64_t>(samples.size())
                                  : options.analysis_end_frame;
    if (begin >= end || end > samples.size() || length == 0)
        return std::nullopt;
    // Two right-hand samples are required to compare the real adjacent-period
    // boundary and its forward slope without inventing wrapped source data.
    if (static_cast<std::uint64_t>(length) + 2 > end - begin)
        return std::nullopt;
    const std::uint64_t last = end - static_cast<std::uint64_t>(length) - 2;
    // Prefix energy makes every candidate's cycle-local RMS normalization O(1),
    // keeping the complete start scan linear in the analysis-window length.
    std::vector<long double> prefix_energy(static_cast<std::size_t>(end - begin) + 1, 0.0L);
    for (std::uint64_t frame = begin; frame < end; ++frame) {
        const auto sample = static_cast<long double>(samples[frame]);
        prefix_energy[static_cast<std::size_t>(frame - begin + 1)] =
            prefix_energy[static_cast<std::size_t>(frame - begin)] + sample * sample;
    }
    SeamMeasurement best;
    bool found = false;
    for (std::uint64_t start = begin; start <= last; ++start) {
        const auto offset = static_cast<std::size_t>(start - begin);
        const auto energy = prefix_energy[offset + length] - prefix_energy[offset];
        const double rms = std::sqrt(static_cast<double>(energy / length));
        const auto candidate = measure_seam(samples, start, length, rms);
        if (!found || candidate.normalized < best.normalized) {
            best = candidate;
            found = true;
        }
        if (start == last)
            break;
    }
    return found ? std::optional<SeamMeasurement>(best) : std::nullopt;
}

bool all_finite(BufferView<const float> source) {
    for (std::size_t channel = 0; channel < source.num_channels(); ++channel)
        for (const float sample : source.channel(channel))
            if (!std::isfinite(sample))
                return false;
    return true;
}

std::optional<std::vector<float>> periodic_fourier_downsample(const std::vector<float>& cycle,
                                                              std::uint32_t table_length) {
    // Drop the destination Nyquist bin as well as every unrepresentable source
    // harmonic. The authored mip policy uses the same N/2-1 ceiling, and the
    // one-bin margin avoids placing a finite-filter transition at Nyquist.
    const std::uint64_t harmonics = table_length / 2u - 1u;
    const std::uint64_t frames = static_cast<std::uint64_t>(cycle.size());
    const std::uint64_t output_frames = table_length;
    const std::uint64_t synthesis_work = output_frames * harmonics;
    if (harmonics == 0 || synthesis_work > kMaximumFourierResampleWork ||
        frames > (kMaximumFourierResampleWork - synthesis_work) / (harmonics + 1u))
        return std::nullopt;

    constexpr double kTwoPi = 6.283185307179586476925286766559;
    std::vector<std::complex<double>> coefficients(static_cast<std::size_t>(harmonics + 1),
                                                   {0.0, 0.0});
    for (std::uint64_t harmonic = 0; harmonic <= harmonics; ++harmonic) {
        std::complex<double> coefficient{0.0, 0.0};
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            const double phase =
                -kTwoPi * static_cast<double>(harmonic * frame) / static_cast<double>(frames);
            coefficient += static_cast<double>(cycle[static_cast<std::size_t>(frame)]) *
                           std::complex<double>{std::cos(phase), std::sin(phase)};
        }
        coefficients[static_cast<std::size_t>(harmonic)] =
            coefficient / static_cast<double>(frames);
    }

    std::vector<float> table(table_length);
    for (std::uint64_t frame = 0; frame < output_frames; ++frame) {
        double sample = coefficients[0].real();
        for (std::uint64_t harmonic = 1; harmonic <= harmonics; ++harmonic) {
            const double phase =
                kTwoPi * static_cast<double>(harmonic * frame) / static_cast<double>(output_frames);
            sample += 2.0 * std::real(coefficients[static_cast<std::size_t>(harmonic)] *
                                      std::complex<double>{std::cos(phase), std::sin(phase)});
        }
        table[static_cast<std::size_t>(frame)] = static_cast<float>(sample);
    }
    return table;
}

std::optional<std::vector<float>> periodic_resample(const std::vector<float>& cycle,
                                                    std::uint32_t table_length) {
    if (cycle.size() > table_length)
        return periodic_fourier_downsample(cycle, table_length);

    signal::SincResampler resampler;
    resampler.build();
    std::vector<float> table(table_length);
    std::vector<float> neighborhood(static_cast<std::size_t>(resampler.taps()));
    const auto cycle_length = static_cast<std::int64_t>(cycle.size());
    for (std::uint32_t i = 0; i < table_length; ++i) {
        const double position = static_cast<double>(i) * cycle.size() / table_length;
        const auto center = static_cast<std::int64_t>(std::floor(position));
        const double fraction = position - std::floor(position);
        for (int tap = 0; tap < resampler.taps(); ++tap) {
            auto index = center + tap - resampler.half_width() + 1;
            index %= cycle_length;
            if (index < 0)
                index += cycle_length;
            neighborhood[static_cast<std::size_t>(tap)] = cycle[static_cast<std::size_t>(index)];
        }
        table[i] = resampler.apply(neighborhood.data(), fraction);
    }
    return table;
}

} // namespace

WavetableCompileResult compile_wavetable(BufferView<const float> source, double source_sample_rate,
                                         const WavetableAuthoringRecipe& recipe,
                                         const WavetableAuthoringProvenance& provenance) {
    WavetableCompileResult result;
    result.recipe = recipe;
    result.provenance = provenance;

    if (source.num_channels() != 1 || source.num_samples() == 0 ||
        source.num_samples() > kWavetableAuthoringMaximumSourceFrames ||
        !std::isfinite(source_sample_rate) || source_sample_rate < 8000.0 ||
        source_sample_rate > 384000.0 || !all_finite(source)) {
        result.status = WavetableCompileStatus::InvalidSource;
        return result;
    }
    if (!valid_recipe(recipe) || !valid_provenance(provenance)) {
        result.status = WavetableCompileStatus::InvalidRecipe;
        return result;
    }

    try {
        result.source_audio_sha256 = source_hash(source, source_sample_rate);
        const auto samples = source.channel(0);
        SeamMeasurement seam;
        if (recipe.explicit_cycle) {
            const auto start = recipe.explicit_cycle->start_frame;
            const auto length = recipe.explicit_cycle->length_frames;
            if (length == 0 || start >= samples.size() ||
                static_cast<std::uint64_t>(length) + 2 >
                    static_cast<std::uint64_t>(samples.size()) - start) {
                result.status = WavetableCompileStatus::InvalidExplicitCycle;
                return result;
            }
            long double energy = 0.0L;
            for (std::uint64_t frame = start; frame < start + length; ++frame) {
                const auto sample = static_cast<long double>(samples[frame]);
                energy += sample * sample;
            }
            seam = measure_seam(samples, start, length,
                                std::sqrt(static_cast<double>(energy / length)));
            result.chosen_cycle = *recipe.explicit_cycle;
            result.cycle_correlation = 0.0;
        } else {
            const auto analysis_end = recipe.automatic_cycle.analysis_end_frame == 0
                                          ? static_cast<std::uint64_t>(source.num_samples())
                                          : recipe.automatic_cycle.analysis_end_frame;
            const auto analysis_begin = recipe.automatic_cycle.analysis_start_frame;
            if (analysis_begin >= analysis_end || analysis_end > source.num_samples() ||
                analysis_end - analysis_begin > kWavetableAuthoringMaximumAnalysisFrames) {
                result.status = WavetableCompileStatus::InvalidRecipe;
                return result;
            }
            const auto lag_count =
                static_cast<std::uint64_t>(recipe.automatic_cycle.maximum_cycle_samples) -
                recipe.automatic_cycle.minimum_cycle_samples + 1;
            const auto analysis_frames = analysis_end - analysis_begin;
            if (lag_count != 0 &&
                analysis_frames > kWavetableAuthoringMaximumCycleSearchWork / lag_count) {
                result.status = WavetableCompileStatus::InvalidRecipe;
                return result;
            }
            const auto estimate =
                estimate_sample_heritage_auto_cycle(source, recipe.automatic_cycle);
            result.status = map_cycle_status(estimate.status);
            if (result.status != WavetableCompileStatus::Ok)
                return result;
            const auto selected =
                select_automatic_seam(samples, recipe.automatic_cycle, estimate.cycle_samples);
            if (!selected) {
                result.status = WavetableCompileStatus::InvalidRecipe;
                return result;
            }
            seam = *selected;
            result.chosen_cycle = {seam.start, estimate.cycle_samples};
            result.cycle_correlation = estimate.correlation;
        }
        result.source_seam_level_error = seam.level;
        result.source_seam_slope_error = seam.slope;
        if (!std::isfinite(seam.normalized) || seam.normalized > recipe.maximum_seam_error) {
            result.status = recipe.explicit_cycle ? WavetableCompileStatus::InvalidExplicitCycle
                                                  : WavetableCompileStatus::NoReliableCycle;
            return result;
        }

        const auto start = static_cast<std::size_t>(result.chosen_cycle.start_frame);
        const auto length = static_cast<std::size_t>(result.chosen_cycle.length_frames);
        std::vector<float> cycle(length);
        double sum = 0.0;
        for (std::size_t i = 0; i < length; ++i)
            sum += samples[start + i];
        const double mean = sum / static_cast<double>(length);
        for (std::size_t i = 0; i < length; ++i)
            cycle[i] = static_cast<float>(static_cast<double>(samples[start + i]) - mean);

        auto resampled = periodic_resample(cycle, recipe.table_length);
        if (!resampled) {
            result.status = WavetableCompileStatus::InvalidRecipe;
            return result;
        }
        auto base = std::move(*resampled);
        float peak = 0.0f;
        for (const float sample : base) {
            if (!std::isfinite(sample)) {
                result.status = WavetableCompileStatus::NonFiniteResult;
                return result;
            }
            peak = std::max(peak, std::abs(sample));
        }
        if (!(peak > 0.0f)) {
            result.status = WavetableCompileStatus::InsufficientSignal;
            return result;
        }
        const float target =
            static_cast<float>(std::pow(10.0, recipe.normalize_target_dbfs / 20.0));
        const float gain = target / peak;
        for (float& sample : base)
            sample *= gain;

        // Authoring is offline and its materialized hash is part of the public
        // content-addressing contract. Use the deterministic scalar backend on
        // every platform instead of the float specialization, whose Apple vDSP
        // path can produce different low-order samples across identical calls.
        signal::FftT<double> fft(static_cast<int>(recipe.table_length));
        if (!fft.ready()) {
            result.status = WavetableCompileStatus::FftUnavailable;
            return result;
        }
        std::vector<std::complex<double>> spectrum(recipe.table_length);
        for (std::size_t i = 0; i < base.size(); ++i)
            spectrum[i] = {base[i], 0.0f};
        fft.forward(spectrum.data());
        // DC removal is an output invariant even if finite sinc arithmetic left
        // a tiny residual after the source-cycle mean subtraction.
        spectrum[0] = {0.0f, 0.0f};

        const auto ceilings = signal::detail::build_wavetable_band_ceilings<float>(
            recipe.num_bands, static_cast<float>(recipe.reference_sample_rate));
        if (ceilings.size() != recipe.num_bands) {
            result.status = WavetableCompileStatus::InvalidRecipe;
            return result;
        }
        result.bands.reserve(recipe.num_bands);
        const double nyquist = recipe.reference_sample_rate * 0.5;
        const std::size_t representable = recipe.table_length / 2 - 1;
        for (const float ceiling : ceilings) {
            const auto safe_harmonics =
                static_cast<std::size_t>(std::floor(nyquist / static_cast<double>(ceiling)));
            const std::size_t guarded = safe_harmonics > recipe.guard_harmonics
                                            ? safe_harmonics - recipe.guard_harmonics
                                            : 1;
            const std::size_t maximum_harmonic =
                std::max<std::size_t>(1, std::min(representable, guarded));
            auto band_spectrum = spectrum;
            for (std::size_t k = maximum_harmonic + 1; k < band_spectrum.size() - maximum_harmonic;
                 ++k)
                band_spectrum[k] = {0.0f, 0.0f};
            fft.inverse(band_spectrum.data());
            signal::WavetableEntry band;
            band.max_frequency_hz = ceiling;
            band.samples.resize(recipe.table_length);
            for (std::size_t i = 0; i < band.samples.size(); ++i) {
                band.samples[i] = static_cast<float>(band_spectrum[i].real());
                if (!std::isfinite(band.samples[i])) {
                    result.bands.clear();
                    result.status = WavetableCompileStatus::NonFiniteResult;
                    return result;
                }
            }
            result.bands.push_back(std::move(band));
        }
        result.materialized_table_sha256 = materialized_hash(result);
        result.status = WavetableCompileStatus::Ok;
        return result;
    } catch (const std::bad_alloc&) {
        result.bands.clear();
        result.status = WavetableCompileStatus::AllocationFailed;
        return result;
    } catch (const std::length_error&) {
        result.bands.clear();
        result.status = WavetableCompileStatus::SizeOverflow;
        return result;
    }
}

} // namespace pulp::audio
