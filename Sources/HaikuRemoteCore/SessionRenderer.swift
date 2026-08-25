import CoreGraphics
import CoreText
import Foundation

/// What the renderer needs from its transport: a way to send replies, and a way
/// to report that the canvas changed.
public protocol SessionRendererDelegate: AnyObject {
    func rendererWantsToSend(_ data: Data)
    func rendererDidUpdate(rect: CGRect?)
    func rendererCursorChanged()
    func rendererLog(_ message: String)
}

/// Applies the RP_* drawing-command stream to a `Canvas`.
///
/// This is the heart of the client. Because the protocol ships drawing calls
/// rather than pixels (PROTOCOL.md §0), this class is effectively a
/// reimplementation of Haiku's `DrawingEngine` on top of CoreGraphics.
public final class SessionRenderer {
    public private(set) var canvas: Canvas
    public weak var delegate: SessionRendererDelegate?

    private var states: [Int32: DrawState] = [:]
    private var palette: [UInt32] = []
    private let text = TextEngine()

    /// Cursor is kept out of the canvas and composited by the view, mirroring the
    /// reference client's separate cursor canvas. Drawing it into the framebuffer
    /// would leave trails, since nothing ever repaints what it covered.
    public private(set) var cursorImage: CGImage?
    public private(set) var cursorHotspot = CGPoint.zero
    public private(set) var cursorPosition = CGPoint.zero
    public private(set) var cursorVisible = true

    public struct Stats {
        public var messages = 0
        public var unhandled: [UInt16: Int] = [:]
        public var stringWidthReplies = 0
        public var drawStringReplies = 0
        public var bitmapBytes = 0
    }
    public private(set) var stats = Stats()

    public init(canvas: Canvas) { self.canvas = canvas }

    /// Swaps in a differently sized canvas for a live display-mode change.
    ///
    /// Per-token state is deliberately **kept**. The server's view states are
    /// unaffected by a screen resize, and because state is sticky it will not
    /// re-send colours or fonts it believes we already have — dropping them here
    /// would leave the repaint drawing in whatever each token's defaults are.
    /// That is the opposite of `resetForReconnect`, where the states really are
    /// stale.
    public func resize(to canvas: Canvas) {
        self.canvas = canvas
    }

    /// Full reset for a reconnect. Stale token states would carry stale clipping
    /// regions and corrupt output (PROTOCOL.md §9.2).
    public func resetForReconnect() {
        states.removeAll()
        canvas.clear()
        cursorImage = nil
        delegate?.rendererDidUpdate(rect: nil)
    }

    private func state(_ token: Int32) -> DrawState {
        if let s = states[token] { return s }
        // The server does not guarantee RP_CREATE_STATE arrives first, so create
        // lazily rather than dropping the message, as the reference client does.
        let s = DrawState(token: token)
        states[token] = s
        return s
    }

    // MARK: - Dispatch

    public func handle(code: UInt16, payload: [UInt8]) {
        stats.messages += 1
        var r = WireReader(payload)
        do {
            if RP.sessionLevel.contains(code) {
                try handleSessionLevel(code: code, r: &r)
            } else {
                let token = try r.i32()
                try handleTokened(code: code, token: token, r: &r)
            }
        } catch {
            delegate?.rendererLog(
                "decode error in \(RP.name(code)) (\(payload.count)B): \(error)")
        }
    }

    private func note(unhandled code: UInt16) {
        stats.unhandled[code, default: 0] += 1
        if stats.unhandled[code] == 1 {
            delegate?.rendererLog("unhandled op \(RP.name(code)) (first occurrence)")
        }
    }

    // MARK: - Session-level

    private func handleSessionLevel(code: UInt16, r: inout WireReader) throws {
        switch code {
        case RP.initConnection:
            delegate?.rendererLog("handshake acknowledged by server")

        case RP.closeConnection:
            delegate?.rendererLog("server sent RP_CLOSE_CONNECTION")

        case RP.getSystemPaletteResult:
            let count = try r.u32()
            var p: [UInt32] = []
            p.reserveCapacity(Int(count))
            for _ in 0..<Int(min(count, 4096)) {
                let c = try r.color()
                // Packed the way the reference client does: r | g<<8 | b<<16 | a<<24
                p.append(UInt32(c.r) | UInt32(c.g) << 8 | UInt32(c.b) << 16
                         | UInt32(c.a) << 24)
            }
            palette = p
            delegate?.rendererLog("system palette: \(p.count) entries")

        case RP.createState:
            let token = try r.i32()
            states[token] = DrawState(token: token)

        case RP.deleteState:
            let token = try r.i32()
            states.removeValue(forKey: token)

        case RP.invalidateRect, RP.invalidateRegion:
            // Nothing to do: the canvas already holds the truth (PROTOCOL.md §0).
            break

        case RP.copyRectNoClipping:
            let dx = try r.i32(), dy = try r.i32()
            let rect = try r.rect()
            copyRect(rect, dx: Int(dx), dy: Int(dy))

        case RP.fillRegionColorNoClipping:
            let rects = try r.region()
            let color = try r.color()
            let ctx = canvas.ctx
            ctx.saveGState()
            ctx.setBlendMode(.normal)
            ctx.setAlpha(1)
            ctx.setFillColor(color.cgColor(forceOpaque: false))
            for rect in rects { ctx.fill(rect.cgRect) }
            ctx.restoreGState()
            invalidate(rects)

        case RP.setCursor:
            cursorHotspot = try r.point().cgPoint
            var bm = try BitmapDecoder.read(&r, minimal: false, forceOpaque: false,
                                          palette: palette.isEmpty ? nil : palette,
                                          warn: { [weak self] in
                                              self?.delegate?.rendererLog($0)
                                          })
            BitmapDecoder.premultiply(&bm)
            cursorImage = BitmapDecoder.makeImage(bm)
            delegate?.rendererCursorChanged()

        case RP.setCursorVisible:
            cursorVisible = try r.bool()
            delegate?.rendererCursorChanged()

        case RP.moveCursorTo:
            cursorPosition = CGPoint(x: CGFloat(try r.f32()),
                                     y: CGFloat(try r.f32()))
            delegate?.rendererCursorChanged()

        default:
            note(unhandled: code)
        }
    }

