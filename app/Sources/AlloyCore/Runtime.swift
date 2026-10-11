// SPDX-License-Identifier: LGPL-2.1-or-later
import CryptoKit
import Foundation

/// A Wine installation (a directory that contains bin/wine).
public struct WineRuntime: Equatable, Sendable {
    public let root: URL
    public var wine: URL { root.appendingPathComponent("bin/wine") }
    public var wineserver: URL { root.appendingPathComponent("bin/wineserver") }

    public init(root: URL) { self.root = root }

    public var isValid: Bool { FileManager.default.isExecutableFile(atPath: wine.path) }

    /// "wine-11.18 (Staging)" style version string, from `wine --version`.
    public func version() -> String? {
        let r = Shell.run(wine.path, ["--version"], env: ["WINEDEBUG": "-all", "WINEPREFIX": "/nonexistent"], timeout: 20)
        let s = r.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        return r.ok && !s.isEmpty ? s : nil
    }
}

/// Known Wine builds: the Gcenx macOS_Wine_builds releases the layer was tested with.
public struct WineRelease: Sendable {
    public let version: String
    public let url: URL
    public let sha256: String
    public let sizeBytes: Int64

    public static let known: [WineRelease] = [
        WineRelease(version: "11.18",
                    url: URL(string: "https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.18/wine-devel-11.18-osx64.tar.xz")!,
                    sha256: "aa0ea4c82e636ae7bca2076387cb0a5affa26509ad13f119ecd0d62bd7ba6f82",
                    sizeBytes: 190_974_384),
    ]

    public static func release(version: String) -> WineRelease? { known.first { $0.version == version } }
}

public enum RuntimeManager {
    /// Directory of an installed (managed) release.
    public static func directory(for version: String, paths: Paths) -> URL {
        paths.runtimeDir.appendingPathComponent("wine-\(version)")
    }

    /// Finds the Wine root inside an extraction directory ("Wine Devel.app/Contents/Resources/wine").
    public static func findRoot(in dir: URL) -> URL? {
        let fm = FileManager.default
        guard let apps = try? fm.contentsOfDirectory(atPath: dir.path) else { return nil }
        for a in apps.sorted() where a.hasSuffix(".app") {
            let root = dir.appendingPathComponent(a).appendingPathComponent("Contents/Resources/wine")
            if fm.isExecutableFile(atPath: root.appendingPathComponent("bin/wine").path) { return root }
        }
        return nil
    }

    /// The Wine to use: config override, WINE_ROOT, the managed release of the configured version, any managed release.
    public static func locate(paths: Paths, config: Config) -> WineRuntime? {
        var candidates: [URL] = []
        if let r = config.wineRoot, !r.isEmpty { candidates.append(URL(fileURLWithPath: (r as NSString).expandingTildeInPath)) }
        if let r = ProcessInfo.processInfo.environment["WINE_ROOT"], !r.isEmpty { candidates.append(URL(fileURLWithPath: r)) }
        if let root = findRoot(in: directory(for: config.wineVersion, paths: paths)) { candidates.append(root) }
        if let dirs = try? FileManager.default.contentsOfDirectory(atPath: paths.runtimeDir.path) {
            for d in dirs.sorted().reversed() where d.hasPrefix("wine-") {
                if let root = findRoot(in: paths.runtimeDir.appendingPathComponent(d)) { candidates.append(root) }
            }
        }
        return candidates.map(WineRuntime.init(root:)).first { $0.isValid }
    }

    /// SHA-256 of a file, read in chunks.
    public static func sha256(of url: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: url)
        defer { try? h.close() }
        var hasher = SHA256()
        while let chunk = try h.read(upToCount: 4 << 20), !chunk.isEmpty { hasher.update(data: chunk) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// Downloads, verifies and unpacks a release. `progress` gets 0...1 (download) and then the stage name.
    @discardableResult
    public static func install(_ release: WineRelease, paths: Paths,
                               progress: @escaping @Sendable (Double, String) -> Void = { _, _ in }) async throws -> WineRuntime {
        try paths.ensureDirectories()
        let dest = directory(for: release.version, paths: paths)
        if let root = findRoot(in: dest) { return WineRuntime(root: root) }

        let archive = paths.downloadsDir.appendingPathComponent(release.url.lastPathComponent)
        var needDownload = true
        if FileManager.default.fileExists(atPath: archive.path), (try? sha256(of: archive)) == release.sha256 { needDownload = false }
        if needDownload {
            progress(0, "Downloading Wine \(release.version)")
            try await Downloader.download(release.url, to: archive) { progress($0, "Downloading Wine \(release.version)") }
        }
        progress(1, "Verifying checksum")
        let sum = try sha256(of: archive)
        guard sum == release.sha256 else {
            try? FileManager.default.removeItem(at: archive)
            throw CoreError("Checksum mismatch for \(archive.lastPathComponent): expected \(release.sha256), got \(sum)",
                            hint: "The download was corrupted or replaced. Try again; nothing was installed.")
        }
        progress(1, "Unpacking Wine (this takes a minute)")
        let staging = paths.runtimeDir.appendingPathComponent(".staging-\(release.version)")
        try? FileManager.default.removeItem(at: staging)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        let r = Shell.run("/usr/bin/tar", ["-xf", archive.path, "-C", staging.path])
        guard r.ok, findRoot(in: staging) != nil else {
            try? FileManager.default.removeItem(at: staging)
            throw CoreError("Could not unpack \(archive.lastPathComponent): \(r.stderr)")
        }
        try? FileManager.default.removeItem(at: dest)
        try FileManager.default.moveItem(at: staging, to: dest)
        _ = Shell.run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", dest.path])
        guard let root = findRoot(in: dest) else { throw CoreError("Unpacked Wine is incomplete") }
        return WineRuntime(root: root)
    }
}

/// A download with progress, to a file (atomically moved into place).
enum Downloader {
    static func download(_ url: URL, to dest: URL, progress: @escaping @Sendable (Double) -> Void) async throws {
        let delegate = ProgressDelegate(progress: progress)
        let (tmp, response) = try await URLSession.shared.download(from: url, delegate: delegate)
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw CoreError("Download of \(url.absoluteString) failed: HTTP \(http.statusCode)")
        }
        try FileManager.default.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: dest)
        try FileManager.default.moveItem(at: tmp, to: dest)
    }

    final class ProgressDelegate: NSObject, URLSessionTaskDelegate, URLSessionDownloadDelegate, @unchecked Sendable {
        let progress: @Sendable (Double) -> Void
        init(progress: @escaping @Sendable (Double) -> Void) { self.progress = progress }
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                        totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
            if totalBytesExpectedToWrite > 0 { progress(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)) }
        }
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {}
    }
}
