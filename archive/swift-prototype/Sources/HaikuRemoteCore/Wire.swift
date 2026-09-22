import CoreGraphics
import Foundation

/// Message codes. See PROTOCOL.md §5 and RemoteMessage.h:38-139.
public enum RP {
    public static let initConnection: UInt16 = 1
    public static let updateDisplayMode: UInt16 = 2
    public static let closeConnection: UInt16 = 3
    public static let getSystemPalette: UInt16 = 4
    public static let getSystemPaletteResult: UInt16 = 5

    public static let createState: UInt16 = 20
    public static let deleteState: UInt16 = 21
    public static let enableSyncDrawing: UInt16 = 22
    public static let disableSyncDrawing: UInt16 = 23
    public static let invalidateRect: UInt16 = 24
    public static let invalidateRegion: UInt16 = 25

    public static let setOffsets: UInt16 = 40
    public static let setHighColor: UInt16 = 41
    public static let setLowColor: UInt16 = 42
    public static let setPenSize: UInt16 = 43
    public static let setStrokeMode: UInt16 = 44
    public static let setBlendingMode: UInt16 = 45
    public static let setPattern: UInt16 = 46
    public static let setDrawingMode: UInt16 = 47
    public static let setFont: UInt16 = 48
    public static let setTransform: UInt16 = 49

    public static let constrainClippingRegion: UInt16 = 60
    public static let copyRectNoClipping: UInt16 = 61
    public static let invertRect: UInt16 = 62
    public static let drawBitmap: UInt16 = 63
    public static let drawBitmapRects: UInt16 = 64

    public static let strokeArc: UInt16 = 80
    public static let strokeBezier: UInt16 = 81
    public static let strokeEllipse: UInt16 = 82
    public static let strokePolygon: UInt16 = 83
    public static let strokeRect: UInt16 = 84
    public static let strokeRoundRect: UInt16 = 85
    public static let strokeShape: UInt16 = 86
    public static let strokeTriangle: UInt16 = 87
    public static let strokeLine: UInt16 = 88
    public static let strokeLineArray: UInt16 = 89

    public static let fillArc: UInt16 = 100
    public static let fillBezier: UInt16 = 101
    public static let fillEllipse: UInt16 = 102
    public static let fillPolygon: UInt16 = 103
    public static let fillRect: UInt16 = 104
    public static let fillRoundRect: UInt16 = 105
    public static let fillShape: UInt16 = 106
    public static let fillTriangle: UInt16 = 107
    public static let fillRegion: UInt16 = 108

    public static let fillArcGradient: UInt16 = 120
    public static let fillBezierGradient: UInt16 = 121
    public static let fillEllipseGradient: UInt16 = 122
    public static let fillPolygonGradient: UInt16 = 123
    public static let fillRectGradient: UInt16 = 124
    public static let fillRoundRectGradient: UInt16 = 125
    public static let fillShapeGradient: UInt16 = 126
    public static let fillTriangleGradient: UInt16 = 127
    public static let fillRegionGradient: UInt16 = 128

    public static let strokePointColor: UInt16 = 140
    public static let strokeLine1pxColor: UInt16 = 141
    public static let strokeRect1pxColor: UInt16 = 142

    public static let fillRectColor: UInt16 = 160
    public static let fillRegionColorNoClipping: UInt16 = 161

    public static let drawString: UInt16 = 180
    public static let drawStringWithOffsets: UInt16 = 181
    public static let drawStringResult: UInt16 = 182
    public static let stringWidth: UInt16 = 183
    public static let stringWidthResult: UInt16 = 184
    public static let readBitmap: UInt16 = 185
    public static let readBitmapResult: UInt16 = 186

    public static let setCursor: UInt16 = 200
    public static let setCursorVisible: UInt16 = 201
    public static let moveCursorTo: UInt16 = 202

    public static let mouseMoved: UInt16 = 220
    public static let mouseDown: UInt16 = 221
    public static let mouseUp: UInt16 = 222
    public static let mouseWheelChanged: UInt16 = 223