    /// RP_COPY_RECT_NO_CLIPPING: used heavily for scrolling.
    private func copyRect(_ rect: BRect, dx: Int, dy: Int) {
        guard let snap = canvas.ctx.makeImage() else { return }
        // Clip the source to the canvas first: cropping() silently intersects,
        // and an out-of-bounds request would otherwise stretch a short piece
        // across the full destination.
        let src = rect.cgRect.intersection(
            CGRect(x: 0, y: 0, width: CGFloat(canvas.width),
                   height: CGFloat(canvas.height)))
        guard !src.isEmpty else { return }
        // makeImage() is in y-down image space already because our context is
        // flipped, so crop directly with the wire coordinates.
        guard let piece = snap.cropping(to: src) else { return }
        let ctx = canvas.ctx
        ctx.saveGState()
        ctx.setBlendMode(.copy)
        ctx.setAlpha(1)
        let dest = src.offsetBy(dx: CGFloat(dx), dy: CGFloat(dy))
        // Flip locally, exactly as drawBitmap does: draw(_:in:) puts image row 0
        // at the rect's *max-y* edge, which under the canvas' flipped CTM is the
        // bottom. Without this the copied block lands vertically mirrored — and
        // since app_server scrolls, moves and resizes by copying pixels
        // (RemoteDrawingEngine::CopyRect, RemoteMessage.cpp RP_COPY_RECT), that
        // turns a Terminal's scrollback and whole moved windows upside down.
        ctx.translateBy(x: dest.minX, y: dest.minY)
        ctx.translateBy(x: 0, y: dest.height)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(piece, in: CGRect(x: 0, y: 0, width: dest.width,
                                   height: dest.height))
        ctx.restoreGState()
        delegate?.rendererDidUpdate(rect: dest)
    }

    // MARK: - Token-addressed

