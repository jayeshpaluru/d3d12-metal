// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// A build of the layer that can be installed: d3d12.dll, dxgi.dll, x86_64-unix/d3d12metal.so (+ libdxilconv.dylib).
public struct LayerBuild: Sendable, Equatable {
    public let dir: URL
    public static let requiredFiles = ["d3d12.dll", "dxgi.dll", "x86_64-unix/d3d12metal.so"]
    public static let optionalFiles = ["x86_64-unix/libdxilconv.dylib"]

    public init(dir: URL) { self.dir = dir }

    public var isComplete: Bool {
        Self.requiredFiles.allSatisfy { FileManager.default.fileExists(atPath: dir.appendingPathComponent($0).path) }
    }

    public var files: [String] {
        Self.requiredFiles + Self.optionalFiles.filter { FileManager.default.fileExists(atPath: dir.appendingPathComponent($0).path) }
    }

    /// Written by tools/build-wine.sh (git describe) next to the DLLs.
    public var version: String {
        (try? String(contentsOf: dir.appendingPathComponent("layer-version.txt"), encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? "unknown"
    }

    public var webhelperShim: URL? {
        let u = dir.appendingPathComponent("steamwebhelper-shim.exe")
        return FileManager.default.fileExists(atPath: u.path) ? u : nil
    }

    /// config.layerDir, D3D12METAL_LAYER_DIR, the app bundle, the managed copy, a source checkout (build-wine/out).
    public static func locate(paths: Paths, config: Config) -> LayerBuild? {
        var candidates: [URL] = []
        if let d = config.layerDir, !d.isEmpty { candidates.append(URL(fileURLWithPath: (d as NSString).expandingTildeInPath)) }
        if let d = ProcessInfo.processInfo.environment["D3D12METAL_LAYER_DIR"], !d.isEmpty { candidates.append(URL(fileURLWithPath: d)) }
        if let r = Bundle.main.resourceURL { candidates.append(r.appendingPathComponent("layer")) }
        // Next to the executable (a release archive: bin/d3d12metal + layer/).
        let exe = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        candidates.append(exe.appendingPathComponent("layer"))
        candidates.append(exe.deletingLastPathComponent().appendingPathComponent("layer"))
        candidates.append(paths.layerDir)
        if let root = SourceTree.find() { candidates.append(root.appendingPathComponent("build-wine/out")) }
        return candidates.map(LayerBuild.init(dir:)).first { $0.isComplete }
    }
}

/// Finds a checkout of this repository (for development runs: profiles/ and build-wine/out).
public enum SourceTree {
    public static func find() -> URL? {
        var starts = [URL(fileURLWithPath: FileManager.default.currentDirectoryPath)]
        starts.append(URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent())
        if let env = ProcessInfo.processInfo.environment["D3D12METAL_REPO"] { starts.insert(URL(fileURLWithPath: env), at: 0) }
        for start in starts {
            var dir = start
            for _ in 0..<8 {
                if FileManager.default.fileExists(atPath: dir.appendingPathComponent("profiles/generic.json").path),
                   FileManager.default.fileExists(atPath: dir.appendingPathComponent("meson.build").path) { return dir }
                let parent = dir.deletingLastPathComponent()
                if parent == dir { break }
                dir = parent
            }
        }
        return nil
    }
}

public struct LayerStatus: Equatable, Sendable, Codable {
    public var installed: Bool
    /// All installed files match the build.
    public var upToDate: Bool
    public var installedVersion: String?
    public var exe: String?
}

/// What the installer remembers about an install, next to the shell-compatible manifest.
struct InstallMeta: Codable, Equatable {
    var version: String
    var exe: String
    var overrideKey: String
    var overrides: [String]       // DLL override names we set
    var pciKey: String?
}

public enum LayerInstaller {
    public static let manifestName = ".d3d12metal-install"       // "<rel>|<backup or ->" lines, as tools/install-game.sh writes
    public static let metaName = ".d3d12metal-install.json"
    public static let confName = "d3d12metal.conf"
    public static let layerDLLOverrides = ["d3d12", "d3d12core", "dxgi"]

    public static func overrideKey(exe: String) -> String { "HKCU\\Software\\Wine\\AppDefaults\\\(exe)\\DllOverrides" }

    // MARK: Manifest

    struct Manifest {
        var entries: [(rel: String, backup: String)] = []
        init(text: String = "") {
            for line in text.split(separator: "\n") {
                let p = line.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false)
                if p.count == 2, !p[0].isEmpty { entries.append((String(p[0]), String(p[1]))) }
            }
        }
        func backup(of rel: String) -> String? { entries.first { $0.rel == rel }?.backup }
        func has(_ rel: String) -> Bool { entries.contains { $0.rel == rel } }
        mutating func record(_ rel: String, _ backup: String) {
            entries.removeAll { $0.rel == rel }
            entries.append((rel, backup))
        }
        var text: String { entries.map { "\($0.rel)|\($0.backup)" }.joined(separator: "\n") + "\n" }
    }

    static func loadManifest(_ gameDir: URL) -> Manifest {
        Manifest(text: (try? String(contentsOf: gameDir.appendingPathComponent(manifestName), encoding: .utf8)) ?? "")
    }

    // MARK: Status

    public static func status(gameDir: URL, build: LayerBuild?) -> LayerStatus {
        let fm = FileManager.default
        guard fm.fileExists(atPath: gameDir.appendingPathComponent(manifestName).path) else {
            return LayerStatus(installed: false, upToDate: false, installedVersion: nil, exe: nil)
        }
        let meta = (try? Data(contentsOf: gameDir.appendingPathComponent(metaName))).flatMap { try? JSONDecoder().decode(InstallMeta.self, from: $0) }
        var upToDate = false
        if let build {
            upToDate = build.files.allSatisfy {
                fm.contentsEqual(atPath: build.dir.appendingPathComponent($0).path, andPath: gameDir.appendingPathComponent($0).path)
            }
        }
        return LayerStatus(installed: true, upToDate: upToDate, installedVersion: meta?.version, exe: meta?.exe)
    }

    // MARK: Install

    /// Copies the layer next to the game, writes the conf and sets the per-exe DLL overrides (when a session is given).
    /// Files that would be overwritten are renamed `*.d3d12metal-orig`. Safe to run again.
    @discardableResult
    public static func install(gameDir: URL, exe: String, build: LayerBuild, conf: String,
                               extraOverrides: [String: String] = [:], pciKey: String? = nil,
                               session: WineSession?, dryRun: Bool = false) throws -> [String] {
        guard build.isComplete else { throw CoreError("The layer build in \(build.dir.path) is incomplete", hint: "tools/build-wine.sh") }
        let fm = FileManager.default
        guard fm.fileExists(atPath: gameDir.path) else { throw CoreError("Game folder \(gameDir.path) does not exist") }
        var log: [String] = []
        var manifest = loadManifest(gameDir)
        if !fm.fileExists(atPath: gameDir.appendingPathComponent("x86_64-unix").path) { manifest.record("x86_64-unix/", "-") }

        func place(rel: String, write: (URL) throws -> Void, same: (URL) -> Bool) throws {
            let dest = gameDir.appendingPathComponent(rel)
            var backup = manifest.backup(of: rel) ?? "-"
            if fm.fileExists(atPath: dest.path) {
                if same(dest) {
                    log.append("up to date:   \(rel)")
                    manifest.record(rel, backup)
                    return
                }
                if !manifest.has(rel) {
                    backup = rel + ".d3d12metal-orig"
                    var n = 1
                    while fm.fileExists(atPath: gameDir.appendingPathComponent(backup).path) { backup = "\(rel).d3d12metal-orig.\(n)"; n += 1 }
                    log.append("backup:       \(rel) -> \(backup)")
                    if !dryRun { try fm.moveItem(at: dest, to: gameDir.appendingPathComponent(backup)) }
                }
            }
            log.append("install:      \(rel)")
            if !dryRun {
                try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? fm.removeItem(at: dest)
                try write(dest)
            }
            manifest.record(rel, backup)
        }

        for rel in build.files {
            let src = build.dir.appendingPathComponent(rel)
            try place(rel: rel, write: { try fm.copyItem(at: src, to: $0) }, same: { fm.contentsEqual(atPath: src.path, andPath: $0.path) })
        }
        let confData = Data(conf.utf8)
        try place(rel: confName, write: { try confData.write(to: $0) }, same: { (try? Data(contentsOf: $0)) == confData })

        if !dryRun { try manifest.text.write(to: gameDir.appendingPathComponent(manifestName), atomically: true, encoding: .utf8) }

        var overrides = Dictionary(uniqueKeysWithValues: layerDLLOverrides.map { ($0, "native,builtin") })
        for (k, v) in extraOverrides { overrides[k] = v }
        let key = overrideKey(exe: exe)
        if let session, !dryRun {
            for (dll, value) in overrides.sorted(by: { $0.key < $1.key }) {
                guard session.regAdd(key: key, name: dll, value: value) else {
                    throw CoreError("Could not set the DLL override \(dll) in the registry of bottle \"\(session.bottle.name)\"")
                }
            }
            if let pciKey {
                for (name, value) in [("DeviceDesc", "GPU"), ("Class", "Display"), ("ClassGUID", "{4D36E968-E325-11CE-BFC1-08002BE10318}"),
                                      ("Driver", "{4D36E968-E325-11CE-BFC1-08002BE10318}\\0000")] {
                    _ = session.regAdd(key: pciKey, name: name, value: value)
                }
            }
            log.append("registry:     \(key): \(overrides.keys.sorted().joined(separator: ", ")) = native,builtin")
        } else {
            log.append("registry:     (not changed)")
        }
        let meta = InstallMeta(version: build.version, exe: exe, overrideKey: key, overrides: overrides.keys.sorted(), pciKey: pciKey)
        if !dryRun { try JSONEncoder().encode(meta).write(to: gameDir.appendingPathComponent(metaName)) }
        return log
    }

    /// Rewrites only d3d12metal.conf (settings changed). The file must already be ours or absent.
    public static func updateConf(gameDir: URL, conf: String) throws {
        let url = gameDir.appendingPathComponent(confName)
        if let old = try? String(contentsOf: url, encoding: .utf8), !old.hasPrefix(ConfBuilder.marker) {
            throw CoreError("\(confName) in the game folder is not managed by \(AppInfo.name)", hint: "Install the layer first, which keeps a backup of it")
        }
        try conf.write(to: url, atomically: true, encoding: .utf8)
    }

    // MARK: Uninstall

    @discardableResult
    public static func uninstall(gameDir: URL, session: WineSession?, dryRun: Bool = false) throws -> [String] {
        let fm = FileManager.default
        var log: [String] = []
        let manifestURL = gameDir.appendingPathComponent(manifestName)
        guard fm.fileExists(atPath: manifestURL.path) else { return ["nothing installed in the game folder (no \(manifestName))"] }
        let manifest = loadManifest(gameDir)
        for (rel, backup) in manifest.entries where !rel.hasSuffix("/") {
            let dest = gameDir.appendingPathComponent(rel)
            let backupURL = gameDir.appendingPathComponent(backup)
            if backup != "-", fm.fileExists(atPath: backupURL.path) {
                log.append("restore:      \(backup) -> \(rel)")
                if !dryRun { try? fm.removeItem(at: dest); try fm.moveItem(at: backupURL, to: dest) }
            } else if fm.fileExists(atPath: dest.path) {
                log.append("remove:       \(rel)")
                if !dryRun { try fm.removeItem(at: dest) }
            }
        }
        for (rel, _) in manifest.entries where rel.hasSuffix("/") {
            let dir = gameDir.appendingPathComponent(rel)
            if let left = try? fm.contentsOfDirectory(atPath: dir.path), left.isEmpty {
                if !dryRun { try? fm.removeItem(at: dir) }
            } else if fm.fileExists(atPath: dir.path) {
                log.append("kept:         \(rel) (not empty)")
            }
        }
        let meta = (try? Data(contentsOf: gameDir.appendingPathComponent(metaName))).flatMap { try? JSONDecoder().decode(InstallMeta.self, from: $0) }
        if let session, let meta, !dryRun {
            for name in meta.overrides { session.regDelete(key: meta.overrideKey, name: name) }
            if session.regValues(key: meta.overrideKey).isEmpty, session.regKeyExists(meta.overrideKey) {
                session.run(["reg", "delete", meta.overrideKey, "/f"], timeout: 60)
            }
            log.append("registry:     removed \(meta.overrides.joined(separator: ", ")) from \(meta.overrideKey)")
        }
        if !dryRun {
            try? fm.removeItem(at: manifestURL)
            try? fm.removeItem(at: gameDir.appendingPathComponent(metaName))
        }
        return log
    }
}
