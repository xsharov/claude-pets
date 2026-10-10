import AppKit
import Foundation

struct Oval {
    let cx: CGFloat
    let cy: CGFloat
    let rx: CGFloat
    let ry: CGFloat
    let color: NSColor
}

func hex(_ value: UInt32, _ alpha: CGFloat = 1.0) -> NSColor {
    NSColor(srgbRed: CGFloat((value >> 16) & 0xFF) / 255.0, green: CGFloat((value >> 8) & 0xFF) / 255.0, blue: CGFloat(value & 0xFF) / 255.0, alpha: alpha)
}

let pet: [Oval] = [
    Oval(cx: 0.5, cy: 0.57, rx: 0.425, ry: 0.385, color: hex(0xB4583A)),
    Oval(cx: 0.5, cy: 0.56, rx: 0.4, ry: 0.36, color: hex(0xD97757)),
    Oval(cx: 0.5, cy: 0.43, rx: 0.27, ry: 0.15, color: hex(0xE69373, 0.55)),
    Oval(cx: 0.34, cy: 0.9, rx: 0.09, ry: 0.055, color: hex(0xB4583A)),
    Oval(cx: 0.66, cy: 0.9, rx: 0.09, ry: 0.055, color: hex(0xB4583A)),
    Oval(cx: 0.385, cy: 0.53, rx: 0.05, ry: 0.075, color: hex(0x1E1A18)),
    Oval(cx: 0.615, cy: 0.53, rx: 0.05, ry: 0.075, color: hex(0x1E1A18)),
    Oval(cx: 0.4, cy: 0.505, rx: 0.018, ry: 0.022, color: hex(0xFFFFFF)),
    Oval(cx: 0.63, cy: 0.505, rx: 0.018, ry: 0.022, color: hex(0xFFFFFF)),
    Oval(cx: 0.29, cy: 0.66, rx: 0.055, ry: 0.032, color: hex(0xF4A38A, 0.8)),
    Oval(cx: 0.71, cy: 0.66, rx: 0.055, ry: 0.032, color: hex(0xF4A38A, 0.8)),
]

func render(_ pixels: Int) -> Data {
    let size = CGFloat(pixels)
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let context = CGContext(data: nil, width: pixels, height: pixels, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.translateBy(x: 0, y: size)
    context.scaleBy(x: size / 1024.0, y: -size / 1024.0)

    let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
    let shape = CGPath(roundedRect: tile, cornerWidth: 185.4, cornerHeight: 185.4, transform: nil)
    context.saveGState()
    context.setShadow(offset: CGSize(width: 0, height: 10), blur: 20, color: NSColor(white: 0, alpha: 0.28).cgColor)
    context.addPath(shape)
    context.setFillColor(hex(0xF6EEE4).cgColor)
    context.fillPath()
    context.restoreGState()

    context.saveGState()
    context.addPath(shape)
    context.clip()
    let gradient = CGGradient(colorsSpace: space, colors: [hex(0xFCF8F3).cgColor, hex(0xEEDFCF).cgColor] as CFArray, locations: [0, 1])!
    context.drawLinearGradient(gradient, start: CGPoint(x: 512, y: 100), end: CGPoint(x: 512, y: 924), options: [])
    let art = CGRect(x: 172, y: 182, width: 680, height: 680)
    for oval in pet {
        context.setFillColor(oval.color.cgColor)
        context.fillEllipse(in: CGRect(x: art.minX + (oval.cx - oval.rx) * art.width, y: art.minY + (oval.cy - oval.ry) * art.height,
                                       width: oval.rx * 2 * art.width, height: oval.ry * 2 * art.height))
    }
    context.restoreGState()

    context.addPath(shape)
    context.setStrokeColor(NSColor(white: 0, alpha: 0.08).cgColor)
    context.setLineWidth(2)
    context.strokePath()

    let image = context.makeImage()!
    return NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])!
}

let arguments = CommandLine.arguments
let target = URL(fileURLWithPath: arguments.count > 1 ? arguments[1] : "res/app.icns")
let iconset = FileManager.default.temporaryDirectory.appendingPathComponent("claude-pets-\(ProcessInfo.processInfo.processIdentifier).iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    try render(points).write(to: iconset.appendingPathComponent("icon_\(points)x\(points).png"))
    try render(points * 2).write(to: iconset.appendingPathComponent("icon_\(points)x\(points)@2x.png"))
}
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", target.path]
try process.run()
process.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
guard process.terminationStatus == 0 else {
    FileHandle.standardError.write("iconutil failed\n".data(using: .utf8)!)
    exit(1)
}
print("wrote \(target.path)")
