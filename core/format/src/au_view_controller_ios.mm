// iOS AUv3 view controller — provides a UIViewController for the AUv3 extension UI
// On iOS, AUv3 extensions present their UI via a UIViewController subclass.
// This wraps the Pulp view system into the AUv3 hosting model.

#if TARGET_OS_IOS

#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

/// AUViewController subclass that hosts a Pulp View tree inside an AUv3 extension.
///
/// The host DAW instantiates this controller when it wants to show the plugin's UI.
/// It creates a PluginViewHost (iOS variant) and attaches it to the controller's view.
@interface PulpAUViewController : AUViewController

/// The audio unit whose UI we are presenting.
@property (nonatomic, strong) AUAudioUnit *audioUnit;

@end

@implementation PulpAUViewController {
    std::unique_ptr<pulp::view::PluginViewHost> _viewHost;
    std::unique_ptr<pulp::view::View> _rootView;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.view.backgroundColor = [UIColor blackColor];
    self.preferredContentSize = CGSizeMake(400, 300);

    // Create a root view for the plugin UI
    _rootView = std::make_unique<pulp::view::View>();

    // Create the iOS plugin view host
    auto size = pulp::view::PluginViewHost::Size{
        static_cast<uint32_t>(self.preferredContentSize.width),
        static_cast<uint32_t>(self.preferredContentSize.height)
    };
    _viewHost = pulp::view::PluginViewHost::create(*_rootView, size);

    if (_viewHost) {
        _viewHost->attach_to_parent((__bridge void*)self.view);
        pulp::runtime::log_info("AU iOS: view controller loaded, {}x{}", size.width, size.height);
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    if (_viewHost) {
        CGSize size = self.view.bounds.size;
        _viewHost->set_size(
            static_cast<uint32_t>(size.width),
            static_cast<uint32_t>(size.height));
    }
}

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                    error:(NSError **)error {
    // The host calls this to get the AU instance associated with this view.
    // If audioUnit was already set externally, return it.
    return self.audioUnit;
}

@end

#endif // TARGET_OS_IOS
