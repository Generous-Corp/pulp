#include "gpu_pass_internal.hpp"

#include <pulp/canvas/canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/runtime/log.hpp>

#if PULP_GPU_PASS_HAS_DAWN
#include <pulp/canvas/skia_canvas.hpp>

#include "gpu_readback.hpp"
#include "wgsl_compile.hpp"
#endif

#include <algorithm>
#include <thread>
#include <utility>

namespace pulp::render {

using runtime::Err;
using runtime::Ok;

// ── Errors ───────────────────────────────────────────────────────────────

std::string_view gpu_error_kind_name(GpuError::Kind kind) {
    switch (kind) {
    case GpuError::Kind::unavailable:
        return "unavailable";
    case GpuError::Kind::invalid_argument:
        return "invalid_argument";
    case GpuError::Kind::shader_compilation:
        return "shader_compilation";
    case GpuError::Kind::validation:
        return "validation";
    case GpuError::Kind::out_of_memory:
        return "out_of_memory";
    case GpuError::Kind::internal:
        return "internal";
    case GpuError::Kind::device_lost:
        return "device_lost";
    case GpuError::Kind::timeout:
        return "timeout";
    }
    return "internal";
}

std::string GpuError::describe() const {
    std::string out(gpu_error_kind_name(kind));
    out += ": ";
    out += message;
    for (const auto& diagnostic : diagnostics) {
        out += '\n';
        out += format_wgsl_diagnostic({}, diagnostic);
    }
    return out;
}

namespace {

GpuError make_error(GpuError::Kind kind, std::string message) {
    GpuError error;
    error.kind = kind;
    error.message = std::move(message);
    return error;
}

} // namespace

// ── Small handle accessors (shared by both builds) ───────────────────────

bool GpuTexture::valid() const noexcept {
    return state_ != nullptr;
}
std::uint32_t GpuTexture::width() const noexcept {
    return state_ ? state_->desc.width : 0u;
}
std::uint32_t GpuTexture::height() const noexcept {
    return state_ ? state_->desc.height : 0u;
}
GpuFormat GpuTexture::format() const noexcept {
    return state_ ? state_->desc.format : GpuFormat::undefined;
}
GpuTextureUsage GpuTexture::usage() const noexcept {
    return state_ ? state_->desc.usage : GpuTextureUsage::none;
}
std::uint32_t GpuTexture::sample_count() const noexcept {
    return state_ ? state_->desc.sample_count : 0u;
}

bool GpuBuffer::valid() const noexcept {
    return state_ != nullptr;
}
std::uint64_t GpuBuffer::size() const noexcept {
    return state_ ? state_->desc.size : 0u;
}
GpuBufferUsage GpuBuffer::usage() const noexcept {
    return state_ ? state_->desc.usage : GpuBufferUsage::none;
}

bool GpuSampler::valid() const noexcept {
    return state_ != nullptr;
}

bool GpuShaderModule::valid() const noexcept {
    return state_ != nullptr;
}
const std::vector<WgslDiagnostic>& GpuShaderModule::warnings() const noexcept {
    static const std::vector<WgslDiagnostic> kNone;
    return state_ ? state_->warnings : kNone;
}

bool GpuRenderPipeline::valid() const noexcept {
    return state_ != nullptr;
}
bool GpuComputePipeline::valid() const noexcept {
    return state_ != nullptr;
}
bool GpuBindGroup::valid() const noexcept {
    return state_ != nullptr;
}

bool GpuTarget::valid() const noexcept {
    return state_ != nullptr;
}
std::uint32_t GpuTarget::width() const noexcept {
    return state_ ? state_->color.width() : 0u;
}
std::uint32_t GpuTarget::height() const noexcept {
    return state_ ? state_->color.height() : 0u;
}
GpuTexture GpuTarget::color() const {
    return state_ ? state_->color : GpuTexture{};
}
std::optional<GpuTexture> GpuTarget::depth() const {
    return state_ ? state_->depth : std::nullopt;
}

bool GpuDevice::valid() const noexcept {
    return state_ != nullptr;
}

const GpuDeviceCapabilities& GpuDevice::capabilities() const {
    static const GpuDeviceCapabilities kNone = [] {
        GpuDeviceCapabilities caps;
        fill_format_table(caps);
        return caps;
    }();
    return state_ ? state_->capabilities : kNone;
}

GpuBinding GpuBinding::of(std::uint32_t binding, GpuBuffer buffer, std::uint64_t offset,
                          std::uint64_t size) {
    GpuBinding b;
    b.binding = binding;
    b.kind = Kind::buffer;
    b.buffer = std::move(buffer);
    b.offset = offset;
    b.size = size;
    return b;
}

GpuBinding GpuBinding::of(std::uint32_t binding, GpuTexture texture) {
    GpuBinding b;
    b.binding = binding;
    b.kind = Kind::texture;
    b.texture = std::move(texture);
    return b;
}

GpuBinding GpuBinding::of(std::uint32_t binding, GpuSampler sampler) {
    GpuBinding b;
    b.binding = binding;
    b.kind = Kind::sampler;
    b.sampler = std::move(sampler);
    return b;
}

GpuRenderPassDesc GpuRenderPassDesc::for_target(const GpuTarget& target,
                                                std::optional<std::array<float, 4>> clear) {
    GpuRenderPassDesc desc;
    GpuColorAttachment color;
    color.texture = target.color();
    color.clear = clear;
    desc.colors.push_back(std::move(color));
    desc.depth = target.depth();
    desc.depth_clear = 1.0f;
    return desc;
}

std::array<std::uint8_t, 4> GpuPixels::rgba8_at(std::uint32_t x, std::uint32_t y) const {
    std::array<std::uint8_t, 4> out{0, 0, 0, 0};
    if (x >= width || y >= height || bytes_per_pixel != 4)
        return out;
    const std::size_t index =
        (static_cast<std::size_t>(y) * width + x) * static_cast<std::size_t>(bytes_per_pixel);
    if (index + 3 >= bytes.size())
        return out;
    switch (format) {
    case GpuFormat::rgba8_unorm:
    case GpuFormat::rgba8_unorm_srgb:
        return {bytes[index], bytes[index + 1], bytes[index + 2], bytes[index + 3]};
    case GpuFormat::bgra8_unorm:
    case GpuFormat::bgra8_unorm_srgb:
        return {bytes[index + 2], bytes[index + 1], bytes[index], bytes[index + 3]};
    default:
        return out;
    }
}

#if PULP_GPU_PASS_HAS_DAWN

// ── Dawn-backed implementation ───────────────────────────────────────────

namespace {

using detail::GpuAccess;

constexpr auto kScopeTimeout = std::chrono::milliseconds(2000);

wgpu::TextureFormat to_wgpu(GpuFormat format) {
    switch (format) {
    case GpuFormat::r8_unorm:
        return wgpu::TextureFormat::R8Unorm;
    case GpuFormat::rg8_unorm:
        return wgpu::TextureFormat::RG8Unorm;
    case GpuFormat::rgba8_unorm:
        return wgpu::TextureFormat::RGBA8Unorm;
    case GpuFormat::rgba8_unorm_srgb:
        return wgpu::TextureFormat::RGBA8UnormSrgb;
    case GpuFormat::bgra8_unorm:
        return wgpu::TextureFormat::BGRA8Unorm;
    case GpuFormat::bgra8_unorm_srgb:
        return wgpu::TextureFormat::BGRA8UnormSrgb;
    case GpuFormat::rgb10a2_unorm:
        return wgpu::TextureFormat::RGB10A2Unorm;
    case GpuFormat::rg11b10_ufloat:
        return wgpu::TextureFormat::RG11B10Ufloat;
    case GpuFormat::r16_float:
        return wgpu::TextureFormat::R16Float;
    case GpuFormat::rg16_float:
        return wgpu::TextureFormat::RG16Float;
    case GpuFormat::rgba16_float:
        return wgpu::TextureFormat::RGBA16Float;
    case GpuFormat::r32_float:
        return wgpu::TextureFormat::R32Float;
    case GpuFormat::rg32_float:
        return wgpu::TextureFormat::RG32Float;
    case GpuFormat::rgba32_float:
        return wgpu::TextureFormat::RGBA32Float;
    case GpuFormat::r32_uint:
        return wgpu::TextureFormat::R32Uint;
    case GpuFormat::rgba8_uint:
        return wgpu::TextureFormat::RGBA8Uint;
    case GpuFormat::depth16_unorm:
        return wgpu::TextureFormat::Depth16Unorm;
    case GpuFormat::depth24_plus:
        return wgpu::TextureFormat::Depth24Plus;
    case GpuFormat::depth24_plus_stencil8:
        return wgpu::TextureFormat::Depth24PlusStencil8;
    case GpuFormat::depth32_float:
        return wgpu::TextureFormat::Depth32Float;
    case GpuFormat::undefined:
        break;
    }
    return wgpu::TextureFormat::Undefined;
}

wgpu::VertexFormat to_wgpu(GpuVertexFormat format) {
    switch (format) {
    case GpuVertexFormat::float32:
        return wgpu::VertexFormat::Float32;
    case GpuVertexFormat::float32x2:
        return wgpu::VertexFormat::Float32x2;
    case GpuVertexFormat::float32x3:
        return wgpu::VertexFormat::Float32x3;
    case GpuVertexFormat::float32x4:
        return wgpu::VertexFormat::Float32x4;
    case GpuVertexFormat::uint32:
        return wgpu::VertexFormat::Uint32;
    case GpuVertexFormat::sint32:
        return wgpu::VertexFormat::Sint32;
    case GpuVertexFormat::unorm8x4:
        return wgpu::VertexFormat::Unorm8x4;
    }
    return wgpu::VertexFormat::Float32;
}

wgpu::PrimitiveTopology to_wgpu(GpuPrimitiveTopology topology) {
    switch (topology) {
    case GpuPrimitiveTopology::triangle_list:
        return wgpu::PrimitiveTopology::TriangleList;
    case GpuPrimitiveTopology::triangle_strip:
        return wgpu::PrimitiveTopology::TriangleStrip;
    case GpuPrimitiveTopology::line_list:
        return wgpu::PrimitiveTopology::LineList;
    case GpuPrimitiveTopology::line_strip:
        return wgpu::PrimitiveTopology::LineStrip;
    case GpuPrimitiveTopology::point_list:
        return wgpu::PrimitiveTopology::PointList;
    }
    return wgpu::PrimitiveTopology::TriangleList;
}

wgpu::CullMode to_wgpu(GpuCullMode cull) {
    switch (cull) {
    case GpuCullMode::none:
        return wgpu::CullMode::None;
    case GpuCullMode::front:
        return wgpu::CullMode::Front;
    case GpuCullMode::back:
        return wgpu::CullMode::Back;
    }
    return wgpu::CullMode::None;
}

wgpu::CompareFunction to_wgpu(GpuCompare compare) {
    switch (compare) {
    case GpuCompare::never:
        return wgpu::CompareFunction::Never;
    case GpuCompare::less:
        return wgpu::CompareFunction::Less;
    case GpuCompare::less_equal:
        return wgpu::CompareFunction::LessEqual;
    case GpuCompare::greater:
        return wgpu::CompareFunction::Greater;
    case GpuCompare::greater_equal:
        return wgpu::CompareFunction::GreaterEqual;
    case GpuCompare::equal:
        return wgpu::CompareFunction::Equal;
    case GpuCompare::not_equal:
        return wgpu::CompareFunction::NotEqual;
    case GpuCompare::always:
        return wgpu::CompareFunction::Always;
    }
    return wgpu::CompareFunction::Less;
}

std::optional<wgpu::BlendState> to_wgpu(GpuBlend blend) {
    wgpu::BlendState state{};
    switch (blend) {
    case GpuBlend::replace:
        return std::nullopt;
    case GpuBlend::alpha:
        state.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha,
                       wgpu::BlendFactor::OneMinusSrcAlpha};
        state.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One,
                       wgpu::BlendFactor::OneMinusSrcAlpha};
        return state;
    case GpuBlend::premultiplied:
        state.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One,
                       wgpu::BlendFactor::OneMinusSrcAlpha};
        state.alpha = state.color;
        return state;
    case GpuBlend::additive:
        state.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::One};
        state.alpha = state.color;
        return state;
    }
    return std::nullopt;
}

