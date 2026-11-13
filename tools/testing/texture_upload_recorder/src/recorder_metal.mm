// Metal hooks for TextureUploadRecorder. Each wrapped method records the upload
// and then calls the original implementation with the original arguments.
//
// Built without ARC (see CMakeLists.txt): hooked_new_texture stands in for a
// `new...` method and must hand the original's +1 result straight back, which
// a plain C function under ARC would return at +0.

#if __has_feature(objc_arc)
#error "recorder_metal.mm must be compiled with -fno-objc-arc"
#endif

#include "recorder_core.hpp"

#import <Metal/Metal.h>
#include <dlfcn.h>
#include <objc/runtime.h>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::testing::detail {

namespace {

struct FormatInfo {
    const char* name;
    std::uint32_t bytes_per_pixel;
};

FormatInfo format_info(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatA8Unorm:
        return {"A8Unorm", 1};
    case MTLPixelFormatR8Unorm:
        return {"R8Unorm", 1};
    case MTLPixelFormatR8Unorm_sRGB:
        return {"R8Unorm_sRGB", 1};
    case MTLPixelFormatR8Uint:
        return {"R8Uint", 1};
    case MTLPixelFormatRG8Unorm:
        return {"RG8Unorm", 2};
    case MTLPixelFormatR16Float:
        return {"R16Float", 2};
    case MTLPixelFormatR16Unorm:
        return {"R16Unorm", 2};
    case MTLPixelFormatRGBA8Unorm:
        return {"RGBA8Unorm", 4};
    case MTLPixelFormatRGBA8Unorm_sRGB:
        return {"RGBA8Unorm_sRGB", 4};
    case MTLPixelFormatBGRA8Unorm:
        return {"BGRA8Unorm", 4};
    case MTLPixelFormatBGRA8Unorm_sRGB:
        return {"BGRA8Unorm_sRGB", 4};
    case MTLPixelFormatRGB10A2Unorm:
        return {"RGB10A2Unorm", 4};
    case MTLPixelFormatR32Float:
        return {"R32Float", 4};
    case MTLPixelFormatRG16Float:
        return {"RG16Float", 4};
    case MTLPixelFormatRGBA16Float:
        return {"RGBA16Float", 8};
    case MTLPixelFormatRGBA16Unorm:
        return {"RGBA16Unorm", 8};
    case MTLPixelFormatRG32Float:
        return {"RG32Float", 8};
    case MTLPixelFormatRGBA32Float:
        return {"RGBA32Float", 16};
    default:
        return {nullptr, 0};
    }
}

struct TextureInfo {
    std::uint64_t id = 0;
};

struct Hooks {
    std::mutex mutex;
    // The original implementation per hooked class and selector.
    std::map<std::pair<Class, SEL>, IMP> originals;
    std::set<Class> texture_classes;
    std::unordered_map<const void*, TextureInfo> textures;
    std::uint64_t next_texture_id = 1;
    bool installed = false;
};

Hooks& hooks() {
    static Hooks h;
    return h;
}

// The original IMP for `self`'s class or the nearest hooked superclass.
IMP original_for(id self, SEL sel) {
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    for (Class cls = object_getClass(self); cls; cls = class_getSuperclass(cls)) {
        const auto it = h.originals.find({cls, sel});
        if (it != h.originals.end())
            return it->second;
    }
    return nullptr;
}

// Replaces `sel` on `cls` with `replacement`, keeping the original for
// original_for(). An inherited method is overridden on `cls` only.
void hook(Class cls, SEL sel, IMP replacement) {
    if (!cls)
        return;
    Method method = class_getInstanceMethod(cls, sel);
    if (!method)
        return;
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    if (h.originals.count({cls, sel}))
        return;
    IMP original = method_getImplementation(method);
    if (original == replacement)
        return;
    if (!class_addMethod(cls, sel, replacement, method_getTypeEncoding(method)))
        original = method_setImplementation(class_getInstanceMethod(cls, sel), replacement);
    h.originals[{cls, sel}] = original;
}

std::string caller_image(const void* address) {
    Dl_info info{};
    if (address && dladdr(address, &info) && info.dli_fname)
        return info.dli_fname;
    return {};
}

std::uint64_t texture_id(id<MTLTexture> texture) {
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    const auto it = h.textures.find((__bridge const void*)texture);
    return it == h.textures.end() ? 0 : it->second.id;
}

void describe(UploadDescription& u, id<MTLTexture> texture) {
    const auto info = format_info(texture.pixelFormat);
    u.pixel_format_value = static_cast<std::uint32_t>(texture.pixelFormat);
    u.pixel_format =
        info.name ? info.name : "MTLPixelFormat(" + std::to_string(u.pixel_format_value) + ")";
    u.bytes_per_pixel = info.bytes_per_pixel;
    u.texture_id = texture_id(texture);
}

