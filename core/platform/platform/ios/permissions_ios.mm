// iOS backend for pulp::platform::permissions.
//
// Maps pulp::platform::Permission onto the iOS system prompts:
//   Microphone    → AVAudioSession.requestRecordPermission
//   Camera        → AVCaptureDevice.requestAccessForMediaType:
//   BluetoothMidi → CBCentralManager.authorization / state observation
//   Notifications → UNUserNotificationCenter.requestAuthorizationWithOptions:
//   LocalNetwork  → granted (no runtime prompt surface; triggered by the
//                   system on first mDNS/bonjour access)
//   BackgroundAudio / ForegroundService
//                 → capability-based; granted when the app entitlement /
//                   UIBackgroundModes=audio is present. We answer Granted
//                   here and rely on the Info.plist to gate it.
//
// All callbacks are hopped back onto the main thread before invoking the
// user RequestCallback so UI code can touch UIKit without dispatch_sync.
//
// PULP_PERMISSIONS_HAS_BACKEND is set via CMake on iOS so this TU owns
// query(), request(), and has_platform_backend().

#if TARGET_OS_IOS || defined(__APPLE__)
#import <TargetConditionals.h>
#endif

#if TARGET_OS_IOS

#import <AVFoundation/AVFoundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <UserNotifications/UserNotifications.h>

#include "../../src/permissions_detail.hpp"
#include <pulp/platform/permissions.hpp>

#include <dispatch/dispatch.h>

namespace pulp::platform {

namespace {

PermissionState microphone_state() {
    switch (AVAudioSession.sharedInstance.recordPermission) {
        case AVAudioSessionRecordPermissionGranted:    return PermissionState::Granted;
        case AVAudioSessionRecordPermissionDenied:     return PermissionState::Denied;
        case AVAudioSessionRecordPermissionUndetermined:
        default:                                       return PermissionState::NotDetermined;
    }
}

PermissionState camera_state() {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
        case AVAuthorizationStatusAuthorized:   return PermissionState::Granted;
        case AVAuthorizationStatusDenied:       return PermissionState::Denied;
        case AVAuthorizationStatusRestricted:   return PermissionState::Restricted;
        case AVAuthorizationStatusNotDetermined:
        default:                                return PermissionState::NotDetermined;
    }
}

PermissionState bluetooth_state() {
    if (@available(iOS 13.1, *)) {
        switch (CBManager.authorization) {
            case CBManagerAuthorizationAllowedAlways:   return PermissionState::Granted;
            case CBManagerAuthorizationDenied:          return PermissionState::Denied;
            case CBManagerAuthorizationRestricted:      return PermissionState::Restricted;
            case CBManagerAuthorizationNotDetermined:
            default:                                    return PermissionState::NotDetermined;
        }
    }
    // Pre-13.1 — no authorization surface, treat as Granted and let the
    // system prompt on first use if NSBluetoothAlwaysUsageDescription is
    // set in Info.plist.
    return PermissionState::Granted;
}

void notifications_state(void (^completion)(PermissionState)) {
    [UNUserNotificationCenter.currentNotificationCenter
        getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
            PermissionState s;
            switch (settings.authorizationStatus) {
                case UNAuthorizationStatusAuthorized:
                case UNAuthorizationStatusProvisional:
                case UNAuthorizationStatusEphemeral:  s = PermissionState::Granted; break;
                case UNAuthorizationStatusDenied:     s = PermissionState::Denied; break;
                case UNAuthorizationStatusNotDetermined:
                default:                              s = PermissionState::NotDetermined; break;
            }
            completion(s);
        }];
}

PermissionState ios_default_state(Permission p) {
    switch (p) {
        case Permission::Microphone:        return microphone_state();
        case Permission::Camera:            return camera_state();
        case Permission::BluetoothMidi:     return bluetooth_state();
        case Permission::Notifications:     return PermissionState::NotDetermined;
        case Permission::LocalNetwork:
        case Permission::BackgroundAudio:
        case Permission::ForegroundService:
            return PermissionState::Granted;
    }
    return PermissionState::NotDetermined;
}

// Dispatch to main queue.
void on_main(RequestCallback cb, PermissionState state) {
    if (!cb) return;
    auto* block_cb = new RequestCallback(std::move(cb));
    dispatch_async(dispatch_get_main_queue(), ^{
        (*block_cb)(state);
        delete block_cb;
    });
}

}  // namespace

PermissionState query(Permission p) {
    if (auto ov = detail::override_lookup(p)) return *ov;
    // Notifications is async-only on iOS; synchronous query returns
    // NotDetermined and callers should use request() to resolve it.
    return ios_default_state(p);
}

void request(Permission p, RequestCallback cb) {
    if (auto ov = detail::override_lookup(p)) {
        on_main(std::move(cb), *ov);
        return;
    }

    switch (p) {
        case Permission::Microphone: {
            auto* block_cb = new RequestCallback(std::move(cb));
            [AVAudioSession.sharedInstance requestRecordPermission:^(BOOL granted) {
                PermissionState s = granted ? PermissionState::Granted : PermissionState::Denied;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (*block_cb) (*block_cb)(s);
                    delete block_cb;
                });
            }];
            return;
        }

        case Permission::Camera: {
            auto* block_cb = new RequestCallback(std::move(cb));
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                     completionHandler:^(BOOL granted) {
                PermissionState s = granted ? PermissionState::Granted : PermissionState::Denied;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (*block_cb) (*block_cb)(s);
                    delete block_cb;
                });
            }];
            return;
        }

        case Permission::Notifications: {
            auto* block_cb = new RequestCallback(std::move(cb));
            UNAuthorizationOptions opts = UNAuthorizationOptionAlert
                                        | UNAuthorizationOptionSound
                                        | UNAuthorizationOptionBadge;
            [UNUserNotificationCenter.currentNotificationCenter
                requestAuthorizationWithOptions:opts
                              completionHandler:^(BOOL granted, NSError* _Nullable) {
                PermissionState s = granted ? PermissionState::Granted : PermissionState::Denied;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (*block_cb) (*block_cb)(s);
                    delete block_cb;
                });
            }];
            return;
        }

        case Permission::BluetoothMidi:
            // No discrete request API before iOS 13.1; authorization is
            // surfaced by creating a CBCentralManager. We answer with the
            // current state — callers that need the prompt should own a
            // CBCentralManager with the standard Info.plist keys set.
            on_main(std::move(cb), bluetooth_state());
            return;

        case Permission::LocalNetwork:
        case Permission::BackgroundAudio:
        case Permission::ForegroundService:
            on_main(std::move(cb), ios_default_state(p));
            return;
    }
    on_main(std::move(cb), PermissionState::NotDetermined);
}

bool has_platform_backend() { return true; }

}  // namespace pulp::platform

#endif  // TARGET_OS_IOS