wgpu::StringView view_of(const std::string& text) {
    return wgpu::StringView(text.data(), text.size());
}

std::string to_string(wgpu::StringView view) {
    if (view.data == nullptr)
        return {};
    if (view.length == wgpu::kStrlen)
        return std::string(view.data);
    return std::string(view.data, view.length);
}

GpuError::Kind kind_of(wgpu::ErrorType type) {
    switch (type) {
    case wgpu::ErrorType::Validation:
        return GpuError::Kind::validation;
    case wgpu::ErrorType::OutOfMemory:
        return GpuError::Kind::out_of_memory;
    default:
        return GpuError::Kind::internal;
    }
}

// Collects the three error scopes a scoped operation pushes (validation,
// out-of-memory, internal). Callbacks may arrive on any thread that
// processes the instance's events, so the record is locked.
struct ScopeResults {
    std::mutex mutex;
    int remaining = 3;
    std::optional<GpuError> error;

    void record(wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
        std::lock_guard<std::mutex> lock(mutex);
        --remaining;
        if (error)
            return;
        if (status == wgpu::PopErrorScopeStatus::CallbackCancelled) {
            error = make_error(GpuError::Kind::device_lost,
                               "the Dawn instance shut down before the operation finished");
        } else if (status != wgpu::PopErrorScopeStatus::Success) {
            error = make_error(GpuError::Kind::internal,
                               "error scope could not be popped: " + to_string(message));
        } else if (type != wgpu::ErrorType::NoError) {
            error = make_error(kind_of(type), to_string(message));
        }
    }
    bool done() {
        std::lock_guard<std::mutex> lock(mutex);
        return remaining <= 0;
    }
};

void push_scopes(const wgpu::Device& device) {
    device.PushErrorScope(wgpu::ErrorFilter::Internal);
    device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
}

std::shared_ptr<ScopeResults> pop_scopes(const wgpu::Device& device) {
    auto results = std::make_shared<ScopeResults>();
    for (int i = 0; i < 3; ++i) {
        device.PopErrorScope(
            wgpu::CallbackMode::AllowProcessEvents,
            [results](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type,
                      wgpu::StringView message) { results->record(status, type, message); });
    }
    return results;
}

