// macOS Audio/MIDI settings window for a standalone without a view layer
// (detail/standalone_settings_window.hpp). Every rule about what the lists offer
// lives in StandaloneSettingsModel; this file only places AppKit controls over it.

#include <pulp/platform/test_visibility.hpp>
#include <pulp/format/detail/standalone_settings_window.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_settings_model.hpp>

#import <AppKit/AppKit.h>

#include <memory>
#include <string>
#include <vector>

@interface PulpSettingsWindowController : NSObject <NSWindowDelegate> {
  @public
    pulp::format::StandaloneApp* app;
    pulp::format::StandaloneSettingsModel* model;
    NSWindow* window;
    NSPopUpButton* outputPopup;
    NSPopUpButton* inputPopup;
    NSPopUpButton* ratePopup;
    NSPopUpButton* bufferPopup;
    NSPopUpButton* midiPopup;
    NSTextField* latencyLabel;
    NSTextField* statusLabel;
    NSStackView* inputRow;
}
- (instancetype)initWithApp:(pulp::format::StandaloneApp*)app;
- (void)reloadFromApp;
- (void)show;
@end

namespace {
PulpSettingsWindowController* g_controller = nil;

NSTextField* make_label(NSString* text, CGFloat width) {
    NSTextField* label = [NSTextField labelWithString:text];
    label.alignment = NSTextAlignmentRight;
    [label.widthAnchor constraintEqualToConstant:width].active = YES;
    return label;
}

// Items are added to the menu directly, each tagged with its model index.
// addItemWithTitle: would merge rows with equal titles (two devices or ports
// with the same name), so the menu position would stop matching the model.
void fill_popup(NSPopUpButton* popup, const std::vector<std::string>& items, int selected) {
    [popup removeAllItems];
    for (size_t i = 0; i < items.size(); ++i) {
        NSMenuItem* item =
            [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:items[i].c_str()]
                                       action:nil
                                keyEquivalent:@""];
        item.tag = static_cast<NSInteger>(i);
        [popup.menu addItem:item];
        [item release];
    }
    if (selected >= 0 && selected < static_cast<int>(items.size()))
        [popup selectItemAtIndex:selected];
}

// The model index of the popup's selected row.
int selected_model_index(NSPopUpButton* popup) {
    NSMenuItem* item = popup.selectedItem;
    return item == nil ? -1 : static_cast<int>(item.tag);
}
} // namespace

@implementation PulpSettingsWindowController

