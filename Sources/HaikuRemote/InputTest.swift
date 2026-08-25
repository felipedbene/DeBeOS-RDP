import CoreGraphics
import Foundation

/// Headless Phase 4 exercise: drives the real `InputEncoder` and pushes the
/// results down the real connection, so the wire format is verified by whatever
/// is on the other end rather than by a mock of our own encoder.
///
/// Synthesising `NSEvent`s would need accessibility permission and would test
/// AppKit more than the protocol; this covers the part that can actually be
/// wrong.
enum InputTest {
    static func run(host: String, port: UInt16, width: Int, height: Int,
                    seconds: Double) -> Never {
        let connection = RemoteConnection()
        let input = InputEncoder()
        input.mapping = Settings.commandMapping

        connection.onLog = { m in
            FileHandle.standardError.write(Data(("  " + m + "\n").utf8))
        }

        print("connecting to \(host):\(port)…")
        connection.connect(host: host, port: port, width: width, height: height)

        // Let the handshake and initial scene settle first.
        spin(1.5)

        print("sending input events…")
        var sent = 0
        func send(_ d: Data, _ label: String) {
            connection.send(d)
            sent += 1
            print("  -> \(label)")
            spin(0.08)
        }

        // A pointer sweep, which is what RP_MOUSE_MOVED carries: position only,
        // no buttons.
        for i in 0..<10 {
            let x = Float(100 + i * 40), y = Float(300 + i * 8)
            connection.send(input.mouseMoved(x: x, y: y))
            sent += 1
            spin(0.03)
        }
        print("  -> 10x RP_MOUSE_MOVED")

        // A click, then a drag, then release. The drag is deliberately expressed
        // as mouseMoved with no buttons field, which is why the server has to
        // remember the button state from the preceding down.
        send(input.mouseDown(x: 500, y: 380, buttons: .primary, clickCount: 1),
             "RP_MOUSE_DOWN primary clicks=1")
        send(input.mouseMoved(x: 520, y: 390), "RP_MOUSE_MOVED (dragging)")
        send(input.mouseUp(x: 520, y: 390, buttons: []),
             "RP_MOUSE_UP buttons=0")

        // A double click and a right click.
        send(input.mouseDown(x: 300, y: 420, buttons: .primary, clickCount: 2),
             "RP_MOUSE_DOWN clicks=2")
        send(input.mouseUp(x: 300, y: 420, buttons: []), "RP_MOUSE_UP")
        send(input.mouseDown(x: 320, y: 420, buttons: .secondary, clickCount: 1),
             "RP_MOUSE_DOWN secondary")
        send(input.mouseUp(x: 320, y: 420, buttons: []), "RP_MOUSE_UP")

        send(input.mouseWheel(deltaX: 0, deltaY: 3), "RP_MOUSE_WHEEL_CHANGED")

        // Modifiers are client-owned state: send the whole new mask on change.
        let shiftMask: UInt = 1 << 17
        if let m = input.modifiersChanged(nsFlagsRawValue: shiftMask) {
            send(m, "RP_MODIFIERS_CHANGED shift down")
        }
        send(input.key(down: true, characters: "H",
                       charactersIgnoringModifiers: "h", keyCode: 0x04),
             "RP_KEY_DOWN 'H'")
        send(input.key(down: false, characters: "H",
                       charactersIgnoringModifiers: "h", keyCode: 0x04),
             "RP_KEY_UP 'H'")
        if let m = input.modifiersChanged(nsFlagsRawValue: 0) {
            send(m, "RP_MODIFIERS_CHANGED shift up")
        }

        for (ch, vk) in [("i", UInt16(0x22)), ("!", UInt16(0x12))] {
            send(input.key(down: true, characters: ch,
                           charactersIgnoringModifiers: ch, keyCode: vk),
                 "RP_KEY_DOWN '\(ch)'")
            send(input.key(down: false, characters: ch,
                           charactersIgnoringModifiers: ch, keyCode: vk),
                 "RP_KEY_UP '\(ch)'")
        }

        // A named key, where the Haiku key code matters more than the text.
        send(input.key(down: true, characters: "\r",
                       charactersIgnoringModifiers: "\r", keyCode: 0x24),
             "RP_KEY_DOWN Return (haiku key 0x47)")
        send(input.key(down: false, characters: "\r",
                       charactersIgnoringModifiers: "\r", keyCode: 0x24),
             "RP_KEY_UP Return")

        spin(seconds)

        print("sent \(sent) input messages")
        if let stats = connection.stats {
            print("messages decoded from server: \(stats.messages)")
            print("RP_STRING_WIDTH replies: \(stats.stringWidthReplies)")
        }
        connection.disconnect()
        spin(0.3)
        exit(0)
    }

    private static func spin(_ seconds: Double) {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            RunLoop.current.run(mode: .default,
                                before: Date().addingTimeInterval(0.02))
        }
    }
}
