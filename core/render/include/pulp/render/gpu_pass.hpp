#pragma once

/// @file gpu_pass.hpp
/// Typed custom GPU passes on Pulp's shared Dawn device.
///
/// A View that needs its own WGSL draws records render or compute passes on
/// the same `wgpu::Device` Skia Graphite draws the UI with, then composites
/// the result through the canvas:
///
///   auto device = GpuDevice::from_surface(*host.gpu_surface());   // once
///   auto shader = device->create_shader_module(kMyWgsl, "glow");  // once
///   auto pipeline = device->create_render_pipeline({...});        // once
///   auto target = device->create_target({.width = w, .height = h});
///
///   // per frame, inside View::paint():
///   {
///       GpuFrame frame(*device, "glow");
///       auto pass = frame.render_pass(GpuRenderPassDesc::for_target(*target, {0, 0, 0, 1}));
///       pass.set_pipeline(*pipeline);
///       pass.draw(3);
///       pass.end();
///       frame.submit();
///   }
///   draw_gpu_texture(canvas, target->color(), 0, 0, w, h);
///
/// The header names no Dawn type. Every object is a reference-counted handle
/// to state that lives in the implementation; copies share it.
///
/// Ownership and ordering rules (all load-bearing):
///   * `GpuSurface` owns the Dawn instance, device and queue. A `GpuDevice`
///     and every object created from it must be released before that
///     surface is destroyed: the native instance does not outlive its
///     surface, so a later call is a use-after-free in Dawn, not an error.
///   * Pulp never touches Skia Graphite's command encoders or the swapchain
///     texture. A `GpuFrame` records into its own encoder and submits to the
///     shared queue. Submit it before drawing its output: the queue runs
///     submissions in order, and Graphite submits the UI frame only when
///     the canvas frame ends, after `paint()` returns.
///   * All device work here runs under WebGPU error scopes, so a mistake in
///     a custom pass comes back as a `GpuError` instead of reaching the
///     device's uncaptured-error callback, which other device users (the
///     GPU-audio provider, for one) treat as poisoning their own work.
///   * Single-threaded: create, record and submit on one thread, normally
///     the UI thread that paints.
///   * Pipelines use WebGPU's automatic (`layout: auto`) bind group layouts;
///     `GpuDevice::create_bind_group()` reads group N's layout from the
///     pipeline.
///   * Device loss is reported, not hidden: every call checks the device's
///     lost state and fails with `GpuError::Kind::device_lost`. Recreate
///     the `GpuDevice` from the new surface after the host rebuilds it.

#include <pulp/platform/gpu_fwd.hpp>
#include <pulp/render/gpu_capabilities.hpp>
#include <pulp/render/wgsl_diagnostics.hpp>
#include <pulp/runtime/result.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::canvas {
class Canvas;
}

namespace pulp::render {

namespace detail {
struct GpuDeviceState;
struct GpuTextureState;
struct GpuBufferState;
struct GpuSamplerState;
struct GpuShaderModuleState;
struct GpuRenderPipelineState;
struct GpuComputePipelineState;
struct GpuBindGroupState;
struct GpuTargetState;
struct GpuFrameState;
struct GpuSubmissionState;
struct GpuAccess;
} // namespace detail

// ── Errors ───────────────────────────────────────────────────────────────

struct GpuError {
    enum class Kind : std::uint8_t {
        /// No device, or this build has no Dawn.
        unavailable,
        /// A descriptor or call the API rejects before reaching Dawn.
        invalid_argument,
        /// WGSL failed to compile; `diagnostics` holds the messages.
        shader_compilation,
        /// Dawn validation rejected the call.
        validation,
        out_of_memory,
        internal,
        device_lost,
        timeout,
    };

    Kind kind = Kind::internal;
    std::string message;
    /// Compiler messages for `shader_compilation` (and any warnings).
    std::vector<WgslDiagnostic> diagnostics;