- (NSStackView*)rowWithTitle:(NSString*)title control:(NSView*)control {
    NSStackView* row = [NSStackView stackViewWithViews:@[ make_label(title, 190), control ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 10;
    row.alignment = NSLayoutAttributeCenterY;
    return row;
}

- (NSPopUpButton*)popupWithAction:(SEL)action {
    NSPopUpButton* popup = [[[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                       pullsDown:NO] autorelease];
    popup.target = self;
    popup.action = action;
    [popup.widthAnchor constraintEqualToConstant:280].active = YES;
    return popup;
}

- (instancetype)initWithApp:(pulp::format::StandaloneApp*)owner {
    if ((self = [super init])) {
        app = owner;
        model = new pulp::format::StandaloneSettingsModel();

        outputPopup = [self popupWithAction:@selector(outputChanged:)];
        inputPopup = [self popupWithAction:@selector(inputChanged:)];
        ratePopup = [self popupWithAction:@selector(rateChanged:)];
        bufferPopup = [self popupWithAction:@selector(bufferChanged:)];
        midiPopup = [self popupWithAction:@selector(midiChanged:)];
        latencyLabel = [NSTextField labelWithString:@""];
        statusLabel = [NSTextField labelWithString:@""];
        statusLabel.textColor = NSColor.secondaryLabelColor;

        inputRow = [self
            rowWithTitle:
                [NSString stringWithUTF8String:pulp::format::StandaloneSettingsModel::input_label()]
                 control:inputPopup];
        NSButton* apply = [NSButton buttonWithTitle:@"Apply" target:self action:@selector(apply:)];
        apply.keyEquivalent = @"\r";
        NSButton* close = [NSButton buttonWithTitle:@"Close"
                                             target:self
                                             action:@selector(closeWindow:)];
        NSStackView* buttons = [NSStackView stackViewWithViews:@[ statusLabel, close, apply ]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        buttons.spacing = 10;

        NSStackView* column = [NSStackView stackViewWithViews:@[
            [self rowWithTitle:@"Output Device" control:outputPopup], inputRow,
            [self rowWithTitle:@"Sample Rate" control:ratePopup],
            [self rowWithTitle:@"Buffer Size" control:bufferPopup],
            [self rowWithTitle:@"MIDI Input" control:midiPopup],
            [self rowWithTitle:@"Output Latency" control:latencyLabel], buttons
        ]];
        column.orientation = NSUserInterfaceLayoutOrientationVertical;
        column.alignment = NSLayoutAttributeTrailing;
        column.spacing = 12;
        column.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);

        window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 520, 300)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        window.title = @"Audio/MIDI Settings";
        window.contentView = column;
        window.delegate = self;
        [self reloadFromApp];
    }
    return self;
}

- (void)dealloc {
    window.delegate = nil;
    [window release];
    delete model;
    [super dealloc];
}

- (void)reloadList {
    fill_popup(outputPopup, model->output_items(), model->output_selected());
    fill_popup(inputPopup, model->input_items(), model->input_selected());
    inputRow.hidden = !model->input_visible();
    fill_popup(ratePopup, model->sample_rate_items(), model->sample_rate_selected());
    fill_popup(bufferPopup, model->buffer_size_items(), model->buffer_size_selected());
    fill_popup(midiPopup, model->midi_items(), model->midi_selected());
    [self reloadLatency];
}

- (void)reloadLatency {
    // The text is "Latency: x ms"; the row's own label already names it.
    std::string text = model->latency_text();
    const std::string prefix = "Latency: ";
    if (text.rfind(prefix, 0) == 0)
        text.erase(0, prefix.size());
    latencyLabel.stringValue = [NSString stringWithUTF8String:text.c_str()];
}

- (void)reloadFromApp {
    model->set_config(app->config());
    model->refresh(app->audio_system(), app->midi_system());
    [self reloadList];
}

- (void)show {
    [window center];
    pulp::platform::order_in_window(window);
    pulp::platform::activate_app_for_window();
}

- (void)outputChanged:(id)sender {
    model->select_output(selected_model_index(outputPopup));
    statusLabel.stringValue = @"";
    [self reloadList];
}
- (void)inputChanged:(id)sender {
    model->select_input(selected_model_index(inputPopup));
    statusLabel.stringValue = @"";
}
- (void)rateChanged:(id)sender {
    model->select_sample_rate(selected_model_index(ratePopup));
    statusLabel.stringValue = @"";
    fill_popup(bufferPopup, model->buffer_size_items(), model->buffer_size_selected());
    [self reloadLatency];
}
- (void)bufferChanged:(id)sender {
    model->select_buffer_size(selected_model_index(bufferPopup));
    statusLabel.stringValue = @"";
    [self reloadLatency];
}
- (void)midiChanged:(id)sender {
    model->select_midi(selected_model_index(midiPopup));
    statusLabel.stringValue = @"";
}

- (void)apply:(id)sender {
    const bool ok = app->apply_config(model->config());
    [self reloadFromApp];
    statusLabel.stringValue = ok ? @"Applied" : @"Could not apply these settings";
}

- (void)closeWindow:(id)sender {
    [window performClose:nil];
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    // Release after the close finishes unwinding.
    PulpSettingsWindowController* self_ = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (g_controller == self_) {
          g_controller = nil;
          [self_ release];
      }
    });
}
@end

namespace pulp::format::detail {

bool standalone_settings_window_supported() {
    return true;
}

namespace {
// Close the window if it shows `app`'s settings; another app's is left alone.
void close_controller_of(const StandaloneApp* app) {
    if (g_controller == nil || g_controller->app != app)
        return;
    PulpSettingsWindowController* controller = g_controller;
    g_controller = nil;
    controller->window.delegate = nil;
    [controller->window orderOut:nil];
    [controller release];
}
} // namespace

bool open_standalone_settings_window(StandaloneApp& app) {
    if (g_controller != nil && g_controller->app != &app)
        close_standalone_settings_windows(g_controller->app);
    if (g_controller == nil)
        g_controller = [[PulpSettingsWindowController alloc] initWithApp:&app];
    else
        [g_controller reloadFromApp];
    const StandaloneApp* owner = &app;
    register_standalone_settings_window_closer(owner, [owner] { close_controller_of(owner); });
    [g_controller show];
    return true;
}

} // namespace pulp::format::detail

#else // TARGET_OS_OSX

#include <pulp/runtime/log.hpp>

namespace pulp::format::detail {
bool standalone_settings_window_supported() {
    return false;
}
bool open_standalone_settings_window(StandaloneApp&) {
    runtime::log_warn("Standalone: this platform has no native Audio/MIDI settings window yet");
    return false;
}
} // namespace pulp::format::detail

#endif
