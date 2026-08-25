import CoreGraphics
import Foundation

/// Software compositing for the drawing modes CoreGraphics cannot express.
///
/// Most of Haiku's 11 drawing modes are not Porter-Duff operators. Several are
/// *conditional* per-pixel rewrites that read the destination and compare it to
/// the current high/low colour, and three of them only touch pixels where the
/// 8x8 stipple selects the high colour. No `CGBlendMode` can express that, so
/// those modes are rasterised to a coverage mask and blended here, pixel by
/// pixel, in the same order Haiku's `Painter` does it.
///
/// Every function below is a direct transcription of a macro in
/// `src/servers/app/drawing/Painter/drawing_modes/`. The file and macro name is
/// cited at each one; PROTOCOL.md §6.5 has the table.
///
/// Note on alpha: every Haiku ASSIGN_* macro except `ASSIGN_COPY` forces
/// `d[3] = 255`, because `app_server` composites into an opaque framebuffer. Our
/// canvas is premultiplied BGRA but likewise always opaque (it starts opaque
/// black and nothing here makes it transparent), and premultiplied == straight
/// when alpha is 255, so these byte-level operations match Haiku exactly.

// MARK: - Source of colour

/// Supplies the source colour for a pixel, in Haiku screen coordinates.
///
/// This is the client-side counterpart of Haiku's `PatternHandler` (for stipple
/// fills) and its gradient span generators: both answer "what colour does the
/// source have at (x, y)".
public protocol PixelSource {
    func color(x: Int, y: Int) -> RGBColor
    /// Whether the source selects the *high* colour here.
    ///
    /// `B_OP_ERASE`, `B_OP_INVERT` and `B_OP_SELECT` are gated on this and leave
    /// every other pixel untouched (`DrawingModeErase.h:31`, `Invert.h:39`,
    /// `Select.h:33`). Sources with no pattern concept return true.
    func isHigh(x: Int, y: Int) -> Bool
}

/// An 8x8 1-bpp stipple with a high and low colour: Haiku's `PatternHandler`.
public struct PatternSource: PixelSource {
    public let bits: [UInt8]
    public let high: RGBColor
    public let low: RGBColor
    private let xOffset: Int
    private let yOffset: Int

    public init(pattern: [UInt8], high: RGBColor, low: RGBColor,
                xOffset: Int32 = 0, yOffset: Int32 = 0) {
        // Pad rather than trap if a short pattern ever arrives.
        var b = pattern
        while b.count < 8 { b.append(0xff) }
        self.bits = Array(b.prefix(8))
        self.high = high
        self.low = low
        // PatternHandler::SetOffsets masks both to & 7 (PatternHandler.cpp:225).
        self.xOffset = Int(xOffset) & 7
        self.yOffset = Int(yOffset) & 7
    }

    /// `PatternHandler::IsHighColor` (PatternHandler.h:162): the stipple is
    /// anchored to the *view origin*, not to the shape being drawn, and bit
    /// order is MSB-first.
    public func isHigh(x: Int, y: Int) -> Bool {
        let px = x - xOffset
        let py = y - yOffset
        // Swift's & on a negative Int is two's-complement, matching C here.
        return bits[py & 7] & (1 << UInt8(7 - (px & 7))) != 0
    }

    public func color(x: Int, y: Int) -> RGBColor {
        isHigh(x: x, y: y) ? high : low
    }
}

/// A single colour everywhere. Used when a solid-pattern fill still has to take
/// the software path because the *mode* needs it.
public struct SolidSource: PixelSource {
    public let value: RGBColor
    public init(_ value: RGBColor) { self.value = value }
    public func color(x: Int, y: Int) -> RGBColor { value }
    public func isHigh(x: Int, y: Int) -> Bool { true }
}

// MARK: - Coverage mask

/// A rasterised shape: one byte of coverage per pixel over a sub-rect of the
/// canvas. This is what Haiku's AGG rasteriser hands to a blend function as
/// `cover`.
public struct CoverageMask {
    public let originX: Int
    public let originY: Int
    public let width: Int
    public let height: Int
    public var bytes: [UInt8]

    public init(originX: Int, originY: Int, width: Int, height: Int,
                bytes: [UInt8]) {
        self.originX = originX
        self.originY = originY
        self.width = width
        self.height = height
        self.bytes = bytes
    }
}

