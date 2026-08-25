import CoreGraphics
import Foundation

// A tiny assertion harness. XCTest is not available in a Command Line Tools
// install, and SwiftPM's manifest library is broken there too, so these tests
// build as a plain executable via build.sh.

var failures: [String] = []
var checks = 0
var currentTest = "?"

func check(_ condition: Bool, _ what: String,
           _ file: StaticString = #file, _ line: UInt = #line) {
    checks += 1
    if !condition { failures.append("\(currentTest): \(what) (line \(line))") }
}

func equal<T: Equatable>(_ a: T, _ b: T, _ what: String,
                         _ file: StaticString = #file, _ line: UInt = #line) {
    checks += 1
    if a != b {
        failures.append("\(currentTest): \(what) — got \(a), expected \(b) "
                        + "(line \(line))")
    }
}

func throwsError<T>(_ what: String, _ line: UInt = #line,
                    _ body: () throws -> T) {
    checks += 1
    do {
        _ = try body()
        failures.append("\(currentTest): \(what) should have thrown (line \(line))")
    } catch {
        // expected
    }
}

func test(_ name: String, _ body: () throws -> Void) {
    currentTest = name
    do { try body() } catch {
        failures.append("\(name): threw unexpectedly: \(error)")
    }
}

// MARK: - Framing

test("header length includes the header itself") {
    var w = WireWriter(RP.initConnection)
    // An empty message is exactly the 6-byte header, and the length counts
    // itself: 01 00 06 00 00 00 (PROTOCOL.md §2).
    equal([UInt8](w.finish()), [0x01, 0x00, 0x06, 0x00, 0x00, 0x00],
          "empty RP_INIT_CONNECTION bytes")
}

test("framer rejects an undersized declared length") {
    let framer = MessageFramer()
    throwsError("totalLength=3 is impossible") {
        try framer.feed([0x01, 0x00, 0x03, 0x00, 0x00, 0x00])
    }
}

test("framer reassembles across arbitrary chunk boundaries") {
    var stream: [UInt8] = []
    for (code, size) in [(RP.fillRect, 16), (RP.initConnection, 0),
                         (RP.setPattern, 12)] {
        var w = WireWriter(code)
        w.append(raw: [UInt8](repeating: 0xAB, count: size))
        stream += [UInt8](w.finish())
    }
    for chunk in [1, 2, 3, 5, 7, 13, stream.count] {
        let framer = MessageFramer()
        var frames: [MessageFramer.Frame] = []
        var i = 0
        while i < stream.count {
            let end = min(i + chunk, stream.count)
            frames += (try? framer.feed(Array(stream[i..<end]))) ?? []
            i = end
        }
        equal(frames.count, 3, "message count at chunk size \(chunk)")
        equal(framer.pendingByteCount, 0, "no leftovers at chunk size \(chunk)")
        if frames.count == 3 {
            equal(frames[0].payload.count, 16, "payload 0 size")
            equal(frames[1].payload.count, 0, "payload 1 size")
            equal(frames[2].payload.count, 12, "payload 2 size")
        }
    }
}

test("framer skips trailing bytes no handler reads") {
    // RP_DRAW_STRING carries a trailing `bool hasDelta` the reference client
    // never reads. Advancing by declared length keeps the NEXT message aligned;
    // advancing by fields consumed would desync here (PROTOCOL.md §2.1).
    var first = WireWriter(RP.drawString)
    first.append(i32: 7)
    first.append(point: BPoint(x: 1, y: 2))
    first.append(string: "hi")
    first.append(bool: false)
    var second = WireWriter(RP.fillRect)
    second.append(i32: 7)

    let framer = MessageFramer()
    let frames = try framer.feed([UInt8](first.finish()) + [UInt8](second.finish()))
    equal(frames.count, 2, "two messages")
    if frames.count == 2 {
        equal(frames[1].code, RP.fillRect, "second message survived intact")
        var r = WireReader(frames[0].payload)
        equal(try r.i32(), 7, "token")
        _ = try r.point()
        equal(try r.string(), "hi", "string")
        equal(r.remaining, 1, "the unread hasDelta byte is still there")
    }
}

// MARK: - Scalars and packing

test("little-endian unaligned reads") {
    // Payloads begin at offset 6, so fields are misaligned by construction and
    // there is no padding anywhere (PROTOCOL.md §3.2).
    var w = WireWriter(RP.setOffsets)
    w.append(i32: -2)
    w.append(u32: 0xDEAD_BEEF)
    w.append(f32: 1.5)
    var r = WireReader([UInt8](w.finish()), at: RP.headerSize)
    equal(try r.i32(), -2, "negative int32")
    equal(try r.u32(), 0xDEAD_BEEF, "uint32")
    equal(try r.f32(), 1.5, "float")
}

test("bool is one byte") {
    var r = WireReader([0x00, 0x01])
    equal(try r.bool(), false, "false")
    equal(try r.bool(), true, "true")
    equal(r.remaining, 0, "two bools consumed two bytes")
}

test("reader throws instead of overrunning") {
    throwsError("int32 from a 2-byte buffer") {
        var r = WireReader([0x01, 0x02])
        return try r.i32()
    }
}

// MARK: - BRect inclusivity

test("BRect edges are inclusive") {
    // 0..199 is 200 wide, not 199. Getting this wrong puts a one-pixel seam on
    // every fill in the session (PROTOCOL.md §3.3).
    let r = BRect(left: 0, top: 0, right: 199, bottom: 39)
    equal(r.width, 200, "width")
    equal(r.height, 40, "height")
    equal(r.cgRect.width, 200, "cgRect width")
    equal(r.cgRect.height, 40, "cgRect height")
}

test("BRect fractional edges round outward") {
    let r = BRect(left: 0.4, top: 0.6, right: 9.2, bottom: 9.8)
    equal(r.cgRect.minX, 0, "floored origin x")
    equal(r.cgRect.minY, 0, "floored origin y")
    equal(r.cgRect.width, 11, "ceiled width")
}

// MARK: - Composite types

test("region layout is count then rects") {
    var w = WireWriter(RP.fillRegion)
    w.append(i32: 2)
    for (l, t, rr, b) in [(0, 0, 9, 9), (10, 10, 19, 19)] {
        w.append(f32: Float(l)); w.append(f32: Float(t))
        w.append(f32: Float(rr)); w.append(f32: Float(b))
    }
    var r = WireReader([UInt8](w.finish()), at: RP.headerSize)
    let rects = try r.region()
    equal(rects.count, 2, "rect count")
    equal(rects[1].left, 10, "second rect left")
    equal(rects[0].width, 10, "first rect inclusive width")
}

test("string is length-prefixed and not NUL-terminated") {
    var w = WireWriter(RP.stringWidth)
    w.append(i32: 3)
    w.append(string: "héllo")
    var r = WireReader([UInt8](w.finish()), at: RP.headerSize)
    equal(try r.i32(), 3, "token")
    equal(try r.string(), "héllo", "round trip with multi-byte UTF-8")
    equal(r.remaining, 0, "no NUL terminator on the wire")
}

test("identity transform is a single byte") {
    var r = WireReader([0x01])
    equal(try r.transform(), .identity, "identity flag short-circuits")
}