void hook_texture_class(Class cls);

// Wrapped methods can call each other inside Metal (one replaceRegion form can
// forward to the other), so only the outermost wrapped call on a thread
// records; a nested one passes straight through.
thread_local int t_depth = 0;

struct Nesting {
    const bool outermost = t_depth++ == 0;
    ~Nesting() {
        --t_depth;
    }
};

// MTLDevice newTextureWithDescriptor:
id hooked_new_texture(id self, SEL sel, MTLTextureDescriptor* descriptor) {
    using Fn = id (*)(id, SEL, MTLTextureDescriptor*);
    const auto original = reinterpret_cast<Fn>(original_for(self, sel));
    id texture = original ? original(self, sel, descriptor) : nil;
    if (texture) {
        hook_texture_class(object_getClass(texture));
        auto& h = hooks();
        std::lock_guard lock(h.mutex);
        h.textures[(__bridge const void*)texture] = TextureInfo{h.next_texture_id++};
    }
    return texture;
}

// MTLTexture replaceRegion:mipmapLevel:withBytes:bytesPerRow:
void hooked_replace_region(id self, SEL sel, MTLRegion region, NSUInteger level, const void* bytes,
                           NSUInteger bytes_per_row) {
    const Nesting nesting;
    if (nesting.outermost && recording()) {
        UploadDescription u;
        u.api = "metal";
        u.entry = "replaceRegion";
        describe(u, (id<MTLTexture>)self);
        u.x = static_cast<std::uint32_t>(region.origin.x);
        u.y = static_cast<std::uint32_t>(region.origin.y);
        u.z = static_cast<std::uint32_t>(region.origin.z);
        u.width = static_cast<std::uint32_t>(region.size.width);
        u.height = static_cast<std::uint32_t>(region.size.height);
        u.depth = static_cast<std::uint32_t>(region.size.depth);
        u.mip = static_cast<std::uint32_t>(level);
        u.data = static_cast<const std::uint8_t*>(bytes);
        u.source_bytes_per_row = bytes_per_row;
        u.caller_image = caller_image(__builtin_return_address(0));
        record_upload(u);
    }
    using Fn = void (*)(id, SEL, MTLRegion, NSUInteger, const void*, NSUInteger);
    if (const auto original = reinterpret_cast<Fn>(original_for(self, sel)))
        original(self, sel, region, level, bytes, bytes_per_row);
}

// MTLTexture replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:
void hooked_replace_region_slice(id self, SEL sel, MTLRegion region, NSUInteger level,
                                 NSUInteger slice, const void* bytes, NSUInteger bytes_per_row,
                                 NSUInteger bytes_per_image) {
    const Nesting nesting;
    if (nesting.outermost && recording()) {
        UploadDescription u;
        u.api = "metal";
        u.entry = "replaceRegion:slice";
        describe(u, (id<MTLTexture>)self);
        u.x = static_cast<std::uint32_t>(region.origin.x);
        u.y = static_cast<std::uint32_t>(region.origin.y);
        u.z = static_cast<std::uint32_t>(region.origin.z);
        u.width = static_cast<std::uint32_t>(region.size.width);
        u.height = static_cast<std::uint32_t>(region.size.height);
        u.depth = static_cast<std::uint32_t>(region.size.depth);
        u.mip = static_cast<std::uint32_t>(level);
        u.slice = static_cast<std::uint32_t>(slice);
        u.data = static_cast<const std::uint8_t*>(bytes);
        u.source_bytes_per_row = bytes_per_row;
        u.source_bytes_per_image = bytes_per_image;
        u.caller_image = caller_image(__builtin_return_address(0));
        record_upload(u);
    }
    using Fn =
        void (*)(id, SEL, MTLRegion, NSUInteger, NSUInteger, const void*, NSUInteger, NSUInteger);
    if (const auto original = reinterpret_cast<Fn>(original_for(self, sel)))
        original(self, sel, region, level, slice, bytes, bytes_per_row, bytes_per_image);
}

void record_blit(const char* entry, id<MTLBuffer> buffer, NSUInteger offset,
                 NSUInteger bytes_per_row, NSUInteger bytes_per_image, MTLSize size,
                 id<MTLTexture> texture, NSUInteger slice, NSUInteger level, MTLOrigin origin,
                 const void* return_address) {
    UploadDescription u;
    u.api = "metal";
    u.entry = entry;
    describe(u, texture);
    u.x = static_cast<std::uint32_t>(origin.x);
    u.y = static_cast<std::uint32_t>(origin.y);
    u.z = static_cast<std::uint32_t>(origin.z);
    u.width = static_cast<std::uint32_t>(size.width);
    u.height = static_cast<std::uint32_t>(size.height);
    u.depth = static_cast<std::uint32_t>(size.depth);
    u.mip = static_cast<std::uint32_t>(level);
    u.slice = static_cast<std::uint32_t>(slice);
    // Private-storage buffers have no CPU contents; the record keeps the
    // geometry and leaves the bytes empty.
    const auto* contents = static_cast<const std::uint8_t*>(buffer.contents);
    u.data = contents ? contents + offset : nullptr;
    u.source_bytes_per_row = bytes_per_row;
    u.source_bytes_per_image = bytes_per_image;
    u.caller_image = caller_image(return_address);
    record_upload(u);
}