    private func handleTokened(code: UInt16, token: Int32,
                              r: inout WireReader) throws {
        let s = state(token)
        switch code {

        // -- state setters ------------------------------------------------
        case RP.setHighColor: s.highColor = try r.color()
        case RP.setLowColor: s.lowColor = try r.color()
        case RP.setPenSize: s.penSize = try r.f32()
        case RP.setPattern: s.pattern = try r.raw(8)
        case RP.setFont: s.font = try r.font()
        case RP.setTransform: s.transform = try r.transform()
        case RP.setOffsets:
            s.xOffset = try r.i32(); s.yOffset = try r.i32()
        case RP.setStrokeMode:
            s.lineCap = Self.capMode(try r.u32())
            s.lineJoin = Self.joinMode(try r.u32())
            s.miterLimit = try r.f32()
        case RP.setBlendingMode:
            let sourceAlpha = try r.u32()
            let alphaFunction = try r.u32()
            s.constantAlpha = (sourceAlpha == 1)   // B_CONSTANT_ALPHA
            if s.blendModesEnabled { s.forceOpaque = s.constantAlpha }
            if alphaFunction != 0 {                // only B_ALPHA_OVERLAY supported
                delegate?.rendererLog("alpha function \(alphaFunction) unsupported")
            }
        case RP.setDrawingMode:
            let raw = try r.u32()
            s.drawingMode = DrawingMode(rawValue: raw) ?? .over
            s.forceOpaque = false
            s.blendModesEnabled = false
            switch s.drawingMode {
            case .copy: s.forceOpaque = true
            case .alpha:
                s.blendModesEnabled = true
                s.forceOpaque = s.constantAlpha
            default: break
            }
        case RP.constrainClippingRegion:
            s.clipRects = try r.region()
        case RP.enableSyncDrawing, RP.disableSyncDrawing:
            break

        // -- rects / ellipses ---------------------------------------------
        case RP.fillRect:
            let rect = try r.rect()
            fill(path: CGPath(rect: rect.cgRect, transform: nil), s, bounds: rect.cgRect)
        case RP.strokeRect:
            let rect = try r.rect()
            stroke(path: CGPath(rect: rect.cgRect, transform: nil), s)
        case RP.fillEllipse:
            let rect = try r.rect()
            fill(path: CGPath(ellipseIn: rect.exactRect, transform: nil), s,
                 bounds: rect.cgRect)
        case RP.strokeEllipse:
            let rect = try r.rect()
            stroke(path: CGPath(ellipseIn: rect.exactRect, transform: nil), s)
        case RP.fillRoundRect, RP.strokeRoundRect:
            let rect = try r.rect()
            let rx = CGFloat(try r.f32()), ry = CGFloat(try r.f32())
            let p = CGPath(roundedRect: rect.exactRect, cornerWidth: rx,
                           cornerHeight: ry, transform: nil)
            if code == RP.fillRoundRect {
                fill(path: p, s, bounds: rect.cgRect)
            } else {
                stroke(path: p, s)
            }
        case RP.invertRect:
            let rect = try r.rect()
            let ctx = canvas.ctx
            ctx.saveGState()
            s.apply(to: ctx)
            ctx.setBlendMode(.difference)
            ctx.setFillColor(CGColor(gray: 1, alpha: 1))
            ctx.fill(rect.cgRect)
            ctx.restoreGState()
            delegate?.rendererDidUpdate(rect: rect.cgRect)

        // -- arcs ----------------------------------------------------------
        case RP.fillArc, RP.strokeArc:
            let rect = try r.rect()
            let angle = try r.f32(), span = try r.f32()
            drawArc(rect, angle: angle, span: span,
                    filled: code == RP.fillArc, s, gradient: nil)
        case RP.fillArcGradient, RP.strokeArcGradient:
            let rect = try r.rect()
            let angle = try r.f32(), span = try r.f32()
            let g = try r.gradient()
            drawArc(rect, angle: angle, span: span,
                    filled: code == RP.fillArcGradient, s, gradient: g)

        // -- lines / points -----------------------------------------------
        case RP.strokeLine:
            let a = try r.point(), b = try r.point()
            let p = CGMutablePath()
            p.move(to: a.cgPoint); p.addLine(to: b.cgPoint)
            stroke(path: p, s)
        case RP.strokeLineGradient:
            let a = try r.point(), b = try r.point()
            let g = try r.gradient()
            let p = CGMutablePath()
            p.move(to: a.cgPoint); p.addLine(to: b.cgPoint)
            strokeGradient(path: p, s, gradient: g,
                           bounds: p.boundingBoxOfPath)
        case RP.strokeLineArray:
            let n = try r.i32()
            guard n >= 0, n <= 1 << 20 else { throw WireError("line array \(n)") }
            let ctx = canvas.ctx
            ctx.saveGState()
            s.apply(to: ctx)
            ctx.setLineCap(.square)
            var dirty = CGRect.null
            for _ in 0..<Int(n) {
                let a = try r.point(), b = try r.point()
                let c = try r.color()
                ctx.setStrokeColor(c.cgColor(forceOpaque: s.forceOpaque))
                ctx.beginPath()
                // Half-pixel offset, matching the reference client, so 1px lines
                // land on pixel centres instead of straddling boundaries.
                ctx.move(to: CGPoint(x: CGFloat(a.x) + 0.5, y: CGFloat(a.y) + 0.5))
                ctx.addLine(to: CGPoint(x: CGFloat(b.x) + 0.5, y: CGFloat(b.y) + 0.5))
                ctx.strokePath()
                dirty = dirty.union(CGRect(x: CGFloat(min(a.x, b.x)),
                                           y: CGFloat(min(a.y, b.y)),
                                           width: CGFloat(abs(b.x - a.x)) + 2,
                                           height: CGFloat(abs(b.y - a.y)) + 2))
            }
            ctx.restoreGState()
            delegate?.rendererDidUpdate(rect: dirty.isNull ? nil : dirty)

        case RP.strokePointColor:
            let p = try r.point()
            let c = try r.color()
            let ctx = canvas.ctx
            ctx.saveGState()
            s.apply(to: ctx)
            ctx.setFillColor(c.cgColor(forceOpaque: s.forceOpaque))
            let rect = CGRect(x: CGFloat(p.x), y: CGFloat(p.y), width: 1, height: 1)
            ctx.fill(rect)
            ctx.restoreGState()
            delegate?.rendererDidUpdate(rect: rect)

        case RP.strokeLine1pxColor:
            let a = try r.point(), b = try r.point()
            let c = try r.color()
            let ctx = canvas.ctx
            ctx.saveGState()
            s.apply(to: ctx)
            ctx.setStrokeColor(c.cgColor(forceOpaque: s.forceOpaque))
            ctx.setLineWidth(1)
            ctx.setLineCap(.square)
            ctx.beginPath()
            ctx.move(to: CGPoint(x: CGFloat(a.x) + 0.5, y: CGFloat(a.y) + 0.5))
            ctx.addLine(to: CGPoint(x: CGFloat(b.x) + 0.5, y: CGFloat(b.y) + 0.5))
            ctx.strokePath()
            ctx.restoreGState()
            delegate?.rendererDidUpdate(
                rect: CGRect(x: CGFloat(min(a.x, b.x)) - 1,
                             y: CGFloat(min(a.y, b.y)) - 1,
                             width: CGFloat(abs(b.x - a.x)) + 3,
                             height: CGFloat(abs(b.y - a.y)) + 3))

        case RP.strokeRect1pxColor:
            let rect = try r.rect()
            let c = try r.color()
            let ctx = canvas.ctx
            ctx.saveGState()
            s.apply(to: ctx)
            ctx.setLineJoin(.miter)
            ctx.setMiterLimit(10)
            ctx.setStrokeColor(c.cgColor(forceOpaque: s.forceOpaque))
            ctx.setLineWidth(1)
            // Inset by half a pixel so a 1px stroke covers the intended pixels.
            ctx.stroke(rect.cgRect.insetBy(dx: 0.5, dy: 0.5))
            ctx.restoreGState()
            delegate?.rendererDidUpdate(rect: rect.cgRect.insetBy(dx: -1, dy: -1))

        case RP.fillRectColor:
            let rect = try r.rect()
            let c = try r.color()
            let ctx = canvas.ctx
            ctx.saveGState()
            s.apply(to: ctx)
            ctx.setFillColor(c.cgColor(forceOpaque: s.forceOpaque))
            ctx.fill(rect.cgRect)
            ctx.restoreGState()
            delegate?.rendererDidUpdate(rect: rect.cgRect)

        // -- regions -------------------------------------------------------
        case RP.fillRegion:
            let rects = try r.region()
            let p = CGMutablePath()
            for rect in rects { p.addRect(rect.cgRect) }
            fill(path: p, s, bounds: p.boundingBoxOfPath)
        case RP.fillRegionGradient:
            let rects = try r.region()
            let g = try r.gradient()
            let p = CGMutablePath()
            for rect in rects { p.addRect(rect.cgRect) }
            fillGradient(path: p, s, gradient: g, bounds: p.boundingBoxOfPath)

        // -- gradient rect/ellipse ----------------------------------------
        case RP.fillRectGradient, RP.strokeRectGradient:
            let rect = try r.rect()
            let g = try r.gradient()
            let p = CGPath(rect: rect.cgRect, transform: nil)
            if code == RP.fillRectGradient {
                fillGradient(path: p, s, gradient: g, bounds: rect.cgRect)
            } else {
                strokeGradient(path: p, s, gradient: g, bounds: rect.cgRect)
            }
        case RP.fillEllipseGradient, RP.strokeEllipseGradient:
            let rect = try r.rect()
            let g = try r.gradient()
            let p = CGPath(ellipseIn: rect.exactRect, transform: nil)
            if code == RP.fillEllipseGradient {
                fillGradient(path: p, s, gradient: g, bounds: rect.cgRect)
            } else {
                strokeGradient(path: p, s, gradient: g, bounds: rect.cgRect)
            }
        case RP.fillRoundRectGradient, RP.strokeRoundRectGradient:
            let rect = try r.rect()
            let rx = CGFloat(try r.f32()), ry = CGFloat(try r.f32())
            let g = try r.gradient()
            let p = CGPath(roundedRect: rect.exactRect, cornerWidth: rx,
                           cornerHeight: ry, transform: nil)
            if code == RP.fillRoundRectGradient {
                fillGradient(path: p, s, gradient: g, bounds: rect.cgRect)
            } else {
                strokeGradient(path: p, s, gradient: g, bounds: rect.cgRect)
            }

        // -- polygons / beziers / triangles / shapes -----------------------
        case RP.fillPolygon, RP.strokePolygon,
             RP.fillPolygonGradient, RP.strokePolygonGradient:
            let bounds = try r.rect()
            let closed = try r.bool()
            let n = try r.i32()
            guard n >= 0, n <= 1 << 20 else { throw WireError("polygon \(n)") }
            let pts = try (0..<Int(n)).map { _ in try r.point() }
            let grad = (code == RP.fillPolygonGradient
                        || code == RP.strokePolygonGradient)
                ? try r.gradient() : nil
            let p = CGMutablePath()
            if let first = pts.first {
                p.move(to: first.cgPoint)
                for pt in pts.dropFirst() { p.addLine(to: pt.cgPoint) }
                if closed || code == RP.fillPolygon
                    || code == RP.fillPolygonGradient {
                    p.closeSubpath()
                }
            }
            let filled = (code == RP.fillPolygon || code == RP.fillPolygonGradient)
            emit(path: p, s, filled: filled, gradient: grad, bounds: bounds.cgRect)

        case RP.fillBezier, RP.strokeBezier,
             RP.fillBezierGradient, RP.strokeBezierGradient:
            let pts = try (0..<4).map { _ in try r.point() }
            let grad = (code == RP.fillBezierGradient
                        || code == RP.strokeBezierGradient)
                ? try r.gradient() : nil
            let p = CGMutablePath()
            p.move(to: pts[0].cgPoint)
            p.addCurve(to: pts[3].cgPoint, control1: pts[1].cgPoint,
                       control2: pts[2].cgPoint)
            let filled = (code == RP.fillBezier || code == RP.fillBezierGradient)
            if filled { p.closeSubpath() }
            emit(path: p, s, filled: filled, gradient: grad,
                 bounds: p.boundingBoxOfPath)

        case RP.fillTriangle, RP.strokeTriangle,
             RP.fillTriangleGradient, RP.strokeTriangleGradient:
            // Note: points come BEFORE bounds here, the opposite of polygons.
            let pts = try (0..<3).map { _ in try r.point() }
            let bounds = try r.rect()
            let grad = (code == RP.fillTriangleGradient
                        || code == RP.strokeTriangleGradient)
                ? try r.gradient() : nil
            let p = CGMutablePath()
            p.move(to: CGPoint(x: CGFloat(pts[0].x) + 0.5,
                               y: CGFloat(pts[0].y) + 0.5))
            for pt in pts.dropFirst() {
                p.addLine(to: CGPoint(x: CGFloat(pt.x) + 0.5,
                                      y: CGFloat(pt.y) + 0.5))
            }
            p.closeSubpath()
            let filled = (code == RP.fillTriangle || code == RP.fillTriangleGradient)
            emit(path: p, s, filled: filled, gradient: grad, bounds: bounds.cgRect)

        case RP.fillShape, RP.strokeShape,
             RP.fillShapeGradient, RP.strokeShapeGradient:
            let bounds = try r.rect()
            let shape = try readShape(&r)
            let offset = try r.point()
            let scale = try r.f32()
            let grad = (code == RP.fillShapeGradient
                        || code == RP.strokeShapeGradient)
                ? try r.gradient() : nil
            var t = CGAffineTransform(translationX: CGFloat(offset.x) + 0.5,
                                      y: CGFloat(offset.y) + 0.5)
                .scaledBy(x: CGFloat(scale), y: CGFloat(scale))
            let p = shape.path(transform: &t)
            let filled = (code == RP.fillShape || code == RP.fillShapeGradient)
            emit(path: p, s, filled: filled, gradient: grad, bounds: bounds.cgRect)

        // -- bitmaps -------------------------------------------------------
        case RP.drawBitmap:
            let bitmapRect = try r.rect()
            let viewRect = try r.rect()
            let options = try r.u32()
            if options != 0 {
                delegate?.rendererLog("bitmap options \(options) ignored")
            }
            var bm = try BitmapDecoder.read(&r, minimal: false,
                                          forceOpaque: s.forceOpaque,
                                          palette: palette.isEmpty ? nil : palette,
                                          warn: { [weak self] in
                                              self?.delegate?.rendererLog($0)
                                          })
            stats.bitmapBytes += bm.bgra.count
            BitmapDecoder.premultiply(&bm)
            drawBitmap(bm, srcRect: bitmapRect, dstRect: viewRect, s)

        case RP.drawBitmapRects:
            let options = try r.u32()
            let csRaw = try r.u32()
            let flags = try r.u32()
            if options != 0 {
                delegate?.rendererLog("bitmap options \(options) ignored")
            }
            let cs = ColorSpace(rawValue: csRaw) ?? .rgb32
            let n = try r.i32()
            guard n >= 0, n <= 1 << 16 else { throw WireError("bitmap rects \(n)") }
            for _ in 0..<Int(n) {
                let dst = try r.rect()
                // Minimal bitmaps: colour space came from the enclosing message.
                var bm = try BitmapDecoder.read(&r, minimal: true, colorSpace: cs,
                                              flags: flags,
                                              forceOpaque: s.forceOpaque,
                                              palette: palette.isEmpty
                                                  ? nil : palette,
                                              warn: { [weak self] in
                                                  self?.delegate?.rendererLog($0)
                                              })
                stats.bitmapBytes += bm.bgra.count
                BitmapDecoder.premultiply(&bm)
                // Each sub-bitmap is already sized for its destination.
                let src = BRect(left: 0, top: 0,
                                right: Float(bm.width - 1),
                                bottom: Float(bm.height - 1))
                drawBitmap(bm, srcRect: src, dstRect: dst, s)
            }

        // -- text: these BLOCK app_server, so reply first ------------------
        case RP.drawString:
            let where_ = try r.point()
            let str = try r.string()
            // A trailing `bool hasDelta` (+ optional deltas) follows and is
            // intentionally not read; the framer skips it by declared length.
            let advance = drawString(str, at: where_, s)
            let pen = BPoint(x: where_.x + advance, y: where_.y)
            delegate?.rendererWantsToSend(
                ClientMessage.drawStringResult(token: token, pen: pen))
            stats.drawStringReplies += 1

        case RP.drawStringWithOffsets:
            let str = try r.string()
            // One BPoint per *codepoint* (the server uses UTF8CountChars); the
            // reference client wrongly iterates UTF-16 units (PROTOCOL.md §7.3).
            let scalars = Array(str.unicodeScalars)
            var last = BPoint(x: 0, y: 0)
            var lastScalar: Character = " "
            for scalar in scalars {
                guard r.remaining >= 8 else { break }
                let p = try r.point()
                last = p
                lastScalar = Character(scalar)
                _ = drawString(String(scalar), at: p, s)
            }
            let advance = text.width(String(lastScalar), s.font)
            delegate?.rendererWantsToSend(
                ClientMessage.drawStringResult(
                    token: token, pen: BPoint(x: last.x + advance, y: last.y)))
            stats.drawStringReplies += 1

        case RP.stringWidth:
            let str = try r.string()
            // Reply before doing anything else: app_server is blocked on a 1s
            // timeout right now (PROTOCOL.md §7.1).
            let w = text.width(str, s.font)
            delegate?.rendererWantsToSend(
                ClientMessage.stringWidthResult(token: token, width: w))
            stats.stringWidthReplies += 1

        case RP.readBitmap:
            let bounds = try r.rect()
            let drawCursor = try r.bool()
            sendReadBitmapResult(token: token, bounds: bounds,
                                 drawCursor: drawCursor)

        default:
            note(unhandled: code)
        }
    }

