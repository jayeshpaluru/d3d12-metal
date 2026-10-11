// swift-tools-version: 6.0
// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation
import PackageDescription

// With only the Command Line Tools selected, SwiftUI's macro plug-in (for @State and friends) is missing: borrow the one
// from an installed Xcode, if there is one.
let swiftUIPlugin = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/usr/lib/swift/host/plugins"
let appSwiftSettings: [SwiftSetting] = FileManager.default.fileExists(atPath: swiftUIPlugin + "/libSwiftUIMacros.dylib")
    ? [.unsafeFlags(["-plugin-path", swiftUIPlugin])] : []

let package = Package(
    name: "d3d12metal",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "Core", targets: ["Core"]),
        .executable(name: "d3d12metal", targets: ["d3d12metal"]),
        .executable(name: "D3D12MetalApp", targets: ["D3D12MetalApp"]),
    ],
    targets: [
        .target(name: "Core", path: "Sources/Core"),
        .executableTarget(name: "d3d12metal", dependencies: ["Core"], path: "Sources/d3d12metal"),
        .executableTarget(name: "D3D12MetalApp", dependencies: ["Core"], path: "Sources/D3D12MetalApp", swiftSettings: appSwiftSettings),
        .testTarget(name: "CoreTests", dependencies: ["Core"], path: "Tests/CoreTests",
                    linkerSettings: [.unsafeFlags(["-Xlinker", "-rpath", "-Xlinker", "/Library/Developer/CommandLineTools/Library/Developer/Frameworks", "-Xlinker", "-rpath", "-Xlinker", "/Library/Developer/CommandLineTools/Library/Developer/usr/lib"])]),
    ],
    swiftLanguageModes: [.v5]
)
