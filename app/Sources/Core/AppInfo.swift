// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// Names and versions used everywhere. The product name is a single constant so it can change.
public enum AppInfo {
    public static let name = "D3D12 Metal"
    public static let cliName = "d3d12metal"
    public static let bundleIdentifier = "org.d3d12metal.app"
    public static let version = "0.9.0"
    /// The Wine build used by default (Gcenx macOS_Wine_builds release tag).
    public static let defaultWineVersion = "11.18"
}

/// Everything the tools keep on disk lives under one directory (override: D3D12METAL_HOME).
public struct Paths: Sendable {
    public let home: URL

    public init(home: URL) { self.home = home }

    public static var `default`: Paths {
        if let env = ProcessInfo.processInfo.environment["D3D12METAL_HOME"], !env.isEmpty {
            return Paths(home: URL(fileURLWithPath: (env as NSString).expandingTildeInPath))
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return Paths(home: support.appendingPathComponent("D3D12Metal"))
    }

    public var runtimeDir: URL { home.appendingPathComponent("runtime") }
    public var bottlesDir: URL { home.appendingPathComponent("bottles") }
    public var bottlesFile: URL { home.appendingPathComponent("bottles.json") }
    public var gamesFile: URL { home.appendingPathComponent("games.json") }
    public var settingsDir: URL { home.appendingPathComponent("settings") }
    public var logsDir: URL { home.appendingPathComponent("logs") }
    public var userProfilesDir: URL { home.appendingPathComponent("profiles") }
    public var layerDir: URL { home.appendingPathComponent("layer") }
    public var downloadsDir: URL { home.appendingPathComponent("downloads") }
    public var configFile: URL { home.appendingPathComponent("config.json") }
    /// The layer's shader cache (the layer's own default location).
    public var shaderCacheDir: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Caches/d3d12metal")
    }

    public func logDir(for gameID: String) -> URL { logsDir.appendingPathComponent(Self.safeName(gameID)) }
    public func settingsFile(for gameID: String) -> URL {
        settingsDir.appendingPathComponent(Self.safeName(gameID) + ".json")
    }

    public func ensureDirectories() throws {
        for dir in [home, runtimeDir, bottlesDir, settingsDir, logsDir, downloadsDir] {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        }
    }

    /// A file-name-safe version of an id ("steam:1817070" -> "steam-1817070").
    public static func safeName(_ s: String) -> String {
        String(s.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" || $0 == "." ? $0 : "-" })
    }
}

/// Global preferences (config.json).
public struct Config: Codable, Equatable, Sendable {
    public var wineRoot: String?          // directory with bin/wine; nil = the managed runtime
    public var wineVersion: String = AppInfo.defaultWineVersion
    public var defaultBottle: String = "Steam"
    public var layerDir: String?          // a layer build to install; nil = bundled / repository build
    public var shaderCacheMaxMB: Int = 1024
    public var setupDone: Bool = false

    public init() {}

    enum CodingKeys: String, CodingKey { case wineRoot, wineVersion, defaultBottle, layerDir, shaderCacheMaxMB, setupDone }
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        wineRoot = try c.decodeIfPresent(String.self, forKey: .wineRoot)
        wineVersion = try c.decodeIfPresent(String.self, forKey: .wineVersion) ?? AppInfo.defaultWineVersion
        defaultBottle = try c.decodeIfPresent(String.self, forKey: .defaultBottle) ?? "Steam"
        layerDir = try c.decodeIfPresent(String.self, forKey: .layerDir)
        shaderCacheMaxMB = try c.decodeIfPresent(Int.self, forKey: .shaderCacheMaxMB) ?? 1024
        setupDone = try c.decodeIfPresent(Bool.self, forKey: .setupDone) ?? false
    }

    public static func load(_ paths: Paths) -> Config {
        guard let data = try? Data(contentsOf: paths.configFile),
              let c = try? JSONDecoder().decode(Config.self, from: data) else { return Config() }
        return c
    }

    public func save(_ paths: Paths) throws {
        try FileManager.default.createDirectory(at: paths.home, withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(self).write(to: paths.configFile, options: .atomic)
    }
}

/// An error with a message for the user and an optional hint on what to do.
public struct CoreError: Error, LocalizedError, CustomStringConvertible {
    public let message: String
    public let hint: String?
    public init(_ message: String, hint: String? = nil) { self.message = message; self.hint = hint }
    public var errorDescription: String? { hint.map { "\(message)\n  hint: \($0)" } ?? message }
    public var description: String { errorDescription ?? message }
}