template <typename Pred>
bool pump_until(const wgpu::Instance& instance, Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        instance.ProcessEvents();
        if (pred())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Wait for popped scopes and turn them into a status.
GpuStatus finish_scopes(const detail::GpuDeviceState& device,
                        const std::shared_ptr<ScopeResults>& results) {
    if (!pump_until(device.instance, [&] { return results->done(); }, kScopeTimeout))
        return Err(make_error(GpuError::Kind::timeout, "error scopes did not resolve"));
    if (device.is_lost())
        return Err(make_error(GpuError::Kind::device_lost, "the GPU device was lost"));
    std::lock_guard<std::mutex> lock(results->mutex);
    if (results->error)
        return Err(*results->error);
    return {};
}

// Run `fn` (which creates Dawn objects) inside error scopes and report what
// the scopes caught.
template <typename Fn> GpuStatus run_scoped(const detail::GpuDeviceState& device, Fn&& fn) {
    push_scopes(device.device);
    fn();
    return finish_scopes(device, pop_scopes(device.device));
}

GpuStatus check_device(const std::shared_ptr<detail::GpuDeviceState>& device) {
    if (!device)
        return Err(make_error(GpuError::Kind::unavailable, "GpuDevice is not initialized"));
    if (device->is_lost())
        return Err(make_error(GpuError::Kind::device_lost, "the GPU device was lost"));
    return {};
}

template <typename Handle>
bool same_device(const Handle& handle, const std::shared_ptr<detail::GpuDeviceState>& device) {
    const auto& state = GpuAccess::state(handle);
    return state && state->device == device;
}

GpuAdapterKind adapter_kind_of(wgpu::AdapterType type) {
    switch (type) {
    case wgpu::AdapterType::DiscreteGPU:
        return GpuAdapterKind::discrete;
    case wgpu::AdapterType::IntegratedGPU:
        return GpuAdapterKind::integrated;
    case wgpu::AdapterType::CPU:
        return GpuAdapterKind::cpu;
    default:
        return GpuAdapterKind::unknown;
    }
}

GpuDeviceCapabilities read_capabilities(const wgpu::Device& device) {
    GpuDeviceCapabilities caps;
    auto& facts = caps.facts;
    facts.available = true;

    wgpu::AdapterInfo info{};
    if (device.GetAdapterInfo(&info) == wgpu::Status::Success) {
        facts.null_backend = info.backendType == wgpu::BackendType::Null;
        facts.adapter = adapter_kind_of(info.adapterType);
    }

    wgpu::Limits limits{};
    if (device.GetLimits(&limits) == wgpu::Status::Success) {
        facts.max_texture_dimension_2d = limits.maxTextureDimension2D;
        facts.max_color_attachments = limits.maxColorAttachments;
        facts.max_storage_textures_per_shader_stage = limits.maxStorageTexturesPerShaderStage;
        facts.max_compute_invocations_per_workgroup = limits.maxComputeInvocationsPerWorkgroup;
        facts.max_buffer_size = limits.maxBufferSize;
        caps.max_texture_dimension_1d = limits.maxTextureDimension1D;
        caps.max_texture_dimension_3d = limits.maxTextureDimension3D;
        caps.max_texture_array_layers = limits.maxTextureArrayLayers;
        caps.max_bind_groups = limits.maxBindGroups;
        caps.max_bindings_per_bind_group = limits.maxBindingsPerBindGroup;
        caps.max_sampled_textures_per_shader_stage = limits.maxSampledTexturesPerShaderStage;
        caps.max_samplers_per_shader_stage = limits.maxSamplersPerShaderStage;
        caps.max_uniform_buffers_per_shader_stage = limits.maxUniformBuffersPerShaderStage;
        caps.max_storage_buffers_per_shader_stage = limits.maxStorageBuffersPerShaderStage;
        caps.max_uniform_buffer_binding_size = limits.maxUniformBufferBindingSize;
        caps.max_storage_buffer_binding_size = limits.maxStorageBufferBindingSize;
        caps.max_vertex_buffers = limits.maxVertexBuffers;
        caps.max_vertex_attributes = limits.maxVertexAttributes;
        caps.max_color_attachment_bytes_per_sample = limits.maxColorAttachmentBytesPerSample;
        caps.max_compute_workgroup_size_x = limits.maxComputeWorkgroupSizeX;
        caps.max_compute_workgroup_size_y = limits.maxComputeWorkgroupSizeY;
        caps.max_compute_workgroup_size_z = limits.maxComputeWorkgroupSizeZ;
        caps.max_compute_workgroups_per_dimension = limits.maxComputeWorkgroupsPerDimension;
        caps.min_uniform_buffer_offset_alignment = limits.minUniformBufferOffsetAlignment;
        caps.min_storage_buffer_offset_alignment = limits.minStorageBufferOffsetAlignment;
    }

    auto& features = facts.features;
    features.rg11b10_ufloat_renderable =
        device.HasFeature(wgpu::FeatureName::RG11B10UfloatRenderable);
    features.float32_filterable = device.HasFeature(wgpu::FeatureName::Float32Filterable);
    features.float32_blendable = device.HasFeature(wgpu::FeatureName::Float32Blendable);
    features.bgra8_unorm_storage = device.HasFeature(wgpu::FeatureName::BGRA8UnormStorage);
    features.timestamp_query = device.HasFeature(wgpu::FeatureName::TimestampQuery);
    features.shader_f16 = device.HasFeature(wgpu::FeatureName::ShaderF16);
    fill_format_table(caps);
    return caps;
}

GpuStatus validate_texture_desc(const GpuTextureDesc& desc, const GpuDeviceCapabilities& caps) {
    auto invalid = [](std::string message) {
        return Err(make_error(GpuError::Kind::invalid_argument, std::move(message)));
    };
    if (desc.width == 0 || desc.height == 0)
        return invalid("texture size must be non-zero");
    const auto max_dim = caps.facts.max_texture_dimension_2d;
    if (max_dim != 0 && (desc.width > max_dim || desc.height > max_dim))
        return invalid("texture size exceeds the device's maxTextureDimension2D (" +
                       std::to_string(max_dim) + ")");
    if (desc.format == GpuFormat::undefined)
        return invalid("texture format is undefined");
    if (desc.usage == GpuTextureUsage::none)
        return invalid("texture usage is empty");
    const auto support = caps.support(desc.format);
    if (has_usage(desc.usage, GpuTextureUsage::render_target) && !support.renderable)
        return invalid(std::string(gpu_format_name(desc.format)) +
                       " is not renderable on this device");
    if (has_usage(desc.usage, GpuTextureUsage::storage) && !support.storage)
        return invalid(std::string(gpu_format_name(desc.format)) +
                       " cannot be a storage texture on this device");
    if (desc.sample_count != 1 && desc.sample_count != 4)
        return invalid("sample_count must be 1 or 4");
    if (desc.sample_count == 4) {
        if (!support.multisample)
            return invalid(std::string(gpu_format_name(desc.format)) +
                           " does not support multisampling");
        if (!has_usage(desc.usage, GpuTextureUsage::render_target))
            return invalid("a multisampled texture must be a render target");
        if (has_usage(desc.usage, GpuTextureUsage::storage))
            return invalid("a multisampled texture cannot be a storage texture");
    }
    return {};
}

} // namespace

namespace detail {

bool GpuDeviceState::is_lost() const {
    if (lost || !device)
        return true;
    if (!has_lost_future)
        return false;
    wgpu::FutureWaitInfo wait{};
    wait.future = lost_future;
    if (instance.WaitAny(1, &wait, 0) == wgpu::WaitStatus::Success && wait.completed)
        lost = true;
    return lost;
}

void GpuFrameState::record_error(GpuError::Kind kind, std::string message) {
    if (!error)
        error = make_error(kind, std::move(message));
}

void GpuFrameState::end_open_pass() {
    if (render_pass) {
        render_pass.End();
        render_pass = nullptr;
    }
    if (compute_pass) {
        compute_pass.End();
        compute_pass = nullptr;
    }
    open_pass = 0;
}

void GpuFrameState::release_writes() {
    for (auto& texture : writes) {
        if (texture && texture->unsubmitted_writes > 0)
            --texture->unsubmitted_writes;
    }
    writes.clear();
}

} // namespace detail

// ── GpuDevice ────────────────────────────────────────────────────────────

GpuResult<GpuDevice> GpuDevice::from_surface(GpuSurface& surface) {
    if (!surface.is_initialized())
        return Err(make_error(GpuError::Kind::unavailable, "GpuSurface is not initialized"));
    auto* device = static_cast<wgpu::Device*>(surface.dawn_device_handle());
    auto* queue = static_cast<wgpu::Queue*>(surface.dawn_queue_handle());
    auto* instance = static_cast<wgpu::Instance*>(surface.dawn_instance_handle());
    if (device == nullptr || queue == nullptr || instance == nullptr || !*device || !*queue ||
        !*instance)
        return Err(make_error(GpuError::Kind::unavailable,
                              "GpuSurface has no Dawn device, queue or instance"));

    auto state = std::make_shared<detail::GpuDeviceState>();
    state->device = *device;
    state->queue = *queue;
    state->instance = *instance;
    state->lost_future = state->device.GetLostFuture();
    state->has_lost_future = true;
    state->capabilities = read_capabilities(state->device);
    if (state->is_lost())
        return Err(make_error(GpuError::Kind::device_lost, "the GPU device was lost"));

    GpuDevice out;
    out.state_ = std::move(state);
    return Ok(std::move(out));
}

bool GpuDevice::is_lost() const {
    return !state_ || state_->is_lost();
}

void GpuDevice::process_events() const {
    if (state_ && state_->instance)
        state_->instance.ProcessEvents();
}

GpuResult<GpuTexture> GpuDevice::create_texture(const GpuTextureDesc& desc) const {
    if (auto ok = check_device(state_); !ok)
        return Err(ok.error());
    if (auto ok = validate_texture_desc(desc, state_->capabilities); !ok)
        return Err(ok.error());

    auto texture_state = std::make_shared<detail::GpuTextureState>();
    texture_state->device = state_;
    texture_state->desc = desc;
    wgpu::TextureDescriptor td{};
    td.label = view_of(texture_state->desc.label);
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {desc.width, desc.height, 1};
    td.format = to_wgpu(desc.format);
    td.mipLevelCount = 1;
    td.sampleCount = desc.sample_count;
    td.usage = static_cast<wgpu::TextureUsage>(static_cast<std::uint32_t>(desc.usage));
    auto status = run_scoped(*state_, [&] {
        texture_state->texture = state_->device.CreateTexture(&td);
        if (texture_state->texture)
            texture_state->view = texture_state->texture.CreateView();
    });
    if (!status)
        return Err(status.error());
    if (!texture_state->texture || !texture_state->view)
        return Err(make_error(GpuError::Kind::internal, "texture creation returned null"));

    GpuTexture out;
    GpuAccess::state(out) = std::move(texture_state);
    return Ok(std::move(out));
}

