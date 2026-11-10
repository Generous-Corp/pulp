// macOS backends for the asynchronous file dialogs: NSOpenPanel and
// NSSavePanel as a sheet on the parent's window, or free standing when the
// parent is not in a window.

#include "../../src/dialog_internal.hpp"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <memory>

namespace pulp::platform::detail {

namespace {

NSString* ns(const std::string& text) {
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    return s != nil ? s : @"";
}

struct Retained {
    id object = nil;
    explicit Retained(id o) : object([o retain]) {}
    ~Retained() {
        [object release];
    }
    Retained(const Retained&) = delete;
    Retained& operator=(const Retained&) = delete;
};

NSArray<UTType*>* content_types(const std::vector<FileFilter>& filters) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const auto& filter : filters) {
        const std::string& exts = filter.extensions;
        std::size_t pos = 0;
        while (pos < exts.size()) {
            const auto sep = exts.find(';', pos);
            std::string ext =
                exts.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
            if (!ext.empty() && ext.front() == '.')
                ext.erase(0, 1);
            if (!ext.empty()) {
                UTType* type = [UTType typeWithFilenameExtension:ns(ext)];
                if (type != nil && ![types containsObject:type])
                    [types addObject:type];
            }
            pos = sep == std::string::npos ? exts.size() : sep + 1;
        }
    }
    return types;
}

NSWindow* sheet_parent(NativeViewHandle parent) {
    NSView* view = (__bridge NSView*)parent;
    NSWindow* window = view != nil ? view.window : nil;
    return window != nil && window.attachedSheet == nil ? window : nil;
}

void begin(NSSavePanel* panel, NSWindow* window, void (^handler)(NSModalResponse)) {
    if (window != nil)
        [panel beginSheetModalForWindow:window completionHandler:handler];
    else
        [panel beginWithCompletionHandler:handler];
}

} // namespace

std::vector<std::string> native_content_type_ids(const std::vector<FileFilter>& filters) {
    std::vector<std::string> ids;
    @autoreleasepool {
        for (UTType* type in content_types(filters))
            ids.emplace_back(type.identifier.UTF8String);
    }
    return ids;
}

std::function<void()> present_native_open(NativeViewHandle parent,
                                          const FileDialog::OpenRequest& request,
                                          std::function<void(std::vector<std::string>)> respond) {
    if (!request.choose_files && !request.choose_directories)
        return {};
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    auto holder = std::make_shared<Retained>(panel);
    panel.title = ns(request.title);
    panel.canChooseFiles = request.choose_files ? YES : NO;
    panel.canChooseDirectories = request.choose_directories ? YES : NO;
    panel.allowsMultipleSelection = request.multiple ? YES : NO;
    NSArray<UTType*>* types = content_types(request.filters);
    if (types.count > 0)
        panel.allowedContentTypes = types;
    if (!request.default_path.empty())
        panel.directoryURL = [NSURL fileURLWithPath:ns(request.default_path)];

    auto box = std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(respond));
    begin(panel, sheet_parent(parent), ^(NSModalResponse code) {
      std::vector<std::string> paths;
      if (code == NSModalResponseOK) {
          for (NSURL* url in static_cast<NSOpenPanel*>(holder->object).URLs)
              if (url.path != nil)
                  paths.emplace_back(url.path.UTF8String);
      }
      (*box)(std::move(paths));
    });
    return [holder] { [static_cast<NSSavePanel*>(holder->object) cancel:nil]; };
}

std::function<void()>
present_native_save(NativeViewHandle parent, const FileDialog::SaveRequest& request,
                    std::optional<std::string> copy_source,
                    std::function<void(std::optional<std::string>, std::string)> respond) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    auto holder = std::make_shared<Retained>(panel);
    panel.title = ns(request.title);
    NSArray<UTType*>* types = content_types(request.filters);
    if (types.count > 0)
        panel.allowedContentTypes = types;
    if (!request.default_path.empty())
        panel.directoryURL = [NSURL fileURLWithPath:ns(request.default_path)];
    if (!request.default_name.empty())
        panel.nameFieldStringValue = ns(request.default_name);

    auto box = std::make_shared<std::function<void(std::optional<std::string>, std::string)>>(
        std::move(respond));
    auto source = std::make_shared<std::optional<std::string>>(std::move(copy_source));
    begin(panel, sheet_parent(parent), ^(NSModalResponse code) {
      NSURL* destination = static_cast<NSSavePanel*>(holder->object).URL;
      if (code != NSModalResponseOK || destination == nil) {
          (*box)(std::nullopt, {});
          return;
      }
      if (!source->has_value()) {
          (*box)(std::string(destination.path.UTF8String), {});
          return;
      }
      // Copy while the panel's grant on the destination is still held.
      const BOOL scoped = [destination startAccessingSecurityScopedResource];
      NSError* error = nil;
      NSData* bytes = [NSData dataWithContentsOfURL:[NSURL fileURLWithPath:ns(**source)]
                                            options:NSDataReadingMappedIfSafe
                                              error:&error];
      BOOL wrote = bytes != nil && [bytes writeToURL:destination
                                             options:NSDataWritingAtomic
                                               error:&error];
      if (scoped)
          [destination stopAccessingSecurityScopedResource];
      if (!wrote) {
          NSString* detail = error.localizedDescription;
          (*box)(std::nullopt, detail != nil ? std::string(detail.UTF8String)
                                             : std::string("the copy was not written"));
          return;
      }
      (*box)(std::string(destination.path.UTF8String), {});
    });
    return [holder] { [static_cast<NSSavePanel*>(holder->object) cancel:nil]; };
}

} // namespace pulp::platform::detail
