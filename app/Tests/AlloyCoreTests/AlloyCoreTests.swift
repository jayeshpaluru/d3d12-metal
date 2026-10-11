// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation
import Darwin
import Testing
@testable import AlloyCore

// MARK: - Helpers

func makeTempDir(_ name: String = "t") throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("d3d12metal-test-\(name)-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

/// A minimal 64-bit PE image whose import table lists `dlls`; the first DLL's thunk is by ordinal when `ordinal`.
func makePE(imports dlls: [String], ordinal: Bool = false) -> Data {
    var d = Data(count: 0x800)
    func w16(_ o: Int, _ v: UInt16) { d[o] = UInt8(v & 0xFF); d[o + 1] = UInt8(v >> 8) }
    func w32(_ o: Int, _ v: UInt32) { w16(o, UInt16(v & 0xFFFF)); w16(o + 2, UInt16(v >> 16)) }
    func w64(_ o: Int, _ v: UInt64) { w32(o, UInt32(v & 0xFFFF_FFFF)); w32(o + 4, UInt32(v >> 32)) }
    w16(0, 0x5A4D)
    w32(0x3C, 0x80)
    w32(0x80, 0x0000_4550)
    w16(0x80 + 4, 0x8664)          // machine
    w16(0x80 + 6, 1)               // sections
    w16(0x80 + 20, 240)            // optional header size
    let opt = 0x80 + 24
    w16(opt, 0x20B)
    w32(opt + 108, 16)             // number of data directories
    w32(opt + 112 + 8, 0x1000)     // import directory RVA
    w32(opt + 112 + 12, 0x100)
    let sec = opt + 240
    w32(sec + 8, 0x400)            // virtual size
    w32(sec + 12, 0x1000)          // virtual address
    w32(sec + 16, 0x400)           // raw size
    w32(sec + 20, 0x400)           // raw pointer
    var nameOffset = 0x400 + 0x100, thunkOffset = 0x400 + 0x300
    for (i, dll) in dlls.enumerated() {
        let desc = 0x400 + i * 20
        let nameRVA = 0x1000 + (nameOffset - 0x400)
        let thunkRVA = 0x1000 + (thunkOffset - 0x400)
        for (j, b) in dll.utf8.enumerated() { d[nameOffset + j] = b }
        w32(desc, UInt32(thunkRVA))            // OriginalFirstThunk
        w32(desc + 12, UInt32(nameRVA))
        w32(desc + 16, UInt32(thunkRVA))
        w64(thunkOffset, (i == 0 && ordinal) ? 0x8000_0000_0000_0000 | 101 : UInt64(nameRVA))
        nameOffset += 32
        thunkOffset += 16
    }
    return d
}

func writeExe(_ dir: URL, _ name: String, imports: [String], ordinal: Bool = false) throws {
    let url = dir.appendingPathComponent(name)
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try makePE(imports: imports, ordinal: ordinal).write(to: url)
}

// MARK: - VDF / acf

let sampleACF = """
"AppState"
{
\t"appid"\t\t"1817070"
\t"name"\t\t"Marvel\u{2019}s Spider-Man Remastered"
\t"StateFlags"\t\t"4"
\t"installdir"\t\t"Marvel's Spider-Man Remastered"
\t"SizeOnDisk"\t\t"70825441975"
\t"InstalledDepots"
\t{
\t\t"1817071"
\t\t{
\t\t\t"manifest"\t\t"123"
\t\t}
\t}
}
"""

@Suite struct SteamTests {
    @Test func parsesAppManifest() throws {
        let app = try #require(SteamLibrary.parseManifest(sampleACF, steamapps: URL(fileURLWithPath: "/x/steamapps")))
        #expect(app.appid == 1817070)
        #expect(app.installDir == "Marvel's Spider-Man Remastered")
        #expect(app.name.hasPrefix("Marvel"))
        #expect(app.fullyInstalled)
        #expect(app.sizeOnDisk == 70_825_441_975)
        #expect(app.gameFolder.path == "/x/steamapps/common/Marvel's Spider-Man Remastered")
    }

    @Test func downloadingAppIsNotFullyInstalled() throws {
        let text = sampleACF.replacingOccurrences(of: "\"StateFlags\"\t\t\"4\"", with: "\"StateFlags\"\t\t\"1026\"")
        let app = try #require(SteamLibrary.parseManifest(text, steamapps: URL(fileURLWithPath: "/x")))
        #expect(!app.fullyInstalled)
    }

    @Test func rejectsBrokenManifest() {
        #expect(SteamLibrary.parseManifest("\"AppState\" { \"name\" \"x\" }", steamapps: URL(fileURLWithPath: "/x")) == nil)
        #expect(SteamLibrary.parseManifest("garbage", steamapps: URL(fileURLWithPath: "/x")) == nil)
    }

    @Test func parsesLibraryFolders() {
        let text = """
        "libraryfolders"
        {
        \t"0" { "path" "C:\\\\Program Files (x86)\\\\Steam" "apps" { "1" "2" } }
        \t"1" { "path" "D:\\\\Games\\\\SteamLibrary" }
        }
        """
        #expect(SteamLibrary.parseLibraryFolders(text) == ["C:\\Program Files (x86)\\Steam", "D:\\Games\\SteamLibrary"])
    }

    @Test func mapsWindowsPaths() {
        let prefix = URL(fileURLWithPath: "/p")
        #expect(SteamLibrary.hostPath(forWindowsPath: "C:\\Program Files (x86)\\Steam", prefix: prefix).path == "/p/drive_c/Program Files (x86)/Steam")
        #expect(SteamLibrary.hostPath(forWindowsPath: "D:\\Games", prefix: prefix).path == "/p/dosdevices/d:/Games")
    }

    @Test func scansPrefixAndFindsD3D12Exe() throws {
        let prefix = try makeTempDir("prefix")
        defer { try? FileManager.default.removeItem(at: prefix) }
        let steamapps = SteamLibrary.steamDirectory(prefix: prefix).appendingPathComponent("steamapps")
        try FileManager.default.createDirectory(at: steamapps.appendingPathComponent("common/Cool Game/bin"), withIntermediateDirectories: true)
        try "\"AppState\"\n{\n\"appid\" \"777\"\n\"name\" \"Cool Game\"\n\"StateFlags\" \"4\"\n\"installdir\" \"Cool Game\"\n}\n"
            .write(to: steamapps.appendingPathComponent("appmanifest_777.acf"), atomically: true, encoding: .utf8)
        try writeExe(steamapps.appendingPathComponent("common/Cool Game"), "launcher.exe", imports: ["kernel32.dll"])
        try writeExe(steamapps.appendingPathComponent("common/Cool Game/bin"), "game.exe", imports: ["kernel32.dll", "D3D12.dll"])
        try writeExe(steamapps.appendingPathComponent("common/Cool Game/bin"), "unins000.exe", imports: ["D3D12.dll"])
        let paths = Paths(home: try makeTempDir("home"))
        let lib = GameLibrary(paths: paths, profiles: ProfileDB(profiles: [ProfileDB.builtinGeneric]))
        let games = lib.steamGames(in: Bottle(name: "t", prefixPath: prefix.path, managed: false))
        #expect(games.count == 1)
        #expect(games[0].exe == "game.exe")
        #expect(games[0].gameDir.lastPathComponent == "bin")
        #expect(games[0].id == "steam:777")
    }
}

// MARK: - PE

@Suite struct PETests {
    @Test func detectsImportByName() throws {
        let info = try #require(PEScanner.scan(data: makePE(imports: ["KERNEL32.dll", "D3D12.dll"])))
        #expect(info.is64Bit)
        #expect(info.importsD3D12)
        #expect(!info.importsD3D12ByOrdinal)
        #expect(info.imports.contains("kernel32.dll"))
    }

    @Test func detectsImportByOrdinal() throws {
        let info = try #require(PEScanner.scan(data: makePE(imports: ["d3d12.dll"], ordinal: true)))
        #expect(info.importsD3D12 && info.importsD3D12ByOrdinal)
    }

    @Test func noD3D12() throws {
        let info = try #require(PEScanner.scan(data: makePE(imports: ["d3d11.dll", "dxgi.dll"])))
        #expect(!info.importsD3D12)
    }

    @Test func rejectsNonPE() {
        #expect(PEScanner.scan(data: Data("hello world".utf8)) == nil)
        #expect(PEScanner.scan(data: Data(count: 10)) == nil)
        var bad = makePE(imports: ["d3d12.dll"])
        bad[0x3C] = 0xFF; bad[0x3D] = 0xFF   // e_lfanew far outside
        #expect(PEScanner.scan(data: bad) == nil)
    }

    @Test func truncatedFileDoesNotCrash() {
        let full = makePE(imports: ["d3d12.dll"])
        for n in stride(from: 0, to: full.count, by: 37) { _ = PEScanner.scan(data: full.prefix(n)) }
    }

    @Test func dynamicReferenceHint() throws {
        let dir = try makeTempDir("dyn")
        defer { try? FileManager.default.removeItem(at: dir) }
        var d = makePE(imports: ["kernel32.dll"])
        d.append(Data("....D3D12CreateDevice....".utf8))
        try d.write(to: dir.appendingPathComponent("a.exe"))
        try makePE(imports: ["kernel32.dll"]).write(to: dir.appendingPathComponent("b.exe"))
        #expect(PEScanner.usesD3D12(url: dir.appendingPathComponent("a.exe")))
        #expect(!PEScanner.usesD3D12(url: dir.appendingPathComponent("b.exe")))
    }
}

// MARK: - Profiles

@Suite struct ProfileTests {
    func repoProfilesDir() -> URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("profiles")
    }

    func db() -> ProfileDB {
        var problems: [String] = []
        let db = ProfileDB.load(directories: [repoProfilesDir()], problems: &problems)
        #expect(problems.isEmpty)
        return db
    }

    @Test func bundledProfilesLoad() {
        let ids = Set(db().profiles.map(\.id))
        #expect(ids.isSuperset(of: ["generic", "spider-man-remastered", "spider-man-2", "control"]))
    }

    @Test func matchesBySteamAppId() {
        #expect(db().match(steamAppId: 1817070, exe: nil).id == "spider-man-remastered")
        #expect(db().match(steamAppId: 2651280, exe: "whatever.exe").id == "spider-man-2")
        #expect(db().match(steamAppId: 870780, exe: nil).id == "control")
    }

    @Test func matchesByExeCaseInsensitive() {
        #expect(db().match(steamAppId: nil, exe: "spider-man2.EXE").id == "spider-man-2")
        #expect(db().match(steamAppId: nil, exe: "Control_DX12.exe").id == "control")
    }

    @Test func fallsBackToGeneric() {
        #expect(db().match(steamAppId: 42, exe: "unknown.exe").id == Profile.genericID)
        #expect(db().match(steamAppId: nil, exe: nil).id == Profile.genericID)
    }

    @Test func appIdBeatsExe() {
        #expect(db().match(steamAppId: 1817070, exe: "Spider-Man2.exe").id == "spider-man-remastered")
    }

    @Test func spiderMan2Content() throws {
        let p = db().match(steamAppId: 2651280, exe: nil)
        #expect(p.launch.env["ROSETTA_ADVERTISE_AVX"] == "1")
        #expect(p.launch.args == ["-nolauncher"])
        #expect(p.conf["quirk_force_compute_barrier"] == "0x324071d329f05ccc")
        #expect(p.requirements.first?.run.first == "install_pspc_sdk_runtime.bat")
    }

    @Test func userProfileOverridesAndBadFilesAreReported() throws {
        let user = try makeTempDir("profiles")
        defer { try? FileManager.default.removeItem(at: user) }
        try #"{"id":"control","name":"My Control","match":{"steamAppId":870780},"conf":{"hud":"1"}}"#
            .write(to: user.appendingPathComponent("control.json"), atomically: true, encoding: .utf8)
        try "{ not json".write(to: user.appendingPathComponent("bad.json"), atomically: true, encoding: .utf8)
        var problems: [String] = []
        let merged = ProfileDB.load(directories: [repoProfilesDir(), user], problems: &problems)
        #expect(merged.match(steamAppId: 870780, exe: nil).name == "My Control")
        #expect(problems.count == 1 && problems[0].hasPrefix("bad.json"))
    }
}

