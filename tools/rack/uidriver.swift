// Click somewhere, or type something, as the window server sees it.
//
//   uidriver click <x> <y>     one left click at a GLOBAL screen point
//   uidriver type  <text>      that text, into whatever has focus
//
// drive_app.py presses the app's own Build button rather than calling into it,
// because "the button works" and "the code behind the button works" are
// different claims and this project has had them disagree. Driving the window
// server is the only way to make the first claim.
//
// This existed as two binaries in /tmp and as no source at all. macOS clears
// /tmp, so the app proof ran on exactly one machine, until it was rebooted --
// it failed on the M5 with "build the click/type helpers first", naming a step
// nobody could carry out because there was nothing to build. That is the same
// failure the seam guard exists for: work that lives only in /tmp is work that
// has already been lost.
//
// Typing goes through setUnicodeString rather than key codes, so the text
// arrives whatever keyboard layout the machine is set to -- a prompt typed as
// key codes on a Dvorak machine is not the prompt anybody read.
//
// Needs Accessibility permission for whatever runs it (Terminal, usually).
// Without it the events are silently dropped, which looks exactly like a
// button that does nothing, so the failure is reported rather than assumed.

import CoreGraphics
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data(("uidriver: " + message + "\n").utf8))
    exit(2)
}

/// Are we allowed to post events at all? Posting without permission succeeds
/// and does nothing, which is indistinguishable from a dead control.
func requireAccessibility() {
    if !CGPreflightListenEventAccess() {
        // Ask once; on a machine that has already granted it this is a no-op.
        _ = CGRequestListenEventAccess()
    }
    if !CGPreflightListenEventAccess() {
        fail("no Accessibility permission — events would be dropped silently. "
             + "Grant it to the app running this (System Settings > Privacy & "
             + "Security > Accessibility).")
    }
}

func click(x: Double, y: Double) {
    let point = CGPoint(x: x, y: y)
    guard let move = CGEvent(mouseEventSource: nil, mouseType: .mouseMoved,
                             mouseCursorPosition: point, mouseButton: .left),
          let down = CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown,
                             mouseCursorPosition: point, mouseButton: .left),
          let up = CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp,
                           mouseCursorPosition: point, mouseButton: .left)
    else { fail("could not build the click events") }

    // Moved first, and with a beat between: a click delivered to a window that
    // has not seen the pointer arrive lands on the window rather than on the
    // control under it.
    move.post(tap: .cghidEventTap)
    usleep(60_000)
    down.post(tap: .cghidEventTap)
    usleep(40_000)
    up.post(tap: .cghidEventTap)
}

func type(_ text: String) {
    // In chunks, because setUnicodeString takes a bounded buffer and a long
    // prompt silently loses its tail otherwise.
    for chunk in Array(text).chunked(into: 16) {
        let piece = String(chunk)
        guard let down = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: true),
              let up = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: false)
        else { fail("could not build the key events") }
        var utf16 = Array(piece.utf16)
        down.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: &utf16)
        up.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: &utf16)
        down.post(tap: .cghidEventTap)
        usleep(8_000)
        up.post(tap: .cghidEventTap)
        usleep(8_000)
    }
}

extension Array {
    func chunked(into size: Int) -> [[Element]] {
        stride(from: 0, to: count, by: size).map {
            Array(self[$0 ..< Swift.min($0 + size, count)])
        }
    }
}

let args = Array(CommandLine.arguments.dropFirst())
guard let command = args.first else {
    fail("usage: uidriver click <x> <y> | uidriver type <text>")
}

switch command {
case "click":
    guard args.count >= 3, let x = Double(args[1]), let y = Double(args[2]) else {
        fail("usage: uidriver click <x> <y>")
    }
    requireAccessibility()
    click(x: x, y: y)
case "type":
    guard args.count >= 2 else { fail("usage: uidriver type <text>") }
    requireAccessibility()
    // Everything after the verb, so an unquoted prompt still arrives whole.
    type(args.dropFirst().joined(separator: " "))
default:
    fail("unknown command \(command) — expected click or type")
}
