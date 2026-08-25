import CoreGraphics
import Foundation

/// Decodes Haiku bitmap records into BGRA, which is what both Haiku's
/// B_RGB32/B_RGBA32 and CoreGraphics' `.byteOrder32Little` natively use — so the
/// common 32-bpp cases are a straight copy with no per-pixel conversion
/// (PROTOCOL.md §6.3). The reference JS client has to swap R and B only because
/// canvas ImageData is RGBA.
public enum BitmapDecoder {

    public struct Header {
        public var width: Int
        public var height: Int
        public var bytesPerRow: Int
        public var colorSpace: ColorSpace
        public var flags: UInt32
        public var bitsLength: Int
    }

    /// Reads a bitmap record. `minimal` records omit colorSpace/flags, which are
    /// supplied by the enclosing message instead (RP_DRAW_BITMAP_RECTS).
    public static func readHeader(_ r: inout WireReader, minimal: Bool,
                                 colorSpace: ColorSpace = .rgb32,
                                 flags: UInt32 = 0) throws -> Header {
        let w = Int(try r.i32())
        let h = Int(try r.i32())
        let bpr = Int(try r.i32())
        var cs = colorSpace
        var fl = flags
        if !minimal {
            let rawCS = try r.u32()
            cs = ColorSpace(rawValue: rawCS) ?? .none
            fl = try r.u32()
        }
        let bitsLength = Int(try r.u32())
        guard w >= 0, h >= 0, bpr >= 0, bitsLength >= 0,
              w <= 1 << 16, h <= 1 << 16, bitsLength <= 1 << 28 else {
            throw WireError("implausible bitmap \(w)x\(h) bpr=\(bpr) bits=\(bitsLength)")
        }
        return Header(width: w, height: h, bytesPerRow: bpr, colorSpace: cs,
                      flags: fl, bitsLength: bitsLength)
    }

    /// Reads header + pixels and converts to tightly packed BGRA.
    ///
    /// `forceOpaque` corresponds to the reference client's `unsetAlpha`, set
    /// under B_OP_COPY: every pixel becomes fully opaque.
    public static func read(_ r: inout WireReader, minimal: Bool,
                           colorSpace: ColorSpace = .rgb32, flags: UInt32 = 0,
                           forceOpaque: Bool,
                           palette: [UInt32]?,
                           warn: ((String) -> Void)? = nil) throws -> DecodedBitmap {
        let h = try readHeader(&r, minimal: minimal, colorSpace: colorSpace,
                              flags: flags)
        let raw = try r.raw(h.bitsLength)
        return convert(header: h, raw: raw, forceOpaque: forceOpaque,
                       palette: palette, warn: warn)
    }

    public static func convert(header h: Header, raw: [UInt8],
                              forceOpaque: Bool,
                              palette: [UInt32]?,
                              warn: ((String) -> Void)? = nil) -> DecodedBitmap {
        let w = h.width, ht = h.height
        var out = [UInt8](repeating: 0, count: max(0, w * ht * 4))
        guard w > 0, ht > 0 else {
            return DecodedBitmap(width: max(0, w), height: max(0, ht), bgra: out)
        }

        /// bytesPerRow can exceed the pixel width; rows are padded and the
        /// padding must be skipped (PROTOCOL.md §6.1).
        func rowStart(_ y: Int) -> Int { y * h.bytesPerRow }

        switch h.colorSpace {
        case .rgb32, .rgba32:
            let hasAlpha = (h.colorSpace == .rgba32)
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src + 3 < raw.count else { break }
                    let b = raw[src], g = raw[src + 1], rr = raw[src + 2]
                    var a = raw[src + 3]
                    if !hasAlpha { a = 255 }
                    if forceOpaque { a = 255 }
                    // B_RGB32's transparent magic means "see through here".
                    if !forceOpaque && h.colorSpace == .rgb32 {
                        let packed = UInt32(raw[src]) | UInt32(raw[src + 1]) << 8
                            | UInt32(raw[src + 2]) << 16 | UInt32(raw[src + 3]) << 24
                        if packed == RGBColor.transparentMagicRGBA32 { a = 0 }
                    }
                    out[dst] = b; out[dst + 1] = g; out[dst + 2] = rr
                    out[dst + 3] = a
                    src += 4; dst += 4
                }
            }