// MARK: - Conf writing

@Suite struct ConfTests {
    @Test func defaultsWriteOnlyLogPaths() {
        let kv = ConfBuilder.entries(profile: ProfileDB.builtinGeneric, settings: GameSettings(), logDir: URL(fileURLWithPath: "/logs/g"))
        #expect(kv.map(\.0) == ["dump_failed", "log_file"])
    }

    @Test func settingsMapToKeys() {
        var s = GameSettings()
        s.hud = true; s.gpuIdentity = .amd; s.featureLevel121 = true; s.gdeflate = false; s.logLevel = .trace
        s.extraConf = ["cache_max_mb": "64"]
        var profile = ProfileDB.builtinGeneric
        profile.conf = ["quirk_force_compute_barrier": "0x1"]
        let kv = Dictionary(uniqueKeysWithValues: ConfBuilder.entries(profile: profile, settings: s, logDir: nil))
        #expect(kv["hud"] == "1")
        #expect(kv["vendor_id"] == "1002" && kv["device_id"] == "73bf" && kv["adapter_name"] == "AMD Radeon RX 6800")
        #expect(kv["feature_level"] == "12_1")
        #expect(kv["gdeflate"] == "0")
        #expect(kv["trace"] == "1" && kv["stats"] == "1" && kv["log"] == nil)
        #expect(kv["quirk_force_compute_barrier"] == "0x1")
        #expect(kv["cache_max_mb"] == "64")
    }

