// The app's icon, drawn procedurally so nothing of the game's own art is used:
// a glowing amber sun over a misty tropical island and prehistoric fern fronds.
//
//   swift app/icon.swift OUT.iconset && iconutil -c icns OUT.iconset
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"

func draw(_ ctx: CGContext, _ s: CGFloat) {
    // The rounded square of a macOS icon: 824 of 1024, corner 185.
    let inset = s * 100 / 1024, side = s - 2 * inset
    let tile = CGPath(roundedRect: CGRect(x: inset, y: inset, width: side, height: side),
                      cornerWidth: side * 0.225, cornerHeight: side * 0.225, transform: nil)
    ctx.saveGState()
    ctx.addPath(tile)
    ctx.clip()

    // Deep jungle twilight sky gradient.
    let sky = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                         colors: [CGColor(red: 0.96, green: 0.56, blue: 0.14, alpha: 1),
                                  CGColor(red: 0.62, green: 0.22, blue: 0.12, alpha: 1),
                                  CGColor(red: 0.08, green: 0.18, blue: 0.13, alpha: 1)] as CFArray,
                         locations: [0, 0.45, 1])!
    ctx.drawLinearGradient(sky, start: CGPoint(x: 0, y: inset), end: CGPoint(x: 0, y: s - inset), options: [])

    // Glowing amber orb.
    let sunCenter = CGPoint(x: s * 0.5, y: inset + side * 0.54)
    let sunR = side * 0.28
    let glow = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: [CGColor(red: 1.0, green: 0.88, blue: 0.36, alpha: 1),
                                   CGColor(red: 0.95, green: 0.52, blue: 0.10, alpha: 0.95),
                                   CGColor(red: 0.85, green: 0.30, blue: 0.05, alpha: 0)] as CFArray,
                          locations: [0, 0.65, 1])!
    ctx.drawRadialGradient(glow, startCenter: sunCenter, startRadius: 0,
                           endCenter: sunCenter, endRadius: sunR * 1.45, options: [])

    // Distant volcanic island silhouette.
    ctx.setFillColor(CGColor(red: 0.07, green: 0.17, blue: 0.10, alpha: 1))
    ctx.beginPath()
    ctx.move(to: CGPoint(x: 0, y: inset))
    ctx.addLine(to: CGPoint(x: 0, y: inset + side * 0.30))
    ctx.addCurve(to: CGPoint(x: s * 0.36, y: inset + side * 0.44),
                 control1: CGPoint(x: s * 0.12, y: inset + side * 0.38),
                 control2: CGPoint(x: s * 0.24, y: inset + side * 0.48))
    ctx.addCurve(to: CGPoint(x: s * 0.68, y: inset + side * 0.36),
                 control1: CGPoint(x: s * 0.48, y: inset + side * 0.40),
                 control2: CGPoint(x: s * 0.56, y: inset + side * 0.32))
    ctx.addCurve(to: CGPoint(x: s, y: inset + side * 0.42),
                 control1: CGPoint(x: s * 0.80, y: inset + side * 0.42),
                 control2: CGPoint(x: s * 0.90, y: inset + side * 0.46))
    ctx.addLine(to: CGPoint(x: s, y: inset))
    ctx.closePath()
    ctx.fillPath()

    // Foreground jungle ridge.
    ctx.setFillColor(CGColor(red: 0.04, green: 0.11, blue: 0.06, alpha: 1))
    ctx.fill(CGRect(x: 0, y: 0, width: s, height: inset + side * 0.22))

    // Two stylized palm silhouettes framing the amber sun.
    func palm(base: CGPoint, top: CGPoint, scale: CGFloat) {
        ctx.setStrokeColor(CGColor(red: 0.03, green: 0.09, blue: 0.05, alpha: 1))
        ctx.setLineCap(.round)
        ctx.setLineWidth(side * 0.032 * scale)
        ctx.beginPath()
        ctx.move(to: base)
        let ctrl = CGPoint(x: (base.x + top.x) * 0.5 + (base.x < s * 0.5 ? -side * 0.04 : side * 0.04),
                           y: (base.y + top.y) * 0.55)
        ctx.addQuadCurve(to: top, control: ctrl)
        ctx.strokePath()
        ctx.setLineWidth(side * 0.018 * scale)
        let angles: [CGFloat] = [-2.5, -1.9, -1.2, -0.4, 0.3, 1.0, 2.2]
        for a in angles {
            let len = side * 0.16 * scale
            let end = CGPoint(x: top.x + cos(a) * len, y: top.y + sin(a) * len - side * 0.03 * scale)
            let mid = CGPoint(x: top.x + cos(a) * len * 0.55, y: top.y + sin(a) * len * 0.55 + side * 0.04 * scale)
            ctx.beginPath()
            ctx.move(to: top)
            ctx.addQuadCurve(to: end, control: mid)
            ctx.strokePath()
        }
    }
    palm(base: CGPoint(x: inset + side * 0.18, y: inset + side * 0.14),
         top: CGPoint(x: inset + side * 0.25, y: inset + side * 0.54), scale: 1.0)
    palm(base: CGPoint(x: inset + side * 0.84, y: inset + side * 0.14),
         top: CGPoint(x: inset + side * 0.76, y: inset + side * 0.48), scale: 0.85)

    // Subtle amber inner border.
    ctx.setStrokeColor(CGColor(red: 0.96, green: 0.70, blue: 0.22, alpha: 0.55))
    ctx.setLineWidth(side * 0.022)
    ctx.addPath(tile)
    ctx.strokePath()
    ctx.restoreGState()
}

func png(_ px: Int, _ name: String) throws {
    let cs = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    draw(ctx, CGFloat(px))
    let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
    try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "\(out)/\(name)"))
}

try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for pt in [16, 32, 128, 256, 512] {
    try png(pt, "icon_\(pt)x\(pt).png")
    try png(pt * 2, "icon_\(pt)x\(pt)@2x.png")
}
