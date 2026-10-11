// SPDX-License-Identifier: LGPL-2.1-or-later
import AppKit
import AlloyCore
import SwiftUI

/// What the library shows about one game.
struct GameState: Identifiable, Equatable {
    var game: Game
    var profile: Profile
    var layer: LayerStatus
    var running: Bool
    var id: String { game.id }
}

enum SidebarItem: String, CaseIterable, Identifiable {
    case library = "Library", steam = "Steam", bottles = "Bottles", health = "Health"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .library: return "square.grid.2x2"
        case .steam: return "gamecontroller"
        case .bottles: return "shippingbox"
        case .health: return "stethoscope"
        }
    }
}

@MainActor
final class AppModel: ObservableObject {
    let ws: Workspace
    @Published var games: [GameState] = []
    @Published var checks: [Check] = []
    @Published var bottles: [Bottle] = []
    @Published var section: SidebarItem? = .library
    @Published var selectedGameID: String?
    @Published var path: [String] = []   // the library's navigation stack (game ids)
    @Published var busy: String?
    @Published var progress: Double?
    @Published var errorMessage: String?
    @Published var infoMessage: String?
    @Published var steamRunning = false
    @Published var showSetup = false
    @Published var showAddGame = false
    @Published var loading = false
    private var timer: Timer?
    private var initialGame: String?
    private var refreshing = false
    private var refreshAgain = false
    private var refreshAgainFull = false

