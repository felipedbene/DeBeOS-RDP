import CoreGraphics
import Foundation

/// Per-view drawing state, keyed by the int32 token the server assigns with
/// RP_CREATE_STATE. State is **sticky**: the server elides no-op changes, so
/// every field must persist until explicitly changed (PROTOCOL.md §5.2).
public final class DrawState {
    public let token: Int32

    public var highColor = RGBColor(r: 0, g: 0, b: 0, a: 255)
    public var lowColor = RGBColor(r: 255, g: 255, b: 255, a: 255)
    public var penSize: Float = 1.0
    public var lineCap: CGLineCap = .butt
    public var lineJoin: CGLineJoin = .miter
    public var miterLimit: Float = 10
    /// 8x8 1-bpp stipple. All-0xff is B_SOLID_HIGH, the overwhelmingly common case.
    public var pattern: [UInt8] = Array(repeating: 0xff, count: 8)
    public var font = HaikuFont()
    public var transform: CGAffineTransform = .identity
    public var xOffset: Int32 = 0
    public var yOffset: Int32 = 0
    public var clipRects: [BRect] = []

    public var drawingMode: DrawingMode = .copy
    /// B_CONSTANT_ALPHA means highColor.alpha acts as a global multiplier
    /// rather than per-pixel alpha.
    public var constantAlpha = false
    public var blendModesEnabled = false
    /// The reference client's `unsetAlpha`: under B_OP_COPY every colour is
    /// forced opaque.
    public var forceOpaque = false

    public init(token: Int32) { self.token = token }

    /// Only `B_SOLID_HIGH` (all bits set) and `B_SOLID_LOW` (all bits clear) are
    /// solid, matching `PatternHandler::IsSolid()` (PatternHandler.h:124).
    ///
    /// Testing "are all eight bytes equal" instead is wrong and quietly so: that
    /// is true of every *vertical* stripe pattern, since identical rows still
    /// vary along x. A pattern of eight 0xf0 bytes is four-on four-off columns,
    /// and treating it as solid fills flat with the high colour.
    public var patternIsSolid: Bool {
        pattern.allSatisfy { $0 == 0xff } || pattern.allSatisfy { $0 == 0x00 }
    }

    /// For a solid pattern, which colour it resolves to: all-bits-clear selects
    /// the low colour, anything else the high colour.
    public var solidColor: RGBColor {
        (pattern.first ?? 0xff) == 0 ? lowColor : highColor
    }

    public var globalAlpha: CGFloat {
        if blendModesEnabled && constantAlpha {
            return CGFloat(highColor.a) / 255.0
        }
        return 1.0
    }

    /// Always `.normal`, because only `B_OP_COPY`, `B_OP_OVER` and `B_OP_ALPHA`
    /// reach the CoreGraphics path and all three are source-over.
    ///
    /// Every other mode is a conditional per-pixel rewrite that no `CGBlendMode`
    /// expresses — see `softBlend`. Mapping them onto approximate CG modes is what
    /// the reference client does, and it is wrong in visible ways: `.darken` and
    /// `.lighten` are per-channel, but `B_OP_MIN`/`B_OP_MAX` compare whole-pixel
    /// *brightness* and replace the entire pixel.
    public var blendMode: CGBlendMode { .normal }

    /// True when this state cannot be painted with CoreGraphics alone.
    ///
    /// A non-solid stipple also forces the software path even in an ordinary
    /// mode, because Haiku anchors the 8x8 pattern to the *view origin*
    /// (`PatternHandler::IsHighColor`) — tiling it from the shape's bounding box
    /// puts the pattern phase in the wrong place.
    public var needsSoftBlend: Bool {
        drawingMode.requiresSoftBlend || !patternIsSolid
    }

    /// The colour source this state paints with.
    ///
    /// A pattern-gated mode always gets a `PatternSource`, even for a solid
    /// pattern: under `B_SOLID_LOW` every bit is clear, so `IsHighColor` is false
    /// everywhere and `B_OP_ERASE`/`INVERT`/`SELECT` must touch nothing at all.
    /// Collapsing that to a single colour would lose the distinction and paint
    /// the whole shape.
    public func pixelSource() -> PixelSource {
        if patternIsSolid && !drawingMode.isPatternGated {
            return SolidSource(solidColor)
        }
        return PatternSource(pattern: pattern, high: highColor, low: lowColor,
                             xOffset: xOffset, yOffset: yOffset)
    }

    /// Geometry-affecting context state only: pen, clip and transform.
    ///
    /// The mask rasteriser needs exactly this and must *not* get the blend mode
    /// or the global alpha — coverage has to come out at full strength so
    /// `softBlend` can use it as the blend macros' `cover` argument.
    public func applyGeometry(to ctx: CGContext) {
        ctx.setLineWidth(CGFloat(max(penSize, 0.0)))
        ctx.setLineCap(lineCap)
        ctx.setLineJoin(lineJoin)
        ctx.setMiterLimit(CGFloat(miterLimit))

        if !clipRects.isEmpty {
            ctx.clip(to: clipRects.map { $0.cgRect })
        }
        if !transform.isIdentity {
            // Haiku applies the transform about the view offset.
            ctx.translateBy(x: CGFloat(xOffset), y: CGFloat(yOffset))
            ctx.concatenate(transform)
            ctx.translateBy(x: CGFloat(-xOffset), y: CGFloat(-yOffset))
        }
    }

    public func apply(to ctx: CGContext) {
        ctx.setAlpha(globalAlpha)
        ctx.setBlendMode(blendMode)
        applyGeometry(to: ctx)
    }

    /// The full view->screen transform, for mapping a path's bounding box into
    /// screen space when sizing a coverage mask, and for inverse-mapping screen
    /// pixels back into gradient space.
    /// The offset is the pivot: shift to the origin, transform, shift back. This
    /// is the same order `applyGeometry` builds on the CTM — CoreGraphics
    /// pre-concatenates, so its calls take effect in reverse.
    public var compositeTransform: CGAffineTransform {
        guard !transform.isIdentity else { return .identity }
        return CGAffineTransform(translationX: CGFloat(-xOffset),
                                 y: CGFloat(-yOffset))
            .concatenating(transform)
            .concatenating(CGAffineTransform(translationX: CGFloat(xOffset),
                                            y: CGFloat(yOffset)))
    }

    /// Screen-space bounds of `box`, clipped by the state's clipping region.
    public func screenBounds(of box: CGRect) -> CGRect {
        var b = transform.isIdentity ? box : box.applying(compositeTransform)
        if !clipRects.isEmpty {
            var clip = CGRect.null
            for r in clipRects { clip = clip.union(r.cgRect) }
            b = b.intersection(clip)
        }
        return b
    }

    /// Colour to paint solid geometry with on the CoreGraphics path, honouring
    /// `forceOpaque` the way the reference client's `applyContext()` does.
    ///
    /// No `invert` special case: `B_OP_INVERT` is one of the modes that always
    /// takes the software path, where it is applied exactly rather than
    /// approximated by differencing against white.
    public func strokeAndFillColor() -> CGColor {
        solidColor.cgColor(forceOpaque: forceOpaque)
    }
}
