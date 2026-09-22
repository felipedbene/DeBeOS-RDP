import CoreGraphics
import CoreText
import Foundation

/// The persistent front buffer.
///
/// This *is* the session's authoritative pixel state: `app_server` keeps no
/// framebuffer of its own (PROTOCOL.md §0), so nothing else holds these pixels
/// and there is no way to ask the server to resend them.
///
/// Backed by a BGRA8888 `CGBitmapContext`, which matches both Haiku's
/// B_RGB32/B_RGBA32 wire layout and CoreGraphics' native order, so received
/// bitmaps blit with no conversion (§6.3).
public final class Canvas {
    public let width: Int
    public let height: Int
    public let ctx: CGContext
    /// Row stride of the backing store. `softBlend` writes through it directly,
    /// which is safe: `makeImage()` snapshots copy-on-write, so an image handed
    /// out earlier is unaffected by later pixel writes.
    public var bytesPerRow: Int { width * 4 }

    public init?(width: Int, height: Int) {
        guard width > 0, height > 0, width <= 16384, height <= 16384 else {
            return nil
        }
        self.width = width
        self.height = height
        let info: CGBitmapInfo = [
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue),
            .byteOrder32Little,
        ]
        guard let c = CGContext(data: nil, width: width, height: height,
                               bitsPerComponent: 8, bytesPerRow: width * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: info.rawValue) else { return nil }
        self.ctx = c

        c.interpolationQuality = .none
        c.setAllowsAntialiasing(false)
        c.setShouldAntialias(false)

        // Haiku's origin is top-left with y increasing downward; a
        // CGBitmapContext is bottom-left y-up. Flip once here so every
        // coordinate off the wire can be used verbatim. This is the *base* CTM:
        // saveGState/restoreGState around each op preserves it.
        c.translateBy(x: 0, y: CGFloat(height))
        c.scaleBy(x: 1, y: -1)

        // With a flipped CTM glyphs would render mirrored; flipping the text
        // matrix cancels it so text draws upright at a y-down baseline.
        c.textMatrix = CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: 0, ty: 0)

        // Start black, matching a fresh Haiku screen before the first paint.
        c.setFillColor(CGColor(gray: 0, alpha: 1))
        c.fill(CGRect(x: 0, y: 0, width: CGFloat(width), height: CGFloat(height)))
    }

    public func snapshot() -> CGImage? { ctx.makeImage() }

    /// Reads one pixel in Haiku screen coordinates (y down from the top).
    ///
    /// The base CTM flip installed above means memory row 0 is wire y 0, so the
    /// offset is direct. Returns straight RGBA, which equals the stored
    /// premultiplied bytes whenever the pixel is opaque — which it always is in
    /// practice, as `app_server` composites into an opaque framebuffer.
    public func pixel(x: Int, y: Int) -> RGBColor? {
        guard x >= 0, x < width, y >= 0, y < height,
              let base = ctx.data else { return nil }
        let p = base.assumingMemoryBound(to: UInt8.self) + y * bytesPerRow + x * 4
        return RGBColor(r: p[2], g: p[1], b: p[0], a: p[3])
    }

    /// Writes one pixel in Haiku screen coordinates. Test-support and the
    /// cursor compositor use this; the renderer goes through CoreGraphics.
    public func setPixel(x: Int, y: Int, to c: RGBColor) {
        guard x >= 0, x < width, y >= 0, y < height,
              let base = ctx.data else { return }
        let p = base.assumingMemoryBound(to: UInt8.self) + y * bytesPerRow + x * 4
        p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = c.a
    }

    public func clear() {
        ctx.saveGState()
        ctx.setBlendMode(.copy)
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: CGFloat(width), height: CGFloat(height)))
        ctx.restoreGState()
    }

    /// Reads a region back out as B_RGB24, for RP_READ_BITMAP.
    /// Row padding satisfies `bytesPerRow >= width * 3`; the reference client's
    /// `(width * 3 + 3) & ~7` can round *below* that and is not copied here
    /// (PROTOCOL.md §7.4).
    /// - Parameter cursor: when set, the cursor image and its top-left corner in
    ///   screen coordinates, composited into the result. `RP_READ_BITMAP` carries
    ///   a `drawCursor` flag asking for exactly this; the cursor is deliberately
    ///   not in the canvas itself, so it has to be added here.
    public func readRegionRGB24(_ rect: BRect,
                                cursor: (image: CGImage, origin: CGPoint)? = nil)
        -> (bytesPerRow: Int, bits: [UInt8], width: Int, height: Int)? {
        guard let image = ctx.makeImage() else { return nil }
        let x = max(0, Int(rect.left.rounded(.down)))
        let y = max(0, Int(rect.top.rounded(.down)))
        let w = min(width - x, Int(rect.width.rounded()))
        let h = min(height - y, Int(rect.height.rounded()))
        guard w > 0, h > 0 else { return nil }
        guard let cropped = image.cropping(
            to: CGRect(x: x, y: y, width: w, height: h)) else { return nil }

        // Re-render the crop into a known BGRA buffer so byte order is certain.
        var scratch = [UInt8](repeating: 0, count: w * h * 4)
        let info: CGBitmapInfo = [
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue),
            .byteOrder32Little,
        ]
        let ok = scratch.withUnsafeMutableBytes { buf -> Bool in
            guard let c = CGContext(data: buf.baseAddress, width: w, height: h,
                                   bitsPerComponent: 8, bytesPerRow: w * 4,
                                   space: CGColorSpaceCreateDeviceRGB(),
                                   bitmapInfo: info.rawValue) else { return false }
            c.draw(cropped, in: CGRect(x: 0, y: 0, width: w, height: h))
            if let cur = cursor {
                // This scratch context is y-up and unflipped, and draw(_:in:)
                // places an image upright, so convert the cursor's top edge to a
                // bottom edge to land it on the right rows.
                let cw = CGFloat(cur.image.width), ch = CGFloat(cur.image.height)
                let localX = cur.origin.x - CGFloat(x)
                let localTop = cur.origin.y - CGFloat(y)
                c.draw(cur.image, in: CGRect(x: localX,
                                             y: CGFloat(h) - (localTop + ch),
                                             width: cw, height: ch))
            }
            return true
        }
        guard ok else { return nil }

        let bytesPerRow = ((w * 3) + 3) & ~3
        var bits = [UInt8](repeating: 0, count: bytesPerRow * h)
        for row in 0..<h {
            var src = row * w * 4
            var dst = row * bytesPerRow
            for _ in 0..<w {
                bits[dst] = scratch[src]         // blue
                bits[dst + 1] = scratch[src + 1] // green
                bits[dst + 2] = scratch[src + 2] // red
                src += 4; dst += 3
            }
        }
        return (bytesPerRow, bits, w, h)
    }
}

