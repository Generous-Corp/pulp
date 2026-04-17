// PulpAudioSession.swift — AVAudioSession wrapper for iOS
// Manages audio session category, interruptions, and route changes.
// macOS does not use AVAudioSession, so this file is iOS-only.

#if os(iOS)

import AVFoundation
import Combine

/// Manages the iOS audio session lifecycle for Pulp plugins and standalone apps.
///
/// Handles:
/// - Category configuration (playback vs playAndRecord)
/// - Interruption handling (phone calls, alarms)
/// - Route changes (headphones connected/disconnected)
/// - Sample rate and buffer duration preferences
/// - Background audio activation
public final class PulpAudioSession: ObservableObject {

    // MARK: - Types

    /// Audio session mode for Pulp usage.
    public enum SessionMode: Sendable {
        /// Playback only — suitable for instruments and playback-only apps.
        case playback
        /// Record and playback — needed for effects that process live input.
        case playAndRecord
    }

    /// Describes the current audio session state.
    public struct SessionState: Sendable {
        public var isActive: Bool = false
        public var sampleRate: Double = 48000.0
        public var bufferDuration: TimeInterval = 0.005  // ~5ms
        public var inputChannels: Int = 0
        public var outputChannels: Int = 0
        public var isInterrupted: Bool = false
    }

    // MARK: - Published State

    @Published public private(set) var state = SessionState()

    /// Called when an interruption begins (e.g., phone call).
    /// The audio engine should pause processing.
    public var onInterruptionBegan: (() -> Void)?

    /// Called when an interruption ends and audio can resume.
    /// The `shouldResume` flag indicates whether playback should auto-restart.
    public var onInterruptionEnded: ((_ shouldResume: Bool) -> Void)?

    /// Called when the audio route changes (e.g., headphones unplugged).
    public var onRouteChanged: ((_ reason: AVAudioSession.RouteChangeReason) -> Void)?

    // MARK: - Private

    private let session = AVAudioSession.sharedInstance()
    private var observers: [NSObjectProtocol] = []

    // MARK: - Init / Deinit

    public init() {
        registerNotifications()
    }

    deinit {
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    // MARK: - Configuration

    /// Configure and activate the audio session.
    ///
    /// - Parameters:
    ///   - mode: Whether to use playback-only or play-and-record.
    ///   - preferredSampleRate: Requested sample rate (iOS may choose differently).
    ///   - preferredBufferDuration: Requested buffer duration in seconds.
    /// - Throws: If the session cannot be configured or activated.
    public func configure(
        mode: SessionMode,
        preferredSampleRate: Double = 48000.0,
        preferredBufferDuration: TimeInterval = 0.005
    ) throws {
        let category: AVAudioSession.Category
        let options: AVAudioSession.CategoryOptions

        switch mode {
        case .playback:
            category = .playback
            options = [.mixWithOthers]
        case .playAndRecord:
            category = .playAndRecord
            options = [.defaultToSpeaker, .allowBluetooth, .mixWithOthers]
        }

        try session.setCategory(category, mode: .default, options: options)
        try session.setPreferredSampleRate(preferredSampleRate)
        try session.setPreferredIOBufferDuration(preferredBufferDuration)
        try session.setActive(true, options: [.notifyOthersOnDeactivation])

        refreshState()
        state.isActive = true
    }

    /// Deactivate the audio session.
    public func deactivate() throws {
        try session.setActive(false, options: [.notifyOthersOnDeactivation])
        state.isActive = false
    }

    /// Re-read current session properties into published state.
    public func refreshState() {
        state.sampleRate = session.sampleRate
        state.bufferDuration = session.ioBufferDuration
        state.inputChannels = session.inputNumberOfChannels
        state.outputChannels = session.outputNumberOfChannels
    }

    /// The actual sample rate granted by the system (may differ from preferred).
    public var actualSampleRate: Double {
        session.sampleRate
    }

    /// The actual I/O buffer duration granted by the system.
    public var actualBufferDuration: TimeInterval {
        session.ioBufferDuration
    }

    // MARK: - Notification Handling

    private func registerNotifications() {
        let interruptionObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.interruptionNotification,
            object: session,
            queue: .main
        ) { [weak self] notification in
            self?.handleInterruption(notification)
        }
        observers.append(interruptionObserver)

        let routeChangeObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.routeChangeNotification,
            object: session,
            queue: .main
        ) { [weak self] notification in
            self?.handleRouteChange(notification)
        }
        observers.append(routeChangeObserver)