GpuResult<GpuBuffer> GpuDevice::create_buffer(const GpuBufferDesc& desc) const {
    if (auto ok = check_device(state_); !ok)
        return Err(ok.error());
    if (desc.size == 0)
        return Err(make_error(GpuError::Kind::invalid_argument, "buffer size must be non-zero"));
    if (desc.usage == GpuBufferUsage::none)
        return Err(make_error(GpuError::Kind::invalid_argument, "buffer usage is empty"));
    if (desc.size % 4 != 0)
        return Err(
            make_error(GpuError::Kind::invalid_argument, "buffer size must be a multiple of 4"));

    auto buffer_state = std::make_shared<detail::GpuBufferState>();
    buffer_state->device = state_;
    buffer_state->desc = desc;
    wgpu::BufferDescriptor bd{};
    bd.label = view_of(buffer_state->desc.label);
    bd.size = desc.size;
    bd.usage = static_cast<wgpu::BufferUsage>(static_cast<std::uint32_t>(desc.usage));
    auto status =
        run_scoped(*state_, [&] { buffer_state->buffer = state_->device.CreateBuffer(&bd); });
    if (!status)
        return Err(status.error());
    if (!buffer_state->buffer)
        return Err(make_error(GpuError::Kind::internal, "buffer creation returned null"));

    GpuBuffer out;
    GpuAccess::state(out) = std::move(buffer_state);
    return Ok(std::move(out));
}

GpuResult<GpuSampler> GpuDevice::create_sampler(const GpuSamplerDesc& desc) const {
    if (auto ok = check_device(state_); !ok)
        return Err(ok.error());
    auto sampler_state = std::make_shared<detail::GpuSamplerState>();
    sampler_state->device = state_;
    wgpu::SamplerDescriptor sd{};
    sd.label = view_of(desc.label);
    const auto address = desc.repeat ? wgpu::AddressMode::Repeat : wgpu::AddressMode::ClampToEdge;
    sd.addressModeU = sd.addressModeV = sd.addressModeW = address;
    const auto filter = desc.linear ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
    sd.magFilter = sd.minFilter = filter;
    sd.mipmapFilter =
        desc.linear ? wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest;
    auto status =
        run_scoped(*state_, [&] { sampler_state->sampler = state_->device.CreateSampler(&sd); });
    if (!status)
        return Err(status.error());
    GpuSampler out;
    GpuAccess::state(out) = std::move(sampler_state);
    return Ok(std::move(out));
}

GpuResult<GpuShaderModule> GpuDevice::create_shader_module(std::string_view wgsl,
                                                           std::string_view label) const {
    if (auto ok = check_device(state_); !ok)
        return Err(ok.error());
    if (wgsl.empty())
        return Err(make_error(GpuError::Kind::invalid_argument, "WGSL source is empty"));

    auto compiled =
        detail::compile_wgsl(state_->device, state_->instance, wgsl, label, kScopeTimeout);
    if (state_->is_lost())
        return Err(make_error(GpuError::Kind::device_lost, "the GPU device was lost"));
    if (compiled.timed_out)
        return Err(make_error(GpuError::Kind::timeout, "shader compilation did not report back"));
    if (!compiled.ok()) {
        GpuError error =
            make_error(GpuError::Kind::shader_compilation,
                       "WGSL compilation failed" +
                           (label.empty() ? std::string() : " for " + std::string(label)));
        error.diagnostics = std::move(compiled.diagnostics);
        if (!compiled.scope_error.empty()) {
            WgslDiagnostic diagnostic;
            diagnostic.message = compiled.scope_error;
            error.diagnostics.push_back(std::move(diagnostic));
        }
        return Err(std::move(error));
    }

    auto module_state = std::make_shared<detail::GpuShaderModuleState>();
    module_state->device = state_;
    module_state->module = std::move(compiled.module);
    module_state->warnings = std::move(compiled.diagnostics);
    GpuShaderModule out;
    GpuAccess::state(out) = std::move(module_state);
    return Ok(std::move(out));
}

GpuResult<GpuRenderPipeline>
GpuDevice::create_render_pipeline(const GpuRenderPipelineDesc& desc) const {
    if (auto ok = check_device(state_); !ok)
        return Err(ok.error());
    auto invalid = [](std::string message) {
        return Err(make_error(GpuError::Kind::invalid_argument, std::move(message)));
    };
    if (!same_device(desc.module, state_))
        return invalid("render pipeline needs a shader module from this device");
    const GpuShaderModule& fragment_module =
        desc.fragment_module ? *desc.fragment_module : desc.module;
    if (!same_device(fragment_module, state_))
        return invalid("fragment module is not from this device");
    if (desc.color_targets.empty() && !desc.depth)
        return invalid("render pipeline needs a color target or a depth state");
    const auto max_colors = state_->capabilities.facts.max_color_attachments;
    if (max_colors != 0 && desc.color_targets.size() > max_colors)
        return invalid("more color targets than the device's maxColorAttachments (" +
                       std::to_string(max_colors) + ")");
    if (desc.sample_count != 1 && desc.sample_count != 4)
        return invalid("sample_count must be 1 or 4");
    for (const auto& target : desc.color_targets) {
        const auto support = state_->capabilities.support(target.format);
        if (gpu_format_is_depth(target.format) || !support.renderable)
            return invalid(std::string(gpu_format_name(target.format)) +
                           " is not a renderable color format");
        if (target.blend != GpuBlend::replace && !support.blendable)
            return invalid(std::string(gpu_format_name(target.format)) + " is not blendable");
        if (desc.sample_count == 4 && !support.multisample)
            return invalid(std::string(gpu_format_name(target.format)) +
                           " does not support multisampling");
    }
    if (desc.depth && !gpu_format_is_depth(desc.depth->format))
        return invalid("depth state needs a depth format");

    // Descriptor storage that must outlive CreateRenderPipeline.
    std::vector<std::vector<wgpu::VertexAttribute>> attributes(desc.vertex_buffers.size());
    std::vector<wgpu::VertexBufferLayout> layouts(desc.vertex_buffers.size());
    for (std::size_t i = 0; i < desc.vertex_buffers.size(); ++i) {
        const auto& buffer = desc.vertex_buffers[i];
        for (const auto& attribute : buffer.attributes) {
            wgpu::VertexAttribute a{};
            a.format = to_wgpu(attribute.format);
            a.offset = attribute.offset;
            a.shaderLocation = attribute.location;
            attributes[i].push_back(a);
        }
        layouts[i].arrayStride = buffer.stride;
        layouts[i].stepMode =
            buffer.per_instance ? wgpu::VertexStepMode::Instance : wgpu::VertexStepMode::Vertex;
        layouts[i].attributeCount = attributes[i].size();
        layouts[i].attributes = attributes[i].data();
    }
    std::vector<std::optional<wgpu::BlendState>> blends;
    std::vector<wgpu::ColorTargetState> targets(desc.color_targets.size());
    blends.reserve(desc.color_targets.size());
    for (std::size_t i = 0; i < desc.color_targets.size(); ++i) {
        blends.push_back(to_wgpu(desc.color_targets[i].blend));
        targets[i].format = to_wgpu(desc.color_targets[i].format);
        targets[i].writeMask = wgpu::ColorWriteMask::All;
        targets[i].blend = blends.back() ? &*blends.back() : nullptr;
    }

    wgpu::FragmentState fragment{};
    fragment.module = GpuAccess::state(fragment_module)->module;
    fragment.entryPoint = view_of(desc.fragment_entry);
    fragment.targetCount = targets.size();
    fragment.targets = targets.data();

    wgpu::DepthStencilState depth{};
    if (desc.depth) {
        depth.format = to_wgpu(desc.depth->format);
        depth.depthWriteEnabled =
            desc.depth->write ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
        depth.depthCompare = to_wgpu(desc.depth->compare);
    }

    wgpu::RenderPipelineDescriptor pd{};
    pd.label = view_of(desc.label);
    pd.layout = nullptr; // layout: auto
    pd.vertex.module = GpuAccess::state(desc.module)->module;
    pd.vertex.entryPoint = view_of(desc.vertex_entry);
    pd.vertex.bufferCount = layouts.size();
    pd.vertex.buffers = layouts.data();
    pd.primitive.topology = to_wgpu(desc.topology);
    pd.primitive.cullMode = to_wgpu(desc.cull);
    pd.multisample.count = desc.sample_count;
    pd.fragment = targets.empty() ? nullptr : &fragment;
    pd.depthStencil = desc.depth ? &depth : nullptr;

    auto pipeline_state = std::make_shared<detail::GpuRenderPipelineState>();
    pipeline_state->device = state_;
    auto status = run_scoped(
        *state_, [&] { pipeline_state->pipeline = state_->device.CreateRenderPipeline(&pd); });
    if (!status)
        return Err(status.error());
    if (!pipeline_state->pipeline)
        return Err(make_error(GpuError::Kind::internal, "render pipeline creation returned null"));
    GpuRenderPipeline out;
    GpuAccess::state(out) = std::move(pipeline_state);
    return Ok(std::move(out));
}