    @Test func extraConfOverridesProfile() {
        var s = GameSettings()
        s.extraConf = ["feature_level": "12_0"]
        var p = ProfileDB.builtinGeneric
        p.conf = ["feature_level": "12_1"]
        #expect(Dictionary(uniqueKeysWithValues: ConfBuilder.entries(profile: p, settings: s, logDir: nil))["feature_level"] == "12_0")
    }

    @Test func renderedFileParsesBack() {
        var s = GameSettings()
        s.hud = true
        let text = ConfBuilder.render(profile: ProfileDB.builtinGeneric, settings: s, logDir: URL(fileURLWithPath: "/l"))
        #expect(text.hasPrefix(ConfBuilder.marker))
        let kv = ConfBuilder.parse(text)
        #expect(kv["hud"] == "1" && kv["log_file"] == "/l/d3d12metal.log")
    }

    @Test func settingsRoundTripAndOldFilesDecode() throws {
        let paths = Paths(home: try makeTempDir("settings"))
        var s = GameSettings()
        s.customEnv = ["A": "b"]; s.customArgs = "-windowed -w 1280"
        try s.save(gameID: "steam:1", paths: paths)
        #expect(GameSettings.load(gameID: "steam:1", paths: paths) == s)
        #expect(s.argumentList == ["-windowed", "-w", "1280"])
        #expect(try JSONDecoder().decode(GameSettings.self, from: Data("{}".utf8)) == GameSettings())
    }
}

// MARK: - Layer install / uninstall

@Suite struct InstallTests {
    func fakeBuild(version: String = "v1", dllContent: String = "dll1") throws -> LayerBuild {
        let dir = try makeTempDir("build")
        try FileManager.default.createDirectory(at: dir.appendingPathComponent("x86_64-unix"), withIntermediateDirectories: true)
        try dllContent.write(to: dir.appendingPathComponent("d3d12.dll"), atomically: true, encoding: .utf8)
        try "dxgi".write(to: dir.appendingPathComponent("dxgi.dll"), atomically: true, encoding: .utf8)
        try "so".write(to: dir.appendingPathComponent("x86_64-unix/d3d12metal.so"), atomically: true, encoding: .utf8)
        try "dxil".write(to: dir.appendingPathComponent("x86_64-unix/libdxilconv.dylib"), atomically: true, encoding: .utf8)
        try version.write(to: dir.appendingPathComponent("layer-version.txt"), atomically: true, encoding: .utf8)
        return LayerBuild(dir: dir)
    }

    func text(_ url: URL) -> String? { try? String(contentsOf: url, encoding: .utf8) }

