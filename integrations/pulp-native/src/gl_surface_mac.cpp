// GlSurface on macOS: an offscreen CGL context drawing into a framebuffer
// object, for tests and offline capture. There is no windowed GL backend on
// macOS: editors there use MetalSurface.

#define GL_SILENCE_DEPRECATION 1

#include "gl_surface_impl.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>

#include <cstdio>
#include <string>

namespace pulp::native {

namespace {

std::string gl_string(GLenum name) {
    const auto* s = reinterpret_cast<const char*>(glGetString(name));
    return s != nullptr ? std::string(s) : std::string();
}

// The CGL profile that can satisfy `request`, or 0 when macOS cannot: it
// offers a legacy 2.1 context and a 3.2+/4.1 core context, and no
// compatibility profile above 2.1.
int cgl_profile_for(const GlContextRequest& request) {
    if (request.major <= 2)
        return kCGLOGLPVersion_Legacy;
    if (request.profile == GlProfile::compatibility)
        return 0;
    return request.major >= 4 ? kCGLOGLPVersion_GL4_Core : kCGLOGLPVersion_GL3_Core;
}

struct OffscreenCgl final : GlSurface::Impl {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CGLContextObj context = nullptr;
    GLuint fbo = 0;
    GLuint color = 0;

    bool offscreen() const override {
        return true;
    }

    bool try_request(const GlSurfaceOptions& options, std::size_t index, GlContextReport& out) {
        const GlContextRequest& request = options.requests[index];
        const int profile = cgl_profile_for(request);
        if (profile == 0)
            return false;
        CGLPixelFormatAttribute attrs[16];
        int n = 0;
        attrs[n++] = kCGLPFAOpenGLProfile;
        attrs[n++] = static_cast<CGLPixelFormatAttribute>(profile);
        attrs[n++] = kCGLPFAColorSize;
        attrs[n++] = static_cast<CGLPixelFormatAttribute>(24);
        attrs[n++] = kCGLPFAAlphaSize;
        attrs[n++] = static_cast<CGLPixelFormatAttribute>(8);
        attrs[n++] = kCGLPFAAllowOfflineRenderers;
        if (request.samples > 0) {
            attrs[n++] = kCGLPFAMultisample;
            attrs[n++] = kCGLPFASampleBuffers;
            attrs[n++] = static_cast<CGLPixelFormatAttribute>(1);
            attrs[n++] = kCGLPFASamples;
            attrs[n++] = static_cast<CGLPixelFormatAttribute>(request.samples);
        }
        attrs[n] = static_cast<CGLPixelFormatAttribute>(0);

        CGLPixelFormatObj format = nullptr;
        GLint count = 0;
        if (CGLChoosePixelFormat(attrs, &format, &count) != kCGLNoError || format == nullptr)
            return false;
        CGLContextObj created = nullptr;
        const CGLError error = CGLCreateContext(format, nullptr, &created);
        CGLReleasePixelFormat(format);
        if (error != kCGLNoError || created == nullptr)
            return false;
        CGLSetCurrentContext(created);

        GlContextReport report;
        report.version = gl_string(GL_VERSION);
        report.renderer = gl_string(GL_RENDERER);
        report.vendor = gl_string(GL_VENDOR);
        std::sscanf(report.version.c_str(), "%d.%d", &report.major, &report.minor);
        report.profile = profile == kCGLOGLPVersion_Legacy ? GlProfile::any : GlProfile::core;
        GLint max_texture = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
        report.max_texture_size = max_texture;
        report.npot_textures = report.major >= 2;  // core in 2.0 and later
        report.software = is_software_renderer(report.renderer);

        // The context must honour this request itself; anything less falls
        // through to the next entry instead of being reported as this one.
        if (!first_honoured_request({request}, report.major, report.minor, report.profile)) {
            CGLSetCurrentContext(nullptr);
            CGLDestroyContext(created);
            return false;
        }
        report.request_index = index;

        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, static_cast<GLsizei>(width),
                              static_cast<GLsizei>(height));
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            CGLSetCurrentContext(nullptr);
            CGLDestroyContext(created);
            fbo = color = 0;
            return false;
        }
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        GLint interval = options.swap_interval;
        CGLSetParameter(created, kCGLCPSwapInterval, &interval);

        context = created;
        out = report;
        return true;
    }

    bool create_context(const GlSurfaceOptions& options, GlContextReport& out) override {
        for (std::size_t i = 0; i < options.requests.size(); ++i)
            if (try_request(options, i, out))
                return true;
        return false;
    }

    bool make_current() override {
        if (context == nullptr || CGLSetCurrentContext(context) != kCGLNoError)
            return false;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        return true;
    }

    void release_current() override {
        if (CGLGetCurrentContext() == context)
            CGLSetCurrentContext(nullptr);
    }

    bool swap() override {
        if (CGLGetCurrentContext() != context)
            return false;
        glFlush();
        return true;
    }

    bool set_interval(int interval) override {
        GLint value = interval;
        return CGLSetParameter(context, kCGLCPSwapInterval, &value) == kCGLNoError;
    }

    void* proc(const char* name) const override {
        return dlsym(RTLD_DEFAULT, name);
    }

    std::uint32_t framebuffer() const override {
        return fbo;
    }

    void destroy_context() override {
        // Destroying the context frees its framebuffer and renderbuffer; the
        // UI thread never makes it current to delete them one by one.
        if (context != nullptr)
            CGLDestroyContext(context);
        context = nullptr;
        fbo = color = 0;
    }
};

} // namespace

std::unique_ptr<GlSurface::Impl> make_windowed_gl_impl(const GlSurfaceOptions&) {
    return nullptr;
}

std::unique_ptr<GlSurface::Impl> make_offscreen_gl_impl(std::uint32_t width, std::uint32_t height,
                                                        const GlSurfaceOptions& options) {
    auto impl = std::make_unique<OffscreenCgl>();
    impl->options = options;
    impl->width = width;
    impl->height = height;
    {
        std::lock_guard<std::mutex> lock(impl->geometry_mutex);
        impl->geometry.generation = 1;
        impl->geometry.width = width;
        impl->geometry.height = height;
        impl->geometry.scale = 1.0;
        impl->geometry.drawable_width = width;
        impl->geometry.drawable_height = height;
        impl->geometry.in_window = false;
    }
    return impl;
}

} // namespace pulp::native