    /// `kind: message`, followed by one formatted diagnostic per line.
    std::string describe() const;
};

std::string_view gpu_error_kind_name(GpuError::Kind kind);

template <typename T> using GpuResult = runtime::Result<T, GpuError>;
/// Success is a default-constructed value.
using GpuStatus = runtime::Result<void, GpuError>;

// ── Flags ────────────────────────────────────────────────────────────────

/// Texture usage bits. Values match WebGPU's `GPUTextureUsage`.
enum class GpuTextureUsage : std::uint32_t {
    none = 0,
    copy_src = 1u << 0,
    copy_dst = 1u << 1,
    sampled = 1u << 2,
    storage = 1u << 3,
    render_target = 1u << 4,
};

/// Buffer usage bits. Values match WebGPU's `GPUBufferUsage`.
enum class GpuBufferUsage : std::uint32_t {
    none = 0,
    map_read = 1u << 0,
    map_write = 1u << 1,
    copy_src = 1u << 2,
    copy_dst = 1u << 3,
    index = 1u << 4,
    vertex = 1u << 5,
    uniform = 1u << 6,
    storage = 1u << 7,
};

constexpr GpuTextureUsage operator|(GpuTextureUsage a, GpuTextureUsage b) {
    return static_cast<GpuTextureUsage>(static_cast<std::uint32_t>(a) |
                                        static_cast<std::uint32_t>(b));
}
constexpr bool has_usage(GpuTextureUsage set, GpuTextureUsage bit) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0;
}
constexpr GpuBufferUsage operator|(GpuBufferUsage a, GpuBufferUsage b) {
    return static_cast<GpuBufferUsage>(static_cast<std::uint32_t>(a) |
                                       static_cast<std::uint32_t>(b));
}
constexpr bool has_usage(GpuBufferUsage set, GpuBufferUsage bit) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0;
}

// ── Resources ────────────────────────────────────────────────────────────

struct GpuTextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GpuFormat format = GpuFormat::rgba8_unorm;
    /// The default suits a pass output that is composited and read back.
    GpuTextureUsage usage =
        GpuTextureUsage::render_target | GpuTextureUsage::sampled | GpuTextureUsage::copy_src;
    /// 1, or 4 for a multisampled attachment (see `GpuDeviceCapabilities`).
    std::uint32_t sample_count = 1;
    std::string label;
};

class GpuTexture {
  public:
    GpuTexture() = default;
    bool valid() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    GpuFormat format() const noexcept;
    GpuTextureUsage usage() const noexcept;
    std::uint32_t sample_count() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuTextureState> state_;
};

struct GpuBufferDesc {
    std::uint64_t size = 0;
    GpuBufferUsage usage = GpuBufferUsage::uniform | GpuBufferUsage::copy_dst;
    std::string label;
};

class GpuBuffer {
  public:
    GpuBuffer() = default;
    bool valid() const noexcept;
    std::uint64_t size() const noexcept;
    GpuBufferUsage usage() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuBufferState> state_;
};

struct GpuSamplerDesc {
    /// Linear filtering when true, nearest when false.
    bool linear = true;
    /// Repeat addressing when true, clamp-to-edge when false.
    bool repeat = false;
    std::string label;
};

class GpuSampler {
  public:
    GpuSampler() = default;
    bool valid() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuSamplerState> state_;
};

/// A compiled WGSL module. Creation fails with `shader_compilation` and the
/// compiler's line/column messages; warnings of a module that compiled stay
/// readable here.
class GpuShaderModule {
  public:
    GpuShaderModule() = default;
    bool valid() const noexcept;
    const std::vector<WgslDiagnostic>& warnings() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuShaderModuleState> state_;
};

// ── Pipelines ────────────────────────────────────────────────────────────

enum class GpuVertexFormat : std::uint8_t {
    float32,
    float32x2,
    float32x3,
    float32x4,
    uint32,
    sint32,
    unorm8x4,
};

struct GpuVertexAttribute {
    GpuVertexFormat format = GpuVertexFormat::float32x2;
    std::uint64_t offset = 0;
    std::uint32_t location = 0;
};

struct GpuVertexBufferLayout {
    std::uint64_t stride = 0;
    bool per_instance = false;
    std::vector<GpuVertexAttribute> attributes;
};

enum class GpuBlend : std::uint8_t {
    /// Write the fragment color as is.
    replace,
    /// Straight alpha: src * a + dst * (1 - a).
    alpha,
    /// Premultiplied alpha: src + dst * (1 - a). Skia composites premultiplied.
    premultiplied,
    /// src + dst.
    additive,
};