GpuResult<GpuComputePipeline>
GpuDevice::create_compute_pipeline(const GpuComputePipelineDesc& desc) const {
    if (auto ok = check_device(state_); !ok)
        return Err(ok.error());
    if (!same_device(desc.module, state_))
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "compute pipeline needs a shader module from this device"));
    wgpu::ComputePipelineDescriptor cd{};
    cd.label = view_of(desc.label);
    cd.layout = nullptr; // layout: auto
    cd.compute.module = GpuAccess::state(desc.module)->module;
    cd.compute.entryPoint = view_of(desc.entry);

    auto pipeline_state = std::make_shared<detail::GpuComputePipelineState>();
    pipeline_state->device = state_;
    auto status = run_scoped(
        *state_, [&] { pipeline_state->pipeline = state_->device.CreateComputePipeline(&cd); });
    if (!status)
        return Err(status.error());
    if (!pipeline_state->pipeline)
        return Err(make_error(GpuError::Kind::internal, "compute pipeline creation returned null"));
    GpuComputePipeline out;
    GpuAccess::state(out) = std::move(pipeline_state);
    return Ok(std::move(out));
}

namespace {

template <typename PipelineState>
GpuResult<GpuBindGroup> make_bind_group(const std::shared_ptr<detail::GpuDeviceState>& device,
                                        const std::shared_ptr<PipelineState>& pipeline,
                                        std::uint32_t group,
                                        const std::vector<GpuBinding>& bindings) {
    if (auto ok = check_device(device); !ok)
        return Err(ok.error());
    if (!pipeline || pipeline->device != device)
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "bind group needs a pipeline from this device"));
    std::vector<wgpu::BindGroupEntry> entries;
    entries.reserve(bindings.size());
    for (const auto& binding : bindings) {
        wgpu::BindGroupEntry entry{};
        entry.binding = binding.binding;
        switch (binding.kind) {
        case GpuBinding::Kind::buffer:
            if (!same_device(binding.buffer, device))
                return Err(make_error(GpuError::Kind::invalid_argument,
                                      "binding " + std::to_string(binding.binding) +
                                          ": buffer is not from this device"));
            entry.buffer = GpuAccess::state(binding.buffer)->buffer;
            entry.offset = binding.offset;
            entry.size = binding.size == 0 ? wgpu::kWholeSize : binding.size;
            break;
        case GpuBinding::Kind::texture:
            if (!same_device(binding.texture, device))
                return Err(make_error(GpuError::Kind::invalid_argument,
                                      "binding " + std::to_string(binding.binding) +
                                          ": texture is not from this device"));
            entry.textureView = GpuAccess::state(binding.texture)->view;
            break;
        case GpuBinding::Kind::sampler:
            if (!same_device(binding.sampler, device))
                return Err(make_error(GpuError::Kind::invalid_argument,
                                      "binding " + std::to_string(binding.binding) +
                                          ": sampler is not from this device"));
            entry.sampler = GpuAccess::state(binding.sampler)->sampler;
            break;
        }
        entries.push_back(entry);
    }

    auto group_state = std::make_shared<detail::GpuBindGroupState>();
    group_state->device = device;
    auto status = run_scoped(*device, [&] {
        wgpu::BindGroupDescriptor bd{};
        bd.layout = pipeline->pipeline.GetBindGroupLayout(group);
        bd.entryCount = entries.size();
        bd.entries = entries.data();
        group_state->group = device->device.CreateBindGroup(&bd);
    });
    if (!status)
        return Err(status.error());
    if (!group_state->group)
        return Err(make_error(GpuError::Kind::internal, "bind group creation returned null"));
    GpuBindGroup out;
    GpuAccess::state(out) = std::move(group_state);
    return Ok(std::move(out));
}

} // namespace

GpuResult<GpuBindGroup>
GpuDevice::create_bind_group(const GpuRenderPipeline& pipeline, std::uint32_t group,
                             const std::vector<GpuBinding>& bindings) const {
    return make_bind_group(state_, GpuAccess::state(pipeline), group, bindings);
}

GpuResult<GpuBindGroup>
GpuDevice::create_bind_group(const GpuComputePipeline& pipeline, std::uint32_t group,
                             const std::vector<GpuBinding>& bindings) const {
    return make_bind_group(state_, GpuAccess::state(pipeline), group, bindings);
}

GpuResult<GpuTarget> GpuDevice::create_target(const GpuTargetDesc& desc) const {
    if (gpu_format_is_depth(desc.color_format))
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "a target's color format cannot be a depth format"));
    GpuTextureDesc color_desc;
    color_desc.width = desc.width;
    color_desc.height = desc.height;
    color_desc.format = desc.color_format;
    color_desc.usage =
        GpuTextureUsage::render_target | GpuTextureUsage::sampled | GpuTextureUsage::copy_src;
    color_desc.label =
        desc.label.empty() ? std::string("Pulp GpuTarget color") : desc.label + " color";
    auto color = create_texture(color_desc);
    if (!color)
        return Err(color.error());

    auto target_state = std::make_shared<detail::GpuTargetState>();
    target_state->device = state_;
    target_state->color = std::move(color.value());
    if (desc.depth_format) {
        if (!gpu_format_is_depth(*desc.depth_format))
            return Err(make_error(GpuError::Kind::invalid_argument,
                                  "depth_format must be a depth format"));
        GpuTextureDesc depth_desc;
        depth_desc.width = desc.width;
        depth_desc.height = desc.height;
        depth_desc.format = *desc.depth_format;
        depth_desc.usage = GpuTextureUsage::render_target;
        depth_desc.label =
            desc.label.empty() ? std::string("Pulp GpuTarget depth") : desc.label + " depth";
        auto depth = create_texture(depth_desc);
        if (!depth)
            return Err(depth.error());
        target_state->depth = std::move(depth.value());
    }
    GpuTarget out;
    GpuAccess::state(out) = std::move(target_state);
    return Ok(std::move(out));
}

GpuStatus GpuDevice::write_buffer(const GpuBuffer& buffer, std::uint64_t offset, const void* data,
                                  std::size_t size) const {
    if (auto ok = check_device(state_); !ok)
        return ok;
    if (!same_device(buffer, state_))
        return Err(make_error(GpuError::Kind::invalid_argument, "buffer is not from this device"));
    if (data == nullptr || size == 0)
        return Err(make_error(GpuError::Kind::invalid_argument, "nothing to write"));
    if (offset % 4 != 0 || size % 4 != 0)
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "buffer writes need 4-byte-aligned offset and size"));
    if (offset + size > buffer.size())
        return Err(make_error(GpuError::Kind::invalid_argument, "write runs past the buffer"));
    if (!has_usage(buffer.usage(), GpuBufferUsage::copy_dst))
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "buffer needs copy_dst usage to be written"));
    const auto& wgpu_buffer = GpuAccess::state(buffer)->buffer;
    return run_scoped(*state_, [&] { state_->queue.WriteBuffer(wgpu_buffer, offset, data, size); });
}

// ── GpuTarget readback ───────────────────────────────────────────────────

GpuResult<GpuPixels> GpuTarget::read_pixels(std::chrono::milliseconds timeout) const {
    if (!state_)
        return Err(make_error(GpuError::Kind::unavailable, "GpuTarget is not initialized"));
    const auto& device = state_->device;
    if (auto ok = check_device(device); !ok)
        return Err(ok.error());
    const auto& texture = GpuAccess::state(state_->color);
    if (texture->unsubmitted_writes > 0)
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "an unsubmitted GpuFrame writes this target; submit it first"));
    const auto format = texture->desc.format;
    const auto bytes_per_pixel = gpu_format_bytes_per_pixel(format);
    if (bytes_per_pixel == 0)
        return Err(make_error(GpuError::Kind::invalid_argument, "format cannot be read back"));

    const auto layout =
        detail::padded_row_layout(texture->desc.width, texture->desc.height, bytes_per_pixel);
    wgpu::Buffer readback;
    auto status = run_scoped(*device, [&] {
        wgpu::BufferDescriptor bd{};
        bd.label = "Pulp GpuTarget readback";
        bd.size = layout.buffer_size;
        bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        readback = device->device.CreateBuffer(&bd);
        if (!readback)
            return;
        auto encoder = device->device.CreateCommandEncoder();
        wgpu::TexelCopyTextureInfo src{};
        src.texture = texture->texture;
        src.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = layout.padded_bytes_per_row;
        dst.layout.rowsPerImage = layout.height;
        wgpu::Extent3D size{layout.width, layout.height, 1};
        encoder.CopyTextureToBuffer(&src, &dst, &size);
        auto commands = encoder.Finish();
        device->queue.Submit(1, &commands);
    });
    if (!status)
        return Err(status.error());
    if (!readback)
        return Err(make_error(GpuError::Kind::internal, "readback buffer creation returned null"));

    std::string error;
    auto bytes = detail::read_padded_buffer(device->instance, readback, layout, timeout, error);
    if (!bytes) {
        if (device->is_lost())
            return Err(make_error(GpuError::Kind::device_lost, "the GPU device was lost"));
        return Err(make_error(error.find("timed out") != std::string::npos
                                  ? GpuError::Kind::timeout
                                  : GpuError::Kind::internal,
                              error));
    }
    GpuPixels pixels;
    pixels.bytes = std::move(*bytes);
    pixels.width = layout.width;
    pixels.height = layout.height;
    pixels.bytes_per_pixel = bytes_per_pixel;
    pixels.format = format;
    return Ok(std::move(pixels));
}

