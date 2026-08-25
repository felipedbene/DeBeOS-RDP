// make-icon -- generate the app icon as a 1024x1024 PNG.
//
// Drawn in code rather than shipped as a binary blob so it stays diffable and can
// be tweaked without a design tool. build.sh renders this, fans it out to the
// sizes macOS wants with `sips`, and packs an .icns with `iconutil`.
//
// The motif is the thing being remoted: Haiku's desktop blue behind one of its
// yellow window tabs. The colours are the ones app_server actually puts on the
// wire — 51,102,152 for the desktop and 255,203,0 for the tab — so the icon and a
// live session are the same blue.
//
//   swiftc -O -o /tmp/make-icon tools/make-icon.swift && /tmp/make-icon out.png

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let side = 1024
let output = CommandLine.arguments.count > 1
    ? CommandLine.arguments[1] : "/tmp/icon.png"

let space = CGColorSpaceCreateDeviceRGB()

func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: Double = 1) -> CGColor {
    CGColor(colorSpace: space,
            components: [CGFloat(r) / 255, CGFloat(g) / 255, CGFloat(b) / 255,
                         CGFloat(a)])!
}

let info = CGImageAlphaInfo.premultipliedFirst.rawValue
    | CGBitmapInfo.byteOrder32Little.rawValue
guard let ctx = CGContext(data: nil, width: side, height: side,
                         bitsPerComponent: 8, bytesPerRow: side * 4,
                         space: space, bitmapInfo: info) else {
    FileHandle.standardError.write(Data("cannot create context\n".utf8))
    exit(1)
}
ctx.setShouldAntialias(true)

let s = CGFloat(side)
// macOS icons sit inside a margin rather than filling the tile.
let inset = s * 0.06
let plate = CGRect(x: inset, y: inset, width: s - inset * 2, height: s - inset * 2)
let corner = plate.width * 0.2237   // the squircle-ish radius Apple uses

// -- the plate: Haiku's desktop blue, lit from above ------------------------
let plateShape = CGPath(roundedRect: plate, cornerWidth: corner,
                        cornerHeight: corner, transform: nil)
ctx.saveGState()
ctx.addPath(plateShape)
ctx.clip()
if let grad = CGGradient(colorsSpace: space,
                         colors: [rgb(72, 132, 188), rgb(33, 68, 106)] as CFArray,
                         locations: [0, 1]) {
    ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: plate.maxY),
                           end: CGPoint(x: 0, y: plate.minY), options: [])
}
ctx.restoreGState()

// -- the window -------------------------------------------------------------
// Proportions follow a Haiku window: a narrow tab sitting on a light body,
// both with the hard 1px-style border Haiku draws.
let win = CGRect(x: s * 0.20, y: s * 0.245, width: s * 0.60, height: s * 0.40)
let tabH = s * 0.085
let tab = CGRect(x: win.minX, y: win.maxY, width: win.width * 0.52, height: tabH)
let border = rgb(20, 26, 33)
let lineW = s * 0.011

ctx.setShadow(offset: CGSize(width: 0, height: -s * 0.012), blur: s * 0.05,
              color: rgb(0, 0, 0, 0.35))

ctx.setFillColor(rgb(222, 222, 222))
ctx.fill(win)
ctx.setFillColor(rgb(255, 203, 0))
ctx.fill(tab)
ctx.setShadow(offset: .zero, blur: 0, color: nil)

ctx.setStrokeColor(border)
ctx.setLineWidth(lineW)
ctx.stroke(win.insetBy(dx: lineW / 2, dy: lineW / 2))
ctx.stroke(tab.insetBy(dx: lineW / 2, dy: lineW / 2))

// -- content lines, standing in for the drawing commands we render ----------
// Deliberately three plain rules: at 16px the icon has to read as "a window",
// and detail here turns to mush.
ctx.setFillColor(rgb(120, 130, 140))
let lineH = s * 0.022
for i in 0..<3 {
    let w = win.width * [0.62, 0.46, 0.54][i]
    let y = win.maxY - s * 0.085 - CGFloat(i) * s * 0.055
    ctx.fill(CGRect(x: win.minX + s * 0.045, y: y, width: w, height: lineH))
}

// -- the link: two nodes joined, for the tunnel ----------------------------
let dotR = s * 0.028
let linkY = win.minY + s * 0.075
let leftDot = CGPoint(x: win.minX + s * 0.055 + dotR, y: linkY)
let rightDot = CGPoint(x: win.maxX - s * 0.055 - dotR, y: linkY)
ctx.setStrokeColor(rgb(52, 120, 90))
ctx.setLineWidth(s * 0.016)
ctx.setLineCap(.round)
ctx.beginPath()
ctx.move(to: leftDot)
ctx.addLine(to: rightDot)
ctx.strokePath()
ctx.setFillColor(rgb(52, 160, 100))
for p in [leftDot, rightDot] {
    ctx.fillEllipse(in: CGRect(x: p.x - dotR, y: p.y - dotR,
                               width: dotR * 2, height: dotR * 2))
}

// -- a top highlight, so the plate does not look flat ----------------------
ctx.saveGState()
ctx.addPath(plateShape)
ctx.clip()
ctx.setStrokeColor(rgb(255, 255, 255, 0.22))
ctx.setLineWidth(s * 0.008)
ctx.addPath(CGPath(roundedRect: plate.insetBy(dx: s * 0.012, dy: s * 0.012),
                   cornerWidth: corner, cornerHeight: corner, transform: nil))
ctx.strokePath()
ctx.restoreGState()

// -- write ------------------------------------------------------------------
guard let image = ctx.makeImage() else {
    FileHandle.standardError.write(Data("cannot snapshot\n".utf8))
    exit(1)
}
let url = URL(fileURLWithPath: output)
guard let dest = CGImageDestinationCreateWithURL(
    url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
    FileHandle.standardError.write(Data("cannot open \(output)\n".utf8))
    exit(1)
}
CGImageDestinationAddImage(dest, image, nil)
guard CGImageDestinationFinalize(dest) else {
    FileHandle.standardError.write(Data("cannot write \(output)\n".utf8))
    exit(1)
}
print("wrote \(output) (\(side)x\(side))")