    public static let keyDown: UInt16 = 240
    public static let keyUp: UInt16 = 241
    public static let unmappedKeyDown: UInt16 = 242
    public static let unmappedKeyUp: UInt16 = 243
    public static let modifiersChanged: UInt16 = 244

    public static let strokeArcGradient: UInt16 = 260
    public static let strokeBezierGradient: UInt16 = 261
    public static let strokeEllipseGradient: UInt16 = 262
    public static let strokePolygonGradient: UInt16 = 263
    public static let strokeRectGradient: UInt16 = 264
    public static let strokeRoundRectGradient: UInt16 = 265
    public static let strokeShapeGradient: UInt16 = 266
    public static let strokeTriangleGradient: UInt16 = 267
    public static let strokeLineGradient: UInt16 = 268

    public static let headerSize = 6

    /// Messages that do NOT begin with an int32 token (PROTOCOL.md §5.1).
    /// createState/deleteState are here because their int32 is the entire
    /// payload rather than a prefix.
    public static let sessionLevel: Set<UInt16> = [
        initConnection, closeConnection, getSystemPaletteResult,
        createState, deleteState, invalidateRect, invalidateRegion,
        copyRectNoClipping, fillRegionColorNoClipping,
        setCursor, setCursorVisible, moveCursorTo,
    ]

    public static func name(_ code: UInt16) -> String {
        Self.names[code] ?? "UNKNOWN(\(code))"
    }

    static let names: [UInt16: String] = [
        1: "RP_INIT_CONNECTION", 2: "RP_UPDATE_DISPLAY_MODE",
        3: "RP_CLOSE_CONNECTION", 4: "RP_GET_SYSTEM_PALETTE",
        5: "RP_GET_SYSTEM_PALETTE_RESULT", 20: "RP_CREATE_STATE",
        21: "RP_DELETE_STATE", 22: "RP_ENABLE_SYNC_DRAWING",
        23: "RP_DISABLE_SYNC_DRAWING", 24: "RP_INVALIDATE_RECT",
        25: "RP_INVALIDATE_REGION", 40: "RP_SET_OFFSETS",
        41: "RP_SET_HIGH_COLOR", 42: "RP_SET_LOW_COLOR", 43: "RP_SET_PEN_SIZE",
        44: "RP_SET_STROKE_MODE", 45: "RP_SET_BLENDING_MODE",
        46: "RP_SET_PATTERN", 47: "RP_SET_DRAWING_MODE", 48: "RP_SET_FONT",
        49: "RP_SET_TRANSFORM", 60: "RP_CONSTRAIN_CLIPPING_REGION",
        61: "RP_COPY_RECT_NO_CLIPPING", 62: "RP_INVERT_RECT",
        63: "RP_DRAW_BITMAP", 64: "RP_DRAW_BITMAP_RECTS", 80: "RP_STROKE_ARC",
        81: "RP_STROKE_BEZIER", 82: "RP_STROKE_ELLIPSE",
        83: "RP_STROKE_POLYGON", 84: "RP_STROKE_RECT",
        85: "RP_STROKE_ROUND_RECT", 86: "RP_STROKE_SHAPE",
        87: "RP_STROKE_TRIANGLE", 88: "RP_STROKE_LINE",
        89: "RP_STROKE_LINE_ARRAY", 100: "RP_FILL_ARC", 101: "RP_FILL_BEZIER",
        102: "RP_FILL_ELLIPSE", 103: "RP_FILL_POLYGON", 104: "RP_FILL_RECT",
        105: "RP_FILL_ROUND_RECT", 106: "RP_FILL_SHAPE",
        107: "RP_FILL_TRIANGLE", 108: "RP_FILL_REGION",
        120: "RP_FILL_ARC_GRADIENT", 121: "RP_FILL_BEZIER_GRADIENT",
        122: "RP_FILL_ELLIPSE_GRADIENT", 123: "RP_FILL_POLYGON_GRADIENT",
        124: "RP_FILL_RECT_GRADIENT", 125: "RP_FILL_ROUND_RECT_GRADIENT",
        126: "RP_FILL_SHAPE_GRADIENT", 127: "RP_FILL_TRIANGLE_GRADIENT",
        128: "RP_FILL_REGION_GRADIENT", 140: "RP_STROKE_POINT_COLOR",
        141: "RP_STROKE_LINE_1PX_COLOR", 142: "RP_STROKE_RECT_1PX_COLOR",
        160: "RP_FILL_RECT_COLOR", 161: "RP_FILL_REGION_COLOR_NO_CLIPPING",
        180: "RP_DRAW_STRING", 181: "RP_DRAW_STRING_WITH_OFFSETS",
        182: "RP_DRAW_STRING_RESULT", 183: "RP_STRING_WIDTH",
        184: "RP_STRING_WIDTH_RESULT", 185: "RP_READ_BITMAP",
        186: "RP_READ_BITMAP_RESULT", 200: "RP_SET_CURSOR",
        201: "RP_SET_CURSOR_VISIBLE", 202: "RP_MOVE_CURSOR_TO",
        220: "RP_MOUSE_MOVED", 221: "RP_MOUSE_DOWN", 222: "RP_MOUSE_UP",
        223: "RP_MOUSE_WHEEL_CHANGED", 240: "RP_KEY_DOWN", 241: "RP_KEY_UP",
        242: "RP_UNMAPPED_KEY_DOWN", 243: "RP_UNMAPPED_KEY_UP",
        244: "RP_MODIFIERS_CHANGED", 260: "RP_STROKE_ARC_GRADIENT",
        261: "RP_STROKE_BEZIER_GRADIENT", 262: "RP_STROKE_ELLIPSE_GRADIENT",
        263: "RP_STROKE_POLYGON_GRADIENT", 264: "RP_STROKE_RECT_GRADIENT",
        265: "RP_STROKE_ROUND_RECT_GRADIENT", 266: "RP_STROKE_SHAPE_GRADIENT",
        267: "RP_STROKE_TRIANGLE_GRADIENT", 268: "RP_STROKE_LINE_GRADIENT",
    ]
}

