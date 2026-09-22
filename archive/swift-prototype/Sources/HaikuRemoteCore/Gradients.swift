import CoreGraphics
import Foundation

/// Gradient evaluation matching `app_server`'s, for the kinds CoreGraphics
/// cannot draw and for gradients under a software drawing mode.
///
/// Haiku rasterises gradients with AGG: a 256-entry colour LUT
/// (`Painter::_MakeGradient`) indexed by a per-kind distance function
/// (`agg_span_gradient.h`) evaluated in a space where the gradient's centre is
/// the origin. Both are transcribed here, with two behaviours that are easy to
/// get wrong because they look like bugs:
///
/// - **`B_GRADIENT_RADIAL_FOCUS` ignores its focal point.**
///   `Painter::_FillPathGradient` default-constructs
///   `agg::gradient_radial_focus`, whose focus is (0, 0), and never calls
///   `init()` with the gradient's focus (Painter.cpp:2101-2110). With the focus
///   at the centre that function reduces to plain radial distance, so on a real
///   Haiku desktop a radial-focus gradient is indistinguishable from a radial
///   one. We match `app_server`, because fidelity to the server is the point of
///   a remote desktop; drawing the focus "properly" would make this client
///   differ visibly from a local screen.
///
/// - **Diamond and conic have a fixed extent.** `_CalcRadialGradientTransform`
///   is called with the default `gradient_d2 = 100` (Painter.h:325), so those
///   two gradients always span 100 view units from the centre, no matter how big
///   the shape is. Scaling them to the shape's bounding box — the obvious guess —
///   is wrong.
public struct GradientSource: PixelSource {
    private let lut: [RGBColor]
    private let kind: Gradient.Kind
    private let center: CGPoint
    /// Linear only: unit direction and length of the gradient axis.
    private let axis: CGVector
    private let length: CGFloat
    private let radius: CGFloat
    /// Maps a screen pixel back into the space the gradient is defined in, when
    /// the view has a transform. Haiku folds `fTransform` into the (inverted)
    /// gradient matrix, so the gradient is transformed along with the geometry.
    private let inverse: CGAffineTransform?

    public struct CGVector { public var dx: CGFloat, dy: CGFloat }

    /// AGG's `gradient_d2` default: the extent of a diamond or conic gradient,
    /// in view units (Painter.h:325).
    public static let defaultExtent: CGFloat = 100

    public init(_ g: Gradient, forceOpaque: Bool = false,
                inverse: CGAffineTransform? = nil) {
        self.lut = Self.makeLUT(g, forceOpaque: forceOpaque)
        self.kind = g.kind
        self.inverse = inverse
        switch g.kind {
        case .linear:
            let dx = CGFloat(g.end.x - g.start.x)
            let dy = CGFloat(g.end.y - g.start.y)
            let len = (dx * dx + dy * dy).squareRoot()
            self.center = g.start.cgPoint
            self.length = len
            self.axis = len > 0 ? CGVector(dx: dx / len, dy: dy / len)
                                : CGVector(dx: 1, dy: 0)
            self.radius = 0
        case .radial, .radialFocus:
            self.center = g.center.cgPoint
            self.radius = CGFloat(g.radius)
            self.axis = CGVector(dx: 1, dy: 0)
            self.length = 0
        case .diamond, .conic, .none:
            self.center = g.center.cgPoint
            self.radius = Self.defaultExtent
            self.axis = CGVector(dx: 1, dy: 0)
            self.length = 0
        }
    }

