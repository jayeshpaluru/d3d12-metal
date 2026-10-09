// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation
import Metal

public struct Check: Codable, Equatable, Sendable, Identifiable {
    public enum Level: String, Codable, Sendable { case ok, warning, error, info }
    public var id: String
    public var title: String
    public var level: Level
    public var detail: String
    public var fix: String?
}

public enum Doctor {
    public static let mscDylib = "/usr/local/lib/libmetalirconverter.dylib"
    public static let mscDownloadURL = URL(string: "https://developer.apple.com/metal/shader-converter/")!

    public static func runAll(_ ws: Workspace) -> [Check] {
        var checks: [Check] = []
        checks.append(system())
        checks.append(rosetta())
        checks.append(avx())
        checks.append(contentsOf: gpu())
        checks.append(msc())
        checks.append(wine(ws))
        checks.append(layer(ws))
        checks.append(contentsOf: steam(ws))
        if !ws.profileProblems.isEmpty {
            checks.append(Check(id: "profiles", title: "Game profiles", level: .warning,
                                detail: ws.profileProblems.joined(separator: "; "), fix: nil))
        }
        return checks
    }

    static func system() -> Check {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        let arm = Shell.run("/usr/sbin/sysctl", ["-n", "hw.optional.arm64"]).stdout.trimmingCharacters(in: .whitespacesAndNewlines) == "1"
        let chip = Shell.run("/usr/sbin/sysctl", ["-n", "machdep.cpu.brand_string"]).stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        let ok = arm && v.majorVersion >= 14
        return Check(id: "system", title: "macOS on Apple Silicon", level: ok ? .ok : .error,
                     detail: "macOS \(v.majorVersion).\(v.minorVersion).\(v.patchVersion), \(chip)",
                     fix: ok ? nil : "Apple Silicon and macOS 14 or newer are required")
    }

    static func rosetta() -> Check {
        let r = Shell.run("/usr/bin/arch", ["-x86_64", "/usr/bin/true"], timeout: 20)
        return Check(id: "rosetta", title: "Rosetta 2", level: r.ok ? .ok : .error,
                     detail: r.ok ? "installed" : "x86-64 programs cannot run",
                     fix: r.ok ? nil : "softwareupdate --install-rosetta --agree-to-license")
    }

    static func avx() -> Check {
        // Rosetta only advertises AVX/AVX2/F16C to x86 programs when asked (modern games require AVX2). Support arrived
        // with macOS 15; the tools set ROSETTA_ADVERTISE_AVX=1 for every Wine process they start.
        let major = ProcessInfo.processInfo.operatingSystemVersion.majorVersion
        if major >= 15 {
            return Check(id: "avx", title: "AVX2 through Rosetta", level: .ok,
                         detail: "supported; \(AppInfo.name) sets ROSETTA_ADVERTISE_AVX=1 for Wine and the games", fix: nil)
        }
        return Check(id: "avx", title: "AVX2 through Rosetta", level: .warning,
                     detail: "Rosetta does not advertise AVX2 before macOS 15; games that require it will not start", fix: "Update macOS")
    }

    static func gpu() -> [Check] {
        guard let dev = MTLCreateSystemDefaultDevice() else {
            return [Check(id: "gpu", title: "Metal GPU", level: .error, detail: "no Metal device", fix: nil)]
        }
        let gb = String(format: "%.1f", Double(dev.recommendedMaxWorkingSetSize) / 1_073_741_824)
        let tier = dev.supportsFamily(.metal3) ? "Metal 3" : "below Metal 3"
        return [Check(id: "gpu", title: "Metal GPU", level: dev.supportsFamily(.metal3) ? .ok : .warning,
                      detail: "\(dev.name), \(tier), \(gb) GB GPU working set, \(dev.hasUnifiedMemory ? "unified" : "discrete") memory", fix: nil)]
    }

    /// The x86-64 slice is needed (Wine runs under Rosetta); Apple's dylib is universal.
    static func msc() -> Check {
        let path = mscDylib
        guard FileManager.default.fileExists(atPath: path) else {
            return Check(id: "msc", title: "Metal Shader Converter", level: .error,
                         detail: "not installed (\(path))",
                         fix: "Download it from Apple (\(mscDownloadURL.absoluteString)) and install the package; it cannot be redistributed with this app")
        }
        let archs = Shell.run("/usr/bin/lipo", ["-archs", path]).stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        if !archs.contains("x86_64") {
            return Check(id: "msc", title: "Metal Shader Converter", level: .error, detail: "\(path) has no x86_64 slice (\(archs))",
                         fix: "Install Apple's package again: the official dylib is universal")
        }
        return Check(id: "msc", title: "Metal Shader Converter", level: .ok, detail: "\(path) (\(archs))", fix: nil)
    }

    static func wine(_ ws: Workspace) -> Check {
        guard let rt = RuntimeManager.locate(paths: ws.paths, config: ws.config) else {
            return Check(id: "wine", title: "Wine", level: .error, detail: "not installed",
                         fix: "\(AppInfo.cliName) runtime install  (downloads Wine \(ws.config.wineVersion) from Gcenx, checksum verified)")
        }
        return Check(id: "wine", title: "Wine", level: .ok, detail: "\(rt.version() ?? "unknown version") at \(rt.root.path)", fix: nil)
    }

    static func layer(_ ws: Workspace) -> Check {
        guard let b = ws.layerBuild else {
            return Check(id: "layer", title: "Layer build", level: .error, detail: "d3d12.dll / dxgi.dll / d3d12metal.so not found",
                         fix: "Build with tools/build-wine.sh, or use a release of \(AppInfo.name)")
        }
        let dxil = FileManager.default.fileExists(atPath: b.dir.appendingPathComponent("x86_64-unix/libdxilconv.dylib").path)
        return Check(id: "layer", title: "Layer build", level: dxil ? .ok : .warning,
                     detail: "\(b.version) at \(b.dir.path)" + (dxil ? "" : " (no libdxilconv.dylib: DXBC shaders unsupported)"),
                     fix: dxil ? nil : "tools/build-dxilconv.sh, then tools/build-wine.sh")
    }

    static func steam(_ ws: Workspace) -> [Check] {
        let list = ws.bottles.list()
        if list.isEmpty {
            return [Check(id: "bottle", title: "Bottles", level: .warning, detail: "none yet", fix: "\(AppInfo.cliName) setup")]
        }
        return list.map { b in
            let steam = SteamLibrary.steamDirectory(prefix: b.prefix).appendingPathComponent("steam.exe")
            let has = FileManager.default.fileExists(atPath: steam.path)
            return Check(id: "steam-\(b.name)", title: "Bottle \"\(b.name)\"", level: has ? .ok : .info,
                         detail: has ? "Steam installed" + (SteamClient.isRunning ? ", running" : ", not running") : "no Steam in this bottle",
                         fix: has ? nil : "\(AppInfo.cliName) steam install --bottle \(b.name)")
        }
    }
}
