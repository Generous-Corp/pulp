// PulpMotionControlSource.swift — application-owned Core Motion input.
//
// This adapter deliberately stops at a coherent application frame. It does
// not know about Pulp parameters, StateStore, or the audio callback. An app
// can forward the frame through its existing queue, OSC/IPC/MIDI bridge, or
// other handoff and reuse the same mapping code on every surface.

import Foundation

/// One complete motion observation published by an application-owned provider.
public struct PulpMotionControlFrame: Sendable, Equatable {
    public let sequence: UInt64
    public let value: Double
    public let confidence: Double
    public let valid: Bool
    public let timestampNanoseconds: UInt64

    public init(
        sequence: UInt64,
        value: Double,
        confidence: Double,
        valid: Bool,
        timestampNanoseconds: UInt64
    ) {
        self.sequence = sequence
        self.value = value
        self.confidence = confidence
        self.valid = valid
        self.timestampNanoseconds = timestampNanoseconds
    }
}

/// Pure conversion shared by the provider and camera-free tests.
public enum PulpMotionControlMath {
    /// Maps finite Y-axis acceleration, including gravity, into 0...1.
    /// Returns nil for malformed input so the caller can publish loss.
    public static func normalizeTilt(
        yAcceleration: Double,
        gravity: Double = 9.80665
    ) -> Double? {
        guard yAcceleration.isFinite, gravity.isFinite, gravity > 0 else { return nil }
        return (((yAcceleration / gravity) + 1.0) * 0.5).clamped(to: 0.0...1.0)
    }
}

private extension Comparable {
    func clamped(to range: ClosedRange<Self>) -> Self {
        min(max(self, range.lowerBound), range.upperBound)
    }
}

#if os(iOS)

import CoreMotion

/// Optional iOS accelerometer provider. Sensor delivery is scheduled on a
/// private serial operation queue; `publish` is never called on an audio
/// render callback and this type has no Pulp or inference dependency.
public final class PulpMotionControlSource {
    private let manager: CMMotionManager
    private let queue: OperationQueue
    private let publish: (PulpMotionControlFrame) -> Void
    private var sequence: UInt64 = 0
    private var running = false

    public init(
        manager: CMMotionManager = CMMotionManager(),
        publish: @escaping (PulpMotionControlFrame) -> Void
    ) {
        self.manager = manager
        self.publish = publish
        self.queue = OperationQueue()
        self.queue.maxConcurrentOperationCount = 1
        self.queue.qualityOfService = .userInteractive
    }

    deinit {
        stop()
    }

    /// Starts accelerometer delivery, returning false when hardware is absent.
    @discardableResult
    public func start(updateInterval: TimeInterval = 1.0 / 60.0) -> Bool {
        guard !running, manager.isAccelerometerAvailable else { return false }
        manager.accelerometerUpdateInterval = updateInterval
        manager.startAccelerometerUpdates(to: queue) { [weak self] data, error in
            guard let self else { return }
            if error != nil || data == nil {
                self.publishLoss()
                return
            }
            self.submit(yAcceleration: data!.acceleration.y, timestamp: data!.timestamp)
        }
        running = true
        return true
    }

    /// Stops delivery and publishes one invalid frame for deterministic loss.
    public func stop() {
        guard running else { return }
        manager.stopAccelerometerUpdates()
        running = false
        publishLoss()
    }

    private func submit(yAcceleration: Double, timestamp: TimeInterval) {
        guard running else { return }
        let normalized = PulpMotionControlMath.normalizeTilt(yAcceleration: yAcceleration)
        sequence &+= 1
        publish(PulpMotionControlFrame(
            sequence: sequence,
            value: normalized ?? 0.0,
            confidence: normalized == nil ? 0.0 : 1.0,
            valid: normalized != nil,
            timestampNanoseconds: normalized == nil ? 0 : UInt64(max(0, timestamp) * 1_000_000_000)
        ))
    }

    private func publishLoss() {
        sequence &+= 1
        publish(PulpMotionControlFrame(
            sequence: sequence,
            value: 0.0,
            confidence: 0.0,
            valid: false,
            timestampNanoseconds: 0
        ))
    }
}

#endif