    @Test func installsAndUninstallsCleanly() throws {
        let game = try makeTempDir("game")
        let build = try fakeBuild()
        try "original-d3d12".write(to: game.appendingPathComponent("d3d12.dll"), atomically: true, encoding: .utf8)
        try "my own conf".write(to: game.appendingPathComponent("d3d12metal.conf"), atomically: true, encoding: .utf8)
        try "game".write(to: game.appendingPathComponent("game.exe"), atomically: true, encoding: .utf8)

        #expect(!LayerInstaller.status(gameDir: game, build: build).installed)
        let conf = ConfBuilder.render(profile: ProfileDB.builtinGeneric, settings: GameSettings(), logDir: nil)
        try LayerInstaller.install(gameDir: game, exe: "game.exe", build: build, conf: conf, session: nil)

        let st = LayerInstaller.status(gameDir: game, build: build)
        #expect(st.installed && st.upToDate && st.installedVersion == "v1" && st.exe == "game.exe")
        #expect(text(game.appendingPathComponent("d3d12.dll")) == "dll1")
        #expect(text(game.appendingPathComponent("d3d12.dll.d3d12metal-orig")) == "original-d3d12")
        #expect(text(game.appendingPathComponent("d3d12metal.conf.d3d12metal-orig")) == "my own conf")
        // The user's own conf is kept (a backup of it too) and the managed lines are added at its end.
        let merged = try #require(text(game.appendingPathComponent("d3d12metal.conf")))
        #expect(merged.hasPrefix("my own conf") && merged.contains(ConfBuilder.blockBegin))
        #expect(text(game.appendingPathComponent("x86_64-unix/d3d12metal.so")) == "so")

        try LayerInstaller.uninstall(gameDir: game, session: nil)
        #expect(!LayerInstaller.status(gameDir: game, build: build).installed)
        #expect(text(game.appendingPathComponent("d3d12.dll")) == "original-d3d12")
        #expect(text(game.appendingPathComponent("d3d12metal.conf")) == "my own conf")
        #expect(!FileManager.default.fileExists(atPath: game.appendingPathComponent("dxgi.dll").path))
        #expect(!FileManager.default.fileExists(atPath: game.appendingPathComponent("x86_64-unix").path))
        #expect(text(game.appendingPathComponent("game.exe")) == "game")
    }

    func block(_ body: String) -> String { ConfBuilder.blockBegin + "\n" + body + "\n" + ConfBuilder.blockEnd + "\n" }
    func rendered(_ body: String) -> String { ConfBuilder.marker + "\n\n" + block(body) }

