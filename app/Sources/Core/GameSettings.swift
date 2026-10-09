// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

public enum GPUIdentity: String, Codable, CaseIterable, Sendable {
    case apple, amd, nvidia

    public var label: String {
        switch self {
        case .apple: return "Apple (default)"
        case .amd: return "AMD Radeon RX 6800"
        case .nvidia: return "NVIDIA GeForce RTX 3080"
        }
    }
    public var adapterName: String? {
        switch self { case .apple: return nil; case .amd: return "AMD Radeon RX 6800"; case .nvidia: return "NVIDIA GeForce RTX 3080" }
    }
    public var vendorID: String? {
        switch self { case .apple: return nil; case .amd: return "1002"; case .nvidia: return "10de" }
    }
    public var deviceID: String? {
        switch self { case .apple: return nil; case .amd: return "73bf"; case .nvidia: return "2206" }
    }
    /// The PCI enum key Wine would have registered for that id; some games read the driver version from it.
    public var pciKey: String? {
        guard let v = vendorID, let d = deviceID else { return nil }
        return "HKLM\\System\\CurrentControlSet\\Enum\\PCI\\VEN_\(v.uppercased())&DEV_\(d.uppercased())&SUBSYS_00000000&REV_00\\00000000"
    }
}

public enum LogLevel: String, Codable, CaseIterable, Sendable {
    case off, stats, trace, verbose
    public var label: String {
        switch self {
        case .off: return "Errors only"
        case .stats: return "Frame statistics"
        case .trace: return "API trace"
        case .verbose: return "Everything (very slow)"
        }
    }
}

/// What the user can change per game (stored in settings/<game>.json, written into d3d12metal.conf on install/apply).
public struct GameSettings: Codable, Equatable, Sendable {
    public var hud: Bool = false
    public var gpuIdentity: GPUIdentity = .apple
    public var featureLevel121: Bool = false
    public var gdeflate: Bool = true
    public var logLevel: LogLevel = .off
    public var customEnv: [String: String] = [:]
    public var customArgs: String = ""
    /// Raw conf keys that win over everything (advanced).
    public var extraConf: [String: String] = [:]

    public init() {}

    enum CodingKeys: String, CodingKey { case hud, gpuIdentity, featureLevel121, gdeflate, logLevel, customEnv, customArgs, extraConf }
    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        hud = try c.decodeIfPresent(Bool.self, forKey: .hud) ?? false
        gpuIdentity = try c.decodeIfPresent(GPUIdentity.self, forKey: .gpuIdentity) ?? .apple
        featureLevel121 = try c.decodeIfPresent(Bool.self, forKey: .featureLevel121) ?? false
        gdeflate = try c.decodeIfPresent(Bool.self, forKey: .gdeflate) ?? true
        logLevel = try c.decodeIfPresent(LogLevel.self, forKey: .logLevel) ?? .off
        customEnv = try c.decodeIfPresent([String: String].self, forKey: .customEnv) ?? [:]
        customArgs = try c.decodeIfPresent(String.self, forKey: .customArgs) ?? ""
        extraConf = try c.decodeIfPresent([String: String].self, forKey: .extraConf) ?? [:]
    }

    public static func load(gameID: String, paths: Paths) -> GameSettings {
        guard let data = try? Data(contentsOf: paths.settingsFile(for: gameID)),
              let s = try? JSONDecoder().decode(GameSettings.self, from: data) else { return GameSettings() }
        return s
    }

    public func save(gameID: String, paths: Paths) throws {
        try FileManager.default.createDirectory(at: paths.settingsDir, withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(self).write(to: paths.settingsFile(for: gameID), options: .atomic)
    }

    public var argumentList: [String] { customArgs.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init) }
}

/// Builds the text of d3d12metal.conf from a profile, the user's settings and the log directory.
public enum ConfBuilder {
    public static let marker = "# managed by d3d12metal"

    public static func entries(profile: Profile, settings: GameSettings, logDir: URL?) -> [(String, String)] {
        var kv: [String: String] = [:]
        if let logDir {
            kv["log_file"] = logDir.appendingPathComponent("d3d12metal.log").path
            kv["dump_failed"] = logDir.appendingPathComponent("failed-shaders").path
        }
        switch settings.logLevel {
        case .off: break
        case .stats: kv["stats"] = "1"
        case .trace: kv["stats"] = "1"; kv["trace"] = "1"
        case .verbose: kv["stats"] = "1"; kv["trace"] = "1"; kv["log"] = "1"
        }
        if settings.hud { kv["hud"] = "1" }
        if let n = settings.gpuIdentity.adapterName, let v = settings.gpuIdentity.vendorID, let d = settings.gpuIdentity.deviceID {
            kv["adapter_name"] = n; kv["vendor_id"] = v; kv["device_id"] = d
        }
        if settings.featureLevel121 { kv["feature_level"] = "12_1" }
        if !settings.gdeflate { kv["gdeflate"] = "0" }
        for (k, v) in profile.conf { kv[k] = v }
        for (k, v) in settings.extraConf { kv[k] = v }
        return kv.sorted { $0.key < $1.key }.map { ($0.key, $0.value) }
    }

    public static func render(profile: Profile, settings: GameSettings, logDir: URL?) -> String {
        var out = """
        \(marker)
        # Written by the \(AppInfo.name) app / CLI from the game's profile ("\(profile.id)") and settings; edits are overwritten.
        # Put your own lines in the game's "extra conf" setting. Environment variables D3D12METAL_<KEY> override these.

        """
        out += "\n"
        for (k, v) in entries(profile: profile, settings: settings, logDir: logDir) { out += "\(k)=\(v)\n" }
        return out
    }

    /// Parses a conf file (key=value lines, # comments).
    public static func parse(_ text: String) -> [String: String] {
        var kv: [String: String] = [:]
        for raw in text.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            guard let eq = line.firstIndex(of: "=") else { continue }
            kv[String(line[..<eq]).trimmingCharacters(in: .whitespaces)] = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
        }
        return kv
    }
}