enum class GpuPrimitiveTopology : std::uint8_t {
    triangle_list,
    triangle_strip,
    line_list,
    line_strip,
    point_list,
};

enum class GpuCullMode : std::uint8_t { none, front, back };

enum class GpuCompare : std::uint8_t {
    never,
    less,
    less_equal,
    greater,
    greater_equal,
    equal,
    not_equal,
    always,
};

struct GpuColorTargetDesc {
    GpuFormat format = GpuFormat::rgba8_unorm;
    GpuBlend blend = GpuBlend::replace;
};

struct GpuDepthStateDesc {
    GpuFormat format = GpuFormat::depth24_plus;
    bool write = true;
    GpuCompare compare = GpuCompare::less;
};

struct GpuRenderPipelineDesc {
    GpuShaderModule module;
    std::string vertex_entry = "vs_main";
    /// Defaults to `module` when unset.
    std::optional<GpuShaderModule> fragment_module;
    std::string fragment_entry = "fs_main";
    std::vector<GpuVertexBufferLayout> vertex_buffers;
    std::vector<GpuColorTargetDesc> color_targets{GpuColorTargetDesc{}};
    std::optional<GpuDepthStateDesc> depth;
    GpuPrimitiveTopology topology = GpuPrimitiveTopology::triangle_list;
    GpuCullMode cull = GpuCullMode::none;
    std::uint32_t sample_count = 1;
    std::string label;
};

class GpuRenderPipeline {
  public:
    GpuRenderPipeline() = default;
    bool valid() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuRenderPipelineState> state_;
};

struct GpuComputePipelineDesc {
    GpuShaderModule module;
    std::string entry = "main";
    std::string label;
};

class GpuComputePipeline {
  public:
    GpuComputePipeline() = default;
    bool valid() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuComputePipelineState> state_;
};

/// One entry of a bind group: a buffer range, a texture or a sampler.
struct GpuBinding {
    enum class Kind : std::uint8_t { buffer, texture, sampler };

    std::uint32_t binding = 0;
    Kind kind = Kind::buffer;
    GpuBuffer buffer;
    std::uint64_t offset = 0;
    /// 0 binds from `offset` to the end of the buffer.
    std::uint64_t size = 0;
    GpuTexture texture;
    GpuSampler sampler;

    static GpuBinding of(std::uint32_t binding, GpuBuffer buffer, std::uint64_t offset = 0,
                         std::uint64_t size = 0);
    static GpuBinding of(std::uint32_t binding, GpuTexture texture);
    static GpuBinding of(std::uint32_t binding, GpuSampler sampler);
};

class GpuBindGroup {
  public:
    GpuBindGroup() = default;
    bool valid() const noexcept;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuBindGroupState> state_;
};

// ── Offscreen target ────────────────────────────────────────────────────

struct GpuTargetDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GpuFormat color_format = GpuFormat::rgba8_unorm;
    /// A depth attachment of this format when set.
    std::optional<GpuFormat> depth_format;
    std::string label;
};

/// Tightly packed pixels read back from a target, rows top to bottom.
struct GpuPixels {
    std::vector<std::uint8_t> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes_per_pixel = 0;
    GpuFormat format = GpuFormat::undefined;

    /// The texel at (x, y) as RGBA bytes. Valid for `rgba8_unorm`,
    /// `rgba8_unorm_srgb`, `bgra8_unorm` and `bgra8_unorm_srgb` (BGRA is
    /// swizzled to RGBA); all zero otherwise or out of range.
    std::array<std::uint8_t, 4> rgba8_at(std::uint32_t x, std::uint32_t y) const;
};

/// Offscreen color target (and optional depth) with CPU readback. The color
/// texture is a render target, sampled and copy-source, so it can be drawn
/// into, composited with `draw_gpu_texture()` and read back.
class GpuTarget {
  public:
    GpuTarget() = default;
    bool valid() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    GpuTexture color() const;
    std::optional<GpuTexture> depth() const;

    /// Copy the color texture to the CPU and wait for it (blocking, not for
    /// the audio thread). Submit the frame that drew the target first; work
    /// submitted earlier on the queue is complete when this returns.
    GpuResult<GpuPixels>
    read_pixels(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuTargetState> state_;
};

// ── Device ──────────────────────────────────────────────────────────────

/// Typed handle to the device a `GpuSurface` owns. Copies share it.
class GpuDevice {
  public:
    GpuDevice() = default;

