// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// A game the tools know about: a Steam game found in a bottle, or one added by hand.
public struct Game: Codable, Identifiable, Equatable, Sendable {
    public var id: String                // "steam:<appid>" or "custom:<slug>"
    public var name: String
    public var bottle: String
    /// The folder that holds the exe (the layer is installed here).
    public var gameDirPath: String
    public var exe: String
    public var steamAppId: Int?
    public var profileID: String
    public var artPath: String?
    public var installed: Bool = true     // Steam: fully installed

    public var gameDir: URL { URL(fileURLWithPath: gameDirPath) }
    public var art: URL? { artPath.map { URL(fileURLWithPath: $0) } }
}

public struct ExeCandidate: Equatable, Sendable {
    public var url: URL
    public var usesD3D12: Bool
    public var size: Int64
    public var name: String { url.lastPathComponent }
}

public enum ExeFinder {
    static let ignoredPrefixes = ["unins", "crash", "vc_redist", "vcredist", "dxsetup", "dotnet", "ndp", "unitycrash", "uninst", "setup",
                                  "oalinst", "ue4prereq", "ueprereq", "installer", "directx", "easyanticheat", "beclient", "crs-"]

    /// The executables in `folder` (up to `maxDepth` levels down), with the ones that use D3D12 first.
    public static func candidates(in folder: URL, maxDepth: Int = 3) -> [ExeCandidate] {
        var found: [ExeCandidate] = []
        let fm = FileManager.default
        func walk(_ dir: URL, depth: Int) {
            guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey, .isSymbolicLinkKey],
                                                          options: [.skipsHiddenFiles]) else { return }
            for url in items.sorted(by: { $0.path < $1.path }) {
                let v = try? url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey, .isSymbolicLinkKey])
                if v?.isSymbolicLink == true { continue }
                if v?.isDirectory == true {
                    if depth < maxDepth { walk(url, depth: depth + 1) }
                } else if url.pathExtension.lowercased() == "exe" {
                    let lower = url.lastPathComponent.lowercased()
                    if ignoredPrefixes.contains(where: { lower.hasPrefix($0) }) { continue }
                    found.append(ExeCandidate(url: url, usesD3D12: PEScanner.usesD3D12(url: url), size: Int64(v?.fileSize ?? 0)))
                }
            }
        }
        walk(folder, depth: 0)
        return found.sorted {
            if $0.usesD3D12 != $1.usesD3D12 { return $0.usesD3D12 }
            return $0.size > $1.size
        }
    }
}

public struct GameLibrary: Sendable {
    public let paths: Paths
    public let profiles: ProfileDB
    public let bottles: BottleStore

    /// Steam apps that are tools, not games.
    static let ignoredAppIDs: Set<Int> = [228980, 1070560, 1391110, 1493710, 1628350]

    public init(paths: Paths, profiles: ProfileDB) {
        self.paths = paths; self.profiles = profiles; self.bottles = BottleStore(paths: paths)
    }

    // MARK: Custom games

    public func customGames() -> [Game] {
        guard let data = try? Data(contentsOf: paths.gamesFile), let list = try? JSONDecoder().decode([Game].self, from: data) else { return [] }
        return list
    }

    private func saveCustom(_ list: [Game]) throws {
        try FileManager.default.createDirectory(at: paths.home, withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(list).write(to: paths.gamesFile, options: .atomic)
    }

    /// Adds a non-Steam game: a folder with an exe, run in a bottle.
    @discardableResult
    public func addCustom(name: String, bottle: String, folder: URL, exe: String, steamAppId: Int? = nil) throws -> Game {
        guard FileManager.default.fileExists(atPath: folder.appendingPathComponent(exe).path) else {
            throw CoreError("\(exe) not found in \(folder.path)")
        }
        var slug = Paths.safeName(name.lowercased()).replacingOccurrences(of: "--", with: "-")
        if slug.isEmpty { slug = "game" }
        var list = customGames()
        var id = "custom:\(slug)"
        var n = 2
        while list.contains(where: { $0.id == id }) { id = "custom:\(slug)-\(n)"; n += 1 }
        let profile = profiles.match(steamAppId: steamAppId, exe: exe)
        let game = Game(id: id, name: name, bottle: bottle, gameDirPath: folder.path, exe: exe, steamAppId: steamAppId, profileID: profile.id)
        list.append(game)
        try saveCustom(list)
        return game
    }

    public func removeCustom(id: String) throws {
        try saveCustom(customGames().filter { $0.id != id })
    }

    // MARK: Steam

    /// Turns the Steam apps installed in a bottle into games with a located exe. Apps without a D3D12 exe
    /// are skipped unless `includeNonD3D12`.
    public func steamGames(in bottle: Bottle, includeNonD3D12: Bool = false) -> [Game] {
        var games: [Game] = []
        for app in SteamLibrary.installedApps(prefix: bottle.prefix) where !Self.ignoredAppIDs.contains(app.appid) {
            guard app.fullyInstalled else { continue }
            let profile = profiles.match(steamAppId: app.appid, exe: nil)
            let cands = ExeFinder.candidates(in: app.gameFolder)
            var chosen: ExeCandidate?
            if let want = profile.exe?.lowercased() {
                chosen = cands.first { $0.name.lowercased() == want }
            }
            if chosen == nil { chosen = cands.first { $0.usesD3D12 } }
            if chosen == nil, includeNonD3D12 { chosen = cands.first }
            guard let exe = chosen else { continue }
            let finalProfile = profile.id == Profile.genericID ? profiles.match(steamAppId: app.appid, exe: exe.name) : profile
            games.append(Game(id: "steam:\(app.appid)", name: app.name, bottle: bottle.name,
                              gameDirPath: exe.url.deletingLastPathComponent().path, exe: exe.name, steamAppId: app.appid,
                              profileID: finalProfile.id,
                              artPath: SteamLibrary.artwork(appid: app.appid, prefix: bottle.prefix)?.path))
        }
        return games
    }

    /// Every game of every bottle, plus the hand-added ones.
    public func allGames() -> [Game] {
        var games: [Game] = []
        for b in bottles.list() { games += steamGames(in: b) }
        for g in customGames() where !games.contains(where: { $0.id == g.id }) { games.append(g) }
        return games
    }

    public func find(_ query: String) -> Game? {
        let games = allGames()
        let q = query.lowercased()
        if let g = games.first(where: { $0.id.lowercased() == q }) { return g }
        if let appid = Int(q), let g = games.first(where: { $0.steamAppId == appid }) { return g }
        if let g = games.first(where: { $0.name.lowercased() == q || $0.profileID == q }) { return g }
        let partial = games.filter { $0.name.lowercased().contains(q) }
        return partial.count == 1 ? partial[0] : nil
    }

    public func profile(for game: Game) -> Profile {
        profiles.profiles.first { $0.id == game.profileID } ?? profiles.match(steamAppId: game.steamAppId, exe: game.exe)
    }
}
