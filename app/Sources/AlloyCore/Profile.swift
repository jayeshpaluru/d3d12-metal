// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// A per-game profile (profiles/*.json): everything game-specific lives here, not in the layer or the tools.
public struct Profile: Codable, Equatable, Sendable {
    public struct Match: Codable, Equatable, Sendable {
        public var steamAppId: Int?
        public var exe: [String]?
        public init(steamAppId: Int? = nil, exe: [String]? = nil) { self.steamAppId = steamAppId; self.exe = exe }
    }

    public struct Launch: Codable, Equatable, Sendable {
        public var args: [String] = []
        public var env: [String: String] = [:]
        /// "steam": through the running Steam client; "direct": the exe itself with SteamAppId in the environment.
        public var mode: String = "steam"
        /// Part of the game's window title, used to find its window (screenshots).
        public var windowTitle: String?
        public init() {}
        enum CodingKeys: String, CodingKey { case args, env, mode, windowTitle }
        public init(from d: Decoder) throws {
            let c = try d.container(keyedBy: CodingKeys.self)
            args = try c.decodeIfPresent([String].self, forKey: .args) ?? []
            env = try c.decodeIfPresent([String: String].self, forKey: .env) ?? [:]
            mode = try c.decodeIfPresent(String.self, forKey: .mode) ?? "steam"
            windowTitle = try c.decodeIfPresent(String.self, forKey: .windowTitle)
        }
    }

    /// Something to run once before the game: the Windows equivalent of Steam's installscript.vdf "Run Process".
    public struct Requirement: Codable, Equatable, Sendable {
        public var id: String
        public var title: String
        /// Registry key (HKCU\... or HKLM\...) whose existence means "already done".
        public var doneKey: String?
        /// Command and arguments run with wine, relative to the game folder (".bat" and ".msi" are handled).
        public var run: [String]
        public var optional: Bool = false
        enum CodingKeys: String, CodingKey { case id, title, doneKey, run, optional }
        public init(id: String, title: String, doneKey: String?, run: [String], optional: Bool = false) {
            self.id = id; self.title = title; self.doneKey = doneKey; self.run = run; self.optional = optional
        }
        public init(from d: Decoder) throws {
            let c = try d.container(keyedBy: CodingKeys.self)
            id = try c.decode(String.self, forKey: .id)
            title = try c.decodeIfPresent(String.self, forKey: .title) ?? id
            doneKey = try c.decodeIfPresent(String.self, forKey: .doneKey)
            run = try c.decode([String].self, forKey: .run)
            optional = try c.decodeIfPresent(Bool.self, forKey: .optional) ?? false
        }
    }

    public var id: String
    public var name: String
    public var match: Match = Match()
    /// Primary executable (the one the layer is installed for) and the Steam install folder name.
    public var exe: String?
    public var installDir: String?
    public var launch: Launch = Launch()
    /// Keys of d3d12metal.conf (without the D3D12METAL_ prefix, lower case).
    public var conf: [String: String] = [:]
    /// Extra per-exe DLL overrides (the layer's own d3d12, d3d12core and dxgi are always added).
    public var dllOverrides: [String: String] = [:]
    public var requirements: [Requirement] = []
    public var notes: [String] = []
    /// "playable", "boots", "untested", ...: shown in the app and docs/COMPATIBILITY.md.
    public var status: String = "untested"

    enum CodingKeys: String, CodingKey { case id, name, match, exe, installDir, launch, conf, dllOverrides, requirements, notes, status }
    public init(id: String, name: String) { self.id = id; self.name = name }
    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? id
        match = try c.decodeIfPresent(Match.self, forKey: .match) ?? Match()
        exe = try c.decodeIfPresent(String.self, forKey: .exe)
        installDir = try c.decodeIfPresent(String.self, forKey: .installDir)
        launch = try c.decodeIfPresent(Launch.self, forKey: .launch) ?? Launch()
        conf = try c.decodeIfPresent([String: String].self, forKey: .conf) ?? [:]
        dllOverrides = try c.decodeIfPresent([String: String].self, forKey: .dllOverrides) ?? [:]
        requirements = try c.decodeIfPresent([Requirement].self, forKey: .requirements) ?? []
        notes = try c.decodeIfPresent([String].self, forKey: .notes) ?? []
        status = try c.decodeIfPresent(String.self, forKey: .status) ?? "untested"
    }

    public static let genericID = "generic"
}

public struct ProfileDB: Sendable {
    public private(set) var profiles: [Profile]

    public init(profiles: [Profile]) { self.profiles = profiles }

    /// The generic profile used when nothing matches.
    public static var builtinGeneric: Profile {
        var p = Profile(id: Profile.genericID, name: "Generic D3D12 game")
        p.notes = ["No game-specific settings: the layer's defaults."]
        return p
    }

    /// Loads every *.json in the directories in order; a later profile with the same id replaces an earlier one
    /// (so user profiles override the bundled ones). Malformed files are skipped and listed in `problems`.
    public static func load(directories: [URL], problems: inout [String]) -> ProfileDB {
        var byID: [String: Profile] = [:]
        var order: [String] = []
        let fm = FileManager.default
        for dir in directories {
            guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { continue }
            for n in names.sorted() where n.hasSuffix(".json") {
                let url = dir.appendingPathComponent(n)
                do {
                    let p = try JSONDecoder().decode(Profile.self, from: Data(contentsOf: url))
                    if byID[p.id] == nil { order.append(p.id) }
                    byID[p.id] = p
                } catch {
                    problems.append("\(url.lastPathComponent): \(error.localizedDescription)")
                }
            }
        }
        var list = order.compactMap { byID[$0] }
        if !list.contains(where: { $0.id == Profile.genericID }) { list.append(builtinGeneric) }
        return ProfileDB(profiles: list)
    }

    public var generic: Profile { profiles.first { $0.id == Profile.genericID } ?? Self.builtinGeneric }

    /// The profile for a game: Steam app id first, then executable name (case-insensitive), else the generic one.
    public func match(steamAppId: Int?, exe: String?) -> Profile {
        if let id = steamAppId, let p = profiles.first(where: { $0.match.steamAppId == id }) { return p }
        if let exe = exe?.lowercased(),
           let p = profiles.first(where: { ($0.match.exe ?? []).contains { $0.lowercased() == exe } }) { return p }
        return generic
    }
}
