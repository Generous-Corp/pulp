#!/usr/bin/env swift

import AppKit
import ApplicationServices
import CoreGraphics
import Foundation

struct Bounds: Codable {
    let x: Double
    let y: Double
    let width: Double
    let height: Double
}

struct WindowInfo: Codable {
    let windowId: UInt32
    let ownerPid: Int32
    let ownerName: String
    let title: String
    let bounds: Bounds
    let scale: Double
}

/// Backing scale factor (1.0 normal, 2.0 Retina) of the screen that contains the
/// center of a CGWindow rect. CGWindow bounds are top-left-origin global points;
/// NSScreen frames are bottom-left-origin. AVFoundation screen capture produces
/// native pixels, so callers must multiply point-space crop bounds by this scale.
func backingScale(forWindowRect rect: CGRect) -> Double {
    let screens = NSScreen.screens
    guard !screens.isEmpty else { return Double(NSScreen.main?.backingScaleFactor ?? 1.0) }
    let primaryHeight: CGFloat = screens.first(where: { $0.frame.origin == .zero })?.frame.height
        ?? NSScreen.main?.frame.height
        ?? screens[0].frame.height
    let center = CGPoint(x: rect.midX, y: rect.midY)
    for screen in screens {
        let f = screen.frame
        // Convert the AppKit (bottom-left) frame to CG (top-left) global coords.
        let cgTop = primaryHeight - (f.origin.y + f.height)
        let cgRect = CGRect(x: f.origin.x, y: cgTop, width: f.width, height: f.height)
        if cgRect.contains(center) {
            return Double(screen.backingScaleFactor)
        }
    }
    return Double(NSScreen.main?.backingScaleFactor ?? 1.0)
}

func usage() -> Never {
    fputs("usage: macos_window_probe.swift window-info (--pid <pid> | --bundle-id <bundle-id>)\n", stderr)
    fputs("       macos_window_probe.swift accessibility-trusted\n", stderr)
    fputs("       macos_window_probe.swift activate --pid <pid>\n", stderr)
    fputs("       macos_window_probe.swift click --x <screen-x> --y <screen-y>\n", stderr)
    exit(2)
}

enum WindowQuery {
    case pid(Int32)
    case bundleId(String)
}

func parseQuery(_ args: [String]) -> WindowQuery {
    if let idx = args.firstIndex(of: "--pid"), idx + 1 < args.count, let pid = Int32(args[idx + 1]) {
        return .pid(pid)
    }
    if let idx = args.firstIndex(of: "--bundle-id"), idx + 1 < args.count {
        return .bundleId(args[idx + 1])
    }
    usage()
}

func parseDoubleFlag(_ args: [String], _ name: String) -> Double {
    guard let idx = args.firstIndex(of: name), idx + 1 < args.count, let value = Double(args[idx + 1]) else {
        usage()
    }
    return value
}

func windowInfos(for bundleId: String) -> (pid: Int32, windows: [WindowInfo])? {
    guard let app = NSRunningApplication.runningApplications(withBundleIdentifier: bundleId).first else {
        return nil
    }
    return (app.processIdentifier, windowInfos(for: app.processIdentifier))
}

