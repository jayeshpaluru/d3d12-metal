// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// A Wine prefix with a name, like a CrossOver bottle. Games and Steam live inside one.
public struct Bottle: Codable, Equatable, Identifiable, Sendable {
    public var name: String
    public var prefixPath: String
    /// True when the prefix was created by us under Application Support (and may be deleted with the bottle).
    public var managed: Bool

    public var id: String { name }
    public var prefix: URL { URL(fileURLWithPath: prefixPath) }
    public var driveC: URL { prefix.appendingPathComponent("drive_c") }

    public init(name: String, prefixPath: String, managed: Bool) {
        self.name = name; self.prefixPath = prefixPath; self.managed = managed
    }
}

public struct BottleStore: Sendable {
    public let paths: Paths
    public init(paths: Paths) { self.paths = paths }

    public func list() -> [Bottle] {
        guard let data = try? Data(contentsOf: paths.bottlesFile),
              let list = try? JSONDecoder().decode([Bottle].self, from: data) else { return [] }
        return list
    }

    public func bottle(named name: String) -> Bottle? { list().first { $0.name.caseInsensitiveCompare(name) == .orderedSame } }

    private func save(_ list: [Bottle]) throws {
        try FileManager.default.createDirectory(at: paths.home, withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(list).write(to: paths.bottlesFile, options: .atomic)
    }

    /// Registers an existing prefix under a name (not deleted with the bottle).
    @discardableResult
    public func add(name: String, existingPrefix: URL) throws -> Bottle {
        guard bottle(named: name) == nil else { throw CoreError("A bottle named \"\(name)\" already exists") }
        guard FileManager.default.fileExists(atPath: existingPrefix.appendingPathComponent("system.reg").path) else {
            throw CoreError("\(existingPrefix.path) is not a Wine prefix (no system.reg)")
        }
        let b = Bottle(name: name, prefixPath: existingPrefix.path, managed: false)
        try save(list() + [b])
        return b
    }

    /// Creates a new prefix with wineboot and registers it.
    @discardableResult
    public func create(name: String, runtime: WineRuntime) throws -> Bottle {
        guard bottle(named: name) == nil else { throw CoreError("A bottle named \"\(name)\" already exists") }
        guard name.range(of: "^[A-Za-z0-9 _.-]+$", options: .regularExpression) != nil else {
            throw CoreError("Bottle names may contain letters, digits, spaces, dots, dashes and underscores")
        }
        let prefix = paths.bottlesDir.appendingPathComponent(Paths.safeName(name)).appendingPathComponent("prefix")
        try FileManager.default.createDirectory(at: prefix, withIntermediateDirectories: true)
        let b = Bottle(name: name, prefixPath: prefix.path, managed: true)
        let r = WineSession(runtime: runtime, bottle: b).run(["wineboot", "-u"], timeout: 300,
                                                            extraEnv: ["WINEDLLOVERRIDES": "mscoree,mshtml="])
        guard FileManager.default.fileExists(atPath: prefix.appendingPathComponent("system.reg").path) else {
            throw CoreError("Creating the prefix failed: \(r.stderr.suffix(400))")
        }
        try save(list() + [b])
        return b
    }

    public func remove(name: String, deleteFiles: Bool) throws {
        guard let b = bottle(named: name) else { throw CoreError("No bottle named \"\(name)\"") }
        try save(list().filter { $0.name != b.name })
        if deleteFiles, b.managed {
            try? FileManager.default.removeItem(at: b.prefix.deletingLastPathComponent())
        }
    }
}

/// Runs Wine programs inside one bottle.
public struct WineSession: Sendable {
    public let runtime: WineRuntime
    public let bottle: Bottle

    public init(runtime: WineRuntime, bottle: Bottle) { self.runtime = runtime; self.bottle = bottle }

    /// The environment every Wine process of the bottle gets. Rosetta hides AVX/AVX2/F16C from x86 code unless asked.
    public func environment(_ extra: [String: String] = [:]) -> [String: String] {
        var env = [
            "WINEPREFIX": bottle.prefixPath,
            "WINEDEBUG": "-all",
            "ROSETTA_ADVERTISE_AVX": "1",
        ]
        for (k, v) in extra { env[k] = v }
        return env
    }

    @discardableResult
    public func run(_ args: [String], cwd: URL? = nil, timeout: TimeInterval? = nil, extraEnv: [String: String] = [:]) -> ShellResult {
        Shell.run(runtime.wine.path, args, env: environment(extraEnv), cwd: cwd, timeout: timeout)
    }

    public func spawn(_ args: [String], cwd: URL? = nil, extraEnv: [String: String] = [:], logFile: URL? = nil) throws -> Process {
        try Shell.spawn(runtime.wine.path, args, env: environment(extraEnv), cwd: cwd, logFile: logFile)
    }

    // MARK: Registry (through `wine reg`, which talks to the prefix's wineserver)

    public func regAdd(key: String, name: String, value: String) -> Bool {
        run(["reg", "add", key, "/v", name, "/t", "REG_SZ", "/d", value, "/f"], timeout: 60).ok
    }

    public func regDelete(key: String, name: String) {
        run(["reg", "delete", key, "/v", name, "/f"], timeout: 60)
    }

    public func regKeyExists(_ key: String) -> Bool {
        run(["reg", "query", key], timeout: 60).ok
    }

    public func regValues(key: String) -> [String: String] {
        let r = run(["reg", "query", key], timeout: 60)
        var out: [String: String] = [:]
        for line in r.stdout.replacingOccurrences(of: "\r", with: "").split(separator: "\n") {
            let parts = line.split(separator: " ", omittingEmptySubsequences: true)
            if parts.count >= 3, parts[1] == "REG_SZ" { out[String(parts[0])] = parts[2...].joined(separator: " ") }
        }
        return out
    }

    /// Stops the processes of this bottle (wineserver -k for this prefix only).
    public func killAll() {
        Shell.run(runtime.wineserver.path, ["-k"], env: environment(), timeout: 30)
    }
}