    // MARK: - Painting helpers

    private func invalidate(_ rects: [BRect]) {
        var union = CGRect.null
        for r in rects { union = union.union(r.cgRect) }
        delegate?.rendererDidUpdate(rect: union.isNull ? nil : union)
    }

    private func emit(path: CGPath, _ s: DrawState, filled: Bool,
                     gradient: Gradient?, bounds: CGRect) {
        if let g = gradient {
            if filled {
                fillGradient(path: path, s, gradient: g, bounds: bounds)
            } else {
                strokeGradient(path: path, s, gradient: g, bounds: bounds)
            }
        } else if filled {
            fill(path: path, s, bounds: bounds)
        } else {
            stroke(path: path, s)
        }
    }

    private func fill(path: CGPath, _ s: DrawState, bounds: CGRect) {
        paint(path: path, filled: true, s, dirty: bounds)
    }

    private func stroke(path: CGPath, _ s: DrawState) {
        let grow = CGFloat(max(s.penSize, 1)) + 1
        paint(path: path, filled: false, s,
              dirty: path.boundingBoxOfPath.insetBy(dx: -grow, dy: -grow))
    }

    private func fillGradient(path: CGPath, _ s: DrawState, gradient: Gradient,
                             bounds: CGRect) {
        paint(path: path, filled: true, s, gradient: gradient, dirty: bounds)
    }

