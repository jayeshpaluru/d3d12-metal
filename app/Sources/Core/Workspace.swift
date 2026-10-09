// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// The entry point of Core: configuration, profiles, runtime, bottles and the operations on games.
public final class Workspace: @unchecked Sendable {
    public let paths: Paths
    public private(set) var config: Config
    public private(set) var profiles: ProfileDB
    public private(set) var profileProblems: [String] = []

    public init(paths: Paths = .default) {
        self.paths = paths
        self.config = Config.load(paths)
        self.profiles = ProfileDB(profiles: [ProfileDB.builtinGeneric])
        reloadProfiles()
    }

    // MARK: Configuration

    public func updateConfig(_ change: (inout Config) -> Void) throws {
        change(&config)
        try config.save(paths)
    }

    /// Bundled profiles first, then the source tree's, then the user's (which override by id).
    public static func profileDirectories(paths: Paths) -> [URL] {
        var dirs: [URL] = []
        if let r = Bundle.main.resourceURL { dirs.append(r.appendingPathComponent("profiles")) }
        if let env = ProcessInfo.processInfo.environment["D3D12METAL_PROFILES"], !env.isEmpty { dirs.append(URL(fileURLWithPath: env)) }
        if let root = SourceTree.find() { dirs.append(root.appendingPathComponent("profiles")) }
        dirs.append(paths.userProfilesDir)
        return dirs
    }

    public func reloadProfiles() {
        var problems: [String] = []
        profiles = ProfileDB.load(directories: Self.profileDirectories(paths: paths), problems: &problems)
        profileProblems = problems
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
            throw CoreError("No layer build found", hint: "Build it with tools/build-wine.sh, or set D3D12METAL_LAYER_DIR")
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

    // MARK: Requirements (Steam's installscript equivalents)

    /// Requirements of the game's profile that are not done yet.
    public func pendingRequirements(for game: Game) -> [Profile.Requirement] {
        guard let session = try? session(bottleName: game.bottle) else { return [] }
        return library.profile(for: game).requirements.filter { r in
            guard let key = r.doneKey else { return true }
            return !session.regKeyExists(key)
        }
    }

    public func runRequirement(_ r: Profile.Requirement, for game: Game) throws {
        let session = try session(bottleName: game.bottle)
        guard let first = r.run.first else { return }
        let target = game.gameDir.appendingPathComponent(first)
        guard FileManager.default.fileExists(atPath: target.path) else {
            throw CoreError("\(first) (needed for \"\(r.title)\") is not in the game folder", hint: "Let Steam finish installing or verify the game files")
        }
        var args: [String]
        switch target.pathExtension.lowercased() {
        case "bat", "cmd": args = ["cmd", "/c"] + r.run
        case "msi": args = ["msiexec", "/i", first] + Array(r.run.dropFirst())
        default: args = r.run
        }
        let res = session.run(args, cwd: game.gameDir, timeout: 900)
        if !res.ok {
            if r.optional { return }
            throw CoreError("\"\(r.title)\" failed (exit \(res.status)): \(res.stderr.suffix(300))")
        }
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
        if !Shell.pids(named: game.exe).isEmpty { throw CoreError("\(game.name) is already running", hint: "`\(AppInfo.cliName) stop \(game.id)`") }

        if install {
            let st = layerStatus(for: game)
            if !st.installed || !st.upToDate {
                progress(st.installed ? "Updating the layer" : "Installing the layer")
                try installLayer(for: game)
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
            if !SteamClient.isRunning {
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

    func gamePIDs(_ game: Game) -> [Int32] {
        var pids = Shell.pids(named: game.exe)
        if pids.isEmpty {
            let escaped = NSRegularExpression.escapedPattern(for: game.exe)
            pids = Shell.pids(matching: "[\\\\\\\\/]\(escaped)( |$)")
        }
        return pids
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