// ── Submission ───────────────────────────────────────────────────────────

GpuSubmissionStatus GpuSubmission::status() const {
    if (!state_)
        return GpuSubmissionStatus::failed;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}

GpuSubmissionStatus GpuSubmission::poll() const {
    if (!state_)
        return GpuSubmissionStatus::failed;
    if (state_->device && state_->device->instance)
        state_->device->instance.ProcessEvents();
    // Read the device's loss state before taking the lock: polling the lost
    // future may run callbacks.
    const bool lost = state_->device && state_->device->is_lost();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->status != GpuSubmissionStatus::pending)
        return state_->status;
    // Dawn may report a submission done once the device is gone; completion
    // observed alongside a lost device is a failure, never a success.
    if (state_->work_done || lost) {
        if (state_->work_ok && !lost) {
            state_->status = GpuSubmissionStatus::succeeded;
        } else {
            state_->status = GpuSubmissionStatus::failed;
            if (!state_->error)
                state_->error = make_error(
                    lost ? GpuError::Kind::device_lost : GpuError::Kind::internal,
                    lost ? "the GPU device was lost" : "the queue reported the submission failed");
        }
    }
    return state_->status;
}

GpuSubmissionStatus GpuSubmission::wait(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto status = poll();
        if (status != GpuSubmissionStatus::pending)
            return status;
        if (std::chrono::steady_clock::now() >= deadline)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::optional<GpuError> GpuSubmission::error() const {
    if (!state_)
        return make_error(GpuError::Kind::unavailable, "no submission");
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->error;
}

// ── Passes ───────────────────────────────────────────────────────────────

struct GpuRenderPass::Impl {
    std::weak_ptr<detail::GpuFrameState> frame;
    std::uint64_t id = 0;

    // The frame's encoder if this pass is still the open one; records a
    // misuse error otherwise.
    wgpu::RenderPassEncoder* encoder(const char* call) {
        auto state = frame.lock();
        if (!state)
            return nullptr;
        if (id == 0 || state->open_pass != id || !state->render_pass) {
            state->record_error(GpuError::Kind::invalid_argument,
                                std::string(call) + " on a render pass that is not open");
            return nullptr;
        }
        return &state->render_pass;
    }
    std::shared_ptr<detail::GpuDeviceState> device() {
        auto state = frame.lock();
        return state ? state->device : nullptr;
    }
    void misuse(std::string message) {
        if (auto state = frame.lock())
            state->record_error(GpuError::Kind::invalid_argument, std::move(message));
    }
};

GpuRenderPass::GpuRenderPass(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuRenderPass::GpuRenderPass(GpuRenderPass&&) noexcept = default;
GpuRenderPass& GpuRenderPass::operator=(GpuRenderPass&& other) noexcept {
    if (this != &other) {
        end();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuRenderPass::~GpuRenderPass() {
    end();
}

bool GpuRenderPass::valid() const noexcept {
    if (!impl_ || impl_->id == 0)
        return false;
    auto state = impl_->frame.lock();
    return state && state->open_pass == impl_->id;
}

void GpuRenderPass::set_pipeline(const GpuRenderPipeline& pipeline) {
    if (!impl_)
        return;
    if (!same_device(pipeline, impl_->device()))
        return impl_->misuse("set_pipeline: pipeline is not from this device");
    if (auto* enc = impl_->encoder("set_pipeline"))
        enc->SetPipeline(GpuAccess::state(pipeline)->pipeline);
}

void GpuRenderPass::set_bind_group(std::uint32_t index, const GpuBindGroup& group) {
    if (!impl_)
        return;
    if (!same_device(group, impl_->device()))
        return impl_->misuse("set_bind_group: bind group is not from this device");
    if (auto* enc = impl_->encoder("set_bind_group"))
        enc->SetBindGroup(index, GpuAccess::state(group)->group);
}

void GpuRenderPass::set_vertex_buffer(std::uint32_t slot, const GpuBuffer& buffer,
                                      std::uint64_t offset) {
    if (!impl_)
        return;
    if (!same_device(buffer, impl_->device()))
        return impl_->misuse("set_vertex_buffer: buffer is not from this device");
    if (auto* enc = impl_->encoder("set_vertex_buffer"))
        enc->SetVertexBuffer(slot, GpuAccess::state(buffer)->buffer, offset);
}

void GpuRenderPass::set_index_buffer(const GpuBuffer& buffer, bool uint32_indices,
                                     std::uint64_t offset) {
    if (!impl_)
        return;
    if (!same_device(buffer, impl_->device()))
        return impl_->misuse("set_index_buffer: buffer is not from this device");
    if (auto* enc = impl_->encoder("set_index_buffer"))
        enc->SetIndexBuffer(GpuAccess::state(buffer)->buffer,
                            uint32_indices ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16,
                            offset);
}

void GpuRenderPass::set_viewport(float x, float y, float width, float height, float min_depth,
                                 float max_depth) {
    if (!impl_)
        return;
    if (auto* enc = impl_->encoder("set_viewport"))
        enc->SetViewport(x, y, width, height, min_depth, max_depth);
}

void GpuRenderPass::set_scissor(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                std::uint32_t height) {
    if (!impl_)
        return;
    if (auto* enc = impl_->encoder("set_scissor"))
        enc->SetScissorRect(x, y, width, height);
}

void GpuRenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count,
                         std::uint32_t first_vertex, std::uint32_t first_instance) {
    if (!impl_)
        return;
    if (auto* enc = impl_->encoder("draw"))
        enc->Draw(vertex_count, instance_count, first_vertex, first_instance);
}

void GpuRenderPass::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count,
                                 std::uint32_t first_index, std::int32_t base_vertex,
                                 std::uint32_t first_instance) {
    if (!impl_)
        return;
    if (auto* enc = impl_->encoder("draw_indexed"))
        enc->DrawIndexed(index_count, instance_count, first_index, base_vertex, first_instance);
}

void GpuRenderPass::end() {
    if (!impl_ || impl_->id == 0)
        return;
    if (auto state = impl_->frame.lock()) {
        if (state->open_pass == impl_->id)
            state->end_open_pass();
    }
    impl_->id = 0;
}

struct GpuComputePass::Impl {
    std::weak_ptr<detail::GpuFrameState> frame;
    std::uint64_t id = 0;

    wgpu::ComputePassEncoder* encoder(const char* call) {
        auto state = frame.lock();
        if (!state)
            return nullptr;
        if (id == 0 || state->open_pass != id || !state->compute_pass) {
            state->record_error(GpuError::Kind::invalid_argument,
                                std::string(call) + " on a compute pass that is not open");
            return nullptr;
        }
        return &state->compute_pass;
    }
    std::shared_ptr<detail::GpuDeviceState> device() {
        auto state = frame.lock();
        return state ? state->device : nullptr;
    }
    void misuse(std::string message) {
        if (auto state = frame.lock())
            state->record_error(GpuError::Kind::invalid_argument, std::move(message));
    }
};

GpuComputePass::GpuComputePass(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuComputePass::GpuComputePass(GpuComputePass&&) noexcept = default;
GpuComputePass& GpuComputePass::operator=(GpuComputePass&& other) noexcept {
    if (this != &other) {
        end();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuComputePass::~GpuComputePass() {
    end();
}

bool GpuComputePass::valid() const noexcept {
    if (!impl_ || impl_->id == 0)
        return false;
    auto state = impl_->frame.lock();
    return state && state->open_pass == impl_->id;
}

void GpuComputePass::set_pipeline(const GpuComputePipeline& pipeline) {
    if (!impl_)
        return;
    if (!same_device(pipeline, impl_->device()))
        return impl_->misuse("set_pipeline: pipeline is not from this device");
    if (auto* enc = impl_->encoder("set_pipeline"))
        enc->SetPipeline(GpuAccess::state(pipeline)->pipeline);
}

void GpuComputePass::set_bind_group(std::uint32_t index, const GpuBindGroup& group) {
    if (!impl_)
        return;
    if (!same_device(group, impl_->device()))
        return impl_->misuse("set_bind_group: bind group is not from this device");
    if (auto* enc = impl_->encoder("set_bind_group"))
        enc->SetBindGroup(index, GpuAccess::state(group)->group);
}

void GpuComputePass::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!impl_)
        return;
    if (auto* enc = impl_->encoder("dispatch"))
        enc->DispatchWorkgroups(x, y, z);
}

void GpuComputePass::end() {
    if (!impl_ || impl_->id == 0)
        return;
    if (auto state = impl_->frame.lock()) {
        if (state->open_pass == impl_->id)
            state->end_open_pass();
    }
    impl_->id = 0;
}

// ── GpuFrame ─────────────────────────────────────────────────────────────

GpuFrame::GpuFrame(const GpuDevice& device, std::string_view label)
    : state_(std::make_shared<detail::GpuFrameState>()) {
    state_->device = GpuAccess::state(device);
    state_->label = std::string(label);
    if (auto ok = check_device(state_->device); !ok) {
        state_->error = ok.error();
        return;
    }
    push_scopes(state_->device->device);
    state_->scopes_open = true;
    wgpu::CommandEncoderDescriptor ed{};
    ed.label = view_of(state_->label);
    state_->encoder = state_->device->device.CreateCommandEncoder(&ed);
    if (!state_->encoder)
        state_->record_error(GpuError::Kind::internal, "command encoder creation returned null");
}

GpuFrame::GpuFrame(GpuFrame&&) noexcept = default;

GpuFrame::~GpuFrame() {
    if (!state_ || state_->submitted)
        return;
    auto result = submit();
    if (!result)
        runtime::log_error("GpuFrame '{}' failed at implicit submit: {}", state_->label,
                           result.error().describe());
}

bool GpuFrame::submitted() const noexcept {
    return state_ && state_->submitted;
}

GpuRenderPass GpuFrame::render_pass(const GpuRenderPassDesc& desc) {
    auto impl = std::make_unique<GpuRenderPass::Impl>();
    impl->frame = state_;
    if (!state_)
        return GpuRenderPass(std::move(impl));
    auto fail = [&](std::string message) {
        state_->record_error(GpuError::Kind::invalid_argument,
                             "render_pass: " + std::move(message));
        return GpuRenderPass(std::move(impl));
    };
    if (state_->submitted)
        return fail("the frame was already submitted");
    if (!state_->encoder)
        return GpuRenderPass(std::move(impl));
    if (state_->open_pass != 0)
        return fail("another pass is still open; end it first");
    if (desc.colors.empty() && !desc.depth)
        return fail("needs at least one color attachment or a depth attachment");
    const auto max_colors = state_->device->capabilities.facts.max_color_attachments;
    if (max_colors != 0 && desc.colors.size() > max_colors)
        return fail("more color attachments than maxColorAttachments");

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    auto check_size = [&](const GpuTexture& texture) {
        if (width == 0) {
            width = texture.width();
            height = texture.height();
            return true;
        }
        return texture.width() == width && texture.height() == height;
    };

    std::vector<wgpu::RenderPassColorAttachment> colors(desc.colors.size());
    std::vector<std::shared_ptr<detail::GpuTextureState>> written;
    for (std::size_t i = 0; i < desc.colors.size(); ++i) {
        const auto& color = desc.colors[i];
        if (!same_device(color.texture, state_->device))
            return fail("color attachment " + std::to_string(i) + " is not from this device");
        if (!has_usage(color.texture.usage(), GpuTextureUsage::render_target) ||
            gpu_format_is_depth(color.texture.format()))
            return fail("color attachment " + std::to_string(i) + " is not a color render target");
        if (!check_size(color.texture))
            return fail("attachments differ in size");
        const auto& tex = GpuAccess::state(color.texture);
        colors[i].view = tex->view;
        colors[i].loadOp = color.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
        colors[i].storeOp = wgpu::StoreOp::Store;
        if (color.clear) {
            const auto& c = *color.clear;
            colors[i].clearValue = {c[0], c[1], c[2], c[3]};
        }
        written.push_back(tex);
        if (color.resolve) {
            if (!same_device(*color.resolve, state_->device))
                return fail("resolve target " + std::to_string(i) + " is not from this device");
            if (color.texture.sample_count() == 1 || color.resolve->sample_count() != 1)
                return fail("resolve needs a multisampled attachment and a single-sample target");
            const auto& resolve = GpuAccess::state(*color.resolve);
            colors[i].resolveTarget = resolve->view;
            written.push_back(resolve);
        }
    }

    wgpu::RenderPassDepthStencilAttachment depth{};
    if (desc.depth) {
        if (!same_device(*desc.depth, state_->device) || !gpu_format_is_depth(desc.depth->format()))
            return fail("depth attachment is not a depth texture from this device");
        if (!check_size(*desc.depth))
            return fail("attachments differ in size");
        const auto& tex = GpuAccess::state(*desc.depth);
        depth.view = tex->view;
        depth.depthLoadOp = desc.depth_clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
        depth.depthStoreOp = wgpu::StoreOp::Store;
        depth.depthClearValue = desc.depth_clear.value_or(1.0f);
        if (desc.depth->format() == GpuFormat::depth24_plus_stencil8) {
            depth.stencilLoadOp = wgpu::LoadOp::Clear;
            depth.stencilStoreOp = wgpu::StoreOp::Store;
            depth.stencilClearValue = 0;
        }
        written.push_back(tex);
    }

    wgpu::RenderPassDescriptor pd{};
    const std::string label = desc.label;
    pd.label = view_of(label);
    pd.colorAttachmentCount = colors.size();
    pd.colorAttachments = colors.data();
    pd.depthStencilAttachment = desc.depth ? &depth : nullptr;
    state_->render_pass = state_->encoder.BeginRenderPass(&pd);
    if (!state_->render_pass)
        return fail("Dawn returned no render pass encoder");

    for (auto& texture : written) {
        if (std::find(state_->writes.begin(), state_->writes.end(), texture) ==
            state_->writes.end()) {
            ++texture->unsubmitted_writes;
            state_->writes.push_back(texture);
        }
    }
    impl->id = state_->next_pass++;
    state_->open_pass = impl->id;
    return GpuRenderPass(std::move(impl));
}

GpuComputePass GpuFrame::compute_pass(std::string_view label) {
    auto impl = std::make_unique<GpuComputePass::Impl>();
    impl->frame = state_;
    if (!state_)
        return GpuComputePass(std::move(impl));
    if (state_->submitted) {
        state_->record_error(GpuError::Kind::invalid_argument,
                             "compute_pass: the frame was already submitted");
        return GpuComputePass(std::move(impl));
    }
    if (!state_->encoder)
        return GpuComputePass(std::move(impl));
    if (state_->open_pass != 0) {
        state_->record_error(GpuError::Kind::invalid_argument,
                             "compute_pass: another pass is still open; end it first");
        return GpuComputePass(std::move(impl));
    }
    const std::string label_copy(label);
    wgpu::ComputePassDescriptor cd{};
    cd.label = view_of(label_copy);
    state_->compute_pass = state_->encoder.BeginComputePass(&cd);
    if (!state_->compute_pass) {
        state_->record_error(GpuError::Kind::internal, "Dawn returned no compute pass encoder");
        return GpuComputePass(std::move(impl));
    }
    impl->id = state_->next_pass++;
    state_->open_pass = impl->id;
    return GpuComputePass(std::move(impl));
}

GpuResult<GpuSubmission> GpuFrame::submit() {
    if (!state_)
        return Err(make_error(GpuError::Kind::invalid_argument, "the frame was moved from"));
    if (state_->submitted)
        return Err(make_error(GpuError::Kind::invalid_argument, "the frame was already submitted"));
    state_->submitted = true;

    if (state_->open_pass != 0) {
        state_->end_open_pass();
        state_->record_error(GpuError::Kind::invalid_argument,
                             "a pass was still open at submit; end passes before submitting");
    }

    auto device = state_->device;
    if (!state_->scopes_open) {
        // The frame never began (no device, or a lost one).
        state_->release_writes();
        return Err(state_->error.value_or(
            make_error(GpuError::Kind::unavailable, "the frame has no device")));
    }

    std::optional<GpuError> recording_error = state_->error;
    wgpu::CommandBuffer commands;
    if (state_->encoder)
        commands = state_->encoder.Finish();
    // Nothing recorded with an API misuse reaches the queue: a partial frame
    // is never submitted.
    if (!recording_error && commands)
        device->queue.Submit(1, &commands);

    auto submission = std::make_shared<detail::GpuSubmissionState>();
    submission->device = device;
    if (!recording_error && commands) {
        device->queue.OnSubmittedWorkDone(
            wgpu::CallbackMode::AllowProcessEvents,
            [submission](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
                std::lock_guard<std::mutex> lock(submission->mutex);
                submission->work_done = true;
                submission->work_ok = status == wgpu::QueueWorkDoneStatus::Success;
            });
    }
    auto scopes = pop_scopes(device->device);
    state_->scopes_open = false;
    // The queue now orders this frame's writes ahead of any later
    // submission, Graphite's included.
    state_->release_writes();

    auto scope_status = finish_scopes(*device, scopes);
    if (recording_error)
        return Err(std::move(*recording_error));
    if (!scope_status)
        return Err(scope_status.error());
    if (!commands)
        return Err(make_error(GpuError::Kind::internal, "the command encoder produced nothing"));

    GpuSubmission out;
    GpuAccess::state(out) = std::move(submission);
    return Ok(std::move(out));
}

// ── Compositing ──────────────────────────────────────────────────────────

GpuStatus draw_gpu_texture(canvas::Canvas& canvas, const GpuTexture& texture, float x, float y,
                           float w, float h) {
    const auto& state = GpuAccess::state(texture);
    if (!state)
        return Err(make_error(GpuError::Kind::invalid_argument, "texture is not initialized"));
    if (auto ok = check_device(state->device); !ok)
        return ok;
    if (!has_usage(state->desc.usage, GpuTextureUsage::sampled))
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "texture needs sampled usage to be composited"));
    if (state->desc.sample_count != 1)
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "a multisampled texture cannot be composited; resolve it first"));
    const auto format = state->desc.format;
    if (format != GpuFormat::rgba8_unorm && format != GpuFormat::bgra8_unorm &&
        format != GpuFormat::rgba16_float)
        return Err(
            make_error(GpuError::Kind::invalid_argument,
                       std::string(gpu_format_name(format)) +
                           " cannot be composited; use rgba8unorm, bgra8unorm or rgba16float"));
    if (state->unsubmitted_writes > 0)
        return Err(make_error(GpuError::Kind::invalid_argument,
                              "an unsubmitted GpuFrame writes this texture; submit it first"));

