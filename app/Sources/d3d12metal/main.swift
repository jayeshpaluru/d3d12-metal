// SPDX-License-Identifier: LGPL-2.1-or-later
import Core
import Foundation

// MARK: - Argument parsing

struct Args {
    var positional: [String] = []
    var options: [String: String] = [:]
    var flags: Set<String> = []
    var passthrough: [String] = []

    /// Options that take a value; everything else starting with "--" is a flag.
    static let valueOptions: Set<String> = [
        "bottle", "prefix", "name", "folder", "exe", "appid", "version", "output", "lines", "hud", "gpu", "feature-level", "gdeflate",
        "log", "env", "args", "wine-root", "timeout", "profile",
    ]

    init(_ argv: [String]) {
        var i = 0
        while i < argv.count {
            let a = argv[i]
            if a == "--" { passthrough = Array(argv[(i + 1)...]); break }
            if a.hasPrefix("--") {
                var key = String(a.dropFirst(2))
                if let eq = key.firstIndex(of: "=") {
                    options[String(key[..<eq])] = String(key[key.index(after: eq)...])
                } else if Self.valueOptions.contains(key), i + 1 < argv.count {
                    i += 1
                    options[key] = argv[i]
                } else {
                    key = key.lowercased()
                    flags.insert(key)
                }
            } else if a == "-h" {
                flags.insert("help")
            } else {
                positional.append(a)
            }
            i += 1
        }
    }

    func has(_ flag: String) -> Bool { flags.contains(flag) }
    func option(_ name: String) -> String? { options[name] }
}

// MARK: - Output

var jsonMode = false

func emit<T: Encodable>(_ value: T, text: @autoclosure () -> String) {
    if jsonMode {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        print(String(decoding: (try? enc.encode(value)) ?? Data("{}".utf8), as: UTF8.self))
    } else {
        let t = text()
        if !t.isEmpty { print(t) }
    }
}

func info(_ s: String) {
    // Progress goes to stderr so --json output stays clean.
    FileHandle.standardError.write(Data((s + "\n").utf8))
}

struct Message: Encodable { var message: String; var details: [String]? }

