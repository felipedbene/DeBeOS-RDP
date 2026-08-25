import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Headless display-mode-change mode.
///
/// `RP_UPDATE_DISPLAY_MODE` is not only a handshake step: the server accepts it
/// at any time and answers with `_NotifyScreenChanged()` and a full repaint. That
/// is what lets the window be resized without reconnecting — and it is the one
/// feature that cannot be checked from a unit test, because it needs a real
/// server on the other end deciding to repaint.
///
/// Connects, settles, resizes, settles again, and asserts the canvas actually
/// changed size and still has content in the region that was previously off
/// screen.
enum ResizeTest {
    static func run(host: String, port: UInt16, width: Int, height: Int,
                    toWidth: Int, toHeight: Int, seconds: Double,
                    output: String?) -> Never {
        let connection = RemoteConnection()
        var latest: CGImage?
        let lock = NSLock()
        var failures: [String] = []

        connection.onLog = { m in
            FileHandle.standardError.write(Data(("  " + m + "\n").utf8))
        }
        connection.onFrame = { img in
            lock.lock(); latest = img; lock.unlock()
        }

        func settle(_ s: Double) {
            let deadline = Date().addingTimeInterval(s)
            while Date() < deadline {
                RunLoop.current.run(mode: .default,
                                    before: Date().addingTimeInterval(0.05))
            }
        }

        func snapshot() -> CGImage? {
            lock.lock(); defer { lock.unlock() }
            return latest
        }

        print("connecting to \(host):\(port) as \(width)x\(height)…")
        connection.connect(host: host, port: port, width: width, height: height)
        settle(seconds)

        guard let before = snapshot() else {
            print("FAIL — no frame at the initial size")
            exit(1)
        }
        print("initial frame: \(before.width)x\(before.height)")
        if before.width != width || before.height != height {
            failures.append("initial frame is \(before.width)x\(before.height), "
                            + "expected \(width)x\(height)")
        }

        print("requesting \(toWidth)x\(toHeight) on the live connection…")
        connection.changeDisplayMode(width: toWidth, height: toHeight)
        settle(seconds)

        guard let after = snapshot() else {
            print("FAIL — no frame after the resize")
            exit(1)
        }
        print("resized frame: \(after.width)x\(after.height)")
        if after.width != toWidth || after.height != toHeight {
            failures.append("resized frame is \(after.width)x\(after.height), "
                            + "expected \(toWidth)x\(toHeight)")
        }

        // The point of the exercise: the server has to have repainted into the
        // area that did not exist before. A canvas that merely got bigger while
        // staying blank would pass a size check and still be useless.
        if toWidth > width || toHeight > height {
            let sampleX = toWidth > width ? (width + toWidth) / 2 : toWidth / 2
            let sampleY = toHeight > height ? (height + toHeight) / 2 : toHeight / 2
            if let px = pixel(after, x: min(sampleX, after.width - 1),
                              y: min(sampleY, after.height - 1)) {
                print("newly exposed pixel at (\(sampleX),\(sampleY)): "
                      + "rgba(\(px.0),\(px.1),\(px.2),\(px.3))")
                if px.3 == 0 {
                    failures.append("the newly exposed area is transparent — "
                                    + "the repaint never arrived")
                }
            }
        }

        if let stats = connection.stats {
            print("messages decoded: \(stats.messages)")
            if stats.unhandled.isEmpty {
                print("unhandled ops: none")
            } else {
                let names = stats.unhandled
                    .map { "\(RP.name($0.key))×\($0.value)" }.sorted()
                failures.append("unhandled ops: \(names.joined(separator: ", "))")
            }
        }

        connection.disconnect()

        if let output {
            let url = URL(fileURLWithPath: (output as NSString).expandingTildeInPath)
            if let dest = CGImageDestinationCreateWithURL(
                url as CFURL, UTType.png.identifier as CFString, 1, nil) {
                CGImageDestinationAddImage(dest, after, nil)
                if CGImageDestinationFinalize(dest) {
                    print("wrote \(url.path)")
                }
            }
        }

        if failures.isEmpty {
            print("\nPASS — resized \(width)x\(height) -> \(toWidth)x\(toHeight) "
                  + "on one connection, with a repaint")
            exit(0)
        }
        print("\nFAIL")
        for f in failures { print("  - \(f)") }
        exit(1)
    }

    /// One pixel out of a CGImage, in top-left origin coordinates.
    private static func pixel(_ img: CGImage, x: Int, y: Int)
        -> (UInt8, UInt8, UInt8, UInt8)? {
        var buf = [UInt8](repeating: 0, count: 4)
        let info: CGBitmapInfo = [
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue),
            .byteOrder32Little,
        ]
        buf.withUnsafeMutableBytes { p in
            guard let ctx = CGContext(data: p.baseAddress, width: 1, height: 1,
                                     bitsPerComponent: 8, bytesPerRow: 4,
                                     space: CGColorSpaceCreateDeviceRGB(),
                                     bitmapInfo: info.rawValue) else { return }
            ctx.draw(img, in: CGRect(x: -CGFloat(x),
                                     y: -CGFloat(img.height - 1 - y),
                                     width: CGFloat(img.width),
                                     height: CGFloat(img.height)))
        }
        return (buf[2], buf[1], buf[0], buf[3])
    }
}