public enum MaskRasterizer {
    /// Rasterises `path` into a coverage mask covering `region`.
    ///
    /// `configure` receives the mask context with the Haiku coordinate system
    /// already installed, so it can apply the same clip and transform the real
    /// draw would use — that way clipping is baked into the coverage and the
    /// blend loop needs no clip test.
    public static func rasterize(path: CGPath, filled: Bool, region: CGRect,
                                 canvasWidth: Int, canvasHeight: Int,
                                 antialias: Bool = false,
                                 configure: (CGContext) -> Void) -> CoverageMask? {
        let clipped = region.integral.intersection(
            CGRect(x: 0, y: 0, width: canvasWidth, height: canvasHeight))
        guard !clipped.isNull, !clipped.isEmpty else { return nil }
        let ox = Int(clipped.minX), oy = Int(clipped.minY)
        let w = Int(clipped.width), h = Int(clipped.height)
        guard w > 0, h > 0 else { return nil }

        // 8bpc single-channel grey, no alpha: coverage is the grey value. An
        // alpha-only context would also work but is fussier about row padding.
        var bytes = [UInt8](repeating: 0, count: w * h)
        let ok = bytes.withUnsafeMutableBytes { buf -> Bool in
            guard let c = CGContext(
                data: buf.baseAddress, width: w, height: h,
                bitsPerComponent: 8, bytesPerRow: w,
                space: CGColorSpaceCreateDeviceGray(),
                bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return false }
            c.setAllowsAntialiasing(antialias)
            c.setShouldAntialias(antialias)
            c.interpolationQuality = .none
            // Match Canvas: flip so wire coordinates are usable directly, then
            // shift so the mask's own origin is at `region`'s top-left.
            c.translateBy(x: 0, y: CGFloat(h))
            c.scaleBy(x: 1, y: -1)
            c.translateBy(x: -CGFloat(ox), y: -CGFloat(oy))
            configure(c)
            c.setFillColor(CGColor(gray: 1, alpha: 1))
            c.setStrokeColor(CGColor(gray: 1, alpha: 1))
            c.addPath(path)
            if filled { c.fillPath() } else { c.strokePath() }
            return true
        }
        guard ok else { return nil }
        return CoverageMask(originX: ox, originY: oy, width: w, height: h,
                            bytes: bytes)
    }
}

// MARK: - The blend functions

/// `brightness_for` (DrawingMode.h:190). Haiku's integer approximation of
/// 0.301r + 0.586g + 0.113b; B_OP_MIN/B_OP_MAX compare *this*, not channels.
@inline(__always)
func haikuBrightness(_ r: UInt8, _ g: UInt8, _ b: UInt8) -> UInt8 {
    let weighted: Int = 308 * Int(r) + 600 * Int(g) + 116 * Int(b)
    return UInt8(weighted / 1024)
}

public extension DrawingMode {
    /// True when this mode cannot be expressed as a `CGBlendMode` and must go
    /// through `softBlend`.
    ///
    /// `copy`, `over` and `alpha` are genuine source-over/source-copy and stay
    /// on the CoreGraphics path, which is faster and gets gradients and bitmap
    /// interpolation for free.
    var requiresSoftBlend: Bool {
        switch self {
        case .copy, .over, .alpha: return false
        case .erase, .invert, .add, .subtract, .blend, .min, .max, .select:
            return true
        }
    }

    /// True when Haiku only touches pixels where the pattern selects the high
    /// colour (DrawingModeErase.h:31, Invert.h:39, Select.h:33).
    var isPatternGated: Bool {
        switch self {
        case .erase, .invert, .select: return true
        default: return false
        }
    }
}