// MARK: - Value types

/// A Haiku BRect. **Both edges are inclusive**: width is right-left+1
/// (PROTOCOL.md §3.3). Keeping this distinct from CGRect prevents accidentally
/// losing that +1, which shows up as one-pixel seams on every fill.
public struct BRect: Equatable {
    public var left: Float, top: Float, right: Float, bottom: Float
    public init(left: Float, top: Float, right: Float, bottom: Float) {
        self.left = left; self.top = top; self.right = right; self.bottom = bottom
    }
    public var width: Float { right - left + 1 }
    public var height: Float { bottom - top + 1 }

    /// The reference client floors the origin and ceils the far edge before
    /// filling (HaikuRemoteDesktop.js:471-478). Matching it keeps our output
    /// aligned with the known-working demo.
    public var cgRect: CGRect {
        let l = floor(Double(left)), t = floor(Double(top))
        let r = ceil(Double(right)), b = ceil(Double(bottom))
        return CGRect(x: l, y: t, width: r - l + 1, height: b - t + 1)
    }
    /// Exact geometry, for ellipses and paths where rounding would distort.
    public var exactRect: CGRect {
        CGRect(x: Double(left), y: Double(top),
               width: Double(width), height: Double(height))
    }
}

public struct BPoint: Equatable {
    public var x: Float, y: Float
    public init(x: Float, y: Float) { self.x = x; self.y = y }
    public var cgPoint: CGPoint { CGPoint(x: Double(x), y: Double(y)) }
}

public struct RGBColor: Equatable {
    public var r: UInt8, g: UInt8, b: UInt8, a: UInt8
    public init(r: UInt8, g: UInt8, b: UInt8, a: UInt8) {
        self.r = r; self.g = g; self.b = b; self.a = a
    }
    /// Haiku's transparent magic value, checked against B_RGB32 pixels.
    public static let transparentMagicRGBA32: UInt32 = 0xff77_7477