func windowInfos(for pid: Int32) -> [WindowInfo] {
    let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    guard let raw = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
        return []
    }

    return raw.compactMap { entry in
        guard let ownerPid = entry[kCGWindowOwnerPID as String] as? Int32, ownerPid == pid else {
            return nil
        }
        let layer = entry[kCGWindowLayer as String] as? Int ?? 0
        guard layer == 0 else {
            return nil
        }
        guard let boundsDict = entry[kCGWindowBounds as String] as? NSDictionary,
              let rect = CGRect(dictionaryRepresentation: boundsDict) else {
            return nil
        }
        guard rect.width >= 40, rect.height >= 40 else {
            return nil
        }
        let alpha = entry[kCGWindowAlpha as String] as? Double ?? 1.0
        guard alpha > 0.01 else {
            return nil
        }
        guard let windowId = entry[kCGWindowNumber as String] as? UInt32 else {
            return nil
        }

        return WindowInfo(
            windowId: windowId,
            ownerPid: ownerPid,
            ownerName: entry[kCGWindowOwnerName as String] as? String ?? "",
            title: entry[kCGWindowName as String] as? String ?? "",
            bounds: Bounds(
                x: rect.origin.x,
                y: rect.origin.y,
                width: rect.width,
                height: rect.height
            ),
            scale: backingScale(forWindowRect: rect)
        )
    }
    .sorted { lhs, rhs in
        let lhsArea = lhs.bounds.width * lhs.bounds.height
        let rhsArea = rhs.bounds.width * rhs.bounds.height
        return lhsArea > rhsArea
    }
}

func postLeftClick(screenX: Double, screenY: Double) throws {
    let screenPoint = CGPoint(x: screenX, y: screenY)
    CGWarpMouseCursorPosition(screenPoint)

    guard let down = CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: screenPoint, mouseButton: .left) else {
        throw NSError(domain: "macos_window_probe", code: 2, userInfo: [NSLocalizedDescriptionKey: "Could not create leftMouseDown event"])
    }
    guard let up = CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: screenPoint, mouseButton: .left) else {
        throw NSError(domain: "macos_window_probe", code: 3, userInfo: [NSLocalizedDescriptionKey: "Could not create leftMouseUp event"])
    }

    down.setIntegerValueField(.mouseEventClickState, value: 1)
    up.setIntegerValueField(.mouseEventClickState, value: 1)
    down.post(tap: .cghidEventTap)
    up.post(tap: .cghidEventTap)

    let payload: [String: Any] = [
        "screenPoint": ["x": screenX, "y": screenY],
        "quartzPoint": ["x": screenPoint.x, "y": screenPoint.y],
    ]
    let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
}

let args = Array(CommandLine.arguments.dropFirst())
guard let command = args.first else {
    usage()
}

switch command {
case "window-info":
    let query = parseQuery(args)
    let pid: Int32
    let windows: [WindowInfo]
    switch query {
    case .pid(let explicitPid):
        pid = explicitPid
        windows = windowInfos(for: pid)
    case .bundleId(let bundleId):
        guard let resolved = windowInfos(for: bundleId) else {
            let payload: [String: Any] = ["pid": NSNull(), "bundleId": bundleId, "windows": []]
            let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
            FileHandle.standardOutput.write(data)
            FileHandle.standardOutput.write("\n".data(using: .utf8)!)
            exit(0)
        }
        pid = resolved.pid
        windows = resolved.windows
    }
    let payload: [String: Any] = [
        "pid": Int(pid),
        "windows": windows.map { window in
            [
                "windowId": Int(window.windowId),
                "ownerPid": Int(window.ownerPid),
                "ownerName": window.ownerName,
                "title": window.title,
                "bounds": [
                    "x": window.bounds.x,
                    "y": window.bounds.y,
                    "width": window.bounds.width,
                    "height": window.bounds.height,
                ],
                "scale": window.scale,
            ]
        },
    ]
    let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
case "accessibility-trusted":
    let payload = ["trusted": AXIsProcessTrusted()]
    let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
case "activate":
    guard case .pid(let explicitPid) = parseQuery(args) else {
        usage()
    }
    let activated = NSRunningApplication(processIdentifier: explicitPid)?.activate(options: [.activateAllWindows, .activateIgnoringOtherApps]) ?? false
    let payload = ["activated": activated, "pid": Int(explicitPid)] as [String : Any]
    let data = try JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
case "click":
    let x = parseDoubleFlag(args, "--x")
    let y = parseDoubleFlag(args, "--y")
    try postLeftClick(screenX: x, screenY: y)
default:
    usage()
}
