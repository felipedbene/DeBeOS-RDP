import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Headless connect-and-screenshot mode.
///
/// This is how the render path gets verified objectively: it produces a PNG that
/// can be diffed against the browser demo's canvas, with no GUI interaction in
/// the loop. Also the fastest way to check a real session's first frame from a
/// terminal.
enum Capture {
    static func run(host: String, port: UInt16, width: Int, height: Int,
                    seconds: Double, output: String) -> Never {
        let connection = RemoteConnection()
        var latest: CGImage?
        let lock = NSLock()

        connection.onLog = { m in
            FileHandle.standardError.write(Data(("  " + m + "\n").utf8))
        }
        connection.onStateChange = { st in
            FileHandle.standardError.write(Data("  state: \(st)\n".utf8))
        }
        connection.onFrame = { img in
            lock.lock(); latest = img; lock.unlock()
        }

        print("connecting to \(host):\(port) as \(width)x\(height)…")
        connection.connect(host: host, port: port, width: width, height: height)

        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
        }

        lock.lock()
        let image = latest
        lock.unlock()

        if let stats = connection.stats {
            print("messages decoded: \(stats.messages)")
            print("RP_DRAW_STRING replies: \(stats.drawStringReplies)")
            print("RP_STRING_WIDTH replies: \(stats.stringWidthReplies)")
            print("bitmap bytes decoded: \(stats.bitmapBytes)")
            if stats.unhandled.isEmpty {
                print("unhandled ops: none")
            } else {
                let names = stats.unhandled
                    .map { "\(RP.name($0.key))×\($0.value)" }
                    .sorted()
                print("unhandled ops: \(names.joined(separator: ", "))")
            }
        }

        connection.disconnect()

        guard let image else {
            print("no frame received — nothing to write")
            exit(1)
        }
        let url = URL(fileURLWithPath: (output as NSString).expandingTildeInPath)
        guard let dest = CGImageDestinationCreateWithURL(
            url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
            print("could not create \(url.path)")
            exit(1)
        }
        CGImageDestinationAddImage(dest, image, nil)
        guard CGImageDestinationFinalize(dest) else {
            print("could not write \(url.path)")
            exit(1)
        }
        print("wrote \(url.path) (\(image.width)x\(image.height))")
        exit(0)
    }
}