    @Test func freshConfIsOursAndRemovedOnUninstall() throws {
        let game = try makeTempDir("game")
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: try fakeBuild(), conf: rendered("hud=1"), session: nil)
        #expect(text(game.appendingPathComponent("d3d12metal.conf")) == rendered("hud=1"))
        try LayerInstaller.uninstall(gameDir: game, session: nil)
        #expect(!FileManager.default.fileExists(atPath: game.appendingPathComponent("d3d12metal.conf").path))
    }

    @Test func userEditedConfSurvivesUpdatesAndUninstall() throws {
        let game = try makeTempDir("game")
        let build = try fakeBuild()
        try "log=1\ntrace=1\n".write(to: game.appendingPathComponent("d3d12metal.conf"), atomically: true, encoding: .utf8)
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: build, conf: rendered("hud=1"), session: nil)
        var conf = try #require(text(game.appendingPathComponent("d3d12metal.conf")))
        #expect(conf.hasPrefix("log=1\ntrace=1\n") && conf.contains("hud=1"))
        // A settings change replaces only the block.
        try LayerInstaller.updateConf(gameDir: game, conf: rendered("hud=0"))
        conf = try #require(text(game.appendingPathComponent("d3d12metal.conf")))
        #expect(conf.contains("hud=0") && !conf.contains("hud=1") && conf.hasPrefix("log=1\ntrace=1\n"))
        // The user edits their lines; a re-install and an uninstall keep the edit.
        try conf.replacingOccurrences(of: "trace=1", with: "trace=0").write(to: game.appendingPathComponent("d3d12metal.conf"), atomically: true, encoding: .utf8)
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: build, conf: rendered("hud=1"), session: nil)
        try LayerInstaller.uninstall(gameDir: game, session: nil)
        let left = try #require(text(game.appendingPathComponent("d3d12metal.conf")))
        #expect(left.contains("trace=0") && left.contains("log=1") && !left.contains("alloy managed"))
        #expect(!FileManager.default.fileExists(atPath: game.appendingPathComponent("d3d12metal.conf.d3d12metal-orig").path))
    }

    @Test func conf_of_the_shell_installer_is_user_owned() throws {
        // tools/install-game.sh writes d3d12metal.conf once and records it as "d3d12metal.conf|-".
        let game = try makeTempDir("game")
        let shellConf = "log_file=/somewhere/d3d12metal.log\ntrace=1\n"
        try shellConf.write(to: game.appendingPathComponent("d3d12metal.conf"), atomically: true, encoding: .utf8)
        try "d3d12.dll|-\nd3d12metal.conf|-\n".write(to: game.appendingPathComponent(".d3d12metal-install"), atomically: true, encoding: .utf8)
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: try fakeBuild(), conf: rendered("hud=1"), session: nil)
        #expect(text(game.appendingPathComponent("d3d12metal.conf"))?.hasPrefix(shellConf) == true)
        try LayerInstaller.uninstall(gameDir: game, session: nil)
        #expect(text(game.appendingPathComponent("d3d12metal.conf")) == shellConf)   // not deleted
        // And a shell-made manifest alone (no block of ours in the file) never makes us delete the file.
        try "d3d12metal.conf|-\n".write(to: game.appendingPathComponent(".d3d12metal-install"), atomically: true, encoding: .utf8)
        try LayerInstaller.uninstall(gameDir: game, session: nil)
        #expect(text(game.appendingPathComponent("d3d12metal.conf")) == shellConf)
    }

    @Test func secondInstallKeepsOriginalBackupAndUpdates() throws {
        let game = try makeTempDir("game")
        try "orig".write(to: game.appendingPathComponent("d3d12.dll"), atomically: true, encoding: .utf8)
        let conf = "# managed by d3d12metal\n"
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: try fakeBuild(version: "v1"), conf: conf, session: nil)
        let newer = try fakeBuild(version: "v2", dllContent: "dll2")
        #expect(!LayerInstaller.status(gameDir: game, build: newer).upToDate)
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: newer, conf: conf, session: nil)
        #expect(text(game.appendingPathComponent("d3d12.dll")) == "dll2")
        #expect(text(game.appendingPathComponent("d3d12.dll.d3d12metal-orig")) == "orig")
        #expect(LayerInstaller.status(gameDir: game, build: newer).installedVersion == "v2")
        try LayerInstaller.uninstall(gameDir: game, session: nil)
        #expect(text(game.appendingPathComponent("d3d12.dll")) == "orig")
    }

    @Test func dryRunChangesNothing() throws {
        let game = try makeTempDir("game")
        let log = try LayerInstaller.install(gameDir: game, exe: "g.exe", build: try fakeBuild(), conf: "x", session: nil, dryRun: true)
        #expect(log.contains { $0.hasPrefix("install:") })
        #expect(try FileManager.default.contentsOfDirectory(atPath: game.path).isEmpty)
    }

    @Test func manifestIsCompatibleWithShellScripts() throws {
        let game = try makeTempDir("game")
        try LayerInstaller.install(gameDir: game, exe: "g.exe", build: try fakeBuild(), conf: "x", session: nil)
        let manifest = try #require(text(game.appendingPathComponent(".d3d12metal-install")))
        #expect(manifest.contains("d3d12.dll|-\n"))
        #expect(manifest.contains("x86_64-unix/|-\n"))
    }

    @Test func updateConfRefusesForeignFile() throws {
        let game = try makeTempDir("game")
        try "mine".write(to: game.appendingPathComponent("d3d12metal.conf"), atomically: true, encoding: .utf8)
        #expect(throws: CoreError.self) { try LayerInstaller.updateConf(gameDir: game, conf: "x") }
        try ConfBuilder.marker.write(to: game.appendingPathComponent("d3d12metal.conf"), atomically: true, encoding: .utf8)
        try LayerInstaller.updateConf(gameDir: game, conf: "# managed by d3d12metal\nhud=1\n")
        #expect(text(game.appendingPathComponent("d3d12metal.conf"))?.contains("hud=1") == true)
    }

    @Test func incompleteBuildIsRefused() throws {
        let empty = LayerBuild(dir: try makeTempDir("empty"))
        #expect(throws: CoreError.self) { try LayerInstaller.install(gameDir: try makeTempDir("g"), exe: "g.exe", build: empty, conf: "", session: nil) }
    }
}

// MARK: - Workspace with a fake Wine

/// A Workspace whose Wine is a shell script that logs its arguments (exit 1 when an argument contains "fail").
struct Fixture {
    var ws: Workspace
    var paths: Paths
    var game: Game
    var bottle: Bottle
    var wineLog: URL
    var gameDir: URL

    var wineCalls: [String] { ((try? String(contentsOf: wineLog, encoding: .utf8)) ?? "").split(separator: "\n").map(String.init) }
}

func makeFixture(profile: String? = nil, exe: String = "fixture-game.exe", files: [String: String] = [:]) throws -> Fixture {
    let root = try makeTempDir("fx")
    let home = root.appendingPathComponent("home"), wine = root.appendingPathComponent("wine"), prefix = root.appendingPathComponent("prefix")
    let gameDir = root.appendingPathComponent("game")
    for d in [home, wine.appendingPathComponent("bin"), prefix, gameDir] { try FileManager.default.createDirectory(at: d, withIntermediateDirectories: true) }
    let log = root.appendingPathComponent("wine.log")
    let script = "#!/bin/sh\necho \"$@\" >> '\(log.path)'\ncase \"$1\" in wineboot) touch \"$WINEPREFIX/system.reg\";; esac\ncase \"$*\" in *fail*) exit 1;; esac\nexit 0\n"
    try script.write(to: wine.appendingPathComponent("bin/wine"), atomically: true, encoding: .utf8)
    try "#!/bin/sh\nexit 0\n".write(to: wine.appendingPathComponent("bin/wineserver"), atomically: true, encoding: .utf8)
    for n in ["wine", "wineserver"] { chmod(wine.appendingPathComponent("bin/" + n).path, 0o755) }
    try "".write(to: prefix.appendingPathComponent("system.reg"), atomically: true, encoding: .utf8)
    try "".write(to: prefix.appendingPathComponent("user.reg"), atomically: true, encoding: .utf8)
    try makePE(imports: ["d3d12.dll"]).write(to: gameDir.appendingPathComponent(exe))
    for (name, content) in files { try content.write(to: gameDir.appendingPathComponent(name), atomically: true, encoding: .utf8) }
    let paths = Paths(home: home)
    var config = Config()
    config.wineRoot = wine.path
    try config.save(paths)
    if let profile {
        try FileManager.default.createDirectory(at: paths.userProfilesDir, withIntermediateDirectories: true)
        try profile.write(to: paths.userProfilesDir.appendingPathComponent("fixture.json"), atomically: true, encoding: .utf8)
    }
    let ws = Workspace(paths: paths)
    let bottle = try ws.bottles.add(name: "Fx", existingPrefix: prefix)
    let game = try ws.library.addCustom(name: "Fixture Game", bottle: "Fx", folder: gameDir, exe: exe)
    return Fixture(ws: ws, paths: paths, game: game, bottle: bottle, wineLog: log, gameDir: gameDir)
}

