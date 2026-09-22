import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Drives a live desktop and checks that it reacted.
///
/// `InputTest` proves the encoder puts the right bytes on the wire; the server
/// decodes them and says so. That is the whole story against a mock, but against
/// a real `app_server` it proves nothing about whether the *desktop* responded —
/// a session that ignored every click would pass it.
///
/// So this mode interleaves input with frame captures and diffs consecutive
/// frames. A step that should open a window and produces zero changed pixels is
/// a failure, and one that changes pixels tells us the round trip worked end to
/// end: our encoder -> app_server -> Tracker -> drawing commands -> our renderer.
///
/// Note the cursor is deliberately excluded from the canvas (drawing it there
/// would leave trails), so a bare pointer move is expected to change *no* pixels.
/// That step is checked against the cursor position the server reports instead.
enum Drive {
    /// One scripted interaction.
    private struct Step {
        let name: String
        /// Whether the desktop is expected to repaint as a result.
        let expectsRepaint: Bool
        let settle: Double
        let action: (RemoteConnection, InputEncoder) -> Void
    }

    static func run(host: String, port: UInt16, width: Int, height: Int,
                    outDir: String) -> Never {
        let connection = RemoteConnection()
        let input = InputEncoder()
        input.mapping = Settings.commandMapping

        let lock = NSLock()
        var latest: CGImage?
        var cursor = CGPoint(x: -1, y: -1)

        connection.onLog = { m in
            FileHandle.standardError.write(Data(("  " + m + "\n").utf8))
        }
        connection.onFrame = { img in
            lock.lock(); latest = img; lock.unlock()
        }
        connection.onCursorChange = { _, _, pos, _ in
            lock.lock(); cursor = pos; lock.unlock()
        }

        func frame() -> CGImage? { lock.lock(); defer { lock.unlock() }; return latest }
        func cursorPos() -> CGPoint { lock.lock(); defer { lock.unlock() }; return cursor }

        print("connecting to \(host):\(port) as \(width)x\(height)…")
        connection.connect(host: host, port: port, width: width, height: height)
        spin(4.0)

        guard var previous = frame().flatMap(pixels) else {
            print("FAIL — no first frame; nothing to drive")
            exit(1)
        }
        save(frame(), to: "\(outDir)/drive-00-baseline.png")
        print("baseline captured\n")

        // Icon positions come from the rendered desktop: Tracker lays Haiku, home
        // and Trash across the top-left, labels underneath.
        let homeIcon = (x: Float(95), y: Float(38))
        let emptyDesktop = (x: Float(640), y: Float(500))

        let steps: [Step] = [
            Step(name: "pointer sweep to the home icon",
                 // The cursor lives outside the canvas, so this must NOT repaint.
                 expectsRepaint: false, settle: 1.0) { c, i in
                for n in 0...12 {
                    let t = Float(n) / 12
                    c.send(i.mouseMoved(x: 640 - (640 - homeIcon.x) * t,
                                        y: 400 - (400 - homeIcon.y) * t))
                    spin(0.04)
                }
            },
            Step(name: "single click the home icon (selects it)",
                 expectsRepaint: true, settle: 1.5) { c, i in
                c.send(i.mouseDown(x: homeIcon.x, y: homeIcon.y,
                                   buttons: .primary, clickCount: 1))
                spin(0.08)
                c.send(i.mouseUp(x: homeIcon.x, y: homeIcon.y, buttons: []))
            },
            Step(name: "double click the home icon (opens a Tracker window)",
                 expectsRepaint: true, settle: 4.0) { c, i in
                c.send(i.mouseDown(x: homeIcon.x, y: homeIcon.y,
                                   buttons: .primary, clickCount: 1))
                spin(0.05)
                c.send(i.mouseUp(x: homeIcon.x, y: homeIcon.y, buttons: []))
                spin(0.05)
                // clickCount 2 is what Tracker keys "open" on.
                c.send(i.mouseDown(x: homeIcon.x, y: homeIcon.y,
                                   buttons: .primary, clickCount: 2))
                spin(0.05)
                c.send(i.mouseUp(x: homeIcon.x, y: homeIcon.y, buttons: []))
            },
            Step(name: "right click empty desktop (context menu)",
                 expectsRepaint: true, settle: 2.0) { c, i in
                c.send(i.mouseDown(x: emptyDesktop.x, y: emptyDesktop.y,
                                   buttons: .secondary, clickCount: 1))
                spin(0.1)
                c.send(i.mouseUp(x: emptyDesktop.x, y: emptyDesktop.y, buttons: []))
            },
            Step(name: "Escape to dismiss the menu",
                 expectsRepaint: true, settle: 1.5) { c, i in
                c.send(i.key(down: true, characters: "\u{1b}",
                             charactersIgnoringModifiers: "\u{1b}", keyCode: 0x35))
                spin(0.05)
                c.send(i.key(down: false, characters: "\u{1b}",
                             charactersIgnoringModifiers: "\u{1b}", keyCode: 0x35))
            },
        ]

        struct Result { let name: String; let changed: Int; let rect: String
                        let expected: Bool; let ok: Bool }
        var results: [Result] = []

        for (index, step) in steps.enumerated() {
            let before = cursorPos()
            step.action(connection, input)
            spin(step.settle)

            let path = String(format: "%@/drive-%02d-%@.png", outDir, index + 1,
                              step.name.replacingOccurrences(of: " ", with: "-")
                                  .replacingOccurrences(of: "(", with: "")
                                  .replacingOccurrences(of: ")", with: ""))
            save(frame(), to: path)

            guard let now = frame().flatMap(pixels) else {
                results.append(Result(name: step.name, changed: -1, rect: "no frame",
                                      expected: step.expectsRepaint, ok: false))
                continue
            }
            let d = diff(previous, now)
            previous = now

            var ok = step.expectsRepaint ? (d.count > 0) : (d.count == 0)
            var note = d.rect
            if !step.expectsRepaint {
                // Nothing should have been painted, but the server should still
                // have told us where the pointer went.
                let after = cursorPos()
                let moved = after != before
                if !moved { ok = false; note += " (cursor never moved either)" }
                else { note += " cursor \(fmt(before)) -> \(fmt(after))" }
            }
            results.append(Result(name: step.name, changed: d.count, rect: note,
                                  expected: step.expectsRepaint, ok: ok))
            print(String(format: "  %-52s %8d px  %@", step.name, d.count, note))
        }

        print("\n== results ==")
        var failures = 0
        for r in results {
            let verdict = r.ok ? "ok" : "FAIL"
            if !r.ok { failures += 1 }
            print(String(format: "  %-4s %-52s expected %@, changed %d px",
                         verdict, r.name,
                         r.expected ? "a repaint" : "no repaint", r.changed))
        }
        if let stats = connection.stats {
            print("\nmessages decoded: \(stats.messages)")
            print("RP_DRAW_STRING replies: \(stats.drawStringReplies)")
            print("RP_STRING_WIDTH replies: \(stats.stringWidthReplies)")
            if stats.unhandled.isEmpty {
                print("unhandled ops: none")
            } else {
                let names = stats.unhandled
                    .map { "\(RP.name($0.key))×\($0.value)" }.sorted()
                print("unhandled ops: \(names.joined(separator: ", "))")
                failures += 1
            }
        }
        connection.disconnect()
        spin(0.3)

        print("\nframes written to \(outDir)/drive-*.png")
        if failures == 0 {
            print("\nPASS — the live desktop responded to every input")
            exit(0)
        }
        print("\nFAIL — \(failures) step(s) did not behave as expected")
        exit(1)
    }