#if PULP_CANVAS_GRAPHITE
    auto* skia = dynamic_cast<canvas::SkiaCanvas*>(&canvas);
    if (skia == nullptr)
        return Err(make_error(GpuError::Kind::unavailable,
                              "the canvas is not a GPU canvas; draw a fallback instead"));
    // Graphite's wrapped SkImage retains the WGPUTexture and releases it when
    // it is done (DawnGraphiteTypes.h, BackendTextures::MakeDawn), so the
    // caller may drop its handle once this returns.
    if (!skia->draw_native_dawn_texture(&state->texture, state->desc.width, state->desc.height,
                                        std::string(gpu_format_name(format)), x, y, w, h))
        return Err(make_error(GpuError::Kind::validation,
                              "the canvas could not wrap the texture (no active GPU frame?)"));
    return {};
#else
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return Err(make_error(GpuError::Kind::unavailable, "this build has no Graphite canvas"));
#endif
}

#else // !PULP_GPU_PASS_HAS_DAWN

// ── Builds without Dawn: everything reports `unavailable` ────────────────

namespace {
GpuError no_dawn() {
    return make_error(GpuError::Kind::unavailable, "this build has no Dawn WebGPU device");
}
} // namespace

GpuResult<GpuDevice> GpuDevice::from_surface(GpuSurface&) {
    return Err(no_dawn());
}
bool GpuDevice::is_lost() const {
    return true;
}
void GpuDevice::process_events() const {}
GpuResult<GpuTexture> GpuDevice::create_texture(const GpuTextureDesc&) const {
    return Err(no_dawn());
}
GpuResult<GpuBuffer> GpuDevice::create_buffer(const GpuBufferDesc&) const {
    return Err(no_dawn());
}
GpuResult<GpuSampler> GpuDevice::create_sampler(const GpuSamplerDesc&) const {
    return Err(no_dawn());
}
GpuResult<GpuShaderModule> GpuDevice::create_shader_module(std::string_view,
                                                           std::string_view) const {
    return Err(no_dawn());
}
GpuResult<GpuRenderPipeline> GpuDevice::create_render_pipeline(const GpuRenderPipelineDesc&) const {
    return Err(no_dawn());
}
GpuResult<GpuComputePipeline>
GpuDevice::create_compute_pipeline(const GpuComputePipelineDesc&) const {
    return Err(no_dawn());
}
GpuResult<GpuBindGroup> GpuDevice::create_bind_group(const GpuRenderPipeline&, std::uint32_t,
                                                     const std::vector<GpuBinding>&) const {
    return Err(no_dawn());
}
GpuResult<GpuBindGroup> GpuDevice::create_bind_group(const GpuComputePipeline&, std::uint32_t,
                                                     const std::vector<GpuBinding>&) const {
    return Err(no_dawn());
}
GpuResult<GpuTarget> GpuDevice::create_target(const GpuTargetDesc&) const {
    return Err(no_dawn());
}
GpuStatus GpuDevice::write_buffer(const GpuBuffer&, std::uint64_t, const void*, std::size_t) const {
    return Err(no_dawn());
}
GpuResult<GpuPixels> GpuTarget::read_pixels(std::chrono::milliseconds) const {
    return Err(no_dawn());
}

