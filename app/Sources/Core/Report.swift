// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// A zip of diagnostics for bug reports, with personal data scrubbed.
public enum Report {
    /// Replaces the user name, home directory, e-mail addresses and Steam ids.
    public static func scrub(_ text: String, user: String = NSUserName(), home: String = NSHomeDirectory()) -> String {
        var s = text
        if !home.isEmpty, home != "/" { s = s.replacingOccurrences(of: home, with: "~") }
        if user.count >= 2 {
            s = s.replacingOccurrences(of: "/Users/\(user)", with: "/Users/<user>")
            for variant in ["C:\\users\\\(user)", "C:\\Users\\\(user)", "c:\\users\\\(user)"] {
                s = s.replacingOccurrences(of: variant, with: "C:\\users\\<user>")
            }
            s = s.replacingOccurrences(of: "\\users\\\(user)\\", with: "\\users\\<user>\\")
        }
        s = s.replacingOccurrences(of: "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}", with: "<email>", options: .regularExpression)
        s = s.replacingOccurrences(of: "7656119[0-9]{10}", with: "<steamid>", options: .regularExpression)
        return s
    }

    /// Writes the report zip into `outputDir` and returns its path.
    public static func create(ws: Workspace, game: Game?, outputDir: URL) throws -> URL {
        let fm = FileManager.default
        let stamp: String = {
            let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; return f.string(from: Date())
        }()
        let name = "d3d12metal-report-\(stamp)"
        let tmp = fm.temporaryDirectory.appendingPathComponent("d3d12metal-report-\(UUID().uuidString)")
        let dir = tmp.appendingPathComponent(name)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: tmp) }

        func put(_ text: String, as file: String) {
            try? scrub(text).write(to: dir.appendingPathComponent(file), atomically: true, encoding: .utf8)
        }
        func putFile(_ url: URL, as file: String, tail: Int = 8 << 20) {
            guard let h = try? FileHandle(forReadingFrom: url) else { return }
            defer { try? h.close() }
            let size = (try? h.seekToEnd()) ?? 0
            try? h.seek(toOffset: size > UInt64(tail) ? size - UInt64(tail) : 0)
            let data = (try? h.readToEnd()) ?? Data()
            put((size > UInt64(tail) ? "[... truncated to the last \(tail >> 20) MB ...]\n" : "") + String(decoding: data, as: UTF8.self), as: file)
        }

        var info = "\(AppInfo.name) \(AppInfo.version)\nlayer: \(ws.layerBuild?.version ?? "none")\n"
        info += "created: \(Date())\n\n"
        for c in Doctor.runAll(ws) { info += "[\(c.level.rawValue)] \(c.title): \(c.detail)\n" }
        put(info, as: "doctor.txt")

        if let game {
            let profile = ws.library.profile(for: game)
            put("id: \(game.id)\nname: \(game.name)\nbottle: \(game.bottle)\nexe: \(game.exe)\nfolder: \(game.gameDirPath)\nprofile: \(profile.id)\n"
                + "layer status: \(ws.layerStatus(for: game))\n", as: "game.txt")
            if let data = try? JSONEncoder().encode(ws.settings(for: game)) { put(String(decoding: data, as: UTF8.self), as: "settings.json") }
            putFile(game.gameDir.appendingPathComponent(LayerInstaller.confName), as: "d3d12metal.conf")
            let logs = ws.paths.logDir(for: game.id)
            for f in (try? fm.contentsOfDirectory(atPath: logs.path)) ?? [] where f.hasSuffix(".log") || f.hasSuffix(".prev") || f.hasSuffix(".methods") {
                putFile(logs.appendingPathComponent(f), as: f)
            }
            let failed = logs.appendingPathComponent("failed-shaders")
            if let n = try? fm.contentsOfDirectory(atPath: failed.path), !n.isEmpty {
                put("\(n.count) shader(s) the converter rejected are in \(failed.lastPathComponent) (not included: game content)\n", as: "failed-shaders.txt")
            }
        }
        try fm.createDirectory(at: outputDir, withIntermediateDirectories: true)
        let zip = outputDir.appendingPathComponent(name + ".zip")
        try? fm.removeItem(at: zip)
        let r = Shell.run("/usr/bin/ditto", ["-c", "-k", "--keepParent", dir.path, zip.path])
        guard r.ok else { throw CoreError("Could not create the report: \(r.stderr)") }
        return zip
    }
}