    // MARK: - Frame comparison

    /// Straight BGRA bytes for a frame, so two frames can be compared directly.
    private static func pixels(_ img: CGImage) -> (w: Int, h: Int, b: [UInt8])? {
        let w = img.width, h = img.height
        var buf = [UInt8](repeating: 0, count: w * h * 4)
        let info: CGBitmapInfo = [
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue),
            .byteOrder32Little,
        ]
        let ok = buf.withUnsafeMutableBytes { p -> Bool in
            guard let c = CGContext(data: p.baseAddress, width: w, height: h,
                                   bitsPerComponent: 8, bytesPerRow: w * 4,
                                   space: CGColorSpaceCreateDeviceRGB(),
                                   bitmapInfo: info.rawValue) else { return false }
            c.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? (w, h, buf) : nil
    }

    /// Number of differing pixels and the bounding box of the change, which is
    /// what makes a result readable: "12000 px in (0,0)-(400,300)" localises the
    /// repaint to a window that opened.
    private static func diff(_ a: (w: Int, h: Int, b: [UInt8]),
                            _ b: (w: Int, h: Int, b: [UInt8]))
        -> (count: Int, rect: String) {
        guard a.w == b.w, a.h == b.h else { return (-1, "size changed") }
        var count = 0
        var minX = Int.max, minY = Int.max, maxX = -1, maxY = -1
        for y in 0..<a.h {
            let row = y * a.w * 4
            for x in 0..<a.w {
                let o = row + x * 4
                if a.b[o] != b.b[o] || a.b[o + 1] != b.b[o + 1]
                    || a.b[o + 2] != b.b[o + 2] {
                    count += 1
                    if x < minX { minX = x }
                    if y < minY { minY = y }
                    if x > maxX { maxX = x }
                    if y > maxY { maxY = y }
                }
            }
        }
        if count == 0 { return (0, "no change") }
        return (count, "in (\(minX),\(minY))-(\(maxX),\(maxY))")
    }

    private static func fmt(_ p: CGPoint) -> String {
        String(format: "(%.0f,%.0f)", p.x, p.y)
    }

    private static func save(_ img: CGImage?, to path: String) {
        guard let img else { return }
        let url = URL(fileURLWithPath: (path as NSString).expandingTildeInPath)
        guard let dest = CGImageDestinationCreateWithURL(
            url as CFURL, UTType.png.identifier as CFString, 1, nil) else { return }
        CGImageDestinationAddImage(dest, img, nil)
        _ = CGImageDestinationFinalize(dest)
    }

    private static func spin(_ seconds: Double) {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            RunLoop.current.run(mode: .default,
                                before: Date().addingTimeInterval(0.02))
        }
    }
}