    /// Wrap the surface's device, queue and instance. Fails with
    /// `unavailable` before the surface is initialized, or in a build
    /// without Dawn. The surface must outlive the returned device and every
    /// object created from it.
    static GpuResult<GpuDevice> from_surface(GpuSurface& surface);

    bool valid() const noexcept;
    /// True once Dawn reports the device lost. Non-blocking.
    bool is_lost() const;
    const GpuDeviceCapabilities& capabilities() const;

    GpuResult<GpuTexture> create_texture(const GpuTextureDesc& desc) const;
    GpuResult<GpuBuffer> create_buffer(const GpuBufferDesc& desc) const;
    GpuResult<GpuSampler> create_sampler(const GpuSamplerDesc& desc = {}) const;
    /// Compile WGSL. A compile error fails with `shader_compilation` and the
    /// compiler's line/column diagnostics; `label` names the source in them.
    GpuResult<GpuShaderModule> create_shader_module(std::string_view wgsl,
                                                    std::string_view label = {}) const;
    GpuResult<GpuRenderPipeline> create_render_pipeline(const GpuRenderPipelineDesc& desc) const;
    GpuResult<GpuComputePipeline> create_compute_pipeline(const GpuComputePipelineDesc& desc) const;
    /// Bind group `group` of a pipeline's automatic layout.
    GpuResult<GpuBindGroup> create_bind_group(const GpuRenderPipeline& pipeline,
                                              std::uint32_t group,
                                              const std::vector<GpuBinding>& bindings) const;
    GpuResult<GpuBindGroup> create_bind_group(const GpuComputePipeline& pipeline,
                                              std::uint32_t group,
                                              const std::vector<GpuBinding>& bindings) const;
    GpuResult<GpuTarget> create_target(const GpuTargetDesc& desc) const;

    /// Queue a buffer write. It executes before any later submission, so
    /// write uniforms first and then submit the frame that reads them.
    GpuStatus write_buffer(const GpuBuffer& buffer, std::uint64_t offset, const void* data,
                           std::size_t size) const;

    /// Run pending Dawn callbacks once (submission status, readback).
    void process_events() const;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuDeviceState> state_;
};

// ── Recording ───────────────────────────────────────────────────────────

struct GpuColorAttachment {
    GpuTexture texture;
    /// Clear to this RGBA before drawing; load the existing contents when unset.
    std::optional<std::array<float, 4>> clear;
    /// Resolve a multisampled `texture` into this single-sample texture.
    std::optional<GpuTexture> resolve;
};

struct GpuRenderPassDesc {
    std::vector<GpuColorAttachment> colors;
    std::optional<GpuTexture> depth;
    /// Clear depth to this value; load when unset.
    std::optional<float> depth_clear = 1.0f;
    std::string label;

    /// The target's color (cleared to `clear`) and its depth, if any.
    static GpuRenderPassDesc for_target(
        const GpuTarget& target,
        std::optional<std::array<float, 4>> clear = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
};

class GpuFrame;

/// An open render pass. Ends on `end()` or destruction. Calls after the
/// pass ended, or on a pass that failed to begin, are recorded as errors on
/// the owning frame and reported by its `submit()`.
class GpuRenderPass {
  public:
    GpuRenderPass(GpuRenderPass&&) noexcept;
    GpuRenderPass& operator=(GpuRenderPass&&) noexcept;
    GpuRenderPass(const GpuRenderPass&) = delete;
    GpuRenderPass& operator=(const GpuRenderPass&) = delete;
    ~GpuRenderPass();

    bool valid() const noexcept;
    void set_pipeline(const GpuRenderPipeline& pipeline);
    void set_bind_group(std::uint32_t index, const GpuBindGroup& group);
    void set_vertex_buffer(std::uint32_t slot, const GpuBuffer& buffer, std::uint64_t offset = 0);
    /// 32-bit indices when `uint32_indices`, 16-bit otherwise.
    void set_index_buffer(const GpuBuffer& buffer, bool uint32_indices = true,
                          std::uint64_t offset = 0);
    void set_viewport(float x, float y, float width, float height, float min_depth = 0.0f,
                      float max_depth = 1.0f);
    void set_scissor(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1,
              std::uint32_t first_vertex = 0, std::uint32_t first_instance = 0);
    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count = 1,
                      std::uint32_t first_index = 0, std::int32_t base_vertex = 0,
                      std::uint32_t first_instance = 0);
    void end();