test("non-identity transform is a bool plus six doubles") {
    var bytes: [UInt8] = [0x00]
    for v in [2.0, 0.0, 0.0, 3.0, 10.0, 20.0] as [Double] {
        withUnsafeBytes(of: v.bitPattern.littleEndian) { bytes += $0 }
    }
    equal(bytes.count, 49, "1 + 6*8 bytes")
    var r = WireReader(bytes)
    let t = try r.transform()
    equal(t.a, 2.0, "sx")
    equal(t.d, 3.0, "sy")
    equal(t.tx, 10.0, "tx")
    equal(t.ty, 20.0, "ty")
    equal(r.remaining, 0, "fully consumed")
}

test("font familyAndStyle is one uint32, not two uint16s") {
    // The server sends uint16 face then uint32 familyAndStyle = family<<16|style.
    // The reference JS client reads three uint16s and so has family and style
    // swapped; decode it correctly (PROTOCOL.md §5.2).
    var bytes: [UInt8] = [0, 0]
    bytes += [0, 0, 0, 0]
    bytes += [3]
    for _ in 0..<3 { bytes += [0, 0, 0, 0] }
    withUnsafeBytes(of: Float(14).bitPattern.littleEndian) { bytes += $0 }
    withUnsafeBytes(of: UInt16(0x0020).littleEndian) { bytes += $0 }
    withUnsafeBytes(of: UInt32((7 << 16) | 9).littleEndian) { bytes += $0 }

    // 1+1+4+1+4+4+4+4+2+4 = 29.
    equal(bytes.count, 29, "font record is 29 bytes")
    var r = WireReader(bytes)
    let f = try r.font()
    equal(f.size, 14, "size")
    check(f.isBold, "bold face bit")
    check(f.isMonospaced, "fixed spacing means monospace")
    equal(f.family, 7, "family from the high half")
    equal(f.style, 9, "style from the low half")
    equal(r.remaining, 0, "fully consumed")
}

test("gradient stop offsets are 0-255 not 0-1") {
    var bytes: [UInt8] = []
    withUnsafeBytes(of: UInt32(0).littleEndian) { bytes += $0 }
    for v in [Float(0), 0, 10, 10] {
        withUnsafeBytes(of: v.bitPattern.littleEndian) { bytes += $0 }
    }
    withUnsafeBytes(of: Int32(2).littleEndian) { bytes += $0 }
    bytes += [255, 0, 0, 255]
    withUnsafeBytes(of: Float(0).bitPattern.littleEndian) { bytes += $0 }
    bytes += [0, 0, 255, 255]
    withUnsafeBytes(of: Float(255).bitPattern.littleEndian) { bytes += $0 }

    var r = WireReader(bytes)
    let g = try r.gradient()
    equal(g.kind, .linear, "linear kind")
    equal(g.stops.count, 2, "stop count")
    equal(g.stops[1].offset, 255, "far end is 255, meaning 1.0 after /255")
}

// MARK: - Dispatch rule

test("session-level set matches the protocol doc") {
    // Getting this set wrong misaligns every later field by 4 bytes.
    for code in [RP.initConnection, RP.closeConnection,
                 RP.getSystemPaletteResult, RP.createState, RP.deleteState,
                 RP.invalidateRect, RP.invalidateRegion, RP.copyRectNoClipping,
                 RP.fillRegionColorNoClipping, RP.setCursor,
                 RP.setCursorVisible, RP.moveCursorTo] {
        check(RP.sessionLevel.contains(code),
              "\(RP.name(code)) must be session-level")
    }
    for code in [RP.fillRect, RP.strokeRect, RP.drawString, RP.stringWidth,
                 RP.setHighColor, RP.constrainClippingRegion, RP.drawBitmap,
                 RP.drawBitmapRects, RP.fillRegion, RP.fillRectColor,
                 RP.strokeRect1pxColor] {
        check(!RP.sessionLevel.contains(code),
              "\(RP.name(code)) must be token-addressed")
    }
}

// MARK: - Client -> server layouts

test("RP_UPDATE_DISPLAY_MODE layout") {
    let d = [UInt8](ClientMessage.updateDisplayMode(width: 1280, height: 800))
    equal(d.count, 14, "6 header + 2 int32")
    var r = WireReader(d)
    equal(try r.u16(), RP.updateDisplayMode, "code")
    equal(try r.u32(), 14, "declared length includes header")
    equal(try r.i32(), 1280, "width")
    equal(try r.i32(), 800, "height")
}

test("RP_STRING_WIDTH_RESULT layout") {
    let d = [UInt8](ClientMessage.stringWidthResult(token: 5, width: 42.5))
    var r = WireReader(d)
    equal(try r.u16(), RP.stringWidthResult, "code")
    equal(try r.u32(), 14, "length")
    equal(try r.i32(), 5, "token")
    equal(try r.f32(), 42.5, "width")
}

test("mouse moved carries no buttons but down carries buttons and clicks") {
    // RP_MOUSE_MOVED omits buttons; the server reuses the last known state,
    // which is why dragging depends on a prior mouse-down (PROTOCOL.md §8).
    equal([UInt8](ClientMessage.mouseMoved(x: 1, y: 2)).count, 6 + 8,
          "moved size")
    equal([UInt8](ClientMessage.mouseDown(x: 1, y: 2, buttons: 1, clicks: 2)).count,
          6 + 8 + 4 + 4, "down size")
    equal([UInt8](ClientMessage.mouseUp(x: 1, y: 2, buttons: 0)).count,
          6 + 8 + 4, "up size")
}

test("key message layout") {
    let d = [UInt8](ClientMessage.key(down: true, text: "A", rawChar: 97,
                                     key: 0x3C))
    var r = WireReader(d)
    equal(try r.u16(), RP.keyDown, "code")
    _ = try r.u32()
    equal(try r.string(), "A", "composed text")
    equal(try r.i32(), 97, "raw char")
    equal(try r.i32(), 0x3C, "haiku key code")
}

// MARK: - Bitmaps

test("B_RGB32 decodes BGRA with no channel swap and forced alpha") {
    // Haiku's B_RGB32 is byte-order BGRA, matching CoreGraphics; the decoder
    // should copy straight through (PROTOCOL.md §6.3).
    let header = BitmapDecoder.Header(width: 2, height: 1, bytesPerRow: 8,
                                     colorSpace: .rgb32, flags: 0, bitsLength: 8)
    // Pixel 0: B=10 G=20 R=30. Pixel 1: B=40 G=50 R=60.
    let raw: [UInt8] = [10, 20, 30, 0, 40, 50, 60, 0]
    let bm = BitmapDecoder.convert(header: header, raw: raw, forceOpaque: false,
                                  palette: nil)
    equal(bm.bgra[0], 10, "blue preserved")
    equal(bm.bgra[1], 20, "green preserved")
    equal(bm.bgra[2], 30, "red preserved")
    equal(bm.bgra[3], 255, "B_RGB32 has no alpha, so opaque")
    equal(bm.bgra[6], 60, "second pixel red")
}

test("B_RGB32 transparent magic clears alpha") {
    // 0xff777477 means see-through.
    let header = BitmapDecoder.Header(width: 1, height: 1, bytesPerRow: 4,
                                     colorSpace: .rgb32, flags: 0, bitsLength: 4)
    let raw: [UInt8] = [0x77, 0x74, 0x77, 0xff]   // little-endian 0xff777477
    let bm = BitmapDecoder.convert(header: header, raw: raw, forceOpaque: false,
                                  palette: nil)
    equal(bm.bgra[3], 0, "magic pixel becomes transparent")

    let opaque = BitmapDecoder.convert(header: header, raw: raw,
                                      forceOpaque: true, palette: nil)
    equal(opaque.bgra[3], 255, "under B_OP_COPY the magic is ignored")
}