    /// The canvas' colour space. Colours must be created in it explicitly:
    /// `CGColor(red:green:blue:alpha:)` builds a *Generic RGB* colour (gamma
    /// 1.8), and drawing that into the DeviceRGB canvas colour-matches it, which
    /// brightens every value — 200 lands as 210. Haiku sends raw device pixels,
    /// so any conversion at all is wrong.
    static let deviceRGB = CGColorSpaceCreateDeviceRGB()

    public func cgColor(forceOpaque: Bool) -> CGColor {
        let comps: [CGFloat] = [CGFloat(r) / 255, CGFloat(g) / 255,
                                CGFloat(b) / 255,
                                forceOpaque ? 1 : CGFloat(a) / 255]
        return CGColor(colorSpace: Self.deviceRGB, components: comps)
            ?? CGColor(gray: 0, alpha: 1)
    }
}

public struct HaikuFont: Equatable {
    public var direction: UInt8 = 0
    public var encoding: UInt8 = 0
    public var flags: UInt32 = 0
    public var spacing: UInt8 = 0
    public var shear: Float = 0
    public var rotation: Float = 0
    public var falseBoldWidth: Float = 0
    public var size: Float = 12
    public var face: UInt16 = 0
    public var family: UInt16 = 0
    public var style: UInt16 = 0

    public static let fixedSpacing: UInt8 = 3
    public static let italicFace: UInt16 = 0x0001
    public static let boldFace: UInt16 = 0x0020

    public var isBold: Bool { face & Self.boldFace != 0 }
    public var isItalic: Bool { face & Self.italicFace != 0 }
    public var isMonospaced: Bool { spacing == Self.fixedSpacing }

    public init() {}
}

public enum DrawingMode: UInt32 {
    case copy = 0, over = 1, erase = 2, invert = 3, add = 4, subtract = 5
    case blend = 6, min = 7, max = 8, select = 9, alpha = 10
}

public enum ColorSpace: UInt32 {
    case none = 0x0000
    case gray1 = 0x0001
    case gray8 = 0x0002
    case rgb24 = 0x0003
    case cmap8 = 0x0004
    case rgb16 = 0x0005
    case rgb32 = 0x0008
    case rgb15 = 0x0010
    case rgba32 = 0x2008
    case rgba15 = 0x2010
    case rgb24big = 0x1003
    case rgb16big = 0x1005
    case rgb32big = 0x1008
    case rgb15big = 0x1010
    case rgba32big = 0x3008
    case rgba15big = 0x3010
}

public struct Gradient {
    public enum Kind: UInt32 {
        case linear = 0, radial = 1, radialFocus = 2, diamond = 3, conic = 4
        case none = 5
    }
    public var kind: Kind = .none
    public var start = BPoint(x: 0, y: 0)
    public var end = BPoint(x: 0, y: 0)
    public var center = BPoint(x: 0, y: 0)
    public var focal = BPoint(x: 0, y: 0)
    public var radius: Float = 0
    public var angle: Float = 0
    /// Offsets as sent: 0-255, not 0-1 (PROTOCOL.md §6.4).
    public var stops: [(color: RGBColor, offset: Float)] = []
}

/// A decoded bitmap, kept in Haiku's native BGRA order so it can go straight
/// to CoreGraphics with no conversion (PROTOCOL.md §6.3).
public struct DecodedBitmap {
    public var width: Int
    public var height: Int
    /// Tightly packed BGRA, width*4 bytes per row.
    public var bgra: [UInt8]
    public init(width: Int, height: Int, bgra: [UInt8]) {
        self.width = width; self.height = height; self.bgra = bgra
    }
}

// MARK: - Reading

public struct WireError: Error, CustomStringConvertible {
    public let description: String
    init(_ s: String) { description = s }
}

/// Cursor over one message's payload.
///
/// The wire format is little-endian and **fully packed with no alignment
/// padding** (PROTOCOL.md §3.2), so every multi-byte read must be an unaligned
/// load. `loadUnaligned` is exactly that; binding memory to a struct pointer
/// would trap or read garbage.
public struct WireReader {
    public let bytes: [UInt8]
    public private(set) var pos: Int
    public let limit: Int

