#pragma once

// The view an AU v3 view controller embeds for a processor that supplies an
// editor factory (`Processor::editor_factory()`, see pulp/format/editor_host.hpp).
//
// The macOS and iOS controllers (au_view_controller_{mac,ios}.mm) open one of
// these instead of a ViewBridge editor when the processor has a factory, so a
// plug-in built with `UI <package>` has an editor as AU v3 the same way it does
// as VST3, CLAP and AU v2. The view owns a ContractEditorSession and is the
// editor's NativeParent for its whole life; the mapping is:
//
//   +openViewForProcessor...      -> a session (factory create) attached to the
//                                    new view, sized to the editor
//   view frame / bounds change    -> Editor::set_size() (the host resized it)
//   backing scale change          -> Editor::set_size() with the new scale
//   view moves into / out of a window -> Editor::visibility_changed()
//   key events (macOS)            -> Editor::offer_key(); a declined key goes up
//                                    the responder chain to the host
//   editor resize request         -> the controller's resizeRequest block, which
//                                    publishes preferredContentSize
//   -closeEditor / dealloc        -> every gesture released, Editor::detach()
//
// A session is also registered with the audio unit's ContractSessionRegistry,
// so an audio unit disposed before its view closes the editor in order.
//
// Class names are process-global in Objective-C: PULP_AUV3_CONTRACT_VIEW_CLASS
// lets a test build its own copy.

#import <TargetConditionals.h>

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#define PULP_AUV3_PLATFORM_VIEW NSView
#else
#import <UIKit/UIKit.h>
#define PULP_AUV3_PLATFORM_VIEW UIView
#endif

#include <pulp/format/contract_editor_session.hpp>

#ifndef PULP_AUV3_CONTRACT_VIEW_CLASS
#define PULP_AUV3_CONTRACT_VIEW_CLASS PulpAUv3ContractView
#endif

namespace pulp::format {
class Processor;
}

/// Asked to resize the host's window to the editor's size. Returns whether the
/// host's view followed. Main thread.
typedef BOOL (^PulpAUv3ContractResizeRequest)(CGSize size);

@interface PULP_AUV3_CONTRACT_VIEW_CLASS : PULP_AUV3_PLATFORM_VIEW

/// A view over a new editor, attached and sized to it, or nil when the factory
/// builds none or the editor refuses the view. `registry` may be null. The
/// block is copied and released by -closeEditor.
+ (instancetype)openViewForProcessor:(pulp::format::Processor*)processor
                               store:(pulp::state::StateStore*)store
                            registry:(pulp::format::ContractSessionRegistry*)registry
                       resizeRequest:(PulpAUv3ContractResizeRequest)resizeRequest;

/// The open session, or null after -closeEditor.
- (pulp::format::ContractEditorSession*)session;

/// An orderly close: gestures released, editor detached and dropped. Safe to
/// call twice. Later resizes and key events touch nothing.
- (void)closeEditor;

@end
