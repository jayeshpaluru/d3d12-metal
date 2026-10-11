// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// The entry point of Core: configuration, profiles, runtime, bottles and the operations on games.
public final class Workspace: @unchecked Sendable {
    public let paths: Paths
    // The app refreshes from background tasks while views read these: every access goes through the lock.
    private let lock = NSLock()
    private var _config: Config
    private var _profiles: ProfileDB
    private var _profileProblems: [String] = []
    private var _gameCache: [String: (signature: String, games: [Game])] = [:]

    public var config: Config { lock.lock(); defer { lock.unlock() }; return _config }
    public var profiles: ProfileDB { lock.lock(); defer { lock.unlock() }; return _profiles }
    public var profileProblems: [String] { lock.lock(); defer { lock.unlock() }; return _profileProblems }

    public init(paths: Paths = .default) {
        self.paths = paths
        self._config = Config.load(paths)
        self._profiles = ProfileDB(profiles: [ProfileDB.builtinGeneric])
        reloadProfiles()
    }

    // MARK: Configuration

    public func updateConfig(_ change: (inout Config) -> Void) throws {
        lock.lock(); defer { lock.unlock() }
        change(&_config)
        try _config.save(paths)
    }

    /// Bundled profiles first, then the source tree's, then the user's (which override by id).
    public static func profileDirectories(paths: Paths) -> [URL] {
        var dirs: [URL] = []
        dirs += ResourceLocator.urls("profiles")
        if let env = AlloyEnv.get("PROFILES") { dirs.append(URL(fileURLWithPath: env)) }
        if let root = SourceTree.find() { dirs.append(root.appendingPathComponent("profiles")) }
        dirs.append(paths.userProfilesDir)
        return dirs
    }

    public func reloadProfiles() {
        var problems: [String] = []
        let db = ProfileDB.load(directories: Self.profileDirectories(paths: paths), problems: &problems)   // off the lock: it reads files
        lock.lock(); defer { lock.unlock() }
        _profiles = db
        _profileProblems = problems
    }

    /// Every game of every bottle plus the hand-added ones. The scan of a bottle's Steam library (reading the executables'
    /// import tables) is repeated only when its manifests or the profiles changed, or with `forceRescan`.
    public func allGames(forceRescan: Bool = false) -> [Game] {
        let lib = library
        let profileSignature = lib.profiles.profiles.map { "\($0.id)/\($0.exe ?? "")/\($0.match.steamAppId ?? 0)" }.joined(separator: ",")
        var games: [Game] = []
        for b in bottles.list() {
            let signature = SteamLibrary.signature(prefix: b.prefix) + "|" + profileSignature
            lock.lock(); let cached = _gameCache[b.name]; lock.unlock()
            if !forceRescan, let cached, cached.signature == signature {
                games += cached.games
            } else {
                let found = lib.steamGames(in: b)
                lock.lock(); _gameCache[b.name] = (signature, found); lock.unlock()
                games += found
            }
        }
        for g in lib.customGames() where !games.contains(where: { $0.id == g.id }) { games.append(g) }
        return games
    }

    public var library: GameLibrary { GameLibrary(paths: paths, profiles: profiles) }
    public var bottles: BottleStore { BottleStore(paths: paths) }
    public var layerBuild: LayerBuild? { LayerBuild.locate(paths: paths, config: config) }

    public func runtime() throws -> WineRuntime {
        guard let rt = RuntimeManager.locate(paths: paths, config: config) else {
            throw CoreError("No Wine found", hint: "Run `\(AppInfo.cliName) runtime install` (or set WINE_ROOT to a directory with bin/wine)")
        }
        return rt
    }

    public func session(bottleName: String) throws -> WineSession {
        guard let b = bottles.bottle(named: bottleName) else {
            throw CoreError("No bottle named \"\(bottleName)\"", hint: "`\(AppInfo.cliName) bottle list`, or create one with `\(AppInfo.cliName) bottle create \(bottleName)`")
        }
        return WineSession(runtime: try runtime(), bottle: b)
    }

    public func steam(bottleName: String) throws -> SteamClient {
        SteamClient(session: try session(bottleName: bottleName), paths: paths)
    }

    // MARK: Game operations

