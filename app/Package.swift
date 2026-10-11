// swift-tools-version: 6.0
// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation
import PackageDescription

// With only the Command Line Tools selected, SwiftUI's macro plug-in (for @State and friends) is missing: borrow the one
// from an installed Xcode (it need not be selected or licensed).
let swiftUIPlugin = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/usr/lib/swift/host/plugins"
let appSwiftSettings: [SwiftSetting] = FileManager.default.fileExists(atPath: swiftUIPlugin + "/libSwiftUIMacros.dylib")
    ? [.unsafeFlags(["-plugin-path", swiftUIPlugin])] : []

let package = Package(
    name: "alloy",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "AlloyCore", targets: ["AlloyCore"]),
        .executable(name: "alloy", targets: ["alloy"]),
        .executable(name: "AlloyApp", targets: ["AlloyApp"]),
    ],
    targets: [
        .target(name: "AlloyCore", path: "Sources/AlloyCore"),
        .executableTarget(name: "alloy", dependencies: ["AlloyCore"], path: "Sources/alloy"),
        .executableTarget(name: "AlloyApp", dependencies: ["AlloyCore"], path: "Sources/AlloyApp", swiftSettings: appSwiftSettings),
        .testTarget(name: "AlloyCoreTests", dependencies: ["AlloyCore"], path: "Tests/AlloyCoreTests",
                    linkerSettings: [.unsafeFlags(["-Xlinker", "-rpath", "-Xlinker", "/Library/Developer/CommandLineTools/Library/Developer/Frameworks", "-Xlinker", "-rpath", "-Xlinker", "/Library/Developer/CommandLineTools/Library/Developer/usr/lib"])]),
    ],
    swiftLanguageModes: [.v5]
)