// MTLBlitCommandEncoder copyFromBuffer:...toTexture:destinationSlice:destinationLevel:destinationOrigin:
void hooked_blit(id self, SEL sel, id<MTLBuffer> buffer, NSUInteger offset,
                 NSUInteger bytes_per_row, NSUInteger bytes_per_image, MTLSize size,
                 id<MTLTexture> texture, NSUInteger slice, NSUInteger level, MTLOrigin origin) {
    const Nesting nesting;
    if (nesting.outermost && recording())
        record_blit("copyFromBuffer", buffer, offset, bytes_per_row, bytes_per_image, size, texture,
                    slice, level, origin, __builtin_return_address(0));
    using Fn = void (*)(id, SEL, id<MTLBuffer>, NSUInteger, NSUInteger, NSUInteger, MTLSize,
                        id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin);
    if (const auto original = reinterpret_cast<Fn>(original_for(self, sel)))
        original(self, sel, buffer, offset, bytes_per_row, bytes_per_image, size, texture, slice,
                 level, origin);
}

// The same with an options: argument.
void hooked_blit_options(id self, SEL sel, id<MTLBuffer> buffer, NSUInteger offset,
                         NSUInteger bytes_per_row, NSUInteger bytes_per_image, MTLSize size,
                         id<MTLTexture> texture, NSUInteger slice, NSUInteger level,
                         MTLOrigin origin, MTLBlitOption options) {
    const Nesting nesting;
    if (nesting.outermost && recording())
        record_blit("copyFromBuffer:options", buffer, offset, bytes_per_row, bytes_per_image, size,
                    texture, slice, level, origin, __builtin_return_address(0));
    using Fn = void (*)(id, SEL, id<MTLBuffer>, NSUInteger, NSUInteger, NSUInteger, MTLSize,
                        id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin, MTLBlitOption);
    if (const auto original = reinterpret_cast<Fn>(original_for(self, sel)))
        original(self, sel, buffer, offset, bytes_per_row, bytes_per_image, size, texture, slice,
                 level, origin, options);
}

void hook_texture_class(Class cls) {
    {
        auto& h = hooks();
        std::lock_guard lock(h.mutex);
        if (!h.texture_classes.insert(cls).second)
            return;
    }
    hook(cls, @selector(replaceRegion:mipmapLevel:withBytes:bytesPerRow:),
         (IMP)hooked_replace_region);
    hook(cls, @selector(replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:),
         (IMP)hooked_replace_region_slice);
}

} // namespace

bool install_platform_hooks(std::string* error) {
    auto& h = hooks();
    {
        std::lock_guard lock(h.mutex);
        if (h.installed)
            return true;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            if (error)
                *error = "no Metal device";
            return false;
        }

        // Hook the texture classes the device hands out for each storage mode
        // up front, so textures made by paths not wrapped here are covered too.
        for (const MTLStorageMode mode : {MTLStorageModeShared, MTLStorageModePrivate,
#if TARGET_OS_OSX
                                          MTLStorageModeManaged
#else
                                          MTLStorageModeShared
#endif
             }) {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:1
                                                                  height:1
                                                               mipmapped:NO];
            d.storageMode = mode;
            if (id<MTLTexture> probe = [device newTextureWithDescriptor:d]) {
                hook_texture_class(object_getClass(probe));
                [probe release];
            }
        }

        // After the probes, so they take no texture ids.
        hook(object_getClass(device), @selector(newTextureWithDescriptor:),
             (IMP)hooked_new_texture);

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        if (blit) {
            const Class cls = object_getClass(blit);
            hook(cls,
                 @selector(copyFromBuffer:
                             sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture
                                         :destinationSlice:destinationLevel:destinationOrigin:),
                 (IMP)hooked_blit);
            hook(cls,
                 @selector
                 (copyFromBuffer:
                     sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture
                                 :destinationSlice:destinationLevel:destinationOrigin:options:),
                 (IMP)hooked_blit_options);
            [blit endEncoding];
        }
        [queue release];
        [device release];
    }
    std::lock_guard lock(h.mutex);
    h.installed = true;
    return true;
}

} // namespace pulp::testing::detail
