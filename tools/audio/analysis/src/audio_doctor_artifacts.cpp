// audio_doctor_artifacts.cpp — JSON curve artifacts for the Audio Doctor
// See audio_doctor_artifacts.hpp for the schema contract; mirrors the
// audio_artifacts.cpp CHOC-first style.

#include <pulp/audio/analysis/audio_doctor_artifacts.hpp>

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <fstream>

namespace pulp::test::audio {

namespace {

// Common header: schema version, analyzer, provenance, and the determinism
// fields a reader needs to judge or reproduce the measurement.
void add_contract_fields(choc::value::Value& root, std::string_view analyzer,
                         std::string_view scenario, std::string_view stimulus,
                         Window window, int fft_length, int analysis_offset,
                         double sample_rate, double bin_hz) {
    root.setMember("schema_version", kDoctorCurveSchemaVersion);
    root.setMember("analyzer", std::string(analyzer));
    root.setMember("scenario", std::string(scenario));
    root.setMember("stimulus", std::string(stimulus));
    root.setMember("window", window_name(window));
    root.setMember("fft_length", static_cast<std::int64_t>(fft_length));
    root.setMember("analysis_offset",
                   static_cast<std::int64_t>(analysis_offset));
    root.setMember("sample_rate", sample_rate);
    root.setMember("bin_hz", bin_hz);
}

std::filesystem::path artifact_dir() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pulp-audio-doctor";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // best-effort; open() reports
    return dir;
}

std::string sanitize(std::string_view scenario) {
    std::string name;
    name.reserve(scenario.size());
    for (char c : scenario)
        name += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                 c == '_' || c == '-')
                    ? c
                    : '-';
    return name.empty() ? std::string("scenario") : name;
}

std::filesystem::path write(const std::string& json, std::string_view scenario,
                            const char* suffix) {
    const auto path = artifact_dir() / (sanitize(scenario) + suffix);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
    out.flush();
    // Return an EMPTY path on any write failure so a caller never advertises an
    // "Artifact:" line for a file that was not actually written.
    if (!out)
        return {};
    return path;
}

} // namespace

std::string response_curve_to_json(const ResponseCurve& curve,
                                   std::string_view scenario) {
    auto points = choc::value::createEmptyArray();
    for (const auto& p : curve.full) {
        auto pt = choc::value::createObject("ResponsePoint");
        pt.setMember("hz", p.hz);
        pt.setMember("magnitude_db", p.magnitude_db);
        points.addArrayElement(pt);
    }
    auto checkpoints = choc::value::createEmptyArray();
    for (const auto& p : curve.checkpoints) {
        auto pt = choc::value::createObject("ResponsePoint");
        pt.setMember("hz", p.hz);
        pt.setMember("magnitude_db", p.magnitude_db);
        checkpoints.addArrayElement(pt);
    }

    auto root = choc::value::createObject("AudioDoctorResponseCurve");
    add_contract_fields(root, curve.analyzer, scenario, curve.stimulus,
                        curve.window, curve.fft_length, curve.analysis_offset,
                        curve.sample_rate, curve.bin_hz);
    root.setMember("checkpoints", checkpoints);
    root.setMember("curve", points);
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

std::string thd_to_json(const ThdResult& thd, std::string_view scenario) {
    auto harmonics = choc::value::createEmptyArray();
    for (const auto& h : thd.harmonics) {
        auto hv = choc::value::createObject("Harmonic");
        hv.setMember("index", static_cast<std::int64_t>(h.index));
        hv.setMember("hz", h.hz);
        hv.setMember("magnitude", h.magnitude);
        hv.setMember("db_below_fundamental", h.db_below_fundamental);
        harmonics.addArrayElement(hv);
    }

    auto root = choc::value::createObject("AudioDoctorThd");
    add_contract_fields(root, thd.analyzer, scenario, "sine", thd.window,
                        thd.fft_length, thd.analysis_offset, thd.sample_rate,
                        thd.bin_hz);
    root.setMember("fundamental_hz", thd.fundamental_hz);
    root.setMember("coherent", thd.coherent);
    root.setMember("num_harmonics", static_cast<std::int64_t>(thd.num_harmonics));
    root.setMember("thd", thd.thd);
    root.setMember("thd_db", thd.thd_db());
    root.setMember("thd_percent", thd.thd_percent());
    root.setMember("thd_plus_n", thd.thd_plus_n);
    root.setMember("thd_plus_n_db", thd.thd_plus_n_db());
    root.setMember("harmonics", harmonics);
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

std::string phase_curve_to_json(const PhaseCurve& curve,
                                std::string_view scenario) {
    // Undefined bins omit the phase/delay members rather than emit a
    // placeholder — see the header's schema note.
    const auto point_value = [](const PhasePoint& p, double sample_rate) {
        auto pt = choc::value::createObject("PhasePoint");
        pt.setMember("hz", p.hz);
        // Deliberately NOT "magnitude_db": this is peak-relative, while the
        // sibling .response.json's "magnitude_db" is the absolute out/in
        // transfer ratio. The key carries the difference so a reader holding
        // both artifacts cannot conflate them.
        pt.setMember("magnitude_db_rel_peak", p.magnitude_db_rel_peak);
        pt.setMember("defined", p.defined);
        if (p.defined) {
            pt.setMember("phase_rad", p.phase_rad);
            pt.setMember("group_delay_samples", p.group_delay_samples);
            if (sample_rate > 0.0)
                pt.setMember("group_delay_seconds",
                             p.group_delay_samples / sample_rate);
        }
        return pt;
    };

    auto points = choc::value::createEmptyArray();
    for (const auto& p : curve.full)
        points.addArrayElement(point_value(p, curve.sample_rate));
    auto checkpoints = choc::value::createEmptyArray();
    for (const auto& p : curve.checkpoints)
        checkpoints.addArrayElement(point_value(p, curve.sample_rate));

    auto root = choc::value::createObject("AudioDoctorPhaseCurve");
    add_contract_fields(root, curve.analyzer, scenario, curve.stimulus,
                        curve.window, curve.fft_length, curve.analysis_offset,
                        curve.sample_rate, curve.bin_hz);
    root.setMember("magnitude_floor_db", curve.magnitude_floor_db);
    root.setMember("checkpoints", checkpoints);
    root.setMember("curve", points);
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

std::filesystem::path write_response_artifact(const ResponseCurve& curve,
                                              std::string_view scenario) {
    return write(response_curve_to_json(curve, scenario), scenario,
                 ".response.json");
}

std::filesystem::path write_thd_artifact(const ThdResult& thd,
                                         std::string_view scenario) {
    return write(thd_to_json(thd, scenario), scenario, ".thd.json");
}

std::filesystem::path write_phase_artifact(const PhaseCurve& curve,
                                           std::string_view scenario) {
    return write(phase_curve_to_json(curve, scenario), scenario,
                 ".groupdelay.json");
}

} // namespace pulp::test::audio