        case .rgb24:
            // BGR in memory, 3 bytes per pixel.
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src + 2 < raw.count else { break }
                    out[dst] = raw[src]
                    out[dst + 1] = raw[src + 1]
                    out[dst + 2] = raw[src + 2]
                    out[dst + 3] = 255
                    src += 3; dst += 4
                }
            }

        case .rgb16:
            // BGR 5:6:5, little-endian 16-bit units.
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src + 1 < raw.count else { break }
                    let v = UInt16(raw[src]) | UInt16(raw[src + 1]) << 8
                    out[dst] = UInt8((v & 0x001f) << 3)       // blue
                    out[dst + 1] = UInt8((v & 0x07e0) >> 3)   // green
                    out[dst + 2] = UInt8((v & 0xf800) >> 8)   // red
                    out[dst + 3] = 255
                    src += 2; dst += 4
                }
            }

        case .rgb15, .rgba15:
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src + 1 < raw.count else { break }
                    let v = UInt16(raw[src]) | UInt16(raw[src + 1]) << 8
                    out[dst] = UInt8((v & 0x001f) << 3)
                    out[dst + 1] = UInt8((v & 0x03e0) >> 2)
                    out[dst + 2] = UInt8((v & 0x7c00) >> 7)
                    let opaque = forceOpaque || h.colorSpace == .rgb15
                        || (v & 0x8000) != 0
                    out[dst + 3] = opaque ? 255 : 0
                    src += 2; dst += 4
                }
            }

        case .gray8:
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src < raw.count else { break }
                    let v = raw[src]
                    out[dst] = v; out[dst + 1] = v; out[dst + 2] = v
                    out[dst + 3] = 255
                    src += 1; dst += 4
                }
            }

        case .gray1:
            // 1 bpp, LSB-first within each byte -- note this is the opposite
            // convention from `pattern`, which is MSB-first (PROTOCOL.md §6.3).
            for y in 0..<ht {
                let rs = rowStart(y)
                var dst = y * w * 4
                for x in 0..<w {
                    let byteIndex = rs + x / 8
                    guard byteIndex < raw.count else { break }
                    let bit = (raw[byteIndex] >> UInt8(x % 8)) & 1
                    let v: UInt8 = bit != 0 ? 255 : 0
                    out[dst] = v; out[dst + 1] = v; out[dst + 2] = v
                    out[dst + 3] = 255
                    dst += 4
                }
            }

        case .cmap8:
            // Palette entries arrive as rgb_color and are packed by the
            // reference client as r | g<<8 | b<<16 | a<<24.
            if palette == nil || palette?.isEmpty == true {
                // The protocol gives no ordering guarantee between
                // RP_GET_SYSTEM_PALETTE_RESULT and drawing traffic, so a
                // paletted bitmap can genuinely arrive first. Say so rather than
                // silently painting a black rectangle.
                warn?("B_CMAP8 bitmap arrived before the system palette; "
                      + "it will render black")
            }
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src < raw.count else { break }
                    let idx = Int(raw[src])
                    let packed = (palette?.indices.contains(idx) ?? false)
                        ? palette![idx] : 0xff00_0000
                    out[dst] = UInt8((packed >> 16) & 0xff)     // blue
                    out[dst + 1] = UInt8((packed >> 8) & 0xff)  // green
                    out[dst + 2] = UInt8(packed & 0xff)         // red
                    out[dst + 3] = forceOpaque ? 255 : UInt8((packed >> 24) & 0xff)
                    src += 1; dst += 4
                }
            }

        case .rgb32big, .rgba32big:
            // ARGB byte order. Not implemented by the reference client; handled
            // here because it is cheap and avoids a silent black bitmap.
            let hasAlpha = (h.colorSpace == .rgba32big)
            for y in 0..<ht {
                var src = rowStart(y)
                var dst = y * w * 4
                for _ in 0..<w {
                    guard src + 3 < raw.count else { break }
                    let a = hasAlpha ? raw[src] : 255
                    out[dst] = raw[src + 3]        // blue
                    out[dst + 1] = raw[src + 2]    // green
                    out[dst + 2] = raw[src + 1]    // red
                    out[dst + 3] = forceOpaque ? 255 : a
                    src += 4; dst += 4
                }
            }

        case .none, .rgb24big, .rgb16big, .rgb15big, .rgba15big:
            // Leave transparent rather than paint garbage. The reference client
            // does not implement these either.
            warn?("colour space \(h.colorSpace) not implemented; "
                  + "\(w)x\(ht) bitmap left blank")
        }

        return DecodedBitmap(width: w, height: ht, bgra: out)
    }

    /// Wraps BGRA bytes in a CGImage with no pixel conversion.
    public static func makeImage(_ bm: DecodedBitmap) -> CGImage? {
        guard bm.width > 0, bm.height > 0, !bm.bgra.isEmpty else { return nil }
        var bytes = bm.bgra
        let bytesPerRow = bm.width * 4
        guard let provider = CGDataProvider(
            data: Data(bytes: &bytes, count: bytes.count) as CFData) else {
            return nil
        }
        // premultipliedFirst + byteOrder32Little == BGRA in memory.
        let info: CGBitmapInfo = [
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue),
            .byteOrder32Little,
        ]
        return CGImage(width: bm.width, height: bm.height,
                       bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: bytesPerRow,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: info, provider: provider,
                       decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }

    /// Premultiplies in place. CoreGraphics' premultipliedFirst expects colour
    /// channels already scaled by alpha; Haiku ships straight (unpremultiplied)
    /// alpha, so translucent bitmaps look too bright without this.
    public static func premultiply(_ bm: inout DecodedBitmap) {
        var i = 0
        while i + 3 < bm.bgra.count {
            let a = UInt32(bm.bgra[i + 3])
            if a != 255 {
                bm.bgra[i] = UInt8(UInt32(bm.bgra[i]) * a / 255)
                bm.bgra[i + 1] = UInt8(UInt32(bm.bgra[i + 1]) * a / 255)
                bm.bgra[i + 2] = UInt8(UInt32(bm.bgra[i + 2]) * a / 255)
            }
            i += 4
        }
    }
}