func requirementProfile(optional: Bool = false, run: String = "setup.bat", extra: String = "\"quiet\"") -> String {
    """
    {"id":"fixture","name":"Fixture","match":{"exe":["fixture-game.exe"]},"launch":{"mode":"direct"},
     "requirements":[{"id":"runtime","title":"Runtime","doneKey":"HKCU\\\\SOFTWARE\\\\Vendor\\\\Game","run":["\(run)",\(extra)],"optional":\(optional)}]}
    """
}

@Suite struct RequirementTests {
    @Test func runsOnceAndIsRecorded() throws {
        let fx = try makeFixture(profile: requirementProfile(), files: ["setup.bat": "@echo off"])
        let game = try #require(fx.ws.library.find("fixture game"))
        #expect(fx.ws.pendingRequirements(for: game).count == 1)
        try fx.ws.runRequirement(fx.ws.pendingRequirements(for: game)[0], for: game)
        #expect(fx.wineCalls.contains { $0.contains("cmd /c setup.bat quiet") })
        #expect(fx.wineCalls.contains { $0.contains("reg add") && $0.contains("Vendor") })   // the HasRunKey equivalent
        // The batch file did not create the registry key (the real one does not): the marker alone ends the loop.
        #expect(fx.ws.pendingRequirements(for: game).isEmpty)
        #expect(FileManager.default.fileExists(atPath: fx.bottle.prefix.appendingPathComponent("alloy-requirements.json").path))
    }

    @Test func optionalFailureIsRecordedAndNotRetried() throws {
        let fx = try makeFixture(profile: requirementProfile(optional: true, run: "fail.bat"), files: ["fail.bat": "x"])
        let game = try #require(fx.ws.library.find("fixture game"))
        try fx.ws.runRequirement(fx.ws.pendingRequirements(for: game)[0], for: game)
        #expect(fx.ws.pendingRequirements(for: game).isEmpty)
    }

    @Test func requiredFailureThrowsAndStaysPending() throws {
        let fx = try makeFixture(profile: requirementProfile(run: "fail.bat"), files: ["fail.bat": "x"])
        let game = try #require(fx.ws.library.find("fixture game"))
        #expect(throws: CoreError.self) { try fx.ws.runRequirement(fx.ws.pendingRequirements(for: game)[0], for: game) }
        #expect(fx.ws.pendingRequirements(for: game).count == 1)
    }

    @Test func registryKeyAlsoCountsAsDone() throws {
        let fx = try makeFixture(profile: requirementProfile(), files: ["setup.bat": "x"])
        let game = try #require(fx.ws.library.find("fixture game"))
        try "WINE REGISTRY Version 2\n\n[Software\\\\Vendor\\\\Game] 1\n".write(to: fx.bottle.prefix.appendingPathComponent("user.reg"), atomically: true, encoding: .utf8)
        #expect(fx.ws.pendingRequirements(for: game).isEmpty)
        #expect(RegistryFile.keyExists("HKCU\\Software\\Vendor\\Game", prefix: fx.bottle.prefix))
        #expect(!RegistryFile.keyExists("HKCU\\Software\\Vendor\\Other", prefix: fx.bottle.prefix))
    }
}

// MARK: - Command line

@Suite struct ArgsTests {
    @Test func repeatedOptionsKeepEveryValue() {
        let a = Args(["1817070", "--env", "A=1", "--env=B=2", "--env", "C=3", "--hud", "on", "--hud", "off", "--dry-run", "--", "--env", "X=9"])
        #expect(a.positional == ["1817070"])
        #expect(a.values("env") == ["A=1", "B=2", "C=3"])
        #expect(a.option("hud") == "off")           // the last one wins for a single value
        #expect(a.has("dry-run"))
        #expect(a.passthrough == ["--env", "X=9"])   // untouched after --
        #expect(a.values("missing").isEmpty && a.option("missing") == nil)
    }

    @Test func aValueOptionAtTheEndIsAFlag() {
        let a = Args(["x", "--env"])
        #expect(a.has("env") && a.values("env").isEmpty)
    }
}

// MARK: - Bottles

@Suite struct BottleTests {
    @Test func namesThatShareAFolderAreRefused() throws {
        let fx = try makeFixture()
        let rt = try fx.ws.runtime()
        let a = try fx.ws.bottles.create(name: "My Game", runtime: rt)
        #expect(a.prefixPath.hasSuffix("/bottles/My-Game/prefix"))
        #expect(throws: CoreError.self) { try fx.ws.bottles.create(name: "My-Game", runtime: rt) }     // same folder
        #expect(throws: CoreError.self) { try fx.ws.bottles.create(name: "my game", runtime: rt) }     // same name
        #expect(fx.ws.bottles.list().count == 2)                                                      // + the fixture's bottle
        #expect(FileManager.default.fileExists(atPath: a.prefix.path))                                // the first one is intact
    }

