// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// The Windows Steam client inside a bottle.
public struct SteamClient: Sendable {
    public static let installerURL = URL(string: "https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe")!
    public let session: WineSession
    public let paths: Paths

    public init(session: WineSession, paths: Paths) { self.session = session; self.paths = paths }

    public var steamDir: URL { SteamLibrary.steamDirectory(prefix: session.bottle.prefix) }
    public var steamExe: URL { steamDir.appendingPathComponent("steam.exe") }
    public var isInstalled: Bool { FileManager.default.fileExists(atPath: steamExe.path) }
    public var cefDir: URL { steamDir.appendingPathComponent("bin/cef/cef.win64") }

    /// Steam runs (any bottle: Wine renames the process, the Windows command line is what identifies it).
    public static var isRunning: Bool { !Shell.pids(matching: "Steam\\\\steam\\.exe").isEmpty }

    /// Downloads Valve's installer (the user accepts Valve's terms in it) and installs silently.
    public func install(progress: @escaping @Sendable (Double, String) -> Void = { _, _ in }) async throws {
        if isInstalled { return }
        try paths.ensureDirectories()
        let setup = paths.downloadsDir.appendingPathComponent("SteamSetup.exe")
        progress(0, "Downloading the Steam installer from Valve")
        try await Downloader.download(Self.installerURL, to: setup) { progress($0, "Downloading the Steam installer from Valve") }
        progress(1, "Installing Steam")
        let r = session.run([setup.path, "/S"], timeout: 600)
        guard isInstalled else {
            throw CoreError("The Steam installer did not install Steam: \(r.stderr.suffix(300))",
                            hint: "Run it by hand: wine SteamSetup.exe in the bottle")
        }
    }

    /// Makes Steam's browser process render (see docs/ARCHITECTURE.md): Chromium's GPU process draws into windows
    /// owned by the browser process, which Wine's mac driver cannot present; the shim runs the real
    /// steamwebhelper with --in-process-gpu. Reinstalled after every Steam update.
    @discardableResult
    public func installWebhelperShim(shim: URL?) -> Bool {
        let fm = FileManager.default
        guard let shim, fm.fileExists(atPath: shim.path), fm.fileExists(atPath: cefDir.appendingPathComponent("steamwebhelper.exe").path) else { return false }
        let target = cefDir.appendingPathComponent("steamwebhelper.exe")
        if fm.contentsEqual(atPath: shim.path, andPath: target.path) { return true }
        let real = cefDir.appendingPathComponent("steamwebhelper_real.exe")
        do {
            try? fm.removeItem(at: real)
            try fm.copyItem(at: target, to: real)
            try fm.removeItem(at: target)
            try fm.copyItem(at: shim, to: target)
            return true
        } catch { return false }
    }

    /// Starts Steam (returns once the process is launched). `extraEnv` is inherited by the games it starts.
    @discardableResult
    public func start(shim: URL?, extraEnv: [String: String] = [:], logFile: URL? = nil) throws -> Process {
        guard isInstalled else { throw CoreError("Steam is not installed in bottle \"\(session.bottle.name)\"", hint: "d3d12metal steam install") }
        installWebhelperShim(shim: shim)
        var env = ["STEAMWEBHELPER_EXTRA": "--in-process-gpu"]
        for (k, v) in extraEnv { env[k] = v }
        return try session.spawn(["C:\\Program Files (x86)\\Steam\\steam.exe", "-no-cef-sandbox", "-noverifyfiles", "-cef-disable-gpu"],
                                 extraEnv: env, logFile: logFile)
    }

    /// Asks Steam to shut down cleanly.
    public func stop() {
        guard isInstalled else { return }
        session.run(["C:\\Program Files (x86)\\Steam\\steam.exe", "-shutdown"], timeout: 60)
    }

    /// Hard stop of every process of the bottle (wineserver -k for this prefix).
    public func forceStop() { session.killAll() }

    /// Asks the running Steam to launch an app.
    public func applaunch(appid: Int, args: [String]) throws -> Process {
        try session.spawn(["C:\\Program Files (x86)\\Steam\\steam.exe", "-applaunch", String(appid)] + args)
    }
}