test("bitmap row padding is skipped") {
    // bytesPerRow can exceed width*bpp; the padding must not shift the image
    // (PROTOCOL.md §6.1).
    let header = BitmapDecoder.Header(width: 2, height: 2, bytesPerRow: 8,
                                     colorSpace: .gray8, flags: 0, bitsLength: 16)
    var raw = [UInt8](repeating: 0, count: 16)
    raw[0] = 11; raw[1] = 22           // row 0 pixels, then 6 bytes padding
    raw[8] = 33; raw[9] = 44           // row 1 pixels
    let bm = BitmapDecoder.convert(header: header, raw: raw, forceOpaque: false,
                                  palette: nil)
    equal(bm.bgra[0], 11, "row 0 pixel 0")
    equal(bm.bgra[4], 22, "row 0 pixel 1")
    equal(bm.bgra[8], 33, "row 1 pixel 0 comes from the padded offset")
    equal(bm.bgra[12], 44, "row 1 pixel 1")
}

test("B_GRAY1 is LSB-first within each byte") {
    // Opposite convention from `pattern`, which is MSB-first (PROTOCOL.md §6.3).
    let header = BitmapDecoder.Header(width: 8, height: 1, bytesPerRow: 1,
                                     colorSpace: .gray1, flags: 0, bitsLength: 1)
    let bm = BitmapDecoder.convert(header: header, raw: [0b0000_0001],
                                  forceOpaque: false, palette: nil)
    equal(bm.bgra[0], 255, "bit 0 set -> leftmost pixel white")
    equal(bm.bgra[4], 0, "bit 1 clear -> next pixel black")
}

test("B_CMAP8 uses the system palette") {
    let header = BitmapDecoder.Header(width: 1, height: 1, bytesPerRow: 1,
                                     colorSpace: .cmap8, flags: 0, bitsLength: 1)
    // Palette entries are packed r | g<<8 | b<<16 | a<<24.
    var palette = [UInt32](repeating: 0, count: 256)
    palette[5] = UInt32(30) | UInt32(20) << 8 | UInt32(10) << 16
        | UInt32(255) << 24
    let bm = BitmapDecoder.convert(header: header, raw: [5], forceOpaque: false,
                                  palette: palette)
    equal(bm.bgra[0], 10, "blue from palette")
    equal(bm.bgra[1], 20, "green from palette")
    equal(bm.bgra[2], 30, "red from palette")
}

test("pattern solidity detection") {
    let s = DrawState(token: 1)
    check(s.patternIsSolid, "default all-0xff pattern is solid high")
    s.pattern = [0, 0, 0, 0, 0, 0, 0, 0]
    check(s.patternIsSolid, "all-zero is solid low")
    equal(s.solidColor.r, s.lowColor.r, "all-zero resolves to the low colour")
    s.pattern = [0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55]
    check(!s.patternIsSolid, "alternating bytes are a real stipple")
    // The one that matters: every row identical does NOT mean solid. These are
    // vertical stripes, and calling them solid fills flat with the high colour.
    // Only all-bits-set and all-bits-clear are solid (PatternHandler.h:124).
    s.pattern = [UInt8](repeating: 0xf0, count: 8)
    check(!s.patternIsSolid, "uniform rows of 0xf0 are four-on four-off columns")
    s.pattern = [UInt8](repeating: 0xcc, count: 8)
    check(!s.patternIsSolid, "uniform rows of 0xcc are two-on two-off columns")
    s.pattern = [UInt8](repeating: 0x7f, count: 8)
    check(!s.patternIsSolid, "even a single clear bit per row is a stipple")
}

test("B_OP_COPY forces opaque colours") {
    let s = DrawState(token: 1)
    s.drawingMode = .copy
    s.forceOpaque = true
    s.highColor = RGBColor(r: 1, g: 2, b: 3, a: 0)
    let c = s.strokeAndFillColor()
    equal(c.alpha, 1.0, "alpha forced to 1 under B_OP_COPY")
}

// MARK: - Renderer integration

test("renderer decodes a full synthetic scene without error") {
    guard let canvas = Canvas(width: 200, height: 120) else {
        failures.append("could not create canvas"); return
    }
    final class Collector: SessionRendererDelegate {
        var sent: [Data] = []
        var logs: [String] = []
        func rendererWantsToSend(_ data: Data) { sent.append(data) }
        func rendererDidUpdate(rect: CGRect?) {}
        func rendererCursorChanged() {}
        func rendererLog(_ message: String) { logs.append(message) }
    }
    let collector = Collector()
    let renderer = SessionRenderer(canvas: canvas)
    renderer.delegate = collector

    func send(_ code: UInt16, _ build: (inout WireWriter) -> Void) {
        var w = WireWriter(code)
        build(&w)
        let bytes = [UInt8](w.finish())
        renderer.handle(code: code,
                        payload: Array(bytes[RP.headerSize...]))
    }

    send(RP.createState) { $0.append(i32: 1) }
    send(RP.fillRegionColorNoClipping) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 199); w.append(f32: 119)
        w.append(raw: [51, 102, 152, 255])
    }
    send(RP.setDrawingMode) { $0.append(i32: 1); $0.append(u32: 1) }
    send(RP.setHighColor) { $0.append(i32: 1); $0.append(raw: [255, 0, 0, 255]) }
    send(RP.fillRect) { w in
        w.append(i32: 1)
        w.append(f32: 10); w.append(f32: 10); w.append(f32: 49); w.append(f32: 29)
    }
    send(RP.strokeRect1pxColor) { w in
        w.append(i32: 1)
        w.append(f32: 60); w.append(f32: 10); w.append(f32: 99); w.append(f32: 29)
        w.append(raw: [0, 0, 0, 255])
    }
    // Text must produce a reply, because app_server blocks on it.
    send(RP.drawString) { w in
        w.append(i32: 1)
        w.append(point: BPoint(x: 10, y: 60))
        w.append(string: "hello")
        w.append(bool: false)
    }
    send(RP.stringWidth) { w in
        w.append(i32: 1)
        w.append(string: "hello")
    }

    equal(renderer.stats.messages, 8, "all messages handled")
    equal(renderer.stats.unhandled.count, 0,
          "no unhandled ops: \(renderer.stats.unhandled)")
    equal(collector.logs.filter { $0.contains("decode error") }.count, 0,
          "no decode errors: \(collector.logs)")
    equal(renderer.stats.drawStringReplies, 1, "replied to RP_DRAW_STRING")
    equal(renderer.stats.stringWidthReplies, 1, "replied to RP_STRING_WIDTH")
    equal(collector.sent.count, 2, "exactly two replies sent")

    // Verify the reply codes are what app_server is waiting for.
    if collector.sent.count == 2 {
        var r0 = WireReader([UInt8](collector.sent[0]))
        equal(try r0.u16(), RP.drawStringResult, "first reply code")
        var r1 = WireReader([UInt8](collector.sent[1]))
        equal(try r1.u16(), RP.stringWidthResult, "second reply code")
        _ = try r1.u32()
        equal(try r1.i32(), 1, "reply carries the token")
        let width = try r1.f32()
        check(width > 0, "measured width is positive (got \(width))")
    }

    // The red fill must actually be on the canvas at 10,10 and not at the
    // bottom-left, which is what happens if the y-flip is missing.
    if let img = canvas.snapshot() {
        equal(img.width, 200, "snapshot width")
        equal(img.height, 120, "snapshot height")
        let px = pixel(img, x: 20, y: 20)
        check(px.r > 200 && px.g < 60 && px.b < 60,
              "pixel at (20,20) should be the red fill, got \(px)")
        let bg = pixel(img, x: 150, y: 100)
        check(bg.b > bg.r && bg.b > 100,
              "pixel at (150,100) should be the blue desktop, got \(bg)")
    } else {
        failures.append("could not snapshot canvas")
    }
}