    init(ws: Workspace = Workspace()) {
        self.ws = ws
        showSetup = !ws.config.setupDone
        // Start in a given place (screenshots, scripts): ALLOY_UI_SECTION=library|steam|bottles|health, ALLOY_UI_GAME=<game id>, ALLOY_UI_SETUP=0|1.
                section = SidebarItem(rawValue: (AlloyEnv.get("UI_SECTION") ?? "").capitalized) ?? .library
        if AlloyEnv.get("UI_SETUP") == "0" { showSetup = false }
        if AlloyEnv.get("UI_SETUP") == "1" { showSetup = true }
        initialGame = AlloyEnv.get("UI_GAME")
        refresh(full: true)
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollRunning() }
        }
    }

    var selected: GameState? { games.first { $0.id == selectedGameID } }
    var bottleName: String { ws.config.defaultBottle }

    // MARK: Refreshing

    /// Rescans the library, bottles and Steam state. `full` also re-runs the health checks (slow: they start helper programs), which
    /// only the setup and health views need. Overlapping calls are coalesced: one runs, and at most one more follows it.
    func refresh(full: Bool = false) {
        if refreshing {
            refreshAgain = true
            refreshAgainFull = refreshAgainFull || full
            return
        }
        refreshing = true
        loading = true
        let ws = self.ws
        Task.detached {
            // Everything is computed into local values off the main actor; only the assignments happen on it.
            ws.reloadProfiles()
            let library = ws.library
            let list = ws.allGames().map { g in
                GameState(game: g, profile: library.profile(for: g), layer: ws.layerStatus(for: g), running: false)
            }
            let running = ws.runningGameIDs(list.map(\.game))
            let bottles = ws.bottles.list()
            let steam = ws.steamRunning(bottleName: ws.config.defaultBottle)
            let checks = full ? Doctor.runAll(ws) : nil
            await MainActor.run {
                self.games = list.map { var s = $0; s.running = running.contains(s.id); return s }
                self.bottles = bottles
                self.steamRunning = steam
                if let checks { self.checks = checks }
                self.loading = false
                if let g = self.initialGame, list.contains(where: { $0.id == g }) { self.path = [g]; self.initialGame = nil }
                if self.selectedGameID == nil || !list.contains(where: { $0.id == self.selectedGameID }) { self.selectedGameID = list.first?.id }
                self.refreshing = false
                if self.refreshAgain {
                    let again = self.refreshAgainFull
                    self.refreshAgain = false
                    self.refreshAgainFull = false
                    self.refresh(full: again)
                }
            }
        }
    }

    /// Updates the running flags from one process listing, by game id (the list may have changed while this ran).
    func pollRunning() {
        guard !refreshing else { return }
        let ws = self.ws
        let current = games.map(\.game)
        Task.detached {
            let running = ws.runningGameIDs(current)
            let steam = ws.steamRunning(bottleName: ws.config.defaultBottle)
            await MainActor.run {
                self.steamRunning = steam
                for i in self.games.indices {
                    let r = running.contains(self.games[i].id)
                    if self.games[i].running != r { self.games[i].running = r }
                }
            }
        }
    }

    func refreshGame(_ id: String) {
        guard let i = games.firstIndex(where: { $0.id == id }) else { return }
        let ws = self.ws, g = games[i].game
        Task.detached {
            let layer = ws.layerStatus(for: g)
            await MainActor.run { if let i = self.games.firstIndex(where: { $0.id == id }) { self.games[i].layer = layer } }
        }
    }

    // MARK: Running work

    /// Runs `work` off the main thread, showing `title` while it runs and any error afterwards.
    func perform(_ title: String, refreshAfter: Bool = true, fullRefresh: Bool = false, work: @escaping @Sendable (@escaping @Sendable (String, Double?) -> Void) async throws -> String?) {
        guard busy == nil else { return }
        busy = title
        progress = nil
        Task {
            do {
                let note = try await work { [weak self] text, p in
                    Task { @MainActor in self?.busy = text; self?.progress = p }
                }
                if let note { self.infoMessage = note }
            } catch {
                self.errorMessage = (error as? CoreError)?.description ?? error.localizedDescription
            }
            self.busy = nil
            self.progress = nil
            if refreshAfter { self.refresh(full: fullRefresh) }
        }
    }

    // MARK: Game actions

    func install(_ s: GameState) {
        let ws = self.ws, g = s.game
        perform(s.layer.installed ? "Updating the layer" : "Installing the layer") { _ in
            try ws.installLayer(for: g)
            return nil
        }
    }

    func uninstall(_ s: GameState) {
        let ws = self.ws, g = s.game
        perform("Removing the layer") { _ in try ws.uninstallLayer(for: g); return nil }
    }

    func play(_ s: GameState) {
        let ws = self.ws, g = s.game
        perform("Starting \(g.name)") { report in
            let r = try ws.launch(g) { report($0, nil) }
            return r.notes.first
        }
    }

    func stop(_ s: GameState) {
        let ws = self.ws, g = s.game
        perform("Stopping \(g.name)") { _ in ws.stop(g); return nil }
    }

    func save(_ settings: GameSettings, for s: GameState) {
        do {
            try ws.saveSettings(settings, for: s.game)
            refreshGame(s.id)
        } catch { errorMessage = (error as? CoreError)?.description ?? error.localizedDescription }
    }

    func report(for s: GameState?) {
        let ws = self.ws, g = s?.game
        let out = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask)[0]
        perform("Collecting diagnostics", refreshAfter: false) { _ in
            let zip = try Report.create(ws: ws, game: g, outputDir: out)
            await MainActor.run { NSWorkspace.shared.activateFileViewerSelecting([zip]) }
            return "Report saved to \(zip.lastPathComponent) in Downloads. Personal paths and ids are scrubbed; look inside before sharing."
        }
    }

    func openLogs(_ s: GameState) {
        let dir = ws.paths.logDir(for: s.game.id)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        NSWorkspace.shared.open(dir)
    }

    // MARK: Setup, Steam, bottles

    func installWine() {
        let ws = self.ws
        perform("Downloading Wine", fullRefresh: true) { report in
            guard let rel = WineRelease.release(version: ws.config.wineVersion) else { throw CoreError("No pinned Wine release \(ws.config.wineVersion)") }
            _ = try await RuntimeManager.install(rel, paths: ws.paths) { p, stage in report(stage, p < 1 ? p : nil) }
            return nil
        }
    }

    func createBottle(_ name: String) {
        let ws = self.ws
        perform("Creating bottle \(name)", fullRefresh: true) { _ in
            let rt = try ws.runtime()
            try ws.bottles.create(name: name, runtime: rt)
            return nil
        }
    }

    func addBottle(name: String, prefix: URL) {
        do { try ws.bottles.add(name: name, existingPrefix: prefix); refresh() } catch { errorMessage = "\(error)" }
    }

    func removeBottle(_ b: Bottle, deleteFiles: Bool) {
        do { try ws.bottles.remove(name: b.name, deleteFiles: deleteFiles); refresh() } catch { errorMessage = "\(error)" }
    }

    func installSteam() {
        let ws = self.ws, name = bottleName
        perform("Installing Steam", fullRefresh: true) { report in
            let steam = try ws.steam(bottleName: name)
            try await steam.install { p, stage in report(stage, p < 1 ? p : nil) }
            return nil
        }
    }

    func startSteam() {
        let ws = self.ws, name = bottleName
        perform("Starting Steam", fullRefresh: true) { _ in
            let steam = try ws.steam(bottleName: name)
            try steam.start(shim: ws.layerBuild?.webhelperShim, logFile: ws.paths.logsDir.appendingPathComponent("steam-\(Paths.safeName(name)).log"))
            return "Steam is starting. Its window opens in about 30 seconds; sign in there."
        }
    }

    func stopSteam(force: Bool = false) {
        let ws = self.ws, name = bottleName
        perform("Stopping Steam", fullRefresh: true) { _ in
            let steam = try ws.steam(bottleName: name)
            if force { steam.forceStop() } else { steam.stop() }
            return nil
        }
    }

    func restartSteam() {
        let ws = self.ws, name = bottleName
        perform("Restarting Steam", fullRefresh: true) { _ in
            let steam = try ws.steam(bottleName: name)
            steam.stop()
            for _ in 0..<20 where steam.isRunning { try await Task.sleep(nanoseconds: 1_000_000_000) }
            if steam.isRunning { steam.forceStop() }
            try steam.start(shim: ws.layerBuild?.webhelperShim, logFile: ws.paths.logsDir.appendingPathComponent("steam-\(Paths.safeName(name)).log"))
            return nil
        }
    }

    func finishSetup() {
        try? ws.updateConfig { $0.setupDone = true }
        showSetup = false
    }

    // MARK: Preferences

    func shaderCacheSize() -> Int64 {
        let dir = ws.paths.shaderCacheDir
        guard let e = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
        var total: Int64 = 0
        for case let url as URL in e { total += Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
        return total
    }

    func clearShaderCache() {
        let dir = ws.paths.shaderCacheDir
        for item in (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? [] {
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(item))
        }
        infoMessage = "Shader cache cleared. The next start of each game compiles its shaders again."
    }

    func updateConfig(_ change: (inout Config) -> Void) {
        do { try ws.updateConfig(change); refresh() } catch { errorMessage = "\(error)" }
    }
}
