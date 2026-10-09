// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// Valve's text KeyValues format (appmanifest_*.acf, libraryfolders.vdf).
public final class VDFNode {
    public var key: String
    public var value: String?
    public var children: [VDFNode] = []
    public init(key: String, value: String? = nil) { self.key = key; self.value = value }

    public subscript(key: String) -> VDFNode? {
        children.first { $0.key.caseInsensitiveCompare(key) == .orderedSame }
    }
    public func string(_ key: String) -> String? { self[key]?.value }
}

public enum VDF {
    /// Parses KeyValues text; the result is a root node whose children are the top-level keys.
    public static func parse(_ text: String) -> VDFNode {
        var tokens: [(String, Bool)] = []   // (text, isBrace)
        let chars = Array(text.unicodeScalars)
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if c == "\"" {
                var s = String.UnicodeScalarView()
                i += 1
                while i < chars.count, chars[i] != "\"" {
                    if chars[i] == "\\", i + 1 < chars.count {
                        i += 1
                        switch chars[i] {
                        case "n": s.append("\n")
                        case "t": s.append("\t")
                        default: s.append(chars[i])   // \\ \" and unknown escapes
                        }
                    } else {
                        s.append(chars[i])
                    }
                    i += 1
                }
                i += 1
                tokens.append((String(s), false))
            } else if c == "{" || c == "}" {
                tokens.append((String(c), true))
                i += 1
            } else if c == "/", i + 1 < chars.count, chars[i + 1] == "/" {
                while i < chars.count, chars[i] != "\n" { i += 1 }
            } else {
                i += 1
            }
        }
        let root = VDFNode(key: "")
        var stack = [root]
        var pos = 0
        while pos < tokens.count {
            let (t, brace) = tokens[pos]
            if brace {
                if t == "}", stack.count > 1 { stack.removeLast() }
                pos += 1
                continue
            }
            let node = VDFNode(key: t)
            stack.last!.children.append(node)
            pos += 1
            guard pos < tokens.count else { break }
            let (next, nextBrace) = tokens[pos]
            if nextBrace {
                if next == "{" { stack.append(node) }
                pos += 1
            } else {
                node.value = next
                pos += 1
            }
        }
        return root
    }
}

public struct SteamApp: Codable, Equatable, Sendable {
    public var appid: Int
    public var name: String
    public var installDir: String
    public var libraryPath: URL          // the steamapps folder it was found in (a host path)
    public var sizeOnDisk: Int64
    public var fullyInstalled: Bool

    public var gameFolder: URL { libraryPath.appendingPathComponent("common").appendingPathComponent(installDir) }
}

public enum SteamLibrary {
    /// Parses one appmanifest_<id>.acf.
    public static func parseManifest(_ text: String, steamapps: URL) -> SteamApp? {
        let root = VDF.parse(text)
        guard let state = root["AppState"],
              let idText = state.string("appid"), let appid = Int(idText),
              let installDir = state.string("installdir") else { return nil }
        let flags = Int(state.string("StateFlags") ?? "") ?? 0
        return SteamApp(appid: appid, name: state.string("name") ?? installDir, installDir: installDir,
                        libraryPath: steamapps, sizeOnDisk: Int64(state.string("SizeOnDisk") ?? "") ?? 0,
                        fullyInstalled: flags & 4 != 0)
    }

    /// The library folders listed in libraryfolders.vdf (Windows paths as written).
    public static func parseLibraryFolders(_ text: String) -> [String] {
        let root = VDF.parse(text)
        guard let folders = root["libraryfolders"] else { return [] }
        return folders.children.compactMap { $0.string("path") }
    }

    /// Maps a Windows path ("C:\\Program Files (x86)\\Steam") to a host path inside a Wine prefix.
    public static func hostPath(forWindowsPath path: String, prefix: URL) -> URL {
        let parts = path.replacingOccurrences(of: "\\\\", with: "\\").split(separator: "\\", omittingEmptySubsequences: true)
        guard let drive = parts.first, drive.hasSuffix(":") else {
            return prefix.appendingPathComponent("drive_c")
        }
        let letter = drive.dropLast().lowercased()
        var url = letter == "c" ? prefix.appendingPathComponent("drive_c")
                                : prefix.appendingPathComponent("dosdevices/\(letter):")
        for p in parts.dropFirst() { url.appendPathComponent(String(p)) }
        return url
    }

    /// Where the Windows Steam client of a prefix lives.
    public static func steamDirectory(prefix: URL) -> URL {
        prefix.appendingPathComponent("drive_c/Program Files (x86)/Steam")
    }

    /// All installed apps of the Steam in `prefix`.
    public static func installedApps(prefix: URL) -> [SteamApp] {
        let steam = steamDirectory(prefix: prefix)
        var folders = [steam.appendingPathComponent("steamapps")]
        if let text = try? String(contentsOf: steam.appendingPathComponent("steamapps/libraryfolders.vdf"), encoding: .utf8) {
            for p in parseLibraryFolders(text) {
                let url = hostPath(forWindowsPath: p, prefix: prefix).appendingPathComponent("steamapps")
                if !folders.contains(url) { folders.append(url) }
            }
        }
        var apps: [SteamApp] = []
        let fm = FileManager.default
        for folder in folders {
            guard let names = try? fm.contentsOfDirectory(atPath: folder.path) else { continue }
            for n in names.sorted() where n.hasPrefix("appmanifest_") && n.hasSuffix(".acf") {
                guard let text = try? String(contentsOf: folder.appendingPathComponent(n), encoding: .utf8),
                      let app = parseManifest(text, steamapps: folder) else { continue }
                // Steamworks Common Redistributables and the like have no game exe: filtered by the caller.
                if !apps.contains(where: { $0.appid == app.appid }) { apps.append(app) }
            }
        }
        return apps
    }

    /// Steam's cached library art, if the client has downloaded it (portrait capsule first).
    public static func artwork(appid: Int, prefix: URL) -> URL? {
        let dir = steamDirectory(prefix: prefix).appendingPathComponent("appcache/librarycache/\(appid)")
        for name in ["library_600x900.jpg", "header.jpg", "library_hero.jpg"] {
            let url = dir.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: url.path) { return url }
        }
        return nil
    }
}