    private func strokeGradient(path: CGPath, _ s: DrawState, gradient: Gradient,
                               bounds: CGRect) {
        paint(path: path, filled: false, s, gradient: gradient, dirty: bounds)
    }

    /// Chooses between CoreGraphics and the software blender for one shape.
    ///
    /// CoreGraphics handles the common case — a solid colour or a linear/radial
    /// gradient in `B_OP_COPY`/`B_OP_OVER`/`B_OP_ALPHA` — because it is faster
    /// and gets gradient interpolation for free. Everything else is a per-pixel
    /// Haiku operator (`softBlend`) or a stipple that has to be anchored to the
    /// view origin, so it goes through a coverage mask.
    private func paint(path: CGPath, filled: Bool, _ s: DrawState,
                      gradient: Gradient? = nil, dirty: CGRect) {
        let grow = filled ? 0 : CGFloat(max(s.penSize, 1)) + 1
        let box = path.boundingBoxOfPath.insetBy(dx: -grow, dy: -grow)

        let source: PixelSource
        let soft: Bool
        if let g = gradient {
            let inverse = s.transform.isIdentity
                ? nil : s.compositeTransform.inverted()
            source = GradientSource(g, forceOpaque: s.forceOpaque,
                                    inverse: inverse)
            soft = s.drawingMode.requiresSoftBlend
                || GradientSource.needsSoftware(g)
        } else {
            source = s.pixelSource()
            soft = s.needsSoftBlend
        }

        if soft {
            softPaint(path: path, filled: filled, s, source: source, box: box)
        } else if let g = gradient {
            cgGradient(path: path, filled: filled, s, gradient: g, box: box)
        } else {
            cgSolid(path: path, filled: filled, s)
        }
        delegate?.rendererDidUpdate(rect: dirty)
    }

