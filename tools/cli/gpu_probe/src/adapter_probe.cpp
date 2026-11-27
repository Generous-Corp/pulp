#include <pulp_tooling/gpu_probe/adapter_probe.hpp>

#include <cstdlib>
#include <string_view>

namespace pulp::tooling::gpu_adapter {
namespace {

std::string quote(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            out += "\\u00";
            out += hex[byte >> 4];
            out += hex[byte & 0x0f];
        } else {
            out += c;
        }
    }
    return out + '"';
}

const char* name(Verdict verdict) {
    switch (verdict) {
    case Verdict::pass:
        return "pass";
    case Verdict::fail:
        return "fail";
    case Verdict::unavailable:
        return "unavailable";
    }
    return "fail";
}

Result make(Verdict verdict, std::string reason, const Evidence& evidence) {
    Result result;
    result.verdict = verdict;
    result.reason = std::move(reason);
    result.evidence = evidence;
    return result;
}

} // namespace

Result classify(const Evidence& evidence, const Frame* frame) {
    if (!evidence.available)
        return make(Verdict::unavailable, "gpu-unavailable", evidence);
    if (evidence.fallback)
        return make(Verdict::fail, "software-fallback", evidence);
    if (evidence.adapter.empty())
        return make(Verdict::fail, "empty-adapter", evidence);
    if (evidence.renderer.empty() || evidence.backend.empty())
        return make(Verdict::fail, "empty-renderer", evidence);
    if (frame == nullptr || frame->width != kSceneWidth || frame->height != kSceneHeight ||
        frame->rgba.size() != std::size_t{kSceneWidth} * kSceneHeight * 4)
        return make(Verdict::fail, "no-frame", evidence);

    Result result = make(Verdict::pass, "ok", evidence);
    for (const auto& swatch : kSwatches) {
        const auto at = (std::size_t{swatch.y} * frame->width + swatch.x) * 4;
        const int dr = std::abs(int{frame->rgba[at]} - swatch.r);
        const int dg = std::abs(int{frame->rgba[at + 1]} - swatch.g);
        const int db = std::abs(int{frame->rgba[at + 2]} - swatch.b);
        ++result.swatches_checked;
        if (dr > kChannelTolerance || dg > kChannelTolerance || db > kChannelTolerance)
            ++result.swatches_mismatched;
    }
    if (result.swatches_mismatched != 0) {
        result.verdict = Verdict::fail;
        result.reason = "oracle-mismatch";
    }
    return result;
}

int exit_code(Verdict verdict) {
    switch (verdict) {
    case Verdict::pass:
        return 0;
    case Verdict::fail:
        return 1;
    case Verdict::unavailable:
        return 2;
    }
    return 1;
}

std::string to_json(const Result& result) {
    const auto& e = result.evidence;
    std::string out = "{\"schema\":\"pulp.gpu-adapter-probe.v1\",\"verdict\":";
    out += quote(name(result.verdict));
    out += ",\"reason\":" + quote(result.reason);
    out += ",\"evidence\":{\"available\":" + std::string(e.available ? "true" : "false");
    out += ",\"native_surface\":" + std::string(e.native_surface ? "true" : "false");
    out += ",\"fallback\":" + std::string(e.fallback ? "true" : "false");
    out += ",\"renderer\":" + quote(e.renderer);
    out += ",\"backend\":" + quote(e.backend);
    out += ",\"adapter\":" + quote(e.adapter);
    out += ",\"texture_format\":" + quote(e.texture_format) + "}";
    out += ",\"swatches_checked\":" + std::to_string(result.swatches_checked);
    out += ",\"swatches_mismatched\":" + std::to_string(result.swatches_mismatched) + "}";
    return out;
}

} // namespace pulp::tooling::gpu_adapter
