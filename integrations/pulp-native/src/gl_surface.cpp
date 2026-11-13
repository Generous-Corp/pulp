// GlSurface: the platform-neutral half. Request selection, teardown order and
// the GlContext entry points; each backend (gl_surface_*.cpp) supplies the
// context and window work through GlSurface::Impl.

#include <pulp/native/gl_surface.hpp>

#include "gl_surface_impl.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace pulp::native {

namespace {

// `samples` is empty when the caller's backend chose the pixel format from the
// request itself, so the sample count needs no separate check.
std::optional<std::size_t> first_honoured(const std::vector<GlContextRequest>& requests, int major,
                                          int minor, GlProfile profile,
                                          std::optional<int> samples) {
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        const bool version_ok = major > r.major || (major == r.major && minor >= r.minor);
        const bool profile_ok = r.profile == GlProfile::any || r.profile == profile;
        const bool samples_ok = !samples.has_value() || r.samples <= 0 || *samples >= r.samples;
        if (version_ok && profile_ok && samples_ok)
            return i;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::size_t> first_honoured_request(const std::vector<GlContextRequest>& requests,
                                                  int major, int minor, GlProfile profile) {
    return first_honoured(requests, major, minor, profile, std::nullopt);
}

std::optional<std::size_t> first_honoured_request(const std::vector<GlContextRequest>& requests,
                                                  int major, int minor, GlProfile profile,
                                                  int samples) {
    return first_honoured(requests, major, minor, profile, samples);
}

std::vector<GlProfile> context_profile_attempts(GlProfile profile) {
    if (profile == GlProfile::any)
        return {GlProfile::core, GlProfile::compatibility};
    return {profile};
}

bool is_usable_wgl_proc(std::intptr_t address) noexcept {
    return address != 0 && address != 1 && address != 2 && address != 3 && address != -1;
}

bool is_software_renderer(const std::string& renderer) {
    std::string lower(renderer);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* name : {"llvmpipe", "softpipe", "gdi generic", "software renderer", "swrast"})
        if (lower.find(name) != std::string::npos)
            return true;
    return false;
}

const char* to_string(GlProfile profile) noexcept {
    switch (profile) {
    case GlProfile::core:
        return "core";
    case GlProfile::compatibility:
        return "compatibility";
    case GlProfile::any:
        return "any";
    }
    return "unknown";
}

// ── GlContext ───────────────────────────────────────────────────────────────

bool GlContext::make_current() {
    auto& impl = *owner_.impl_;
    if (impl.tearing_down.load())
        return false;
    if (!impl.created) {
        if (!impl.create_context(impl.options, impl.report))
            return false;
        impl.created = true;
        return true; // a backend leaves a new context current
    }
    return impl.make_current();
}

void GlContext::release_current() {
    auto& impl = *owner_.impl_;
    if (impl.created)
        impl.release_current();
}

bool GlContext::swap_buffers() {
    auto& impl = *owner_.impl_;
    if (impl.tearing_down.load() || !impl.created)
        return false;
    return impl.swap();
}

bool GlContext::set_swap_interval(int interval) {
    auto& impl = *owner_.impl_;
    if (!impl.created || (interval != 0 && interval != 1))
        return false;
    return impl.set_interval(interval);
}

void* GlContext::get_proc_address(const char* name) const {
    return name != nullptr ? owner_.impl_->proc(name) : nullptr;
}

const GlContextReport& GlContext::report() const noexcept {
    return owner_.impl_->report;
}

std::uint32_t GlContext::framebuffer() const noexcept {
    return owner_.impl_->framebuffer();
}

// ── GlSurface ───────────────────────────────────────────────────────────────

GlSurface::GlSurface(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GlSurface::~GlSurface() {
    begin_teardown();
    impl_->destroy_window();
}

void GlSurface::begin_teardown() {
    if (impl_->torn_down)
        return;
    impl_->tearing_down = true;
    // The client stops using the context before this returns.
    if (impl_->release_handler)
        impl_->release_handler();
    if (impl_->created)
        impl_->destroy_context();
    impl_->created = false;
    impl_->torn_down = true;
}

bool GlSurface::attach(void* parent) {
    if (parent == nullptr || impl_->invalidated || impl_->offscreen() || impl_->attached())
        return false;
    impl_->tearing_down = false;
    impl_->torn_down = false;
    return impl_->attach(parent);
}

void GlSurface::detach() {
    if (!impl_->attached())
        return;
    begin_teardown();
    impl_->detach_window();
}

void GlSurface::invalidate() {
    if (impl_->invalidated)
        return;
    impl_->invalidated = true;
    begin_teardown();
    impl_->forget_window();
}

bool GlSurface::attached() const noexcept {
    return impl_->attached();
}

bool GlSurface::offscreen() const noexcept {
    return impl_->offscreen();
}

void* GlSurface::native_window() const noexcept {
    return impl_->native_window();
}

std::string GlSurface::window_class_name() {
    return windowed_gl_class_name();
}

GlContext& GlSurface::context() noexcept {
    return context_;
}

SurfaceGeometry GlSurface::geometry() const {
    std::lock_guard<std::mutex> lock(impl_->geometry_mutex);
    return impl_->geometry;
}

void GlSurface::set_listener(std::function<void(const SurfaceEvent&)> listener) {
    impl_->listener = std::move(listener);
}

void GlSurface::set_release_handler(std::function<void()> handler) {
    impl_->release_handler = std::move(handler);
}

std::unique_ptr<GlSurface> GlSurface::create(const GlSurfaceOptions& options) {
    auto impl = make_windowed_gl_impl(options);
    if (!impl)
        return nullptr;
    return std::unique_ptr<GlSurface>(new GlSurface(std::move(impl)));
}

std::unique_ptr<GlSurface> GlSurface::create_offscreen(std::uint32_t width, std::uint32_t height,
                                                       const GlSurfaceOptions& options) {
    if (width == 0 || height == 0)
        return nullptr;
    auto impl = make_offscreen_gl_impl(width, height, options);
    if (!impl)
        return nullptr;
    return std::unique_ptr<GlSurface>(new GlSurface(std::move(impl)));
}

} // namespace pulp::native