struct Pixel: CustomStringConvertible {
    var r: UInt8, g: UInt8, b: UInt8, a: UInt8
    var description: String { "rgba(\(r),\(g),\(b),\(a))" }
}

/// Reads a pixel in top-left origin coordinates, matching Haiku's convention.
func pixel(_ img: CGImage, x: Int, y: Int) -> Pixel {
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
        // Shift the image so the pixel of interest lands in the 1x1 context.
        ctx.draw(img, in: CGRect(x: -CGFloat(x), y: -CGFloat(img.height - 1 - y),
                                 width: CGFloat(img.width),
                                 height: CGFloat(img.height)))
    }
    return Pixel(r: buf[2], g: buf[1], b: buf[0], a: buf[3])
}

test("copyRect moves pixels, as used for scrolling") {
    guard let canvas = Canvas(width: 64, height: 64) else {
        failures.append("no canvas"); return
    }
    let renderer = SessionRenderer(canvas: canvas)
    func send(_ code: UInt16, _ build: (inout WireWriter) -> Void) {
        var w = WireWriter(code)
        build(&w)
        renderer.handle(code: code,
                        payload: Array([UInt8](w.finish())[RP.headerSize...]))
    }
    send(RP.fillRegionColorNoClipping) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 63); w.append(f32: 63)
        w.append(raw: [0, 0, 0, 255])
    }
    // A *vertically asymmetric* source: green top half, blue bottom half. A
    // uniform square is invariant under a y-mirror, so it cannot tell a correct
    // copy from an upside-down one — which is exactly how a mirrored
    // RP_COPY_RECT survived here and flipped every Terminal scrollback.
    send(RP.fillRectColor) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 15); w.append(f32: 7)
        w.append(raw: [0, 255, 0, 255])
    }
    send(RP.fillRectColor) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 8); w.append(f32: 15); w.append(f32: 15)
        w.append(raw: [0, 0, 255, 255])   // blue
    }
    // Move the square down by 32.
    send(RP.copyRectNoClipping) { w in
        w.append(i32: 0); w.append(i32: 32)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 15); w.append(f32: 15)
    }
    if let img = canvas.snapshot() {
        // Exact values, not thresholds: the top half must still be the top half.
        let top = pixel(img, x: 8, y: 36)
        let bottom = pixel(img, x: 8, y: 44)
        check(top.r == 0 && top.g == 255 && top.b == 0,
              "copied top half should stay green at y=36, got \(top)")
        check(bottom.r == 0 && bottom.g == 0 && bottom.b == 255,
              "copied bottom half should stay blue at y=44, got \(bottom)")
        // And the source is untouched by the copy.
        let srcTop = pixel(img, x: 8, y: 4)
        check(srcTop.r == 0 && srcTop.g == 255 && srcTop.b == 0,
              "source top half should still be green, got \(srcTop)")
    }
}

test("copyRect keeps a one-line scroll upright") {
    // The real-world shape of the bug: Terminal scrolls by copying its text area
    // up one line, and a mirrored blit reverses the whole scrollback block.
    guard let canvas = Canvas(width: 32, height: 32) else {
        failures.append("no canvas"); return
    }
    let renderer = SessionRenderer(canvas: canvas)
    func send(_ code: UInt16, _ build: (inout WireWriter) -> Void) {
        var w = WireWriter(code)
        build(&w)
        renderer.handle(code: code,
                        payload: Array([UInt8](w.finish())[RP.headerSize...]))
    }
    // Three distinct 4px "lines" at y 0..3, 4..7, 8..11.
    let lines: [(Float, [UInt8])] = [(0, [255, 0, 0, 255]),    // red
                                     (4, [0, 255, 0, 255]),    // green
                                     (8, [0, 0, 255, 255])]    // blue
    for (y, bgra) in lines {
        send(RP.fillRectColor) { w in
            w.append(i32: 1)
            w.append(f32: 0); w.append(f32: y)
            w.append(f32: 31); w.append(f32: y + 3)
            w.append(raw: bgra)
        }
    }
    // Scroll the 12px block up by one 4px line.
    send(RP.copyRectNoClipping) { w in
        w.append(i32: 0); w.append(i32: -4)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 31); w.append(f32: 11)
    }
    if let img = canvas.snapshot() {
        // red scrolled off the top; green then blue, still in that order.
        let a = pixel(img, x: 16, y: 1)
        let b = pixel(img, x: 16, y: 5)
        check(a.r == 0 && a.g == 255 && a.b == 0,
              "after scrolling up, green should be the first line, got \(a)")
        check(b.r == 0 && b.g == 0 && b.b == 255,
              "after scrolling up, blue should be the second line, got \(b)")
    }
}

// MARK: - Drawing modes

/// Drives the renderer over a canvas and reads pixels back.
final class Scene {
    let canvas: Canvas
    let renderer: SessionRenderer
    let sink = Sink()

    final class Sink: SessionRendererDelegate {
        var sent: [Data] = []
        var logs: [String] = []
        func rendererWantsToSend(_ data: Data) { sent.append(data) }
        func rendererDidUpdate(rect: CGRect?) {}
        func rendererCursorChanged() {}
        func rendererLog(_ message: String) { logs.append(message) }
    }

    init?(_ w: Int, _ h: Int) {
        guard let c = Canvas(width: w, height: h) else { return nil }
        canvas = c
        renderer = SessionRenderer(canvas: c)
        renderer.delegate = sink
    }

    func send(_ code: UInt16, _ build: (inout WireWriter) -> Void) {
        var w = WireWriter(code)
        build(&w)
        renderer.handle(code: code,
                        payload: Array([UInt8](w.finish())[RP.headerSize...]))
    }

    /// Paints the whole canvas via the session-level colour fill, which applies
    /// no state and so is a reliable way to prime the destination.
    func background(_ c: RGBColor) {
        send(RP.fillRegionColorNoClipping) { w in
            w.append(i32: 1)
            w.append(f32: 0); w.append(f32: 0)
            w.append(f32: Float(canvas.width - 1))
            w.append(f32: Float(canvas.height - 1))
            w.append(raw: [c.r, c.g, c.b, c.a])
        }
    }

    func mode(_ m: DrawingMode, token: Int32 = 1) {
        send(RP.setDrawingMode) { $0.append(i32: token); $0.append(u32: m.rawValue) }
    }
    func high(_ c: RGBColor, token: Int32 = 1) {
        send(RP.setHighColor) { $0.append(i32: token)
                                $0.append(raw: [c.r, c.g, c.b, c.a]) }
    }
    func low(_ c: RGBColor, token: Int32 = 1) {
        send(RP.setLowColor) { $0.append(i32: token)
                               $0.append(raw: [c.r, c.g, c.b, c.a]) }
    }
    func pattern(_ bytes: [UInt8], token: Int32 = 1) {
        send(RP.setPattern) { $0.append(i32: token); $0.append(raw: bytes) }
    }
    func offsets(_ x: Int32, _ y: Int32, token: Int32 = 1) {
        send(RP.setOffsets) { $0.append(i32: token)
                              $0.append(i32: x); $0.append(i32: y) }
    }
    func fillRect(_ l: Float, _ t: Float, _ r: Float, _ b: Float,
                  token: Int32 = 1) {
        send(RP.fillRect) { w in
            w.append(i32: token)
            w.append(f32: l); w.append(f32: t); w.append(f32: r); w.append(f32: b)
        }
    }
    func fillRectColor(_ l: Float, _ t: Float, _ r: Float, _ b: Float,
                       _ c: RGBColor, token: Int32 = 1) {
        send(RP.fillRectColor) { w in
            w.append(i32: token)
            w.append(f32: l); w.append(f32: t); w.append(f32: r); w.append(f32: b)
            w.append(raw: [c.r, c.g, c.b, c.a])
        }
    }

