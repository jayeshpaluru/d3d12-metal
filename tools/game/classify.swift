// Classifies a screenshot of the game by comparing a downscaled grayscale copy with reference captures.
//   classify <png> <refdir>      prints "<label> <distance>" of the closest reference (distance 0..255, mean absolute
//                                difference of 64x72 grayscale cells of the menu area), or "unknown <distance>" above the threshold
//   classify --save <png> <refdir>/<label>.png    stores the capture as a reference (several per label: <label>.2.png)
// References are named <label>.png or <label>.<n>.png and are made locally from your own captures (build-wine/ref/, not committed).
import CoreGraphics
import Foundation
import ImageIO

let w = 64, h = 72
let threshold = 6.0, hudThreshold = 20.0   // menus are compared on the left 43% of the window only (the animated 3D background is on the right)

// References named gameplay*.png are compared on the middle of the window (the open world is up; the same save always shows the same spot).
let hud = CGRect(x: 0.2, y: 0.3, width: 0.55, height: 0.65)
let menu = CGRect(x: 0, y: 0, width: 0.43, height: 1)

func thumb(_ path: String, _ area: CGRect) -> [Double]? {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let full = CGImageSourceCreateImageAtIndex(src, 0, nil),
          let img = full.cropping(to: CGRect(x: area.minX * Double(full.width), y: area.minY * Double(full.height), width: area.width * Double(full.width), height: area.height * Double(full.height))) else { return nil }
    var px = [UInt8](repeating: 0, count: w * h)
    let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    ctx.interpolationQuality = .medium
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    return px.map { Double($0) }
}

var args = Array(CommandLine.arguments.dropFirst())
if args.first == "--save" {
    let from = args[1], to = args[2]
    try? FileManager.default.createDirectory(atPath: (to as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
    try? FileManager.default.removeItem(atPath: to)
    try FileManager.default.copyItem(atPath: from, toPath: to)
    exit(0)
}
guard args.count == 2, let menuShot = thumb(args[0], menu), let hudShot = thumb(args[0], hud) else { print("error 255"); exit(1) }
var best = ("unknown", 255.0), bestLimit = threshold
for f in (try? FileManager.default.contentsOfDirectory(atPath: args[1])) ?? [] where f.hasSuffix(".png") {
    let isHud = f.hasPrefix("gameplay")
    guard let ref = thumb(args[1] + "/" + f, isHud ? hud : menu) else { continue }
    let d = zip(isHud ? hudShot : menuShot, ref).reduce(0.0) { $0 + abs($1.0 - $1.1) } / Double(w * h)
    if d < best.1 { best = (String(f.split(separator: ".")[0]), d); bestLimit = isHud ? hudThreshold : threshold }
}
print(String(format: "%@ %.1f", best.1 <= bestLimit ? best.0 : "unknown", best.1))