    public init(_ bytes: [UInt8], at offset: Int = 0, limit: Int? = nil) {
        self.bytes = bytes
        self.pos = offset
        self.limit = limit ?? bytes.count
    }

    public var remaining: Int { limit - pos }

    private mutating func load<T>(_ type: T.Type) throws -> T {
        let size = MemoryLayout<T>.size
        guard pos + size <= limit else {
            throw WireError("need \(size) bytes, \(remaining) left")
        }
        let value = bytes.withUnsafeBytes {
            $0.loadUnaligned(fromByteOffset: pos, as: T.self)
        }
        pos += size
        return value
    }

    public mutating func u8() throws -> UInt8 { try load(UInt8.self) }
    public mutating func i8() throws -> Int8 { try load(Int8.self) }
    /// sizeof(bool) == 1 on Haiku/gcc arm64.
    public mutating func bool() throws -> Bool { try load(UInt8.self) != 0 }
    public mutating func u16() throws -> UInt16 { UInt16(littleEndian: try load(UInt16.self)) }
    public mutating func i16() throws -> Int16 { Int16(littleEndian: try load(Int16.self)) }
    public mutating func u32() throws -> UInt32 { UInt32(littleEndian: try load(UInt32.self)) }
    public mutating func i32() throws -> Int32 { Int32(littleEndian: try load(Int32.self)) }

    public mutating func f32() throws -> Float {
        Float(bitPattern: try u32())
    }
    public mutating func f64() throws -> Double {
        Double(bitPattern: UInt64(littleEndian: try load(UInt64.self)))
    }

    public mutating func point() throws -> BPoint {
        BPoint(x: try f32(), y: try f32())
    }
    public mutating func rect() throws -> BRect {
        BRect(left: try f32(), top: try f32(), right: try f32(), bottom: try f32())
    }
    public mutating func color() throws -> RGBColor {
        RGBColor(r: try u8(), g: try u8(), b: try u8(), a: try u8())
    }

    public mutating func raw(_ n: Int) throws -> [UInt8] {
        guard n >= 0, pos + n <= limit else {
            throw WireError("need \(n) bytes, \(remaining) left")
        }
        let slice = Array(bytes[pos..<(pos + n)])
        pos += n
        return slice
    }

    /// AddRegion: int32 rectCount then that many BRects.
    public mutating func region() throws -> [BRect] {
        let n = try i32()
        guard n >= 0, n <= 1 << 20 else { throw WireError("absurd rect count \(n)") }
        return try (0..<Int(n)).map { _ in try rect() }
    }

    /// AddString: uint32 length then raw UTF-8, not NUL-terminated.
    public mutating func string() throws -> String {
        let n = try u32()
        let raw = try self.raw(Int(n))
        return String(decoding: raw, as: UTF8.self)
    }

    public mutating func font() throws -> HaikuFont {
        var f = HaikuFont()
        f.direction = try u8()
        f.encoding = try u8()
        f.flags = try u32()
        f.spacing = try u8()
        f.shear = try f32()
        f.rotation = try f32()
        f.falseBoldWidth = try f32()
        f.size = try f32()
        f.face = try u16()
        // The server sends a single uint32 familyAndStyle = family<<16|style.
        // The reference JS client reads this as two uint16s and therefore has
        // them swapped (PROTOCOL.md §5.2). Decode it correctly here.
        let familyAndStyle = try u32()
        f.family = UInt16(truncatingIfNeeded: familyAndStyle >> 16)
        f.style = UInt16(truncatingIfNeeded: familyAndStyle & 0xffff)
        return f
    }

    /// AddTransform: bool isIdentity, else 6 doubles as sx, shy, shx, sy, tx, ty.
    public mutating func transform() throws -> CGAffineTransform {
        if try bool() { return .identity }
        let sx = try f64(), shy = try f64(), shx = try f64()
        let sy = try f64(), tx = try f64(), ty = try f64()
        return CGAffineTransform(a: sx, b: shy, c: shx, d: sy, tx: tx, ty: ty)
    }