    func px(_ x: Int, _ y: Int) -> RGBColor {
        canvas.pixel(x: x, y: y) ?? RGBColor(r: 0, g: 0, b: 0, a: 0)
    }
}

func rgb(_ r: UInt8, _ g: UInt8, _ b: UInt8) -> RGBColor {
    RGBColor(r: r, g: g, b: b, a: 255)
}

func sameColor(_ a: RGBColor, _ b: RGBColor, tolerance: Int = 0) -> Bool {
    abs(Int(a.r) - Int(b.r)) <= tolerance
        && abs(Int(a.g) - Int(b.g)) <= tolerance
        && abs(Int(a.b) - Int(b.b)) <= tolerance
}

test("Canvas.pixel agrees with a snapshot read") {
    // softBlend addresses the backing store directly, so the direct byte offset
    // and the CGImage's row order have to mean the same thing. If this fails,
    // every software-blended op is drawn upside down.
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.fillRectColor(4, 2, 7, 3, rgb(255, 128, 64))
    let direct = s.px(5, 2)
    check(sameColor(direct, rgb(255, 128, 64)),
          "direct read at (5,2) should be the fill, got \(direct)")
    if let img = s.canvas.snapshot() {
        let viaImage = pixel(img, x: 5, y: 2)
        equal(direct.r, viaImage.r, "red matches the snapshot")
        equal(direct.g, viaImage.g, "green matches the snapshot")
        equal(direct.b, viaImage.b, "blue matches the snapshot")
    }
    let untouched = s.px(5, 8)
    check(sameColor(untouched, rgb(0, 0, 0)),
          "row 8 was never painted, got \(untouched)")
}

test("B_OP_SUBTRACT subtracts per channel and clamps at zero") {
    // ASSIGN_SUBTRACT (DrawingModeSubtract.h:29). CoreGraphics' .difference,
    // which the reference client uses, would give |d-s| and never clamp.
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 100, 40))
    s.mode(.subtract)
    s.high(rgb(50, 150, 40))
    s.fillRect(0, 0, 31, 15)
    let got = s.px(10, 8)
    check(sameColor(got, rgb(150, 0, 0)),
          "200-50, 100-150 clamped to 0, 40-40 -> (150,0,0), got \(got)")
}

test("B_OP_BLEND is a 50% average, not additive lightening") {
    // ASSIGN_BLEND (DrawingModeBlend.h:26).
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 200, 200))
    s.mode(.blend)
    s.high(rgb(100, 0, 50))
    s.fillRect(0, 0, 31, 15)
    let got = s.px(10, 8)
    check(sameColor(got, rgb(150, 100, 125)),
          "(200+100)/2, (200+0)/2, (200+50)/2 -> (150,100,125), got \(got)")
}

test("B_OP_ADD saturates instead of wrapping") {
    // ASSIGN_ADD (DrawingModeAdd.h:27).
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 10, 0))
    s.mode(.add)
    s.high(rgb(100, 20, 0))
    s.fillRect(0, 0, 31, 15)
    let got = s.px(10, 8)
    check(sameColor(got, rgb(255, 30, 0)),
          "200+100 saturates to 255, 10+20 -> (255,30,0), got \(got)")
}

test("B_OP_MIN compares whole-pixel brightness, not channels") {
    // ASSIGN_MIN (DrawingModeMin.h:21) uses brightness_for, so it replaces the
    // ENTIRE pixel or none of it. CoreGraphics' .darken is per-channel and would
    // give (0,0,0) here — a colour that is in neither operand.
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(255, 0, 0))          // brightness 76
    s.mode(.min)
    s.high(rgb(0, 128, 0))                // brightness 75, so this one wins
    s.fillRect(0, 0, 31, 15)
    let got = s.px(10, 8)
    check(sameColor(got, rgb(0, 128, 0)),
          "the darker-by-brightness source replaces the pixel, got \(got)")
}

test("B_OP_MAX leaves the pixel alone when the source is dimmer") {
    // Same pair as above: per-channel .lighten would give (255,128,0).
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(255, 0, 0))          // brightness 76
    s.mode(.max)
    s.high(rgb(0, 128, 0))                // brightness 75
    s.fillRect(0, 0, 31, 15)
    let got = s.px(10, 8)
    check(sameColor(got, rgb(255, 0, 0)),
          "source is dimmer so nothing changes, got \(got)")
}

test("B_OP_ERASE paints the low colour, it does not cut a hole") {
    // ASSIGN_ERASE (DrawingModeErase.h:21). .destinationOut would leave
    // transparent pixels, which app_server's opaque framebuffer never has.
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 200, 200))
    s.high(rgb(255, 0, 0))
    s.low(rgb(0, 0, 255))
    s.mode(.erase)
    s.fillRect(0, 0, 31, 15)
    let got = s.px(10, 8)
    check(sameColor(got, rgb(0, 0, 255)),
          "erased pixels become the low colour, got \(got)")
    equal(got.a, 255, "and stay opaque")
}

test("B_OP_SELECT swaps high and low and ignores everything else") {
    // compare() (DrawingModeSelect.h:31).
    guard let s = Scene(64, 16) else { failures.append("no canvas"); return }
    s.background(rgb(10, 20, 30))
    s.fillRectColor(0, 0, 15, 15, rgb(255, 0, 0))     // the high colour
    s.fillRectColor(16, 0, 31, 15, rgb(0, 0, 255))    // the low colour
    s.high(rgb(255, 0, 0))
    s.low(rgb(0, 0, 255))
    s.mode(.select)
    s.fillRect(0, 0, 63, 15)
    let wasHigh = s.px(8, 8)
    let wasLow = s.px(24, 8)
    let neither = s.px(48, 8)
    check(sameColor(wasHigh, rgb(0, 0, 255)),
          "a high-coloured pixel becomes low, got \(wasHigh)")
    check(sameColor(wasLow, rgb(255, 0, 0)),
          "a low-coloured pixel becomes high, got \(wasLow)")
    check(sameColor(neither, rgb(10, 20, 30)),
          "any other pixel is untouched, got \(neither)")
}

test("B_OP_INVERT only touches pixels the stipple selects") {
    // ASSIGN_INVERT is gated on IsHighColor (DrawingModeInvert.h:39), so a
    // striped pattern inverts alternate rows and leaves the rest.
    guard let s = Scene(16, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 200, 200))
    s.mode(.invert)
    s.pattern([0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00])
    s.fillRect(0, 0, 15, 15)
    let inverted = s.px(8, 0)
    let kept = s.px(8, 1)
    check(sameColor(inverted, rgb(55, 55, 55)),
          "row 0 is selected and inverts to 55, got \(inverted)")
    check(sameColor(kept, rgb(200, 200, 200)),
          "row 1 is not selected and is untouched, got \(kept)")
}