    public func settings(for game: Game) -> GameSettings { GameSettings.load(gameID: game.id, paths: paths) }

    public func saveSettings(_ s: GameSettings, for game: Game) throws {
        try s.save(gameID: game.id, paths: paths)
        if LayerInstaller.status(gameDir: game.gameDir, build: nil).installed {
            try LayerInstaller.updateConf(gameDir: game.gameDir, conf: confText(for: game, settings: s))
        }
    }

    public func confText(for game: Game, settings: GameSettings) -> String {
        ConfBuilder.render(profile: library.profile(for: game), settings: settings, logDir: paths.logDir(for: game.id))
    }

    public func layerStatus(for game: Game) -> LayerStatus {
        LayerInstaller.status(gameDir: game.gameDir, build: layerBuild)
    }

    @discardableResult
    public func installLayer(for game: Game, dryRun: Bool = false) throws -> [String] {
        guard let build = layerBuild else {
            throw CoreError("No layer build found", hint: "Build it with tools/build-wine.sh, or set ALLOY_LAYER_DIR")
        }
        let profile = library.profile(for: game)
        let settings = self.settings(for: game)
        try FileManager.default.createDirectory(at: paths.logDir(for: game.id), withIntermediateDirectories: true)
        let session = try? self.session(bottleName: game.bottle)
        return try LayerInstaller.install(gameDir: game.gameDir, exe: game.exe, build: build,
                                          conf: confText(for: game, settings: settings),
                                          extraOverrides: profile.dllOverrides, pciKey: settings.gpuIdentity.pciKey,
                                          session: session, dryRun: dryRun)
    }

    @discardableResult
    public func uninstallLayer(for game: Game, dryRun: Bool = false) throws -> [String] {
        try LayerInstaller.uninstall(gameDir: game.gameDir, session: try? session(bottleName: game.bottle), dryRun: dryRun)
    }

    // MARK: Launch / stop

    public struct LaunchResult: Codable, Sendable {
        public var mode: String
        public var logDir: String
        public var pid: Int32?
        public var notes: [String]
    }

    /// Everything the game's process should see in its environment.
    public func environment(for game: Game, settings: GameSettings) -> [String: String] {
        var env = library.profile(for: game).launch.env
        for (k, v) in settings.customEnv { env[k] = v }
        return env
    }

    public func launchArguments(for game: Game, settings: GameSettings, extra: [String]) -> [String] {
        library.profile(for: game).launch.args + settings.argumentList + extra
    }

    @discardableResult
    public func launch(_ game: Game, extraArgs: [String] = [], install: Bool = true, runRequirements: Bool = true,
                       progress: @escaping @Sendable (String) -> Void = { _ in }) throws -> LaunchResult {
        let profile = library.profile(for: game)
        let settings = self.settings(for: game)
        let session = try session(bottleName: game.bottle)
        var notes: [String] = []
        if isRunning(game) { throw CoreError("\(game.name) is already running", hint: "`\(AppInfo.cliName) stop \(game.id)`") }

        if install {
            let st = layerStatus(for: game)
            if !st.installed || !st.upToDate {
                progress(st.installed ? "Updating the layer" : "Installing the layer")
                try installLayer(for: game)
            } else {
                // Keeps the conf (log paths, profile keys) current when the data folder moved or a profile changed.
                try? LayerInstaller.updateConf(gameDir: game.gameDir, conf: confText(for: game, settings: settings))
            }
        }
        if runRequirements {
            for r in pendingRequirements(for: game) {
                progress("First launch setup: \(r.title)")
                try runRequirement(r, for: game)
            }
        }

        let logDir = paths.logDir(for: game.id)
        try FileManager.default.createDirectory(at: logDir, withIntermediateDirectories: true)
        let layerLog = logDir.appendingPathComponent("d3d12metal.log")
        for name in ["d3d12metal.log", "d3d12metal.log.methods"] {
            let f = logDir.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: f.path) {
                let prev = logDir.appendingPathComponent(name + ".prev")
                try? FileManager.default.removeItem(at: prev)
                try? FileManager.default.moveItem(at: f, to: prev)
            }
        }
        FileManager.default.createFile(atPath: layerLog.path, contents: nil)

