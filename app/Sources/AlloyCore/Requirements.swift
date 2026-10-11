// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// Reading a prefix's registry files without starting Wine.
public enum RegistryFile {
    /// True when the key (HKCU\... or HKLM\...) or one of its subkeys exists in the prefix's user.reg / system.reg.
    public static func keyExists(_ key: String, prefix: URL) -> Bool {
        keysExist([key], prefix: prefix)[key] ?? false
    }

    /// Several keys at once: each registry file is read once.
    public static func keysExist(_ keys: [String], prefix: URL) -> [String: Bool] {
        var texts: [String: String] = [:]
        var result: [String: Bool] = [:]
        for key in keys {
            let parts = key.split(separator: "\\", omittingEmptySubsequences: true).map(String.init)
            guard parts.count >= 2 else { result[key] = false; continue }
            let hive = parts[0].uppercased()
            let file = hive == "HKCU" || hive == "HKEY_CURRENT_USER" ? "user.reg" : "system.reg"
            if texts[file] == nil {
                texts[file] = ((try? String(contentsOf: prefix.appendingPathComponent(file), encoding: .utf8)) ?? "").lowercased()
            }
            // user.reg writes [Software\\Foo\\Bar] with doubled backslashes; HKLM\Software and HKLM\System are the same in system.reg.
            let path = parts.dropFirst().joined(separator: "\\\\").lowercased()
            let text = texts[file]!
            result[key] = text.contains("[" + path + "]") || text.contains("[" + path + "\\\\")
        }
        return result
    }
}

extension Workspace {
    /// Alloy's own record of first-launch setup that was run (or skipped), in the bottle: a requirement that does not leave the
    /// registry key its profile names must still not run again at every launch.
    func requirementMarkerURL(_ bottle: Bottle) -> URL { bottle.prefix.appendingPathComponent("alloy-requirements.json") }

    func requirementMarkers(_ bottle: Bottle) -> [String: String] {
        guard let data = try? Data(contentsOf: requirementMarkerURL(bottle)),
              let m = try? JSONDecoder().decode([String: String].self, from: data) else { return [:] }
        return m
    }

    func requirementKey(_ r: Profile.Requirement, _ game: Game) -> String { library.profile(for: game).id + ":" + r.id }

    /// Requirements of the game's profile that are not done: neither recorded by Alloy nor visible in the registry.
    public func pendingRequirements(for game: Game) -> [Profile.Requirement] {
        let profile = library.profile(for: game)
        guard !profile.requirements.isEmpty, let bottle = bottles.bottle(named: game.bottle) else { return [] }
        let markers = requirementMarkers(bottle)
        let keys = profile.requirements.compactMap(\.doneKey)
        let present = RegistryFile.keysExist(keys, prefix: bottle.prefix)
        return profile.requirements.filter { r in
            if markers[requirementKey(r, game)] != nil { return false }
            if let key = r.doneKey, present[key] == true { return false }
            return true
        }
    }

    private func recordRequirement(_ r: Profile.Requirement, for game: Game, bottle: Bottle, status: String) {
        var markers = requirementMarkers(bottle)
        markers[requirementKey(r, game)] = status
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try? enc.encode(markers).write(to: requirementMarkerURL(bottle), options: .atomic)
    }

    /// Runs one requirement and records the outcome: "done" on success; "skipped" when an optional one failed (so it does not
    /// loop); a failed required one throws and is tried again next time. Steam writes the installscript's HasRunKey after a
    /// "Run Process"; the profile's doneKey is created the same way, in case the game looks for it.
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
            guard r.optional else { throw CoreError("\"\(r.title)\" failed (exit \(res.status)): \(res.stderr.suffix(300))") }
            recordRequirement(r, for: game, bottle: session.bottle, status: "skipped")
            return
        }
        if let key = r.doneKey { _ = session.regAdd(key: key, name: "AlloyInstalled", value: "1") }
        recordRequirement(r, for: game, bottle: session.bottle, status: "done")
    }
}
