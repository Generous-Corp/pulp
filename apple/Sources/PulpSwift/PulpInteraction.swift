// PulpInteraction.swift: Swift front end for pulp::runtime::InteractionEvent.
//
// An iOS, iPadOS or macOS app (touch, pencil, pointer, camera tracking) forwards
// what it observes through `PulpInteractionProvider`. The provider stamps the
// source, epoch and sequence and forwards each event; the router's tracker in
// the C++ core decides everything else: ordering, validation, and the cancels
// that follow a repeated press, an epoch change or a timeout. Nothing here
// restates those rules, and platform lifecycle and permissions (camera access,
// scene phases) stay with the application.

import Foundation

public enum PulpInteractionPhase: UInt8, Sendable {
    case press = 0
    case hold = 1
    case move = 2
    case release = 3
    case cancel = 4
}

/// Layout of `PulpInteractionEvent` in core/runtime/include/pulp/runtime/interaction_c.h
/// (48 bytes). The package tests check every offset.
@frozen
public struct PulpInteractionEvent: Equatable {
    public var sourceId: UInt32
    public var epoch: UInt32
    public var sequence: UInt64
    public var providerTimeNs: UInt64
    public var objectId: UInt32
    public var phase: UInt8
    public var reserved: (UInt8, UInt8, UInt8)
    public var x: Float
    public var y: Float
    public var strength: Float
    public var confidence: Float

    public init(
        sourceId: UInt32 = 0,
        epoch: UInt32 = 0,
        sequence: UInt64 = 0,
        providerTimeNs: UInt64 = 0,
        objectId: UInt32 = 0,
        phase: UInt8 = 5,
        x: Float = 0,
        y: Float = 0,
        strength: Float = 0,
        confidence: Float = 0
    ) {
        self.sourceId = sourceId
        self.epoch = epoch
        self.sequence = sequence
        self.providerTimeNs = providerTimeNs
        self.objectId = objectId
        self.phase = phase
        self.reserved = (0, 0, 0)
        self.x = x
        self.y = y
        self.strength = strength
        self.confidence = confidence
    }

    public static func == (lhs: PulpInteractionEvent, rhs: PulpInteractionEvent) -> Bool {
        lhs.sourceId == rhs.sourceId && lhs.epoch == rhs.epoch && lhs.sequence == rhs.sequence
            && lhs.providerTimeNs == rhs.providerTimeNs && lhs.objectId == rhs.objectId
            && lhs.phase == rhs.phase && lhs.x == rhs.x && lhs.y == rhs.y
            && lhs.strength == rhs.strength && lhs.confidence == rhs.confidence
    }
}

/// Where the Swift package delivers events. In an app linked against the Pulp
/// core this is `pulp_interaction_publish` on the router handle; the package
/// keeps it replaceable so it builds and tests without the native host, the
/// same seam `PulpBridgeBackend` gives the parameter calls.
struct PulpInteractionBackend {
    var publish: (OpaquePointer?, PulpInteractionEvent) -> Bool = { _, _ in false }
}

enum PulpInteractionRuntime {
    private static var backend = PulpInteractionBackend()

    static func installTestBackend(_ backend: PulpInteractionBackend?) {
        self.backend = backend ?? PulpInteractionBackend()
    }

    static func publish(_ router: OpaquePointer?, _ event: PulpInteractionEvent) -> Bool {
        backend.publish(router, event)
    }
}

/// Mirrors `pulp_interaction_publish` in interaction_c.h.
func pulp_interaction_publish(
    _ router: OpaquePointer?,
    _ event: UnsafePointer<PulpInteractionEvent>?
) -> Bool {
    guard let event else { return false }
    return PulpInteractionRuntime.publish(router, event.pointee)
}

/// Forwards one provider's interactions to a router. The provider numbers its
/// own events (sequence is monotonic within a source and epoch) and says when
/// its stream restarts; every verdict belongs to the tracker.
public final class PulpInteractionProvider {
    public let sourceId: UInt32
    public private(set) var epoch: UInt32
    public private(set) var sequence: UInt64 = 0

    private let router: OpaquePointer?
    private let clock: () -> UInt64
    private let lock = NSLock()

    /// - Parameters:
    ///   - sourceId: the provider's stable id; the router accepts one source.
    ///   - router: the `PulpInteractionRouter*` from `pulp_interaction_router_create`.
    ///   - clock: provider time in nanoseconds, or 0 to let the router use its own.
    public init(
        sourceId: UInt32,
        router: OpaquePointer?,
        epoch: UInt32 = 0,
        clock: @escaping () -> UInt64 = { 0 }
    ) {
        self.sourceId = sourceId
        self.router = router
        self.epoch = epoch
        self.clock = clock
    }

    /// Maps a point in a view of `width` x `height` to the 0...1 surface the
    /// contract uses, optionally flipping y for a bottom-left origin. Values are
    /// not clamped: an out-of-range or non-finite point is the router's to refuse.
    public static func normalized(
        x: Double, y: Double, width: Double, height: Double, flipY: Bool = false
    ) -> (x: Float, y: Float) {
        let nx = width != 0 ? x / width : .nan
        var ny = height != 0 ? y / height : .nan
        if flipY { ny = 1 - ny }
        return (Float(nx), Float(ny))
    }

    /// Starts a new epoch for this source (a restarted tracking session, a new
    /// surface). Sequence numbering restarts; the tracker cancels what was held.
    public func beginEpoch() {
        lock.lock()
        defer { lock.unlock() }
        epoch &+= 1
        sequence = 0
    }

    @discardableResult
    public func press(
        object: UInt32, x: Float, y: Float, strength: Float = 1, confidence: Float = 1
    ) -> Bool {
        forward(.press, object, x, y, strength, confidence)
    }

    @discardableResult
    public func hold(
        object: UInt32, x: Float, y: Float, strength: Float = 1, confidence: Float = 1
    ) -> Bool {
        forward(.hold, object, x, y, strength, confidence)
    }

    @discardableResult
    public func move(
        object: UInt32, x: Float, y: Float, strength: Float = 1, confidence: Float = 1
    ) -> Bool {
        forward(.move, object, x, y, strength, confidence)
    }

    @discardableResult
    public func release(
        object: UInt32, x: Float, y: Float, strength: Float = 0, confidence: Float = 1
    ) -> Bool {
        forward(.release, object, x, y, strength, confidence)
    }

    @discardableResult
    public func cancel(
        object: UInt32, x: Float, y: Float, strength: Float = 0, confidence: Float = 1
    ) -> Bool {
        forward(.cancel, object, x, y, strength, confidence)
    }

    private func forward(
        _ phase: PulpInteractionPhase, _ object: UInt32,
        _ x: Float, _ y: Float, _ strength: Float, _ confidence: Float
    ) -> Bool {
        lock.lock()
        sequence &+= 1
        var event = PulpInteractionEvent(
            sourceId: sourceId,
            epoch: epoch,
            sequence: sequence,
            providerTimeNs: clock(),
            objectId: object,
            phase: phase.rawValue,
            x: x,
            y: y,
            strength: strength,
            confidence: confidence
        )
        lock.unlock()
        return pulp_interaction_publish(router, &event)
    }
}