test("B_SOLID_LOW makes a gated mode a no-op") {
    // Every bit clear means IsHighColor is false everywhere, so B_OP_INVERT has
    // nothing to invert. Collapsing a solid pattern to one colour loses this.
    guard let s = Scene(16, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 200, 200))
    s.mode(.invert)
    s.pattern([0, 0, 0, 0, 0, 0, 0, 0])
    s.fillRect(0, 0, 15, 15)
    let got = s.px(8, 8)
    check(sameColor(got, rgb(200, 200, 200)),
          "B_SOLID_LOW selects no pixels, so nothing inverts, got \(got)")
}

test("the stipple is anchored to the view origin, not the shape") {
    // PatternHandler::IsHighColor subtracts the view offsets before indexing
    // (PatternHandler.h:162), and SetOffsets masks them to & 7. Tiling from the
    // shape's bounding box instead puts the pattern in the wrong phase.
    guard let s = Scene(16, 16) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.high(rgb(255, 255, 255))
    s.low(rgb(0, 0, 0))
    s.pattern([0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00])
    s.offsets(0, 1)
    s.mode(.over)
    s.fillRect(0, 0, 15, 15)
    // With yOffset 1, row y indexes pattern row (y-1) & 7: row 1 is the set row.
    let row1 = s.px(8, 1)
    let row0 = s.px(8, 0)
    check(sameColor(row1, rgb(255, 255, 255)),
          "row 1 takes the high colour once offset by 1, got \(row1)")
    check(sameColor(row0, rgb(0, 0, 0)),
          "row 0 now indexes a clear pattern row, got \(row0)")
}

test("a vertical-stripe stipple actually renders as stripes") {
    // 0xf0 in every row is four columns on, four off. Because every byte is
    // equal it is easy to misclassify as solid, in which case the whole rect
    // comes out flat high and the stipple silently vanishes.
    guard let s = Scene(16, 16) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.high(rgb(255, 255, 255))
    s.low(rgb(0, 0, 128))
    s.pattern([UInt8](repeating: 0xf0, count: 8))
    s.mode(.over)
    s.fillRect(0, 0, 15, 15)
    // Bit (7 - x%8) of the row byte: x%8 of 0...3 selects high.
    for x in 0...3 {
        let got = s.px(x, 5)
        check(sameColor(got, rgb(255, 255, 255)),
              "x=\(x) is a set bit, so high, got \(got)")
    }
    for x in 4...7 {
        let got = s.px(x, 5)
        check(sameColor(got, rgb(0, 0, 128)),
              "x=\(x) is a clear bit, so low, got \(got)")
    }
    // And it repeats every 8 pixels.
    check(sameColor(s.px(8, 5), rgb(255, 255, 255)), "the pattern tiles in x")
    check(sameColor(s.px(12, 5), rgb(0, 0, 128)), "…including the clear half")
}

test("a software-blended shape still respects the clipping region") {
    // Clipping is baked into the coverage mask, so this checks the mask context
    // really does get the state's clip applied.
    guard let s = Scene(32, 16) else { failures.append("no canvas"); return }
    s.background(rgb(200, 200, 200))
    s.send(RP.constrainClippingRegion) { w in
        w.append(i32: 1)
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 15); w.append(f32: 15)
    }
    s.mode(.subtract)
    s.high(rgb(100, 100, 100))
    s.fillRect(0, 0, 31, 15)
    let inside = s.px(8, 8)
    let outside = s.px(24, 8)
    check(sameColor(inside, rgb(100, 100, 100)),
          "inside the clip the subtract applied, got \(inside)")
    check(sameColor(outside, rgb(200, 200, 200)),
          "outside the clip nothing was written, got \(outside)")
}

// MARK: - Read-back

test("RP_READ_BITMAP composites the cursor only when asked") {
    // The cursor is kept out of the canvas so it cannot leave a trail, which
    // means a read-back that wants it has to have it added on the way out.
    func readBack(drawCursor: Bool) -> [UInt8] {
        guard let s = Scene(32, 32) else { return [] }
        s.background(rgb(0, 0, 0))
        // A 4x4 solid white cursor with its hotspot at the top-left.
        s.send(RP.setCursor) { w in
            w.append(point: BPoint(x: 0, y: 0))
            w.append(i32: 4); w.append(i32: 4)          // width, height
            w.append(i32: 16)                            // bytesPerRow
            w.append(u32: ColorSpace.rgba32.rawValue)
            w.append(u32: 0)                             // flags
            w.append(u32: 64)                            // byte count
            w.append(raw: [UInt8](repeating: 255, count: 64))
        }
        s.send(RP.moveCursorTo) { $0.append(f32: 8); $0.append(f32: 8) }
        s.send(RP.readBitmap) { w in
            w.append(i32: 1)
            w.append(f32: 0); w.append(f32: 0)
            w.append(f32: 31); w.append(f32: 31)
            w.append(bool: drawCursor)
        }
        guard let reply = s.sink.sent.last else { return [] }
        var r = WireReader([UInt8](reply))
        _ = try? r.u16(); _ = try? r.u32()               // code, length
        _ = try? r.i32()                                  // token
        guard let width = try? r.i32(), let _ = try? r.i32(),
              let bytesPerRow = try? r.i32() else { return [] }
        _ = try? r.u32(); _ = try? r.u32(); _ = try? r.u32()
        guard let bits = try? r.raw(Int(bytesPerRow) * 32) else { return [] }
        check(Int(bytesPerRow) >= Int(width) * 3,
              "row stride fits three bytes per pixel")
        return bits
    }

    let without = readBack(drawCursor: false)
    let with = readBack(drawCursor: true)
    check(!without.isEmpty && !with.isEmpty, "both read-backs produced data")
    if !without.isEmpty && !with.isEmpty {
        // Row 8, pixel 8: black without the cursor, white with it.
        let offset = 8 * (((32 * 3) + 3) & ~3) + 8 * 3
        if offset + 2 < without.count && offset + 2 < with.count {
            equal(without[offset], 0, "no cursor means the pixel stays black")
            equal(with[offset], 255, "with the cursor the pixel is white")
        }
        // Away from the cursor the two must be identical.
        let far = 20 * (((32 * 3) + 3) & ~3) + 20 * 3
        if far + 2 < with.count {
            equal(with[far], 0, "the cursor does not affect distant pixels")
        }
    }
}

// MARK: - Shape arcs

test("a shape arc draws a quarter ellipse instead of being skipped") {
    // Neither the reference client nor the previous version of this one drew arc
    // ops: they consumed the points and moved on, so any shape containing one
    // came out with a piece missing.
    guard let s = Scene(64, 64) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.high(rgb(255, 255, 255))
    s.mode(.over)
    // MoveTo (60,30), then a small-CW arc of radius 30 to (30,60): a quarter
    // ellipse about the centre (30,30).
    s.send(RP.fillShape) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 63); w.append(f32: 63)
        w.append(i32: 2)                              // op count
        w.append(u32: 0x8000_0000 | 1)                // MoveTo, 1 point
        w.append(u32: 0x0400_0000 | 3)                // small arc CW, 3 points
        w.append(i32: 4)                              // point count
        w.append(point: BPoint(x: 60, y: 30))         // move target
        w.append(point: BPoint(x: 30, y: 30))         // (rx, ry)
        w.append(point: BPoint(x: 0, y: 0))           // (angle, unused)
        w.append(point: BPoint(x: 30, y: 60))         // arc end
        w.append(point: BPoint(x: 0, y: 0))           // shape offset
        w.append(f32: 1)                              // scale
    }
    equal(s.sink.logs.filter { $0.contains("decode error") }.count, 0,
          "shape decoded cleanly: \(s.sink.logs)")
    // Filling an open path closes it with a chord, so the painted region is the
    // circular *segment* between the chord (x + y = 90) and the arc — not the
    // quarter disc. (48,48) is past the chord and inside radius 30 of the centre
    // (30,30); (58,58) is outside the radius entirely.
    let inside = s.px(48, 48)
    let outside = s.px(58, 58)
    check(inside.r > 200, "inside the arc segment is filled, got \(inside)")
    check(outside.r < 50, "outside the arc radius is untouched, got \(outside)")
}