GpuSubmissionStatus GpuSubmission::status() const {
    return GpuSubmissionStatus::failed;
}
GpuSubmissionStatus GpuSubmission::poll() const {
    return GpuSubmissionStatus::failed;
}
GpuSubmissionStatus GpuSubmission::wait(std::chrono::milliseconds) const {
    return GpuSubmissionStatus::failed;
}
std::optional<GpuError> GpuSubmission::error() const {
    return no_dawn();
}

struct GpuRenderPass::Impl {};
GpuRenderPass::GpuRenderPass(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuRenderPass::GpuRenderPass(GpuRenderPass&&) noexcept = default;
GpuRenderPass& GpuRenderPass::operator=(GpuRenderPass&&) noexcept = default;
GpuRenderPass::~GpuRenderPass() = default;
bool GpuRenderPass::valid() const noexcept {
    return false;
}
void GpuRenderPass::set_pipeline(const GpuRenderPipeline&) {}
void GpuRenderPass::set_bind_group(std::uint32_t, const GpuBindGroup&) {}
void GpuRenderPass::set_vertex_buffer(std::uint32_t, const GpuBuffer&, std::uint64_t) {}
void GpuRenderPass::set_index_buffer(const GpuBuffer&, bool, std::uint64_t) {}
void GpuRenderPass::set_viewport(float, float, float, float, float, float) {}
void GpuRenderPass::set_scissor(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void GpuRenderPass::draw(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void GpuRenderPass::draw_indexed(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t,
                                 std::uint32_t) {}
void GpuRenderPass::end() {}

struct GpuComputePass::Impl {};
GpuComputePass::GpuComputePass(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuComputePass::GpuComputePass(GpuComputePass&&) noexcept = default;
GpuComputePass& GpuComputePass::operator=(GpuComputePass&&) noexcept = default;
GpuComputePass::~GpuComputePass() = default;
bool GpuComputePass::valid() const noexcept {
    return false;
}
void GpuComputePass::set_pipeline(const GpuComputePipeline&) {}
void GpuComputePass::set_bind_group(std::uint32_t, const GpuBindGroup&) {}
void GpuComputePass::dispatch(std::uint32_t, std::uint32_t, std::uint32_t) {}
void GpuComputePass::end() {}

GpuFrame::GpuFrame(const GpuDevice&, std::string_view)
    : state_(std::make_shared<detail::GpuFrameState>()) {}
GpuFrame::GpuFrame(GpuFrame&&) noexcept = default;
GpuFrame::~GpuFrame() = default;
bool GpuFrame::submitted() const noexcept {
    return state_ && state_->submitted;
}
GpuRenderPass GpuFrame::render_pass(const GpuRenderPassDesc&) {
    return GpuRenderPass(std::make_unique<GpuRenderPass::Impl>());
}
GpuComputePass GpuFrame::compute_pass(std::string_view) {
    return GpuComputePass(std::make_unique<GpuComputePass::Impl>());
}
GpuResult<GpuSubmission> GpuFrame::submit() {
    if (state_)
        state_->submitted = true;
    return Err(no_dawn());
}

GpuStatus draw_gpu_texture(canvas::Canvas&, const GpuTexture&, float, float, float, float) {
    return Err(no_dawn());
}

#endif // PULP_GPU_PASS_HAS_DAWN

} // namespace pulp::render