        let mediaResetObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.mediaServicesWereResetNotification,
            object: session,
            queue: .main
        ) { [weak self] _ in
            self?.handleMediaServicesReset()
        }
        observers.append(mediaResetObserver)
    }

    private func handleInterruption(_ notification: Notification) {
        guard let userInfo = notification.userInfo,
              let typeValue = userInfo[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: typeValue) else {
            return
        }

        switch type {
        case .began:
            state.isInterrupted = true
            onInterruptionBegan?()
            emitNative(event: PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN, options: 0, reason: 0)

        case .ended:
            state.isInterrupted = false
            let shouldResume: Bool
            var optionsBitfield: Int32 = 0
            if let optionsValue = userInfo[AVAudioSessionInterruptionOptionKey] as? UInt {
                let options = AVAudioSession.InterruptionOptions(rawValue: optionsValue)
                shouldResume = options.contains(.shouldResume)
                if shouldResume {
                    optionsBitfield |= Int32(PULP_IOS_INTERRUPTION_OPTION_SHOULD_RESUME.rawValue)
                }
            } else {
                shouldResume = false
            }
            onInterruptionEnded?(shouldResume)
            emitNative(
                event: PULP_IOS_AUDIO_EVENT_INTERRUPTION_ENDED,
                options: optionsBitfield,
                reason: 0)

        @unknown default:
            break
        }
    }

    private func handleRouteChange(_ notification: Notification) {
        guard let userInfo = notification.userInfo,
              let reasonValue = userInfo[AVAudioSessionRouteChangeReasonKey] as? UInt,
              let reason = AVAudioSession.RouteChangeReason(rawValue: reasonValue) else {
            return
        }

        refreshState()
        onRouteChanged?(reason)
        emitNative(
            event: PULP_IOS_AUDIO_EVENT_ROUTE_CHANGED,
            options: 0,
            reason: Self.mapRouteChangeReason(reason))
    }

    private func handleMediaServicesReset() {
        // Media services were reset — reconfigure the session.
        // This is rare but can happen when the system audio daemon restarts.
        state.isActive = false
        state.isInterrupted = false
        refreshState()
        emitNative(event: PULP_IOS_AUDIO_EVENT_MEDIA_SERVICES_RESET, options: 0, reason: 0)
    }

    // MARK: - Native C ABI forwarding

    /// Build a PulpIosAudioSessionEvent from the current session state and
    /// deliver it to the C ABI listener registered via
    /// pulp_ios_audio_session_set_callback. No-op when no listener is
    /// attached — pulp_ios_audio_session_emit handles that internally.
    private func emitNative(event: PulpIosAudioEvent,
                            options: Int32,
                            reason: Int32) {
        var payload = PulpIosAudioSessionEvent(
            event: event,
            reason: reason,
            options: options,
            sample_rate: session.sampleRate,
            io_buffer_duration_seconds: session.ioBufferDuration,
            output_channels: Int32(session.outputNumberOfChannels),
            input_channels: Int32(session.inputNumberOfChannels))
        pulp_ios_audio_session_emit(&payload)
    }

    /// Map AVAudioSession.RouteChangeReason → PulpIosRouteChangeReason so
    /// native listeners can branch on the same taxonomy without linking
    /// AVFoundation.
    private static func mapRouteChangeReason(
        _ reason: AVAudioSession.RouteChangeReason
    ) -> Int32 {
        switch reason {
        case .unknown:
            return Int32(PULP_IOS_ROUTE_CHANGE_UNKNOWN.rawValue)
        case .newDeviceAvailable:
            return Int32(PULP_IOS_ROUTE_CHANGE_NEW_DEVICE_AVAILABLE.rawValue)
        case .oldDeviceUnavailable:
            return Int32(PULP_IOS_ROUTE_CHANGE_OLD_DEVICE_UNAVAILABLE.rawValue)
        case .categoryChange:
            return Int32(PULP_IOS_ROUTE_CHANGE_CATEGORY_CHANGE.rawValue)
        case .override:
            return Int32(PULP_IOS_ROUTE_CHANGE_OVERRIDE.rawValue)
        case .wakeFromSleep:
            return Int32(PULP_IOS_ROUTE_CHANGE_WAKE_FROM_SLEEP.rawValue)
        case .noSuitableRouteForCategory:
            return Int32(PULP_IOS_ROUTE_CHANGE_NO_SUITABLE_ROUTE.rawValue)
        case .routeConfigurationChange:
            return Int32(PULP_IOS_ROUTE_CHANGE_CONFIGURATION_CHANGE.rawValue)
        @unknown default:
            return Int32(PULP_IOS_ROUTE_CHANGE_UNKNOWN.rawValue)
        }
    }
}

#endif // os(iOS)