test("a zero-radius arc degenerates to a line, per the SVG rules") {
    let p = CGMutablePath()
    var t = CGAffineTransform.identity
    p.move(to: CGPoint(x: 0, y: 0))
    SessionRenderer.Shape.appendArc(to: p, from: CGPoint(x: 0, y: 0),
                                    to: CGPoint(x: 10, y: 0), rx: 0, ry: 5,
                                    angle: 0, largeArc: false, sweep: false,
                                    transform: &t)
    equal(p.currentPoint.x, 10, "a zero radius still reaches the endpoint")
}

test("the large-arc flag selects the major arc") {
    // Two arcs share any pair of endpoints; the flag picks which. The chord has
    // to be shorter than the diameter for the distinction to exist at all — with
    // a diameter chord both arcs are the same semicircle.
    func bounds(largeArc: Bool) -> CGRect {
        let p = CGMutablePath()
        var t = CGAffineTransform.identity
        p.move(to: CGPoint(x: 0, y: 0))
        SessionRenderer.Shape.appendArc(to: p, from: CGPoint(x: 0, y: 0),
                                        to: CGPoint(x: 10, y: 0), rx: 10, ry: 10,
                                        angle: 0, largeArc: largeArc,
                                        sweep: true, transform: &t)
        return p.boundingBoxOfPath
    }
    let minor = bounds(largeArc: false)
    let major = bounds(largeArc: true)
    // Sagitta of a 10 chord on a radius-10 circle is ~1.34; the major arc spans
    // the rest of the circle, ~18.66.
    check(minor.height < 3,
          "the minor arc barely bulges, got height \(minor.height)")
    check(major.height > 15,
          "the major arc wraps most of the circle, got height \(major.height)")
    check(major.width > 19,
          "and reaches the full diameter across, got width \(major.width)")
}

// MARK: - Gradients

/// Appends a gradient with black-to-white stops for the given kind.
func appendGradient(_ w: inout WireWriter, kind: Gradient.Kind,
                    center: BPoint = BPoint(x: 0, y: 0),
                    start: BPoint = BPoint(x: 0, y: 0),
                    end: BPoint = BPoint(x: 0, y: 0),
                    focal: BPoint = BPoint(x: 0, y: 0),
                    radius: Float = 0, angle: Float = 0) {
    w.append(u32: kind.rawValue)
    switch kind {
    case .linear:
        w.append(point: start); w.append(point: end)
    case .radial:
        w.append(point: center); w.append(f32: radius)
    case .radialFocus:
        w.append(point: center); w.append(point: focal); w.append(f32: radius)
    case .diamond:
        w.append(point: center)
    case .conic:
        w.append(point: center); w.append(f32: angle)
    case .none:
        break
    }
    w.append(i32: 2)
    w.append(raw: [0, 0, 0, 255]); w.append(f32: 0)
    w.append(raw: [255, 255, 255, 255]); w.append(f32: 255)
}

test("gradient LUT matches app_server's 256-entry interpolation") {
    var g = Gradient()
    g.kind = .linear
    g.stops = [(RGBColor(r: 0, g: 0, b: 0, a: 255), 0),
               (RGBColor(r: 255, g: 255, b: 255, a: 255), 255)]
    let lut = GradientSource.makeLUT(g, forceOpaque: false)
    equal(lut.count, 256, "256 entries, as Painter::_MakeGradient builds")
    // Not exactly 0: Haiku divides by (dist + 1), so the first entry of a
    // full-range black-to-white ramp comes out as 1. Reproducing that off-by-one
    // is the point — this LUT has to match the server's, not be independently
    // correct.
    check(lut[0].r <= 2, "first entry is essentially the first stop, got \(lut[0].r)")
    equal(lut[255].r, 255, "last entry is the last stop")
    // Painter.cpp:2213 computes f = (offset - i) / (dist + 1) with dist = 255.
    check(abs(Int(lut[128].r) - 128) <= 2,
          "the midpoint is mid-grey, got \(lut[128].r)")
}

test("gradient stops outside their offsets clamp, they do not repeat") {
    var g = Gradient()
    g.kind = .linear
    g.stops = [(RGBColor(r: 10, g: 10, b: 10, a: 255), 64),
               (RGBColor(r: 200, g: 200, b: 200, a: 255), 128)]
    let lut = GradientSource.makeLUT(g, forceOpaque: false)
    equal(lut[0].r, 10, "before the first stop holds the first colour")
    equal(lut[255].r, 200, "after the last stop holds the last colour")
}

test("a diamond gradient spans a fixed 100 units, not the shape") {
    // _CalcRadialGradientTransform is called with the default gradient_d2 = 100
    // (Painter.h:325), so extent is independent of the shape's size. Scaling to
    // the bounding box — the intuitive guess — puts the midpoint in the wrong
    // place, which this catches.
    guard let s = Scene(300, 300) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.mode(.over)
    s.send(RP.fillRectGradient) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 299); w.append(f32: 299)
        appendGradient(&w, kind: .diamond, center: BPoint(x: 150, y: 150))
    }
    let centre = s.px(150, 150)
    let mid = s.px(200, 150)          // 50 units out: t = 0.5
    let past = s.px(280, 150)         // 130 units out: clamped to white
    check(centre.r < 8, "the centre is the first stop, got \(centre)")
    check(abs(Int(mid.r) - 128) <= 10,
          "50 of 100 units out is mid-grey, got \(mid.r)")
    check(past.r > 247, "beyond 100 units it clamps to white, got \(past)")
    // Chebyshev distance, so the diagonal at (200,200) is also t = 0.5.
    let diagonal = s.px(200, 200)
    check(abs(Int(diagonal.r) - Int(mid.r)) <= 6,
          "max(|dx|,|dy|) makes the diagonal match the axis, got \(diagonal.r)")
}

test("a conic gradient sweeps on angle and ignores its angle field") {
    // agg::gradient_conic is |atan2(y, x)| / pi, and app_server never passes the
    // gradient's own angle to it.
    guard let s = Scene(200, 200) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.mode(.over)
    s.send(RP.fillRectGradient) { w in
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 199); w.append(f32: 199)
        appendGradient(&w, kind: .conic, center: BPoint(x: 100, y: 100),
                       angle: 45)
    }
    let east = s.px(180, 100)     // atan2(0, +dx) = 0 -> first stop
    let west = s.px(20, 100)      // atan2(0, -dx) = pi -> last stop
    let north = s.px(100, 20)     // |atan2(-dy, 0)| = pi/2 -> midpoint
    check(east.r < 12, "0 degrees is the first stop, got \(east.r)")
    check(west.r > 243, "180 degrees is the last stop, got \(west.r)")
    check(abs(Int(north.r) - 128) <= 12,
          "90 degrees is the midpoint, got \(north.r)")
    // Mirrored, because the function takes the absolute angle.
    let south = s.px(100, 180)
    check(abs(Int(south.r) - Int(north.r)) <= 6,
          "the sweep is mirrored about the x axis, got \(south.r)")
}

