// Classifies a screenshot of the game by comparing a downscaled grayscale copy with reference captures.
//   classify <png> <refdir>      prints "<label> <distance>" of the closest reference (distance 0..255, mean absolute
//                                difference of 64x36 grayscale cells), or "unknown <distance>" above the threshold
//   classify --save <png> <refdir>/<label>.png    stores the capture as a reference (several per label: <label>.2.png)
// References are named <label>.png or <label>.<n>.png and are made locally from your own captures (build-wine/ref/, not committed).
import CoreGraphics
import Foundation
import ImageIO

let w = 64, h = 36
let threshold = 12.0

func thumb(_ path: String) -> [Double]? {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
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
guard args.count == 2, let shot = thumb(args[0]) else { print("error 255"); exit(1) }
var best = ("unknown", 255.0)
for f in (try? FileManager.default.contentsOfDirectory(atPath: args[1])) ?? [] where f.hasSuffix(".png") {
    guard let ref = thumb(args[1] + "/" + f) else { continue }
    let d = zip(shot, ref).reduce(0.0) { $0 + abs($1.0 - $1.1) } / Double(w * h)
    if d < best.1 { best = (String(f.split(separator: ".")[0]), d) }
}
print(String(format: "%@ %.1f", best.1 <= threshold ? best.0 : "unknown", best.1))