    public mutating func gradient() throws -> Gradient {
        var g = Gradient()
        let rawKind = try u32()
        g.kind = Gradient.Kind(rawValue: rawKind) ?? .none
        switch g.kind {
        case .linear:
            g.start = try point(); g.end = try point()
        case .radial:
            g.center = try point(); g.radius = try f32()
        case .radialFocus:
            g.center = try point(); g.focal = try point(); g.radius = try f32()
        case .diamond:
            g.center = try point()
        case .conic:
            g.center = try point(); g.angle = try f32()
        case .none:
            break
        }
        let stopCount = try i32()
        guard stopCount >= 0, stopCount <= 1 << 16 else {
            throw WireError("absurd gradient stop count \(stopCount)")
        }
        for _ in 0..<Int(stopCount) {
            let c = try color()
            let off = try f32()
            g.stops.append((c, off))
        }
        return g
    }
}

// MARK: - Writing

/// Builds one framed message. `finish()` back-patches the length field, which
/// **includes** the 6-byte header (PROTOCOL.md §2).
public struct WireWriter {
    public private(set) var bytes: [UInt8] = []

    public init(_ code: UInt16) {
        bytes.reserveCapacity(64)
        append(u16: code)
        append(u32: 0)   // length placeholder, patched in finish()
    }

    public mutating func append(u8 v: UInt8) { bytes.append(v) }
    public mutating func append(bool v: Bool) { bytes.append(v ? 1 : 0) }

    public mutating func append(u16 v: UInt16) {
        withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) }
    }
    public mutating func append(u32 v: UInt32) {
        withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) }
    }
    public mutating func append(i32 v: Int32) {
        withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) }
    }
    public mutating func append(f32 v: Float) { append(u32: v.bitPattern) }

    public mutating func append(point p: BPoint) {
        append(f32: p.x); append(f32: p.y)
    }
    public mutating func append(raw r: [UInt8]) { bytes.append(contentsOf: r) }

    /// Mirrors AddString: uint32 byte length then raw UTF-8.
    public mutating func append(string s: String) {
        let utf8 = Array(s.utf8)
        append(u32: UInt32(utf8.count))
        bytes.append(contentsOf: utf8)
    }

    public mutating func finish() -> Data {
        let total = UInt32(bytes.count)
        withUnsafeBytes(of: total.littleEndian) { src in
            for i in 0..<4 { bytes[2 + i] = src[i] }
        }
        return Data(bytes)
    }
}

// MARK: - Framing

/// Splits a byte stream into messages.
///
/// Deliberately advances by the *declared* length rather than by fields the
/// handler consumed: several messages carry trailing data no client reads
/// (PROTOCOL.md §2.1), and a single read may contain many messages or a partial
/// one (§2.2).
public final class MessageFramer {
    /// The largest message *body* this framer will believe.
    ///
    /// The length field is read from the same unverified bytes as the opcode, so
    /// a desynchronised stream can declare any size at all — and because a short
    /// frame is completed by *waiting for the rest to arrive*, a bogus length
    /// parks the reader forever on bytes nobody will send. That is a hung
    /// session, not a lost message.
    ///
    /// The bound is not a round number picked for comfort: it is exactly the
    /// peer's own framing limit. `app_server`'s parser refuses a body above
    /// 64 MiB (`kMaxMessageDataSize`, RemoteMessage.cpp:49) and returns
    /// `B_BAD_DATA` into its resynchronise path, so nothing larger can cross the
    /// link in either direction. Matching it means the two framing layers accept
    /// and refuse the same set of frames; any other value would make one side
    /// hang on what the other happily sent, or drop what the other considers
    /// legal. The largest legitimate message is a full-screen 32-bpp bitmap,
    /// tens of megabytes at present screen sizes, so this leaves real traffic
    /// untouched.
    public static let maxBodySize = 64 * 1024 * 1024

    private var buffer: [UInt8] = []
    public private(set) var messageCount = 0

    public init() {}

    public struct Frame {
        public let code: UInt16
        public let payload: [UInt8]
    }

