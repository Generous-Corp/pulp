#include <pulp/render/gpu_capabilities.hpp>

#include <array>

namespace pulp::render {

namespace {

struct FormatRow {
    GpuFormat format;
    std::string_view name;
    std::uint32_t bytes_per_pixel;
    bool depth;
};

// Order matches GpuFormat after `undefined`.
constexpr std::array<FormatRow, 20> kFormatRows{{
    {GpuFormat::r8_unorm, "r8unorm", 1, false},
    {GpuFormat::rg8_unorm, "rg8unorm", 2, false},
    {GpuFormat::rgba8_unorm, "rgba8unorm", 4, false},
    {GpuFormat::rgba8_unorm_srgb, "rgba8unorm-srgb", 4, false},
    {GpuFormat::bgra8_unorm, "bgra8unorm", 4, false},
    {GpuFormat::bgra8_unorm_srgb, "bgra8unorm-srgb", 4, false},
    {GpuFormat::rgb10a2_unorm, "rgb10a2unorm", 4, false},
    {GpuFormat::rg11b10_ufloat, "rg11b10ufloat", 4, false},
    {GpuFormat::r16_float, "r16float", 2, false},
    {GpuFormat::rg16_float, "rg16float", 4, false},
    {GpuFormat::rgba16_float, "rgba16float", 8, false},
    {GpuFormat::r32_float, "r32float", 4, false},
    {GpuFormat::rg32_float, "rg32float", 8, false},
    {GpuFormat::rgba32_float, "rgba32float", 16, false},
    {GpuFormat::r32_uint, "r32uint", 4, false},
    {GpuFormat::rgba8_uint, "rgba8uint", 4, false},
    {GpuFormat::depth16_unorm, "depth16unorm", 0, true},
    {GpuFormat::depth24_plus, "depth24plus", 0, true},
    {GpuFormat::depth24_plus_stencil8, "depth24plus-stencil8", 0, true},
    {GpuFormat::depth32_float, "depth32float", 0, true},
}};

const FormatRow* find_row(GpuFormat format) {
    for (const auto& row : kFormatRows) {
        if (row.format == format)
            return &row;
    }
    return nullptr;
}

} // namespace

const std::vector<GpuFormat>& all_gpu_formats() {
    static const std::vector<GpuFormat> formats = [] {
        std::vector<GpuFormat> out;
        out.reserve(kFormatRows.size());
        for (const auto& row : kFormatRows)
            out.push_back(row.format);
        return out;
    }();
    return formats;
}

std::string_view gpu_format_name(GpuFormat format) {
    const auto* row = find_row(format);
    return row ? row->name : std::string_view("undefined");
}

std::uint32_t gpu_format_bytes_per_pixel(GpuFormat format) {
    const auto* row = find_row(format);
    return row ? row->bytes_per_pixel : 0u;
}

bool gpu_format_is_depth(GpuFormat format) {
    const auto* row = find_row(format);
    return row != nullptr && row->depth;
}

// The WebGPU specification's texture format capability tables, plus the
// optional features that extend them.
GpuFormatSupport gpu_format_support(GpuFormat format, const GpuDeviceFeatures& features) {
    GpuFormatSupport s;
    s.format = format;
    switch (format) {
    case GpuFormat::r8_unorm:
    case GpuFormat::rg8_unorm:
    case GpuFormat::rgba8_unorm_srgb:
    case GpuFormat::bgra8_unorm_srgb:
    case GpuFormat::rgb10a2_unorm:
    case GpuFormat::r16_float:
    case GpuFormat::rg16_float:
        s.renderable = s.blendable = s.multisample = s.filterable = true;
        break;
    case GpuFormat::rgba8_unorm:
    case GpuFormat::rgba16_float:
        s.renderable = s.blendable = s.multisample = s.filterable = true;
        s.storage = true;
        break;
    case GpuFormat::bgra8_unorm:
        s.renderable = s.blendable = s.multisample = s.filterable = true;
        s.storage = features.bgra8_unorm_storage;
        break;
    case GpuFormat::rg11b10_ufloat:
        s.filterable = true;
        s.renderable = s.blendable = s.multisample = features.rg11b10_ufloat_renderable;
        break;
    case GpuFormat::r32_float:
        s.renderable = s.multisample = s.storage = true;
        s.blendable = features.float32_blendable;
        s.filterable = features.float32_filterable;
        break;
    case GpuFormat::rg32_float:
    case GpuFormat::rgba32_float:
        s.renderable = s.storage = true;
        s.blendable = features.float32_blendable;
        s.filterable = features.float32_filterable;
        break;
    case GpuFormat::r32_uint:
    case GpuFormat::rgba8_uint:
        s.renderable = s.multisample = s.storage = true;
        break;
    case GpuFormat::depth16_unorm:
    case GpuFormat::depth24_plus:
    case GpuFormat::depth24_plus_stencil8:
    case GpuFormat::depth32_float:
        s.renderable = s.multisample = true;
        break;
    case GpuFormat::undefined:
        break;
    }
    return s;
}

void fill_format_table(GpuDeviceCapabilities& caps) {
    caps.formats.clear();
    caps.formats.reserve(kFormatRows.size());
    for (const auto& row : kFormatRows)
        caps.formats.push_back(gpu_format_support(row.format, caps.facts.features));
}

GpuFormatSupport GpuDeviceCapabilities::support(GpuFormat format) const {
    for (const auto& entry : formats) {
        if (entry.format == format)
            return entry;
    }
    GpuFormatSupport none;
    none.format = format;
    return none;
}

std::vector<GpuFormat> GpuDeviceCapabilities::renderable_color_formats() const {
    std::vector<GpuFormat> out;
    for (const auto& entry : formats) {
        if (entry.renderable && !gpu_format_is_depth(entry.format))
            out.push_back(entry.format);
    }
    return out;
}

std::vector<GpuFormat> GpuDeviceCapabilities::storage_formats() const {
    std::vector<GpuFormat> out;
    for (const auto& entry : formats) {
        if (entry.storage)
            out.push_back(entry.format);
    }
    return out;
}

std::vector<std::uint32_t> GpuDeviceCapabilities::sample_counts(GpuFormat format) const {
    const auto s = support(format);
    if (!s.renderable)
        return {};
    if (s.multisample)
        return {1u, 4u};
    return {1u};
}

std::string_view gpu_render_tier_name(GpuRenderTier tier) {
    switch (tier) {
    case GpuRenderTier::none:
        return "none";
    case GpuRenderTier::basic:
        return "basic";
    case GpuRenderTier::standard:
        return "standard";
    case GpuRenderTier::high:
        return "high";
    }
    return "none";
}

GpuRenderTier pick_tier(const GpuDeviceFacts& facts) {
    if (!facts.available || facts.null_backend)
        return GpuRenderTier::none;
    if (facts.adapter == GpuAdapterKind::cpu)
        return GpuRenderTier::basic;

    const bool meets_standard = facts.max_color_attachments >= 4 &&
                                facts.max_storage_textures_per_shader_stage >= 4 &&
                                facts.max_compute_invocations_per_workgroup >= 256 &&
                                facts.max_texture_dimension_2d >= 8192;
    if (!meets_standard)
        return GpuRenderTier::basic;

    const bool large_limits = facts.max_texture_dimension_2d >= 16384 &&
                              facts.max_color_attachments >= 8 && facts.features.float32_blendable;
    if (facts.adapter == GpuAdapterKind::discrete || large_limits)
        return GpuRenderTier::high;
    return GpuRenderTier::standard;
}

} // namespace pulp::render