test("B_GRADIENT_RADIAL_FOCUS renders exactly like B_GRADIENT_RADIAL") {
    // app_server default-constructs agg::gradient_radial_focus and never sets
    // the focus (Painter.cpp:2101), so the focal point has no effect on a real
    // Haiku screen. We match the server rather than the API's intent.
    func render(_ kind: Gradient.Kind) -> [RGBColor] {
        guard let s = Scene(120, 120) else { return [] }
        s.background(rgb(0, 0, 0))
        s.mode(.over)
        s.send(RP.fillRectGradient) { w in
            w.append(i32: 1)
            w.append(f32: 0); w.append(f32: 0)
            w.append(f32: 119); w.append(f32: 119)
            appendGradient(&w, kind: kind, center: BPoint(x: 60, y: 60),
                           focal: BPoint(x: 20, y: 20), radius: 50)
        }
        return [s.px(60, 60), s.px(80, 60), s.px(20, 20), s.px(100, 100)]
    }
    let radial = render(.radial)
    let focus = render(.radialFocus)
    equal(radial.count, 4, "radial sampled")
    equal(focus.count, 4, "radial-focus sampled")
    if radial.count == 4 && focus.count == 4 {
        for i in 0..<4 {
            check(sameColor(radial[i], focus[i], tolerance: 1),
                  "sample \(i) matches: \(radial[i]) vs \(focus[i])")
        }
    }
}

// MARK: - Input encoding

test("modifier mapping puts Haiku COMMAND where the user chose") {
    let e = InputEncoder()
    e.mapping = .optionIsCommand
    let optionMask: UInt = 1 << 19
    _ = e.modifiersChanged(nsFlagsRawValue: optionMask)
    check(e.modifiers.contains(.command),
          "macOS Option maps to Haiku COMMAND by default")

    let e2 = InputEncoder()
    e2.mapping = .commandIsCommand
    let commandMask: UInt = 1 << 20
    _ = e2.modifiersChanged(nsFlagsRawValue: commandMask)
    check(e2.modifiers.contains(.command),
          "with commandIsCommand, macOS Command maps to Haiku COMMAND")
}

test("modifiersChanged only emits on an actual change") {
    let e = InputEncoder()
    let shift: UInt = 1 << 17
    check(e.modifiersChanged(nsFlagsRawValue: shift) != nil, "first change emits")
    check(e.modifiersChanged(nsFlagsRawValue: shift) == nil,
          "same flags emit nothing")
    check(e.modifiersChanged(nsFlagsRawValue: 0) != nil, "release emits")
}

test("button mask uses Haiku bit values") {
    let e = InputEncoder()
    equal(e.buttons(from: 1).rawValue, 1, "primary")
    equal(e.buttons(from: 2).rawValue, 2, "secondary")
    equal(e.buttons(from: 4).rawValue, 4, "tertiary")
    equal(e.buttons(from: 3).rawValue, 3, "primary+secondary")
}

// MARK: - Reconnect policy

test("backoff doubles from one second and stops at the ceiling") {
    let p = ReconnectPolicy()
    equal(p.delay(attempt: 1), 1, "first retry is immediate-ish")
    equal(p.delay(attempt: 2), 2, "then two seconds")
    equal(p.delay(attempt: 3), 4, "then four")
    equal(p.delay(attempt: 6), 30, "clamped to the 30s ceiling")
    equal(p.delay(attempt: 100), 30, "still clamped much later")
    // A long outage calls this with big attempt numbers. If the exponent is not
    // capped before pow(), the result is +infinity and min() cannot save it —
    // asyncAfter with an infinite delay never fires, so the client would sit
    // there looking connected-but-dead forever.
    check(p.delay(attempt: 10_000).isFinite,
          "a huge attempt count still yields a finite delay")
    equal(p.delay(attempt: 0), 1, "a nonsense attempt number is treated as the first")
}

test("a link that never connected gives up, one that did retries forever") {
    let p = ReconnectPolicy()
    check(p.shouldRetry(attempt: 1, everConnected: false),
          "a first cold attempt is made")
    check(p.shouldRetry(attempt: 5, everConnected: false),
          "up to the cold-start limit")
    check(!p.shouldRetry(attempt: 6, everConnected: false),
          "past the limit it stops, rather than hammering a typo'd host all night")
    check(p.shouldRetry(attempt: 999, everConnected: true),
          "but a link that worked once is always worth retrying")
}

// MARK: - Live resolution change

test("resizing keeps per-token state, reconnecting drops it") {
    // The distinction matters because state is sticky: the server elides
    // unchanged settings, so throwing away a token's colour on resize would make
    // the repaint that follows draw in the wrong colour.
    guard let s = Scene(64, 32) else { failures.append("no canvas"); return }
    s.background(rgb(0, 0, 0))
    s.high(rgb(255, 0, 0))
    s.mode(.over)

    guard let bigger = Canvas(width: 96, height: 48) else {
        failures.append("no second canvas"); return
    }
    s.renderer.resize(to: bigger)
    equal(s.renderer.canvas.width, 96, "renderer took the new canvas")

    // Fill without re-sending the colour. If resize dropped the state this comes
    // out black (the DrawState default) instead of red.
    s.fillRect(0, 0, 95, 47)
    let after = s.renderer.canvas.pixel(x: 50, y: 40) ?? rgb(0, 0, 0)
    check(sameColor(after, rgb(255, 0, 0)),
          "the sticky high colour survived the resize, got \(after)")

    // A reconnect is the opposite case: those states really are stale.
    s.renderer.resetForReconnect()
    s.renderer.handle(code: RP.fillRect, payload: {
        var w = WireWriter(RP.fillRect)
        w.append(i32: 1)
        w.append(f32: 0); w.append(f32: 0); w.append(f32: 95); w.append(f32: 47)
        return Array([UInt8](w.finish())[RP.headerSize...])
    }())
    let reset = s.renderer.canvas.pixel(x: 50, y: 40) ?? rgb(9, 9, 9)
    check(sameColor(reset, rgb(0, 0, 0)),
          "after a reconnect the token is back to defaults, got \(reset)")
}

// MARK: - Tunnel config

test("ssh arguments never enable connection sharing") {
    // A shared master could outlive this process, which is the orphan we are
    // trying to prevent.
    let c = SSHTunnel.Config(host: "h", user: "baron",
                            identityFile: "~/.ssh/k", localPort: 10900,
                            remotePort: 10900)
    let a = c.arguments
    equal(c.forwardSpec, "10900:127.0.0.1:10900", "forward spec")
    check(a.contains("-N"), "no remote command")
    check(a.contains("ExitOnForwardFailure=yes"), "fail loudly on a dead forward")
    check(a.contains("ControlMaster=no"), "no shared master")
    check(a.contains("ControlPath=none"), "no control socket")
    check(a.contains("BatchMode=yes"), "never prompt while unattended")
    check(a.last == "baron@h", "destination is last")
    check(a.contains("/Users") || a.contains(NSString(string: "~/.ssh/k")
        .expandingTildeInPath), "identity path expanded")
}

// MARK: - Report

print("")
if failures.isEmpty {
    print("PASS — \(checks) checks")
    exit(0)
} else {
    print("FAIL — \(failures.count) of \(checks) checks failed")
    for f in failures { print("  - \(f)") }
    exit(1)
}
