import Foundation
import PulpInteractionBridge

public struct InteractionPoint: Sendable { public let x: Float; public let y: Float; public init(x: Float, y: Float) { self.x = x; self.y = y } }
public struct InteractionSurface: Sendable { public let width: Float; public let height: Float; public init(width: Float, height: Float) { self.width = width; self.height = height } }

public enum InteractionPhase: UInt8, Sendable { case press = 0, hold, move, release, cancel, invalid }

public struct InteractionEvent: Sendable, Equatable {
    public let sourceID: UInt32
    public let epoch: UInt32
    public let sequence: UInt64
    public let providerTimeNanoseconds: UInt64
    public let objectID: UInt32
    public let phase: InteractionPhase
    public let x: Float
    public let y: Float
    public let confidence: Float

    init(_ raw: PulpInteractionEvent) {
        sourceID = raw.source_id; epoch = raw.epoch; sequence = raw.sequence
        providerTimeNanoseconds = raw.provider_time_ns; objectID = raw.object_id
        phase = InteractionPhase(rawValue: raw.phase.rawValue) ?? .invalid
        x = raw.x; y = raw.y; confidence = raw.confidence
    }

    fileprivate var cValue: PulpInteractionEvent {
        PulpInteractionEvent(source_id: sourceID, epoch: epoch, object_id: objectID,
                             sequence: sequence, provider_time_ns: providerTimeNanoseconds,
                             phase: phase.cValue, x: x, y: y, confidence: confidence)
    }
}

public protocol InteractionProvider: AnyObject {
    func start()
    func stop()
}

/// Not thread-safe: call from a single thread or serialize access externally.
public final class InteractionSource: @unchecked Sendable {
    private let sourceID: UInt32
    private let surfaceSize: InteractionSurface
    private let clock: () -> UInt64
    private var epoch: UInt32 = 1
    private var sequence: UInt64 = 0
    private var lastEvent: InteractionEvent?

    public init(sourceID: UInt32, surfaceSize: InteractionSurface, clock: @escaping () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) {
        precondition(sourceID != 0)
        self.sourceID = sourceID; self.surfaceSize = surfaceSize; self.clock = clock
    }

    public func event(objectID: UInt32, phase: InteractionPhase, point: InteractionPoint, confidence: Float) -> InteractionEvent {
        var x: Float = 0, y: Float = 0
        precondition(pulp_interaction_normalize(point.x, point.y, surfaceSize.width, surfaceSize.height, &x, &y))
        sequence &+= 1
        var raw = PulpInteractionEvent(source_id: sourceID, epoch: epoch, object_id: objectID,
                                        sequence: sequence, provider_time_ns: clock(), phase: phase.cValue,
                                        x: x, y: y, confidence: confidence)
        precondition(pulp_interaction_validate(&raw))
        let result = InteractionEvent(raw); lastEvent = result; return result
    }

    public func stop() -> InteractionEvent {
        let prior = lastEvent
        let objectID = prior?.objectID ?? 0
        let point = InteractionPoint(x: (prior?.x ?? 0) * surfaceSize.width, y: (prior?.y ?? 0) * surfaceSize.height)
        return event(objectID: objectID, phase: .cancel, point: point, confidence: prior?.confidence ?? 0)
    }
}

public final class ManualInteractionProvider: InteractionProvider {
    private let onLifecycle: (Bool) -> Void
    public init(onLifecycle: @escaping (Bool) -> Void) { self.onLifecycle = onLifecycle }
    public func start() { onLifecycle(true) }
    public func stop() { onLifecycle(false) }
}

private extension InteractionPhase {
    var cValue: PulpInteractionPhase {
        PulpInteractionPhase(rawValue: rawValue)
    }
}

public final class InteractionReplayRecorder {
    private let handle: UnsafeMutableRawPointer
    public init() { handle = UnsafeMutableRawPointer(pulp_interaction_recorder_create()) }
    deinit { pulp_interaction_recorder_destroy(handle) }
    @discardableResult public func record(_ events: [InteractionEvent]) -> Bool {
        guard !events.isEmpty, events.count <= 16 else { return false }
        let raw = events.map(\.cValue)
        return raw.withUnsafeBufferPointer { buffer in
            pulp_interaction_recorder_record(handle, buffer.baseAddress, UInt8(raw.count))
        }
    }
    public var batchCount: Int { Int(pulp_interaction_recorder_count(handle)) }
    public func event(batch: Int, index: Int) -> InteractionEvent? {
        guard batch >= 0, batch <= 255, index >= 0, index <= 255 else { return nil }
        var raw = PulpInteractionEvent()
        guard pulp_interaction_recorder_event(handle, UInt8(batch), UInt8(index), &raw) else { return nil }
        return InteractionEvent(raw)
    }
}