    @Test func deletingRemovesOnlyTheBottlesOwnFolder() throws {
        let fx = try makeFixture()
        let rt = try fx.ws.runtime()
        let a = try fx.ws.bottles.create(name: "Alpha", runtime: rt)
        let b = try fx.ws.bottles.create(name: "Beta", runtime: rt)
        try fx.ws.bottles.remove(name: "Alpha", deleteFiles: true)
        #expect(!FileManager.default.fileExists(atPath: a.prefix.deletingLastPathComponent().path))
        #expect(FileManager.default.fileExists(atPath: b.prefix.path))
        // An existing prefix that was merely registered is never deleted.
        #expect(throws: CoreError.self) { try fx.ws.bottles.remove(name: "Fx", deleteFiles: true) }
        #expect(FileManager.default.fileExists(atPath: fx.bottle.prefix.path))
        try fx.ws.bottles.remove(name: "Fx", deleteFiles: false)
        #expect(FileManager.default.fileExists(atPath: fx.bottle.prefix.path))
    }

    @Test func aFolderSharedByTwoEntriesIsNotDeleted() throws {
        let fx = try makeFixture()
        let rt = try fx.ws.runtime()
        let a = try fx.ws.bottles.create(name: "Alpha", runtime: rt)
        // A hand-edited bottles.json with a second managed entry in the same folder.
        var list = fx.ws.bottles.list()
        list.append(Bottle(name: "Alpha Copy", prefixPath: a.prefixPath, managed: true))
        try JSONEncoder().encode(list).write(to: fx.paths.bottlesFile)
        #expect(throws: CoreError.self) { try fx.ws.bottles.remove(name: "Alpha", deleteFiles: true) }
        #expect(FileManager.default.fileExists(atPath: a.prefix.path))
    }
}

// MARK: - Running games

@Suite struct RunningTests {
    /// Starts a stand-in game process: a script named like the exe that keeps a file open in the bottle's wineserver directory,
    /// as every Wine process of a prefix does.
    func startStandIn(exe: String, bottle: Bottle) throws -> (process: Process, cleanup: () -> Void) {
        let server = URL(fileURLWithPath: "/tmp/.wine-\(getuid())").appendingPathComponent(try #require(ProcessScan.serverDirectoryName(prefix: bottle.prefix)))
        try FileManager.default.createDirectory(at: server, withIntermediateDirectories: true)
        let dir = try makeTempDir("standin")
        let script = dir.appendingPathComponent(exe)
        try "#!/bin/sh\nexec 3>\"$1\"\nsleep 40\n".write(to: script, atomically: true, encoding: .utf8)
        chmod(script.path, 0o755)
        let p = try Shell.spawn("/bin/sh", [script.path, server.appendingPathComponent("tmpmap-test").path])
        Thread.sleep(forTimeInterval: 0.6)
        return (p, { p.terminate(); _ = Shell.run("/usr/bin/pkill", ["-f", "sleep 40"]); try? FileManager.default.removeItem(at: server) })
    }

    @Test func runningIsPerBottleAndWorksForLongExeNames() throws {
        let exe = "fixture-very-long-game-name.exe"      // longer than the 15 characters of a process name
        let a = try makeFixture(exe: exe)
        let b = try makeFixture(exe: exe)                // the same exe in another bottle
        let standIn = try startStandIn(exe: exe, bottle: a.bottle)
        defer { standIn.cleanup() }
        #expect(a.ws.isRunning(a.game))
        #expect(!b.ws.isRunning(b.game))
        // launch refuses a game that already runs, by the same check
        #expect(throws: CoreError.self) { try a.ws.launch(a.game, install: false, runRequirements: false) }
    }
}

// MARK: - Shell

@Suite struct ShellTests {
    @Test func collectsOutputAndStatus() {
        let r = Shell.run("/bin/sh", ["-c", "echo out; echo err >&2; exit 3"])
        #expect(r.status == 3 && r.stdout == "out\n" && r.stderr == "err\n")
    }

    @Test func doesNotWaitForGrandchildrenHoldingThePipes() {
        // `sleep` inherits stdout and stderr and outlives the shell (as wineserver does after `wine reg`).
        let start = Date()
        let r = Shell.run("/bin/sh", ["-c", "sleep 30 & echo hi"])
        #expect(r.ok && r.stdout == "hi\n")
        #expect(Date().timeIntervalSince(start) < 5)
        _ = Shell.run("/usr/bin/pkill", ["-f", "^sleep 30$"])
    }

    @Test func timeoutReturnsEvenWithAGrandchild() {
        let start = Date()
        let r = Shell.run("/bin/sh", ["-c", "sleep 31 & sleep 31"], timeout: 0.5)
        #expect(!r.ok)
        #expect(Date().timeIntervalSince(start) < 5)
        _ = Shell.run("/usr/bin/pkill", ["-f", "^sleep 31$"])
    }

    @Test func largeOutputIsComplete() {
        let r = Shell.run("/bin/sh", ["-c", "yes abcdefghij | head -c 3000000"])
        #expect(r.stdout.utf8.count == 3_000_000)
    }
}

// MARK: - Misc

@Suite struct MiscTests {
    @Test func reportScrubsPersonalData() {
        let s = Report.scrub("path /Users/alice/code and C:\\users\\alice\\AppData mail a.b@example.com id 76561198000000000 home /Users/alice/x",
                             user: "alice", home: "/Users/alice")
        #expect(!s.contains("alice"))
        #expect(!s.contains("example.com"))
        #expect(!s.contains("76561198000000000"))
        #expect(s.contains("~/code"))
    }