/// Applies `mask` to the canvas using Haiku's per-pixel semantics.
///
/// `globalAlpha` scales coverage the way `B_CONSTANT_ALPHA` scales it in
/// `app_server`: it becomes the `cover` argument of the BLEND_* macros.
public func softBlend(mask: CoverageMask, source: PixelSource,
                      mode: DrawingMode,
                      high: RGBColor = RGBColor(r: 0, g: 0, b: 0, a: 255),
                      low: RGBColor = RGBColor(r: 255, g: 255, b: 255, a: 255),
                      globalAlpha: CGFloat = 1.0,
                      into canvas: Canvas) {
    guard let base = canvas.ctx.data else { return }
    let bpr = canvas.bytesPerRow
    let dst = base.assumingMemoryBound(to: UInt8.self)
    let ga = UInt32(max(0, Swift.min(255, Int((globalAlpha * 255).rounded()))))
    let gated = mode.isPatternGated

    mask.bytes.withUnsafeBufferPointer { cov in
        for row in 0..<mask.height {
            let y = mask.originY + row
            guard y >= 0, y < canvas.height else { continue }
            let rowBase = row * mask.width
            for col in 0..<mask.width {
                let raw = UInt32(cov[rowBase + col])
                if raw == 0 { continue }
                // cover, 0-255, exactly the blend macros' `a`.
                let a = ga == 255 ? raw : (raw * ga) / 255
                if a == 0 { continue }

                let x = mask.originX + col
                guard x >= 0, x < canvas.width else { continue }
                if gated && !source.isHigh(x: x, y: y) { continue }

                let d = dst + y * bpr + x * 4
                let db = d[0], dg = d[1], dr = d[2]
                let src = source.color(x: x, y: y)

                // Each branch computes the target colour, or bails out, exactly
                // as the corresponding ASSIGN_*/BLEND_* macro pair does.
                var tr: UInt8, tg: UInt8, tb: UInt8
                switch mode {
                case .erase:
                    // ASSIGN_ERASE: gated pixels become the LOW colour, whatever
                    // the source colour at that pixel is
                    // (DrawingModeErase.h:21).
                    tr = low.r; tg = low.g; tb = low.b
                case .invert:
                    // ASSIGN_INVERT (DrawingModeInvert.h:24).
                    tr = 255 &- dr; tg = 255 &- dg; tb = 255 &- db
                case .add:
                    // ASSIGN_ADD, saturating (DrawingModeAdd.h:27).
                    tr = UInt8(Swift.min(255, Int(dr) + Int(src.r)))
                    tg = UInt8(Swift.min(255, Int(dg) + Int(src.g)))
                    tb = UInt8(Swift.min(255, Int(db) + Int(src.b)))
                case .subtract:
                    // ASSIGN_SUBTRACT, clamped at 0 (DrawingModeSubtract.h:29).
                    tr = UInt8(Swift.max(0, Int(dr) - Int(src.r)))
                    tg = UInt8(Swift.max(0, Int(dg) - Int(src.g)))
                    tb = UInt8(Swift.max(0, Int(db) - Int(src.b)))
                case .blend:
                    // ASSIGN_BLEND: a true 50% average (DrawingModeBlend.h:26).
                    tr = UInt8((Int(dr) + Int(src.r)) >> 1)
                    tg = UInt8((Int(dg) + Int(src.g)) >> 1)
                    tb = UInt8((Int(db) + Int(src.b)) >> 1)
                case .min:
                    // ASSIGN_MIN compares whole-pixel brightness and replaces the
                    // pixel; it is NOT a per-channel min (DrawingModeMin.h:21).
                    if haikuBrightness(src.r, src.g, src.b)
                        >= haikuBrightness(dr, dg, db) { continue }
                    tr = src.r; tg = src.g; tb = src.b
                case .max:
                    if haikuBrightness(src.r, src.g, src.b)
                        <= haikuBrightness(dr, dg, db) { continue }
                    tr = src.r; tg = src.g; tb = src.b
                case .select:
                    // compare() (DrawingModeSelect.h:31): swap high and low where
                    // the destination already is one of them, leave the rest.
                    if dr == high.r && dg == high.g && db == high.b {
                        tr = low.r; tg = low.g; tb = low.b
                    } else if dr == low.r && dg == low.g && db == low.b {
                        tr = high.r; tg = high.g; tb = high.b
                    } else {
                        continue
                    }
                case .copy, .over, .alpha:
                    // Handled on the CoreGraphics path; a non-solid pattern can
                    // still land here, in which case it is a plain assign.
                    tr = src.r; tg = src.g; tb = src.b
                }

                if a == 255 {
                    d[0] = tb; d[1] = tg; d[2] = tr; d[3] = 255
                } else {
                    // BLEND (DrawingMode.h:28).
                    d[0] = UInt8((((Int(tb) - Int(db)) * Int(a))
                                  + (Int(db) << 8)) >> 8)
                    d[1] = UInt8((((Int(tg) - Int(dg)) * Int(a))
                                  + (Int(dg) << 8)) >> 8)
                    d[2] = UInt8((((Int(tr) - Int(dr)) * Int(a))
                                  + (Int(dr) << 8)) >> 8)
                    d[3] = 255
                }
            }
        }
    }
}
