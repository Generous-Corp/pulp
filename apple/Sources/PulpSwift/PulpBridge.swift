// PulpBridge.swift — SwiftPM bridge seam for the Apple UI layer
// Keeps the Swift package testable without linking the full native host.

import Darwin
import Foundation

public typealias PulpParamID = UInt32

let pulpEmptyCString = UnsafePointer(strdup("")!)

@frozen
public struct PulpParamInfo {
    public var id: PulpParamID
    public var name: UnsafePointer<CChar>?
    public var unit: UnsafePointer<CChar>?
    public var min_value: Float
    public var max_value: Float
    public var default_value: Float
    public var step: Float

    public init(
        id: PulpParamID = 0,
        name: UnsafePointer<CChar>? = nil,
        unit: UnsafePointer<CChar>? = nil,
        min_value: Float = 0,
        max_value: Float = 1,
        default_value: Float = 0,
        step: Float = 0
    ) {
        self.id = id
        self.name = name ?? pulpEmptyCString
        self.unit = unit ?? pulpEmptyCString
        self.min_value = min_value
        self.max_value = max_value
        self.default_value = default_value
        self.step = step
    }
}

struct PulpBridgeParameterInfo {
    var id: PulpParamID
    var name: String
    var unit: String
    var minValue: Float
    var maxValue: Float
    var defaultValue: Float
    var step: Float
}

struct PulpBridgeBackend {
    var parameters: () -> [PulpBridgeParameterInfo] = { [] }
    var getValue: (PulpParamID) -> Float = { _ in 0 }
    var setValue: (PulpParamID, Float) -> Void = { _, _ in }
    var getNormalizedValue: (PulpParamID) -> Float = { _ in 0 }
    var setNormalizedValue: (PulpParamID, Float) -> Void = { _, _ in }
    var beginGesture: (PulpParamID) -> Void = { _ in }
    var endGesture: (PulpParamID) -> Void = { _ in }
    var resetToDefault: (PulpParamID) -> Void = { _ in }
}

private final class PulpCStringArena {
    static let shared = PulpCStringArena()

    private let lock = NSLock()
    private var storage: [String: UnsafeMutablePointer<CChar>] = [:]

    func pointer(for string: String) -> UnsafePointer<CChar> {
        lock.lock()
        defer { lock.unlock() }
        if let existing = storage[string] {
            return UnsafePointer(existing)
        }
        let allocated = strdup(string) ?? strdup("")!
        storage[string] = allocated
        return UnsafePointer(allocated)
    }
}

enum PulpBridgeRuntime {
    private static var backend = PulpBridgeBackend()

    static func installTestBackend(_ backend: PulpBridgeBackend?) {
        self.backend = backend ?? PulpBridgeBackend()
    }

    static func parameters() -> [PulpBridgeParameterInfo] {
        backend.parameters()
    }

    static func getValue(_ id: PulpParamID) -> Float {
        backend.getValue(id)
    }

    static func setValue(_ id: PulpParamID, _ value: Float) {
        backend.setValue(id, value)
    }

    static func getNormalizedValue(_ id: PulpParamID) -> Float {
        backend.getNormalizedValue(id)
    }

    static func setNormalizedValue(_ id: PulpParamID, _ value: Float) {
        backend.setNormalizedValue(id, value)
    }

    static func beginGesture(_ id: PulpParamID) {
        backend.beginGesture(id)
    }

    static func endGesture(_ id: PulpParamID) {
        backend.endGesture(id)
    }

    static func resetToDefault(_ id: PulpParamID) {
        backend.resetToDefault(id)
    }
}

func pulp_param_count() -> Int32 {
    Int32(PulpBridgeRuntime.parameters().count)
}

func pulp_param_info(_ index: Int32, _ outInfo: UnsafeMutablePointer<PulpParamInfo>?) -> Bool {
    let parameters = PulpBridgeRuntime.parameters()
    guard index >= 0,
          let outInfo,
          Int(index) < parameters.count else {
        return false
    }
    let parameter = parameters[Int(index)]
    outInfo.pointee = PulpParamInfo(
        id: parameter.id,
        name: PulpCStringArena.shared.pointer(for: parameter.name),
        unit: PulpCStringArena.shared.pointer(for: parameter.unit),
        min_value: parameter.minValue,
        max_value: parameter.maxValue,
        default_value: parameter.defaultValue,
        step: parameter.step
    )
    return true
}

func pulp_param_get(_ id: PulpParamID) -> Float {
    PulpBridgeRuntime.getValue(id)
}

func pulp_param_set(_ id: PulpParamID, _ value: Float) {
    PulpBridgeRuntime.setValue(id, value)
}

func pulp_param_get_normalized(_ id: PulpParamID) -> Float {
    PulpBridgeRuntime.getNormalizedValue(id)
}

func pulp_param_set_normalized(_ id: PulpParamID, _ normalized: Float) {
    PulpBridgeRuntime.setNormalizedValue(id, normalized)
}

func pulp_param_begin_gesture(_ id: PulpParamID) {
    PulpBridgeRuntime.beginGesture(id)
}

func pulp_param_end_gesture(_ id: PulpParamID) {
    PulpBridgeRuntime.endGesture(id)
}

func pulp_param_reset(_ id: PulpParamID) {
    PulpBridgeRuntime.resetToDefault(id)
}