    /// `Painter::_MakeGradient` (Painter.cpp:2175): 256 entries, linear between
    /// stops, clamped to the first and last stop outside their offsets. Wire
    /// offsets are 0-255, which is why the server divides by 255.
    static func makeLUT(_ g: Gradient, forceOpaque: Bool) -> [RGBColor] {
        let size = 256
        let fallback = RGBColor(r: 0, g: 0, b: 0, a: forceOpaque ? 255 : 0)
        guard let first = g.stops.first else {
            return [RGBColor](repeating: fallback, count: size)
        }
        func fix(_ c: RGBColor) -> RGBColor {
            forceOpaque ? RGBColor(r: c.r, g: c.g, b: c.b, a: 255) : c
        }
        var out = [RGBColor](repeating: fix(first.color), count: size)
        var from = first
        // floorf(count * offset / 255 + 0.5) — the +0.5 is inside the floor.
        var index = Int((Double(size) * Double(first.offset) / 255 + 0.5)
                        .rounded(.down))
        index = Swift.max(0, Swift.min(index, size))
        for i in 1..<Swift.max(g.stops.count, 1) {
            let to = g.stops[i]
            var offset = Int((Double(size - 1) * Double(to.offset) / 255 + 0.5)
                             .rounded(.down))
            offset = Swift.min(offset, size - 1)
            let dist = offset - index
            if dist >= 0 {
                let startIndex = Swift.max(index, 0)
                let stopIndex = Swift.min(offset, size - 1)
                if startIndex <= stopIndex {
                    for j in startIndex...stopIndex {
                        let f = Double(offset - j) / Double(dist + 1)
                        let t = 1.0 - f
                        out[j] = fix(RGBColor(
                            r: UInt8((Double(from.color.r) * f
                                      + Double(to.color.r) * t + 0.5)
                                     .rounded(.down)),
                            g: UInt8((Double(from.color.g) * f
                                      + Double(to.color.g) * t + 0.5)
                                     .rounded(.down)),
                            b: UInt8((Double(from.color.b) * f
                                      + Double(to.color.b) * t + 0.5)
                                     .rounded(.down)),
                            a: UInt8((Double(from.color.a) * f
                                      + Double(to.color.a) * t + 0.5)
                                     .rounded(.down))))
                    }
                }
            }
            index = offset + 1
            from = to
        }
        if index < size {
            for j in Swift.max(index, 0)..<size { out[j] = fix(from.color) }
        }
        return out
    }

    /// The normalised position along the gradient, before LUT lookup. AGG clamps
    /// rather than repeating (`span_gradient::generate`).
    func parameter(x: Int, y: Int) -> CGFloat {
        var p = CGPoint(x: CGFloat(x) + 0.5, y: CGFloat(y) + 0.5)
        if let inv = inverse { p = p.applying(inv) }
        let dx = p.x - center.x
        let dy = p.y - center.y
        switch kind {
        case .linear:
            // agg::gradient_x after _CalcLinearGradientTransform: the projection
            // onto the axis, normalised by its length.
            guard length > 0 else { return 0 }
            return (dx * axis.dx + dy * axis.dy) / length
        case .radial, .radialFocus:
            guard radius > 0 else { return 0 }
            return (dx * dx + dy * dy).squareRoot() / radius
        case .diamond:
            // agg::gradient_diamond: Chebyshev distance, extent 100.
            return Swift.max(abs(dx), abs(dy)) / radius
        case .conic:
            // agg::gradient_conic: |atan2(y, x)| / pi. Note the gradient's own
            // `angle` field is never used by app_server.
            return abs(atan2(dy, dx)) / .pi
        case .none:
            return 0
        }
    }

    public func color(x: Int, y: Int) -> RGBColor {
        let t = parameter(x: x, y: y)
        var i = Int(t * CGFloat(lut.count))
        if i < 0 { i = 0 }
        if i >= lut.count { i = lut.count - 1 }
        return lut[i]
    }

    /// Gradients have no stipple, so nothing is masked out.
    public func isHigh(x: Int, y: Int) -> Bool { true }

    /// True when CoreGraphics cannot draw this gradient kind and it must be
    /// evaluated per pixel.
    public static func needsSoftware(_ g: Gradient) -> Bool {
        switch g.kind {
        case .diamond, .conic: return true
        case .linear, .radial, .radialFocus, .none: return false
        }
    }
}