    /// Rasterises the shape to a coverage mask and runs Haiku's per-pixel
    /// operator over it. Clipping and the view transform are baked into the mask
    /// by `applyGeometry`, so the blend loop needs no clip test.
    private func softPaint(path: CGPath, filled: Bool, _ s: DrawState,
                          source: PixelSource, box: CGRect) {
        guard let mask = MaskRasterizer.rasterize(
            path: path, filled: filled, region: s.screenBounds(of: box),
            canvasWidth: canvas.width, canvasHeight: canvas.height,
            configure: { s.applyGeometry(to: $0) }) else { return }
        softBlend(mask: mask, source: source, mode: s.drawingMode,
                  high: s.highColor, low: s.lowColor,
                  globalAlpha: s.globalAlpha, into: canvas)
    }

    private func cgSolid(path: CGPath, filled: Bool, _ s: DrawState) {
        let ctx = canvas.ctx
        ctx.saveGState()
        s.apply(to: ctx)
        ctx.addPath(path)
        if filled {
            ctx.setFillColor(s.strokeAndFillColor())
            ctx.fillPath()
        } else {
            ctx.setStrokeColor(s.strokeAndFillColor())
            ctx.strokePath()
        }
        ctx.restoreGState()
    }

    private func cgGradient(path: CGPath, filled: Bool, _ s: DrawState,
                           gradient: Gradient, box: CGRect) {
        let ctx = canvas.ctx
        ctx.saveGState()
        s.apply(to: ctx)
        ctx.addPath(path)
        if !filled { ctx.replacePathWithStrokedPath() }
        ctx.clip()
        drawGradient(gradient, s, in: box)
        ctx.restoreGState()
    }

    private func drawGradient(_ g: Gradient, _ s: DrawState, in box: CGRect) {
        let ctx = canvas.ctx
        var comps: [CGFloat] = []
        var locs: [CGFloat] = []
        for stop in g.stops {
            let c = stop.color
            let a: CGFloat = s.forceOpaque ? 1 : CGFloat(c.a) / 255
            comps.append(contentsOf: [CGFloat(c.r) / 255, CGFloat(c.g) / 255,
                                      CGFloat(c.b) / 255, a])
            // Offsets arrive as 0-255, not 0-1 (PROTOCOL.md §6.4).
            locs.append(min(max(CGFloat(stop.offset) / 255.0, 0), 1))
        }
        guard locs.count >= 2,
              let cg = CGGradient(colorSpace: CGColorSpaceCreateDeviceRGB(),
                                  colorComponents: comps, locations: locs,
                                  count: locs.count) else {
            // Fall back to a solid fill so geometry is at least visible.
            ctx.setFillColor(g.stops.first?.color.cgColor(forceOpaque: s.forceOpaque)
                             ?? CGColor(gray: 0, alpha: 1))
            ctx.fill(box)
            return
        }
        let opts: CGGradientDrawingOptions = [.drawsBeforeStartLocation,
                                              .drawsAfterEndLocation]
        switch g.kind {
        case .linear:
            ctx.drawLinearGradient(cg, start: g.start.cgPoint,
                                   end: g.end.cgPoint, options: opts)
        case .radial:
            ctx.drawRadialGradient(cg, startCenter: g.center.cgPoint,
                                   startRadius: 0, endCenter: g.center.cgPoint,
                                   endRadius: CGFloat(g.radius), options: opts)
        case .radialFocus:
            // Deliberately centred, not focused. app_server default-constructs
            // agg::gradient_radial_focus and never passes the focal point
            // (Painter.cpp:2101), so on a real Haiku screen this is identical to
            // B_GRADIENT_RADIAL. Matching the server beats "fixing" it, since
            // the client's job is to look like the desktop it mirrors.
            ctx.drawRadialGradient(cg, startCenter: g.center.cgPoint,
                                   startRadius: 0, endCenter: g.center.cgPoint,
                                   endRadius: CGFloat(g.radius), options: opts)
        case .diamond, .conic, .none:
            // Diamond and conic are evaluated per pixel by GradientSource and
            // never reach this path; .none has no geometry to describe. Fall
            // back to a flat fill rather than leaving the shape blank.
            ctx.setFillColor(g.stops.first?.color
                             .cgColor(forceOpaque: s.forceOpaque)
                             ?? CGColor(gray: 0, alpha: 1))
            ctx.fill(box)
        }
    }

