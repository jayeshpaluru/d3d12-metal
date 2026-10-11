// SPDX-License-Identifier: LGPL-2.1-or-later
// Draws the app icon into an .iconset folder: swift tools/make-icon.swift <out.iconset>; then iconutil -c icns.
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)

func render(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)
    let inset = s * 0.08
    let rect = NSRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let path = NSBezierPath(roundedRect: rect, xRadius: s * 0.22, yRadius: s * 0.22)
    NSGradient(colors: [NSColor(calibratedRed: 0.36, green: 0.30, blue: 0.95, alpha: 1), NSColor(calibratedRed: 0.10, green: 0.62, blue: 0.95, alpha: 1)])!
        .draw(in: path, angle: -60)
    // A stylised triangle (the first thing every renderer draws) inside a ring.
    NSColor.white.withAlphaComponent(0.95).setFill()
    let tri = NSBezierPath()
    tri.move(to: NSPoint(x: s * 0.5, y: s * 0.74))
    tri.line(to: NSPoint(x: s * 0.28, y: s * 0.32))
    tri.line(to: NSPoint(x: s * 0.72, y: s * 0.32))
    tri.close()
    tri.fill()
    NSColor.white.withAlphaComponent(0.55).setStroke()
    let ring = NSBezierPath(ovalIn: NSRect(x: s * 0.2, y: s * 0.2, width: s * 0.6, height: s * 0.6))
    ring.lineWidth = s * 0.025
    ring.stroke()
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for (name, px) in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128), ("128x128@2x", 256),
                   ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)] {
    try render(px).write(to: URL(fileURLWithPath: out).appendingPathComponent("icon_\(name).png"))
}
