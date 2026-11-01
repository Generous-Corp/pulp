// PulpMotionControlSource.swift — application-owned Core Motion input.
//
// This adapter deliberately stops at a coherent application frame. It does
// not know about Pulp parameters, StateStore, or the audio callback. An app
// can forward the frame through its existing queue, OSC/IPC/MIDI bridge, or
// other handoff and reuse the same mapping code on every surface.

import Foundation

/// One complete motion observation published by an application-owned provider.
public struct PulpMotionControlFrame: Sendable, Equatable {
    public enum Status: String, Sendable {
        case accepted
        case paused
        case denied
        case unavailable
    }
    public let sourceId: String
    public let epoch: UInt64
    public let sequence: UInt64
    public let value: Double
    public let confidence: Double
    public let valid: Bool
    public let timestampNanoseconds: UInt64
    public let status: Status

    public init(
        sequence: UInt64,
        value: Double,
        confidence: Double,
        valid: Bool,
        timestampNanoseconds: UInt64,
        sourceId: String = "ios.motion.accelerometer",
        epoch: UInt64 = 0,
        status: Status? = nil
    ) {
        self.sequence = sequence
        self.sourceId = sourceId
        self.epoch = epoch
        self.value = value
        self.confidence = confidence
        self.valid = valid
        self.timestampNanoseconds = timestampNanoseconds
        self.status = status ?? (valid ? .accepted : .unavailable)
    }
}

/// Pure conversion shared by the provider and camera-free tests.
public enum PulpMotionControlMath {
    /// Maps finite Y-axis acceleration, including gravity, into 0...1.
    /// `yAcceleration` and `gravity` share one unit: metres per second squared
    /// with the default, or g with `gravity: 1.0`.
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
    private let publishQueue = DispatchQueue(label: "com.pulp.motion-control.publish")
    private let publish: (PulpMotionControlFrame) -> Void
    private let stateLock = NSLock()
    private var sequence: UInt64 = 0
    private var running = false
    private var epoch: UInt64 = 0

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
        guard manager.isAccelerometerAvailable else { return false }
        stateLock.lock()
        guard !running else {
            stateLock.unlock()
            return false
        }
        // Set this before starting delivery: Core Motion may invoke the
        // callback immediately on the provider queue.
        running = true
        epoch &+= 1
        stateLock.unlock()
        manager.accelerometerUpdateInterval = updateInterval
        manager.startAccelerometerUpdates(to: queue) { [weak self] data, error in
            guard let self else { return }
            if error != nil || data == nil {
                self.publishLoss()
                return
            }
            self.submit(yAcceleration: data!.acceleration.y, timestamp: data!.timestamp)
        }
        return true
    }

    /// Stops delivery and publishes one invalid frame for deterministic loss.
    public func stop() {
        manager.stopAccelerometerUpdates()
        stateLock.lock()
        guard running else {
            stateLock.unlock()
            return
        }
        running = false
        sequence &+= 1
        let frame = PulpMotionControlFrame(
            sequence: sequence,
            value: 0.0,
            confidence: 0.0,
            valid: false,
            timestampNanoseconds: 0,
            epoch: epoch,
            status: .unavailable
        )
        // Enqueue while holding stateLock so this loss frame follows any
        // in-flight sensor frame. The application closure runs later on a
        // separate serial queue, never while stateLock is held.
        publishQueue.async { [publish] in publish(frame) }
        stateLock.unlock()
    }

    /// Publishes one invalid frame for a sensor error while delivery is
    /// running, with the same ordering guarantees as `submit` and `stop`.
    private func publishLoss() {
        stateLock.lock()
        guard running else {
            stateLock.unlock()
            return
        }
        sequence &+= 1
        let frame = PulpMotionControlFrame(
            sequence: sequence,
            value: 0.0,
            confidence: 0.0,
            valid: false,
            timestampNanoseconds: 0,
            epoch: epoch,
            status: .unavailable
        )
        publishQueue.async { [publish] in publish(frame) }
        stateLock.unlock()
    }

    private func submit(yAcceleration: Double, timestamp: TimeInterval) {
        stateLock.lock()
        guard running else {
            stateLock.unlock()
            return
        }
        // Core Motion reports acceleration in g, so one g of gravity is 1.0.
        let normalized = PulpMotionControlMath.normalizeTilt(
            yAcceleration: yAcceleration,
            gravity: 1.0
        )
        sequence &+= 1
        let frame = PulpMotionControlFrame(
            sequence: sequence,
            value: normalized ?? 0.0,
            confidence: normalized == nil ? 0.0 : 1.0,
            valid: normalized != nil,
            timestampNanoseconds: normalized == nil ? 0 : UInt64(max(0, timestamp) * 1_000_000_000),
            epoch: epoch,
            status: normalized == nil ? .unavailable : .accepted
        )
        publishQueue.async { [publish] in publish(frame) }
        stateLock.unlock()
    }
}

#endif