    /// Appends `data` and returns every complete message now available.
    public func feed(_ data: [UInt8]) throws -> [Frame] {
        buffer.append(contentsOf: data)
        var out: [Frame] = []
        var offset = 0
        while buffer.count - offset >= RP.headerSize {
            let code = buffer.withUnsafeBytes {
                UInt16(littleEndian: $0.loadUnaligned(fromByteOffset: offset,
                                                      as: UInt16.self))
            }
            let total = Int(buffer.withUnsafeBytes {
                UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset + 2,
                                                      as: UInt32.self))
            })
            guard total >= RP.headerSize else {
                throw WireError("message claims \(total) bytes, header is \(RP.headerSize)")
            }
            // Bounded from above as well as below. Without this the `break`
            // below is reached for any absurd length and the session waits out
            // its life for bytes that will never arrive; `ingest` turns the
            // throw into a logged disconnect, which the reconnect policy can
            // actually recover from.
            guard total - RP.headerSize <= Self.maxBodySize else {
                throw WireError("message claims a \(total - RP.headerSize) byte "
                    + "body, beyond the \(Self.maxBodySize) byte maximum; "
                    + "treating as a framing desync")
            }
            guard buffer.count - offset >= total else { break }
            let payload = Array(buffer[(offset + RP.headerSize)..<(offset + total)])
            out.append(Frame(code: code, payload: payload))
            messageCount += 1
            offset += total
        }
        if offset > 0 { buffer.removeFirst(offset) }
        return out
    }

    public var pendingByteCount: Int { buffer.count }

    public func reset() {
        buffer.removeAll(keepingCapacity: true)
    }
}

// MARK: - Convenience builders for the nine client->server messages

public enum ClientMessage {
    public static func initConnection() -> Data {
        var w = WireWriter(RP.initConnection); return w.finish()
    }
    public static func updateDisplayMode(width: Int, height: Int) -> Data {
        var w = WireWriter(RP.updateDisplayMode)
        w.append(i32: Int32(width)); w.append(i32: Int32(height))
        return w.finish()
    }
    public static func getSystemPalette() -> Data {
        var w = WireWriter(RP.getSystemPalette); return w.finish()
    }
    public static func drawStringResult(token: Int32, pen: BPoint) -> Data {
        var w = WireWriter(RP.drawStringResult)
        w.append(i32: token); w.append(point: pen)
        return w.finish()
    }
    public static func stringWidthResult(token: Int32, width: Float) -> Data {
        var w = WireWriter(RP.stringWidthResult)
        w.append(i32: token); w.append(f32: width)
        return w.finish()
    }
    public static func mouseMoved(x: Float, y: Float) -> Data {
        var w = WireWriter(RP.mouseMoved)
        w.append(f32: x); w.append(f32: y)
        return w.finish()
    }
    public static func mouseDown(x: Float, y: Float, buttons: UInt32,
                                clicks: Int32) -> Data {
        var w = WireWriter(RP.mouseDown)
        w.append(f32: x); w.append(f32: y)
        w.append(u32: buttons); w.append(i32: clicks)
        return w.finish()
    }
    public static func mouseUp(x: Float, y: Float, buttons: UInt32) -> Data {
        var w = WireWriter(RP.mouseUp)
        w.append(f32: x); w.append(f32: y); w.append(u32: buttons)
        return w.finish()
    }
    public static func mouseWheel(dx: Float, dy: Float) -> Data {
        var w = WireWriter(RP.mouseWheelChanged)
        w.append(f32: dx); w.append(f32: dy)
        return w.finish()
    }
    /// `bytes` is the already-composed UTF-8 text for the keystroke.
    public static func key(down: Bool, text: String, rawChar: Int32,
                          key: Int32) -> Data {
        var w = WireWriter(down ? RP.keyDown : RP.keyUp)
        w.append(string: text)
        w.append(i32: rawChar)
        w.append(i32: key)
        return w.finish()
    }
    public static func modifiersChanged(_ mods: UInt32) -> Data {
        var w = WireWriter(RP.modifiersChanged)
        w.append(u32: mods)
        return w.finish()
    }
}