  private:
    friend class GpuFrame;
    struct Impl;
    explicit GpuRenderPass(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// An open compute pass. Same rules as `GpuRenderPass`.
class GpuComputePass {
  public:
    GpuComputePass(GpuComputePass&&) noexcept;
    GpuComputePass& operator=(GpuComputePass&&) noexcept;
    GpuComputePass(const GpuComputePass&) = delete;
    GpuComputePass& operator=(const GpuComputePass&) = delete;
    ~GpuComputePass();

    bool valid() const noexcept;
    void set_pipeline(const GpuComputePipeline& pipeline);
    void set_bind_group(std::uint32_t index, const GpuBindGroup& group);
    void dispatch(std::uint32_t x, std::uint32_t y = 1, std::uint32_t z = 1);
    void end();

  private:
    friend class GpuFrame;
    struct Impl;
    explicit GpuComputePass(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

enum class GpuSubmissionStatus : std::uint8_t { pending, succeeded, failed };

/// Completion of one submitted frame. Querying never blocks unless `wait()`
/// is called.
class GpuSubmission {
  public:
    GpuSubmission() = default;
    /// Current status without running callbacks.
    GpuSubmissionStatus status() const;
    /// Run pending callbacks once, then report.
    GpuSubmissionStatus poll() const;
    /// Run callbacks until terminal or `timeout`; `pending` on timeout.
    GpuSubmissionStatus wait(std::chrono::milliseconds timeout) const;
    /// The failure, once `status()` is `failed`.
    std::optional<GpuError> error() const;

  private:
    friend struct detail::GpuAccess;
    std::shared_ptr<detail::GpuSubmissionState> state_;
};

/// One command encoder's worth of passes, submitted once.
///
/// The frame opens WebGPU error scopes when it begins and closes them at
/// submit, so every Dawn error raised while recording is attributed to this
/// frame. Keep a frame's lifetime to its own recording: do not draw on the
/// Skia canvas while a frame is open. One pass may be open at a time.
///
/// Destroying an unsubmitted frame submits it. A failure there is logged,
/// since a destructor cannot return it; call `submit()` to see it.
class GpuFrame {
  public:
    explicit GpuFrame(const GpuDevice& device, std::string_view label = {});
    GpuFrame(GpuFrame&&) noexcept;
    GpuFrame& operator=(GpuFrame&&) = delete;
    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;
    ~GpuFrame();

    GpuRenderPass render_pass(const GpuRenderPassDesc& desc);
    GpuComputePass compute_pass(std::string_view label = {});

    /// Finish the encoder and submit it. Returns the recording, validation
    /// and device errors of this frame, or a handle that reports GPU-side
    /// completion. A second call fails with `invalid_argument`.
    GpuResult<GpuSubmission> submit();
    bool submitted() const noexcept;

  private:
    std::shared_ptr<detail::GpuFrameState> state_;
};

// ── Compositing ─────────────────────────────────────────────────────────

/// Draw `texture` into the canvas rectangle (x, y, w, h).
///
/// Requirements:
///   * the canvas is a GPU (Skia Graphite) canvas; any other canvas, such as
///     a recording or CPU canvas, fails with `unavailable`. It must draw on
///     the device the texture was created on, the one the `GpuSurface`
///     behind `GpuDevice::from_surface()` owns;
///   * the texture is single-sample, has `sampled` usage and is
///     `rgba8_unorm`, `bgra8_unorm` or `rgba16_float` (checked);
///   * no unsubmitted `GpuFrame` writes it (checked): submit first, so the
///     queue runs the pass before Graphite samples the result.
///
/// The texture's contents are treated as premultiplied alpha. The draw holds
/// a reference to the texture until Graphite releases the wrapped image, so
/// the caller may drop its own handle after this returns.
GpuStatus draw_gpu_texture(canvas::Canvas& canvas, const GpuTexture& texture, float x, float y,
                           float w, float h);

} // namespace pulp::render