func fail(_ message: String, hint: String? = nil, code: Int32 = 1) -> Never {
    if jsonMode {
        let obj: [String: String] = ["error": message, "hint": hint ?? ""]
        let data = (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data()
        print(String(decoding: data, as: UTF8.self))
    } else {
        FileHandle.standardError.write(Data(("error: " + message + "\n").utf8))
        if let hint { FileHandle.standardError.write(Data(("  hint: " + hint + "\n").utf8)) }
    }
    exit(code)
}

func run<T>(_ body: () throws -> T) -> T {
    do { return try body() } catch let e as CoreError { fail(e.message, hint: e.hint) } catch { fail(error.localizedDescription) }
}

func capture<T>(_ body: () async throws -> T) async -> Result<T, Error> {
    do { return .success(try await body()) } catch { return .failure(error) }
}

func pad(_ s: String, _ n: Int) -> String { s.count >= n ? s : s + String(repeating: " ", count: n - s.count) }

// MARK: - Help

let topHelp = """
\(AppInfo.name) \(AppInfo.version): run Windows D3D12 games on Apple Silicon (Direct3D 12 on Metal, under Wine).

Usage: \(AppInfo.cliName) [--json] <command> [options]

Setup
  setup [--bottle NAME]            Install Wine, create the default bottle, install Steam (guided; asks nothing)
  doctor                           Check Rosetta, AVX, GPU, Metal Shader Converter, Wine, layer build, Steam
  runtime status|install           The Wine build (pinned release, SHA-256 verified)
  bottle list|create|add|remove    Wine prefixes ("bottles") that hold Steam and games
  steam install|start|stop|status  The Windows Steam client in a bottle

Games
  library [--all]                  Games found in the bottles (Steam) and added by hand
  scan <folder>                    List the executables in a folder and which ones use D3D12
  game add|remove                  Add a non-Steam game (folder + exe) or forget it
  info <game>                      Profile, layer status, settings, paths of a game
  install|update|uninstall <game>  The layer in the game's folder (backs up files, undo with uninstall)
  config <game> [options]          Show or change the game's settings
  launch <game> [-- args]          Install if needed, run first-launch setup, start the game
  stop <game>                      Stop just the game (Steam keeps running)
  logs <game> [--lines N]          Print the end of the layer log
  report [game] [--output DIR]     Write a zip of logs and settings for a bug report (personal paths scrubbed)
  profiles [list|show ID]          The per-game profiles

<game> is a Steam app id, a name (or part of it), or an id like steam:1817070 / custom:my-game.
--json prints machine-readable output; progress and errors go to stderr.
Run `\(AppInfo.cliName) <command> --help` for a command.
"""

let helps: [String: String] = [
    "setup": "setup [--bottle NAME] [--no-steam]\n  Downloads Wine (\(AppInfo.defaultWineVersion)) if missing, creates the bottle (default \"Steam\") and installs Steam into it.\n  Then run `steam start` and sign in. Metal Shader Converter must be installed from Apple (see `doctor`).",
    "doctor": "doctor\n  Checks the system and prints what to fix. Exit status 1 if anything is an error.",
    "runtime": "runtime status\nruntime install [--version V]\n  Downloads the Gcenx Wine build and verifies its SHA-256 against the pinned value.",
    "bottle": "bottle list\nbottle create NAME\nbottle add NAME --prefix DIR   register an existing Wine prefix\nbottle remove NAME [--delete-files]",
    "steam": "steam install|start|stop|status [--bottle NAME] [--force]\n  start: Steam with the in-process-gpu webhelper shim and ROSETTA_ADVERTISE_AVX=1. stop: asks Steam to exit (--force: wineserver -k for that bottle only).",
    "library": "library [--bottle NAME] [--all]\n  Lists games. --all also lists Steam apps without a D3D12 executable.",
    "scan": "scan FOLDER\n  Lists .exe files up to three levels down and whether they link d3d12.dll (or mention D3D12CreateDevice).",
    "game": "game add --name N --folder DIR --exe FILE [--bottle B] [--appid ID]\ngame remove ID",
    "info": "info GAME",
    "install": "install GAME [--dry-run]   (update and install are the same: files that differ are replaced, backups kept)",
    "update": "update GAME [--dry-run]",
    "uninstall": "uninstall GAME [--dry-run]\n  Restores backed-up files, removes ours, removes the registry overrides.",
    "config": "config GAME [--hud on|off] [--gpu apple|amd|nvidia] [--feature-level 12_0|12_1] [--gdeflate on|off]\n            [--log off|stats|trace|verbose] [--env KEY=VALUE]... [--args \"...\"] [--reset]\n  Without options: prints the settings. Changes are written to d3d12metal.conf when the layer is installed.",
    "launch": "launch GAME [--no-install] [--no-setup] [--wait] [-- game arguments]",
    "stop": "stop GAME",
    "logs": "logs GAME [--lines N] [--path]\n  --path prints the log directory instead.",
    "report": "report [GAME] [--output DIR]",
    "profiles": "profiles [list]\nprofiles show ID",
]

// MARK: - Commands

let argv = Array(CommandLine.arguments.dropFirst())
var rest = argv
if let i = rest.firstIndex(of: "--json") { jsonMode = true; rest.remove(at: i) }
guard let command = rest.first else { print(topHelp); exit(0) }
if command == "help" || command == "--help" || command == "-h" {
    if rest.count > 1, let h = helps[rest[1]] { print("Usage: \(AppInfo.cliName) \(h)") } else { print(topHelp) }
    exit(0)
}
if command == "--version" || command == "version" { print("\(AppInfo.cliName) \(AppInfo.version)"); exit(0) }
let args = Args(Array(rest.dropFirst()))
if args.has("help") {
    if let h = helps[command] { print("Usage: \(AppInfo.cliName) \(h)") } else { print(topHelp) }
    exit(0)
}

let ws = Workspace()
func bottleName() -> String { args.option("bottle") ?? ws.config.defaultBottle }

func resolveGame(_ idx: Int = 0) -> Game {
    guard args.positional.count > idx else { fail("missing <game>", hint: "`\(AppInfo.cliName) library` lists the games") }
    guard let g = ws.library.find(args.positional[idx]) else {
        fail("no game matches \"\(args.positional[idx])\"", hint: "`\(AppInfo.cliName) library --all` lists the games; add one with `game add`")
    }
    return g
}

struct GameRow: Encodable {
    var id: String, name: String, bottle: String, exe: String, folder: String, profile: String
    var layerInstalled: Bool, layerUpToDate: Bool, running: Bool
}

func row(_ g: Game) -> GameRow {
    let st = ws.layerStatus(for: g)
    return GameRow(id: g.id, name: g.name, bottle: g.bottle, exe: g.exe, folder: g.gameDirPath, profile: g.profileID,
                   layerInstalled: st.installed, layerUpToDate: st.upToDate, running: ws.isRunning(g))
}

switch command {
case "doctor":
    let checks = Doctor.runAll(ws)
    emit(checks, text: checks.map { c in
        let mark = ["ok": "[ ok ]", "warning": "[warn]", "error": "[FAIL]", "info": "[info]"][c.level.rawValue] ?? "[?]"
        return "\(mark) \(c.title): \(c.detail)" + (c.fix.map { "\n         fix: \($0)" } ?? "")
    }.joined(separator: "\n"))
    if checks.contains(where: { $0.level == .error }) { exit(1) }

case "runtime":
    switch args.positional.first ?? "status" {
    case "status":
        let rt = RuntimeManager.locate(paths: ws.paths, config: ws.config)
        struct S: Encodable { var installed: Bool; var path: String?; var version: String?; var pinned: String }
        emit(S(installed: rt != nil, path: rt?.root.path, version: rt?.version(), pinned: ws.config.wineVersion),
             text: rt.map { "Wine: \($0.version() ?? "?")\n  \($0.root.path)" } ?? "Wine is not installed. Run `\(AppInfo.cliName) runtime install`.")
    case "install":
        let v = args.option("version") ?? ws.config.wineVersion
        guard let rel = WineRelease.release(version: v) else {
            fail("no pinned release for Wine \(v)", hint: "Known: \(WineRelease.known.map(\.version).joined(separator: ", ")). For another build set WINE_ROOT or `config`")
        }
        let sem = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var result: Result<WineRuntime, Error>!
        Task {
            let last = Box()
            result = await capture {
                try await RuntimeManager.install(rel, paths: ws.paths) { p, stage in
                    let pct = Int(p * 100)
                    if pct != last.v && pct % 5 == 0 { last.v = pct; info("\(stage) \(pct)%") }
                }
            }
            sem.signal()
        }
        sem.wait()
        let rt = run { try result.get() }
        try? ws.updateConfig { $0.wineVersion = v }
        emit(Message(message: "installed", details: [rt.root.path]), text: "Wine \(v) installed at \(rt.root.path)")
    default: fail("unknown runtime command", hint: helps["runtime"])
    }

case "bottle":
    switch args.positional.first ?? "list" {
    case "list":
        let list = ws.bottles.list()
        emit(list, text: list.isEmpty ? "No bottles. `\(AppInfo.cliName) setup` creates one." : list.map { "\(pad($0.name, 16)) \($0.prefixPath)\($0.managed ? "" : "  (existing prefix)")" }.joined(separator: "\n"))
    case "create":
        guard args.positional.count > 1 else { fail("missing bottle name") }
        let rt = run { try ws.runtime() }
        info("Creating the prefix (first start of Wine, up to a minute)...")
        let b = run { try ws.bottles.create(name: args.positional[1], runtime: rt) }
        emit(b, text: "Created bottle \"\(b.name)\" at \(b.prefixPath)")
    case "add":
        guard args.positional.count > 1, let p = args.option("prefix") else { fail("usage: bottle add NAME --prefix DIR") }
        let b = run { try ws.bottles.add(name: args.positional[1], existingPrefix: URL(fileURLWithPath: (p as NSString).expandingTildeInPath)) }
        emit(b, text: "Added bottle \"\(b.name)\" -> \(b.prefixPath)")
    case "remove":
        guard args.positional.count > 1 else { fail("missing bottle name") }
        run { try ws.bottles.remove(name: args.positional[1], deleteFiles: args.has("delete-files")) }
        emit(Message(message: "removed", details: nil), text: "Removed bottle \"\(args.positional[1])\"")
    default: fail("unknown bottle command", hint: helps["bottle"])
    }

case "setup":
    let name = bottleName()
    if RuntimeManager.locate(paths: ws.paths, config: ws.config) == nil {
        guard let rel = WineRelease.release(version: ws.config.wineVersion) else { fail("no pinned Wine release") }
        info("Downloading Wine \(rel.version)...")
        let sem = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var result: Result<WineRuntime, Error>!
        Task {
            let last = Box()
            result = await capture {
                try await RuntimeManager.install(rel, paths: ws.paths) { p, stage in
                    let pct = Int(p * 100)
                    if pct != last.v && pct % 10 == 0 { last.v = pct; info("\(stage) \(pct)%") }
                }
            }
            sem.signal()
        }
        sem.wait()
        _ = run { try result.get() }
    }
    let rt = run { try ws.runtime() }
    if ws.bottles.bottle(named: name) == nil {
        info("Creating bottle \"\(name)\"...")
        _ = run { try ws.bottles.create(name: name, runtime: rt) }
    }
    if !args.has("no-steam") {
        let steam = run { try ws.steam(bottleName: name) } as SteamClient
        if !steam.isInstalled {
            let sem = DispatchSemaphore(value: 0)
            nonisolated(unsafe) var result: Result<Void, Error>!
            Task {
                result = await capture { try await steam.install { _, stage in info(stage) } }
                sem.signal()
            }
            sem.wait()
            run { try result.get() }
        }
    }
    try? ws.updateConfig { $0.setupDone = true }
    let checks = Doctor.runAll(ws)
    emit(checks, text: "Setup finished. Next: `\(AppInfo.cliName) steam start`, sign in to Steam, install your games.\n"
        + checks.filter { $0.level == .error }.map { "still to do: \($0.title): \($0.fix ?? $0.detail)" }.joined(separator: "\n"))

case "steam":
    let sub = args.positional.first ?? "status"
    let steam = run { try ws.steam(bottleName: bottleName()) } as SteamClient
    switch sub {
    case "install":
        let sem = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var result: Result<Void, Error>!
        Task { result = await capture { try await steam.install { _, stage in info(stage) } }; sem.signal() }
        sem.wait()
        run { try result.get() }
        emit(Message(message: "installed", details: nil), text: "Steam installed. `\(AppInfo.cliName) steam start`")
    case "start":
        let p = run { try steam.start(shim: ws.layerBuild?.webhelperShim, logFile: ws.paths.logsDir.appendingPathComponent("steam-\(Paths.safeName(bottleName())).log")) }
        emit(Message(message: "started", details: ["pid \(p.processIdentifier)"]), text: "Steam is starting (pid \(p.processIdentifier)). The window opens in about 30 seconds.")
    case "stop":
        if args.has("force") { steam.forceStop() } else { steam.stop() }
        emit(Message(message: "stopped", details: nil), text: "Asked Steam to exit.")
    case "status":
        struct S: Encodable { var installed: Bool; var running: Bool; var bottle: String }
        emit(S(installed: steam.isInstalled, running: SteamClient.isRunning, bottle: bottleName()),
             text: "Steam in \"\(bottleName())\": \(steam.isInstalled ? "installed" : "not installed"), \(SteamClient.isRunning ? "running" : "not running")")
    default: fail("unknown steam command", hint: helps["steam"])
    }

case "library", "list":
    var games: [Game] = []
    if let b = args.option("bottle") {
        guard let bottle = ws.bottles.bottle(named: b) else { fail("no bottle named \(b)") }
        games = ws.library.steamGames(in: bottle, includeNonD3D12: args.has("all")) + ws.library.customGames().filter { $0.bottle == b }
    } else {
        for b in ws.bottles.list() { games += ws.library.steamGames(in: b, includeNonD3D12: args.has("all")) }
        games += ws.library.customGames()
    }
    let rows = games.map(row)
    emit(rows, text: rows.isEmpty ? "No games found. Install games in Steam (`steam start`) or `game add`." :
        rows.map { "\(pad($0.id, 18)) \(pad($0.name, 36)) \($0.layerInstalled ? ($0.layerUpToDate ? "layer ok " : "layer old") : "no layer ") \($0.running ? "running" : "")" }.joined(separator: "\n"))

case "scan":
    guard let f = args.positional.first else { fail("usage: scan FOLDER") }
    struct C: Encodable { var exe: String; var path: String; var usesD3D12: Bool; var size: Int64 }
    let list = ExeFinder.candidates(in: URL(fileURLWithPath: (f as NSString).expandingTildeInPath)).map { C(exe: $0.name, path: $0.url.path, usesD3D12: $0.usesD3D12, size: $0.size) }
    emit(list, text: list.isEmpty ? "No executables found." : list.map { "\($0.usesD3D12 ? "D3D12" : "     ") \($0.path)" }.joined(separator: "\n"))

case "game":
    switch args.positional.first ?? "" {
    case "add":
        guard let name = args.option("name"), let folder = args.option("folder"), let exe = args.option("exe") else {
            fail("usage: game add --name N --folder DIR --exe FILE [--bottle B] [--appid ID]")
        }
        let g = run { try ws.library.addCustom(name: name, bottle: bottleName(), folder: URL(fileURLWithPath: (folder as NSString).expandingTildeInPath), exe: exe, steamAppId: args.option("appid").flatMap(Int.init)) }
        emit(g, text: "Added \(g.name) as \(g.id) (profile: \(g.profileID))")
    case "remove":
        guard args.positional.count > 1 else { fail("usage: game remove ID") }
        run { try ws.library.removeCustom(id: args.positional[1]) }
        emit(Message(message: "removed", details: nil), text: "Removed")
    default: fail("usage: game add|remove", hint: helps["game"])
    }

case "info":
    let g = resolveGame()
    let p = ws.library.profile(for: g)
    struct I: Encodable { var game: GameRow; var profile: Profile; var settings: GameSettings; var layer: LayerStatus; var logDir: String; var pendingSetup: [String] }
    let st = ws.layerStatus(for: g)
    let i = I(game: row(g), profile: p, settings: ws.settings(for: g), layer: st, logDir: ws.paths.logDir(for: g.id).path,
              pendingSetup: ws.pendingRequirements(for: g).map(\.title))
    var t = "\(g.name)  [\(g.id)]\n  folder:  \(g.gameDirPath)\n  exe:     \(g.exe)\n  bottle:  \(g.bottle)\n  profile: \(p.id) (\(p.status))\n"
    t += "  layer:   \(st.installed ? "installed \(st.installedVersion ?? "?")\(st.upToDate ? "" : " (update available)")" : "not installed")\n"
    t += "  logs:    \(i.logDir)\n"
    for n in p.notes { t += "  note: \(n)\n" }
    emit(i, text: t)

case "install", "update":
    let g = resolveGame()
    let log = run { try ws.installLayer(for: g, dryRun: args.has("dry-run")) }
    emit(log, text: log.joined(separator: "\n"))

case "uninstall":
    let g = resolveGame()
    let log = run { try ws.uninstallLayer(for: g, dryRun: args.has("dry-run")) }
    emit(log, text: log.joined(separator: "\n"))

case "config":
    let g = resolveGame()
    var s = args.has("reset") ? GameSettings() : ws.settings(for: g)
    var changed = args.has("reset")
    func onOff(_ v: String) -> Bool { ["on", "1", "true", "yes"].contains(v.lowercased()) }
    if let v = args.option("hud") { s.hud = onOff(v); changed = true }
    if let v = args.option("gpu") { guard let id = GPUIdentity(rawValue: v) else { fail("--gpu must be apple, amd or nvidia") }; s.gpuIdentity = id; changed = true }
    if let v = args.option("feature-level") { s.featureLevel121 = (v == "12_1"); changed = true }
    if let v = args.option("gdeflate") { s.gdeflate = onOff(v); changed = true }
    if let v = args.option("log") { guard let l = LogLevel(rawValue: v) else { fail("--log must be off, stats, trace or verbose") }; s.logLevel = l; changed = true }
    if let v = args.option("args") { s.customArgs = v; changed = true }
    if let v = args.option("env") {
        let parts = v.split(separator: "=", maxSplits: 1).map(String.init)
        guard parts.count == 2 else { fail("--env needs KEY=VALUE") }
        s.customEnv[parts[0]] = parts[1]; changed = true
    }
    if changed { run { try ws.saveSettings(s, for: g) } }
    emit(s, text: """
        hud: \(s.hud)
        gpu: \(s.gpuIdentity.rawValue)
        feature level 12_1: \(s.featureLevel121)
        gdeflate: \(s.gdeflate)
        log: \(s.logLevel.rawValue)
        env: \(s.customEnv.isEmpty ? "-" : s.customEnv.map { "\($0.key)=\($0.value)" }.sorted().joined(separator: " "))
        args: \(s.customArgs.isEmpty ? "-" : s.customArgs)
        """ + (changed && ws.layerStatus(for: g).installed ? "\n(d3d12metal.conf updated)" : changed ? "\n(takes effect when the layer is installed)" : ""))

case "launch":
    let g = resolveGame()
    let r = run { try ws.launch(g, extraArgs: args.passthrough, install: !args.has("no-install"), runRequirements: !args.has("no-setup")) { info($0) } }
    emit(r, text: "Started \(g.name) (\(r.mode)). Logs: \(r.logDir)\n" + r.notes.map { "note: \($0)" }.joined(separator: "\n"))
    if args.has("wait") {
        Thread.sleep(forTimeInterval: 20)
        while ws.isRunning(g) { Thread.sleep(forTimeInterval: 2) }
    }

case "stop":
    let g = resolveGame()
    let stopped = ws.stop(g)
    emit(Message(message: stopped ? "stopped" : "not running", details: nil), text: stopped ? "Stopped \(g.exe)" : "\(g.exe) is not running")

case "logs":
    let g = resolveGame()
    let dir = ws.paths.logDir(for: g.id)
    if args.has("path") { emit(Message(message: dir.path, details: nil), text: dir.path); break }
    let file = dir.appendingPathComponent("d3d12metal.log")
    guard let text = try? String(contentsOf: file, encoding: .utf8) else { fail("no log yet at \(file.path)", hint: "Launch the game first") }
    let n = Int(args.option("lines") ?? "") ?? 80
    let lines = text.split(separator: "\n", omittingEmptySubsequences: false).suffix(n).map(String.init)
    emit(lines, text: lines.joined(separator: "\n"))

case "report":
    let g: Game? = args.positional.isEmpty ? nil : resolveGame()
    let out = URL(fileURLWithPath: ((args.option("output") ?? FileManager.default.currentDirectoryPath) as NSString).expandingTildeInPath)
    let zip = run { try Report.create(ws: ws, game: g, outputDir: out) }
    emit(Message(message: zip.path, details: nil), text: "Report written to \(zip.path)\nPersonal paths, e-mail addresses and Steam ids are scrubbed; look inside before sharing.")

case "profiles":
    if args.positional.first == "show" {
        guard args.positional.count > 1, let p = ws.profiles.profiles.first(where: { $0.id == args.positional[1] }) else { fail("no such profile") }
        let enc = JSONEncoder(); enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        print(String(decoding: (try? enc.encode(p)) ?? Data(), as: UTF8.self))
    } else {
        let list = ws.profiles.profiles
        emit(list, text: list.map { "\(pad($0.id, 24)) \(pad($0.status, 10)) \($0.name)" }.joined(separator: "\n"))
    }

default:
    fail("unknown command \"\(command)\"", hint: "`\(AppInfo.cliName) --help`")
}

final class Box: @unchecked Sendable { var v = -1 }