    private func drawBitmap(_ bm: DecodedBitmap, srcRect: BRect, dstRect: BRect,
                           _ s: DrawState) {
        guard let full = BitmapDecoder.makeImage(bm) else { return }
        var img = full
        // Crop to the requested source sub-rect if it is not the whole bitmap.
        let sx = Int(srcRect.left.rounded()), sy = Int(srcRect.top.rounded())
        let sw = Int(srcRect.width.rounded()), sh = Int(srcRect.height.rounded())
        if sx != 0 || sy != 0 || sw != bm.width || sh != bm.height {
            let crop = CGRect(x: sx, y: sy, width: sw, height: sh)
                .intersection(CGRect(x: 0, y: 0, width: bm.width, height: bm.height))
            if !crop.isEmpty, let c = full.cropping(to: crop) { img = c }
        }
        let ctx = canvas.ctx
        ctx.saveGState()
        s.apply(to: ctx)
        let dst = dstRect.cgRect
        // The context is y-flipped, so flip again locally: otherwise every
        // bitmap lands upside down.
        ctx.translateBy(x: dst.minX, y: dst.minY)
        ctx.translateBy(x: 0, y: dst.height)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: dst.width, height: dst.height))
        ctx.restoreGState()
        delegate?.rendererDidUpdate(rect: dst)
    }

    @discardableResult
    private func drawString(_ str: String, at p: BPoint, _ s: DrawState) -> Float {
        let ctx = canvas.ctx
        ctx.saveGState()
        s.apply(to: ctx)
        // Text always paints in the high colour, not the pattern.
        let color = s.highColor.cgColor(forceOpaque: s.forceOpaque)
        let advance = text.draw(str, at: p, font: s.font, color: color, in: ctx)
        ctx.restoreGState()
        let h = CGFloat(s.font.size) * 2
        delegate?.rendererDidUpdate(
            rect: CGRect(x: CGFloat(p.x) - 2, y: CGFloat(p.y) - h,
                         width: CGFloat(advance) + 4, height: h + 4))
        return advance
    }

    private func sendReadBitmapResult(token: Int32, bounds: BRect,
                                     drawCursor: Bool = false) {
        // The cursor lives outside the canvas so it cannot leave a trail, so a
        // request that wants it has to have it composited in on the way out.
        var cursor: (image: CGImage, origin: CGPoint)?
        if drawCursor, cursorVisible, let image = cursorImage {
            cursor = (image, CGPoint(x: cursorPosition.x - cursorHotspot.x,
                                     y: cursorPosition.y - cursorHotspot.y))
        }
        guard let read = canvas.readRegionRGB24(bounds, cursor: cursor) else {
            delegate?.rendererLog("readBitmap: could not read region")
            return
        }
        var w = WireWriter(RP.readBitmapResult)
        w.append(i32: token)
        w.append(i32: Int32(read.width))
        w.append(i32: Int32(read.height))
        w.append(i32: Int32(read.bytesPerRow))
        w.append(u32: ColorSpace.rgb24.rawValue)
        w.append(u32: 0)
        w.append(u32: UInt32(read.bits.count))
        w.append(raw: read.bits)
        delegate?.rendererWantsToSend(w.finish())
    }

    private func drawArc(_ rect: BRect, angle: Float, span: Float, filled: Bool,
                        _ s: DrawState, gradient: Gradient?) {
        let box = rect.exactRect
        let cx = box.midX, cy = box.midY
        let rx = box.width / 2, ry = box.height / 2
        // Haiku measures degrees counter-clockwise from the positive x axis. The
        // context is y-flipped, so that reads as clockwise here.
        let start = -Double(angle) * .pi / 180
        let end = -Double(angle + span) * .pi / 180
        let p = CGMutablePath()
        let t = CGAffineTransform(translationX: cx, y: cy)
            .scaledBy(x: rx == 0 ? 1 : rx, y: ry == 0 ? 1 : ry)
        if filled { p.move(to: .zero, transform: t) }
        p.addArc(center: .zero, radius: 1, startAngle: start, endAngle: end,
                 clockwise: span > 0, transform: t)
        if filled { p.closeSubpath() }
        emit(path: p, s, filled: filled, gradient: gradient, bounds: rect.cgRect)
    }

    // MARK: - Shapes

    struct Shape {
        var ops: [UInt32] = []
        var points: [BPoint] = []

        /// Op flags are a bitmask and more than one can be set on a single word;
        /// the reference client tests them in this fixed order and that ordering
        /// is part of the contract (PROTOCOL.md §5.3).
        static let moveTo: UInt32 = 0x8000_0000
        static let close: UInt32 = 0x4000_0000
        static let bezierTo: UInt32 = 0x2000_0000
        static let lineTo: UInt32 = 0x1000_0000
        static let smallArcCCW: UInt32 = 0x0800_0000
        static let smallArcCW: UInt32 = 0x0400_0000
        static let largeArcCCW: UInt32 = 0x0200_0000
        static let largeArcCW: UInt32 = 0x0100_0000

        func path(transform t: inout CGAffineTransform) -> CGPath {
            let p = CGMutablePath()
            var i = 0
            // The arc ops need the current point in *untransformed* shape space,
            // so track it here rather than reading p.currentPoint.
            var current = CGPoint.zero
            var started = false
            for word in ops {
                let op = word & 0xff00_0000
                let count = Int(word & 0x00ff_ffff)
                if op & Self.moveTo != 0, i < points.count {
                    current = points[i].cgPoint
                    p.move(to: current, transform: t)
                    started = true
                    i += 1
                }
                if op & Self.lineTo != 0 {
                    for _ in 0..<count where i < points.count {
                        current = points[i].cgPoint
                        p.addLine(to: current, transform: t)
                        started = true
                        i += 1
                    }
                }
                if op & Self.bezierTo != 0 {
                    for _ in 0..<(count / 3) where i + 2 < points.count {
                        p.addCurve(to: points[i + 2].cgPoint,
                                   control1: points[i].cgPoint,
                                   control2: points[i + 1].cgPoint,
                                   transform: t)
                        current = points[i + 2].cgPoint
                        started = true
                        i += 3
                    }
                }
                let arcMask = Self.smallArcCCW | Self.smallArcCW
                    | Self.largeArcCCW | Self.largeArcCW
                if op & arcMask != 0 {
                    // Three points per arc: (rx, ry), (angle, unused), end
                    // (BShape::ArcTo, Shape.cpp:479; ServerPicture.cpp:158).
                    let largeArc = op & (Self.largeArcCW | Self.largeArcCCW) != 0
                    // Painter.cpp:1838 passes the CW flags as AGG's sweep flag.
                    let sweep = op & (Self.smallArcCW | Self.largeArcCW) != 0
                    for _ in 0..<(count / 3) where i + 2 < points.count {
                        if !started {
                            p.move(to: current, transform: t)
                            started = true
                        }
                        let end = points[i + 2].cgPoint
                        Self.appendArc(
                            to: p, from: current, to: end,
                            rx: CGFloat(points[i].x), ry: CGFloat(points[i].y),
                            // The angle is in RADIANS: it passes unconverted
                            // from BShape::ArcTo into agg::arc_to, which takes
                            // radians. Nothing anywhere treats it as degrees.
                            angle: CGFloat(points[i + 1].x),
                            largeArc: largeArc, sweep: sweep, transform: &t)
                        current = end
                        i += 3
                    }
                }
                if op & Self.close != 0 { p.closeSubpath() }
            }
            return p
        }

        /// Appends one SVG-style elliptical arc as cubic Béziers.
        ///
        /// This is the endpoint-to-centre parameterisation from the SVG spec
        /// (appendix F.6.5), which is what `agg::path_storage::arc_to`
        /// implements and what Haiku's `Painter` feeds (Painter.cpp:1833).
        /// CoreGraphics has no elliptical-arc primitive, so the conversion has to
        /// be done here.
        ///
        /// The arc is computed in shape-local space and only its control points
        /// are transformed. That is exact because the shape transform is a
        /// uniform scale plus a translation — Haiku applies one `viewScale` to
        /// both radii — so scaling commutes with the arc construction.
        static func appendArc(to p: CGMutablePath, from start: CGPoint,
                             to end: CGPoint, rx rxIn: CGFloat, ry ryIn: CGFloat,
                             angle: CGFloat, largeArc: Bool, sweep: Bool,
                             transform t: inout CGAffineTransform) {
            // Per the SVG spec, a zero-length arc is dropped and a zero radius
            // degenerates to a straight line.
            if start == end { return }
            var rx = abs(rxIn), ry = abs(ryIn)
            guard rx > 0, ry > 0 else {
                p.addLine(to: end, transform: t)
                return
            }

            let cosA = cos(angle), sinA = sin(angle)
            let dx2 = (start.x - end.x) / 2, dy2 = (start.y - end.y) / 2
            let x1p = cosA * dx2 + sinA * dy2
            let y1p = -sinA * dx2 + cosA * dy2

            // Scale the radii up if they are too small to span the endpoints.
            let lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
            if lambda > 1 {
                let s = lambda.squareRoot()
                rx *= s
                ry *= s
            }

            let rx2 = rx * rx, ry2 = ry * ry
            let x1p2 = x1p * x1p, y1p2 = y1p * y1p
            let den = rx2 * y1p2 + ry2 * x1p2
            let num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2
            var coef = den > 0 ? (Swift.max(0, num) / den).squareRoot() : 0
            if largeArc == sweep { coef = -coef }
            let cxp = coef * (rx * y1p / ry)
            let cyp = coef * -(ry * x1p / rx)
            let cx = cosA * cxp - sinA * cyp + (start.x + end.x) / 2
            let cy = sinA * cxp + cosA * cyp + (start.y + end.y) / 2

            let ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry
            let vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry
            let theta1 = atan2(uy, ux)
            let nu = (ux * ux + uy * uy).squareRoot()
            let nv = (vx * vx + vy * vy).squareRoot()
            var dot = (ux * vx + uy * vy) / (nu * nv)
            dot = Swift.max(-1, Swift.min(1, dot))
            var sweepAngle = acos(dot)
            if ux * vy - uy * vx < 0 { sweepAngle = -sweepAngle }
            if !sweep && sweepAngle > 0 { sweepAngle -= 2 * .pi }
            if sweep && sweepAngle < 0 { sweepAngle += 2 * .pi }

            // A cubic Bézier approximates at most a quarter turn well.
            let segments = Swift.max(1, Int(ceil(abs(sweepAngle) / (.pi / 2))))
            let delta = sweepAngle / CGFloat(segments)
            let alpha = (4.0 / 3.0) * tan(delta / 4)

            /// Point on the ellipse at parameter `a`, and the derivative there.
            func evaluate(_ a: CGFloat) -> (CGPoint, CGPoint) {
                let ca = cos(a), sa = sin(a)
                let px = cx + rx * cosA * ca - ry * sinA * sa
                let py = cy + rx * sinA * ca + ry * cosA * sa
                let dx = -rx * cosA * sa - ry * sinA * ca
                let dy = -rx * sinA * sa + ry * cosA * ca
                return (CGPoint(x: px, y: py), CGPoint(x: dx, y: dy))
            }

            var theta = theta1
            for segment in 0..<segments {
                let next = theta + delta
                let (from, dFrom) = evaluate(theta)
                let (rawTo, dTo) = evaluate(next)
                // Land on the exact endpoint rather than accumulating error.
                let to = segment == segments - 1 ? end : rawTo
                let c1 = CGPoint(x: from.x + alpha * dFrom.x,
                                 y: from.y + alpha * dFrom.y)
                let c2 = CGPoint(x: to.x - alpha * dTo.x,
                                 y: to.y - alpha * dTo.y)
                p.addCurve(to: to, control1: c1, control2: c2, transform: t)
                theta = next
            }
        }
    }

    private func readShape(_ r: inout WireReader) throws -> Shape {
        var s = Shape()
        let opCount = try r.i32()
        guard opCount >= 0, opCount <= 1 << 20 else {
            throw WireError("shape opCount \(opCount)")
        }
        s.ops = try (0..<Int(opCount)).map { _ in try r.u32() }
        let pointCount = try r.i32()
        guard pointCount >= 0, pointCount <= 1 << 20 else {
            throw WireError("shape pointCount \(pointCount)")
        }
        s.points = try (0..<Int(pointCount)).map { _ in try r.point() }
        return s
    }

    // MARK: - Enum mapping

    static func capMode(_ v: UInt32) -> CGLineCap {
        // B_ROUND_CAP = B_ROUND_JOIN = 0, B_BUTT_CAP = B_BUTT_JOIN = 3,
        // B_SQUARE_CAP = B_SQUARE_JOIN = 4.
        switch v {
        case 0: return .round
        case 4: return .square
        default: return .butt
        }
    }

    static func joinMode(_ v: UInt32) -> CGLineJoin {
        switch v {
        case 0: return .round
        case 1: return .miter
        case 2: return .bevel
        default: return .miter
        }
    }
}