        var env = environment(for: game, settings: settings)
        let args = launchArguments(for: game, settings: settings, extra: extraArgs)
        let wineLog = logDir.appendingPathComponent("wine.log")

        if profile.launch.mode == "steam", let appid = game.steamAppId {
            let steam = SteamClient(session: session, paths: paths)
            guard steam.isInstalled else { throw CoreError("Steam is not installed in bottle \"\(game.bottle)\"", hint: "`\(AppInfo.cliName) steam install`") }
            if !steam.isRunning {
                progress("Starting Steam")
                _ = try steam.start(shim: layerBuild?.webhelperShim, extraEnv: env, logFile: paths.logsDir.appendingPathComponent("steam-\(Paths.safeName(game.bottle)).log"))
                Thread.sleep(forTimeInterval: 45)   // the client must be up before -applaunch is forwarded to it
                notes.append("Steam was not running: it was started with the game's environment.")
            } else if !env.isEmpty {
                notes.append("Steam was already running: the profile's environment (\(env.keys.sorted().joined(separator: ", "))) is only inherited if Steam was started by \(AppInfo.name).")
            }
            progress("Launching \(game.name) through Steam")
            let p = try steam.applaunch(appid: appid, args: args)
            return LaunchResult(mode: "steam", logDir: logDir.path, pid: p.processIdentifier, notes: notes)
        }

        if let appid = game.steamAppId { env["SteamAppId"] = String(appid); env["SteamGameId"] = String(appid) }
        progress("Launching \(game.name)")
        let p = try session.spawn([game.exe] + args, cwd: game.gameDir, extraEnv: env, logFile: wineLog)
        return LaunchResult(mode: "direct", logDir: logDir.path, pid: p.processIdentifier, notes: notes)
    }

    public func isRunning(_ game: Game) -> Bool { !gamePIDs(game).isEmpty }

    /// Whether Steam runs in a bottle (not in another one).
    public func steamRunning(bottleName: String) -> Bool {
        bottles.bottle(named: bottleName).map { SteamClient.isRunning(in: $0) } ?? false
    }

    /// (pid, command line) of every process: one `ps` for any number of games.
    static func processTable() -> [(pid: Int32, command: String)] {
        Shell.run("/bin/ps", ["-axww", "-o", "pid=,command="], timeout: 10).stdout.split(separator: "\n").compactMap { line in
            let s = line.drop { $0 == " " }
            guard let space = s.firstIndex(of: " "), let pid = Int32(s[..<space]) else { return nil }
            return (pid, String(s[s.index(after: space)...]))
        }
    }

    /// Wine names a process after its exe ("Spider-Man2.exe -nolauncher"), or shows a Windows path ("C:\\...\\game.exe args").
    static func commandRuns(_ command: String, exe: String) -> Bool {
        let c = command.lowercased(), e = exe.lowercased()
        if c == e || c.hasPrefix(e + " ") { return true }
        return ["\\", "/"].contains { c.hasSuffix($0 + e) || c.contains($0 + e + " ") }
    }

    /// The game's processes in its own bottle: a process of the same exe in another bottle is not this game.
    func gamePIDs(_ game: Game, table: [(pid: Int32, command: String)]? = nil) -> [Int32] {
        let rows = table ?? Self.processTable()
        let me = getpid()
        let candidates = rows.filter { $0.pid != me && Self.commandRuns($0.command, exe: game.exe) }.map(\.pid)
        return ProcessScan.filter(candidates, in: bottles.bottle(named: game.bottle))
    }

    /// The ids of the games that run, from a single process listing (the lsof check is only made for processes that look like a game).
    public func runningGameIDs(_ games: [Game]) -> Set<String> {
        let table = Self.processTable()
        return Set(games.filter { !gamePIDs($0, table: table).isEmpty }.map(\.id))
    }

    /// Stops just the game (never wineserver, so Steam keeps running).
    @discardableResult
    public func stop(_ game: Game) -> Bool {
        var pids = gamePIDs(game)
        if pids.isEmpty { return false }
        for p in pids { kill(p, SIGTERM) }
        for _ in 0..<10 {
            Thread.sleep(forTimeInterval: 1)
            pids = gamePIDs(game)
            if pids.isEmpty { return true }
        }
        for p in pids { kill(p, SIGKILL) }
        return true
    }
}