/// Text measurement and drawing via CoreText.
///
/// This exists because `app_server` delegates text layout to the client and
/// **blocks waiting for the answer** (PROTOCOL.md §7.1) — so measurement has to
/// be both fast and cached.
public final class TextEngine {
    private var fontCache: [FontKey: CTFont] = [:]
    private var widthCache: [WidthKey: Float] = [:]

    private struct FontKey: Hashable {
        let size: Float, bold: Bool, italic: Bool, mono: Bool
    }
    private struct WidthKey: Hashable {
        let text: String, size: Float, bold: Bool, italic: Bool, mono: Bool
    }

    public init() {}

    public func ctFont(for f: HaikuFont) -> CTFont {
        let key = FontKey(size: f.size, bold: f.isBold, italic: f.isItalic,
                          mono: f.isMonospaced)
        if let cached = fontCache[key] { return cached }
        // The reference client picks Helvetica, or monospace when spacing is
        // B_FIXED_SPACING, and ignores family/style entirely
        // (PROTOCOL.md §7.3). Matching that keeps us close to the browser demo.
        let name = f.isMonospaced ? "Menlo" : "Helvetica"
        var font = CTFontCreateWithName(name as CFString, CGFloat(f.size), nil)
        var traits: CTFontSymbolicTraits = []
        if f.isBold { traits.insert(.traitBold) }
        if f.isItalic { traits.insert(.traitItalic) }
        if !traits.isEmpty,
           let styled = CTFontCreateCopyWithSymbolicTraits(
               font, CGFloat(f.size), nil, traits, traits) {
            font = styled
        }
        fontCache[key] = font
        return font
    }

    // CoreText attribute names, used directly: the `.font` /
    // `.foregroundColor` conveniences live in AppKit, and this module
    // deliberately does not link it.
    private static let fontKey =
        NSAttributedString.Key(kCTFontAttributeName as String)
    private static let colorKey =
        NSAttributedString.Key(kCTForegroundColorAttributeName as String)

    private func line(_ text: String, _ f: HaikuFont) -> CTLine {
        let attrs: [NSAttributedString.Key: Any] = [
            Self.fontKey: ctFont(for: f),
        ]
        return CTLineCreateWithAttributedString(
            NSAttributedString(string: text, attributes: attrs))
    }

    /// Advance width, cached. UI labels repeat constantly, and every miss costs
    /// `app_server` a blocking round trip.
    public func width(_ text: String, _ f: HaikuFont) -> Float {
        let key = WidthKey(text: text, size: f.size, bold: f.isBold,
                           italic: f.isItalic, mono: f.isMonospaced)
        if let w = widthCache[key] { return w }
        let w = Float(CTLineGetTypographicBounds(line(text, f), nil, nil, nil))
        if widthCache.count < 8192 { widthCache[key] = w }
        return w
    }

    /// Draws at a y-down baseline and returns the advance width.
    @discardableResult
    public func draw(_ text: String, at p: BPoint, font f: HaikuFont,
                    color: CGColor, in ctx: CGContext) -> Float {
        let attrs: [NSAttributedString.Key: Any] = [
            Self.fontKey: ctFont(for: f),
            Self.colorKey: color,
        ]
        let l = CTLineCreateWithAttributedString(
            NSAttributedString(string: text, attributes: attrs))
        ctx.textPosition = CGPoint(x: CGFloat(p.x), y: CGFloat(p.y))
        CTLineDraw(l, ctx)
        return Float(CTLineGetTypographicBounds(l, nil, nil, nil))
    }
}