    @Test func reportScrubsWinePaths() {
        let s = Report.scrub("conf=Z:\\Users\\alice\\code\\g\\d3d12metal.conf in Z:\\Users\\ALICE", user: "alice", home: "/Users/alice")
        #expect(!s.lowercased().contains("alice"))
        #expect(s.contains("~\\code"))
    }

    @Test func vdfEscapesAndComments() {
        let root = VDF.parse("// c\n\"a\" { \"k\" \"x\\\\y\\\"z\" }")
        #expect(root["a"]?.string("k") == "x\\y\"z")
    }

    @Test func customGamesRoundTrip() throws {
        let paths = Paths(home: try makeTempDir("home"))
        let folder = try makeTempDir("g")
        try Data().write(to: folder.appendingPathComponent("Control_DX12.exe"))
        let lib = GameLibrary(paths: paths, profiles: ProfileDB(profiles: [ProfileDB.builtinGeneric]))
        let g = try lib.addCustom(name: "My Game!", bottle: "b", folder: folder, exe: "Control_DX12.exe")
        #expect(g.id == "custom:my-game-")
        #expect(lib.customGames().count == 1)
        #expect(lib.find("my game!")?.id == g.id)
        #expect(throws: CoreError.self) { try lib.addCustom(name: "x", bottle: "b", folder: folder, exe: "missing.exe") }
        try lib.removeCustom(id: g.id)
        #expect(lib.customGames().isEmpty)
    }

    @Test func environmentPrefersNewNameAndFallsBackToOld() {
        #expect(AlloyEnv.get("HOME", environment: ["ALLOY_HOME": "/a", "D3D12METAL_HOME": "/b"]) == "/a")
        #expect(AlloyEnv.get("HOME", environment: ["D3D12METAL_HOME": "/b"]) == "/b")
        #expect(AlloyEnv.get("HOME", environment: ["ALLOY_HOME": ""]) == nil)
        #expect(AlloyEnv.get("HOME", environment: [:]) == nil)
    }

    @Test func legacyDataFolderIsMovedAndBottlePathsFixed() throws {
        let support = try makeTempDir("support")
        defer { try? FileManager.default.removeItem(at: support) }
        let old = support.appendingPathComponent("D3D12Metal")
        try FileManager.default.createDirectory(at: old.appendingPathComponent("bottles/Steam/prefix"), withIntermediateDirectories: true)
        try "{\"x\":1}".write(to: old.appendingPathComponent("config.json"), atomically: true, encoding: .utf8)
        let json = "[{\"name\":\"Steam\",\"prefixPath\":\"\(old.path)/bottles/Steam/prefix\",\"managed\":true}]"
        try json.write(to: old.appendingPathComponent("bottles.json"), atomically: true, encoding: .utf8)
        Paths.migrateLegacyHome(in: support)
        let new = support.appendingPathComponent("Alloy")
        #expect(!FileManager.default.fileExists(atPath: old.path))
        #expect(FileManager.default.fileExists(atPath: new.appendingPathComponent("config.json").path))
        let bottles = try String(contentsOf: new.appendingPathComponent("bottles.json"), encoding: .utf8)
        #expect(bottles.contains(new.path + "/bottles/Steam/prefix") && !bottles.contains("D3D12Metal"))
        // A second call, or an existing new folder, changes nothing.
        try FileManager.default.createDirectory(at: old, withIntermediateDirectories: true)
        Paths.migrateLegacyHome(in: support)
        #expect(FileManager.default.fileExists(atPath: old.path))
    }

    @Test func serverDirectoryNameFollowsDeviceAndInode() throws {
        let dir = try makeTempDir("prefix")
        defer { try? FileManager.default.removeItem(at: dir) }
        let name = try #require(ProcessScan.serverDirectoryName(prefix: dir))
        let attrs = try FileManager.default.attributesOfItem(atPath: dir.path)
        let ino = (attrs[.systemFileNumber] as? NSNumber)?.uint64Value ?? 0
        #expect(name.hasPrefix("server-") && name.hasSuffix("-" + String(ino, radix: 16)))
        #expect(ProcessScan.serverDirectoryName(prefix: dir.appendingPathComponent("missing")) == nil)
    }

    @Test func aProcessWithoutFilesInTheBottleIsNotInIt() throws {
        let dir = try makeTempDir("prefix")
        defer { try? FileManager.default.removeItem(at: dir) }
        let bottle = Bottle(name: "t", prefixPath: dir.path, managed: false)
        #expect(!ProcessScan.process(getpid(), belongsTo: bottle))
        #expect(ProcessScan.filter([getpid()], in: bottle).isEmpty)
        #expect(ProcessScan.filter([getpid()], in: nil) == [getpid()])
    }

    @Test func legacyConfMarkerCountsAsManaged() {
        #expect(ConfBuilder.isManaged("# managed by alloy\nhud=1"))
        #expect(ConfBuilder.isManaged("# managed by d3d12metal\nhud=1"))
        #expect(!ConfBuilder.isManaged("hud=1"))
    }

    @Test func wineReleaseIsPinned() {
        #expect(WineRelease.release(version: AppInfo.defaultWineVersion)?.sha256.count == 64)
    }

    @Test func checksumOfFile() throws {
        let f = try makeTempDir("sum").appendingPathComponent("a")
        try Data("abc".utf8).write(to: f)
        #expect(try RuntimeManager.sha256(of: f) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    }
}
