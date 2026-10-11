// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

public struct ShellResult: Sendable {
    public var status: Int32
    public var stdout: String
    public var stderr: String
    public var ok: Bool { status == 0 }
}

public enum Shell {
    /// Runs a program to completion and collects its output.
    @discardableResult
    public static func run(_ executable: String, _ args: [String] = [], env: [String: String] = [:],
                           cwd: URL? = nil, timeout: TimeInterval? = nil) -> ShellResult {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: executable)
        p.arguments = args
        p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 }
        if let cwd { p.currentDirectoryURL = cwd }
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        p.standardInput = FileHandle.nullDevice
        var outData = Data(), errData = Data()
        let lock = NSLock()
        out.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            lock.lock(); outData.append(d); lock.unlock()
        }
        err.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            lock.lock(); errData.append(d); lock.unlock()
        }
        do { try p.run() } catch {
            return ShellResult(status: 127, stdout: "", stderr: "cannot run \(executable): \(error.localizedDescription)")
        }
        if let timeout {
            let deadline = Date().addingTimeInterval(timeout)
            while p.isRunning, Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
            if p.isRunning { p.terminate(); p.waitUntilExit() }
        } else {
            p.waitUntilExit()
        }
        out.fileHandleForReading.readabilityHandler = nil
        err.fileHandleForReading.readabilityHandler = nil
        lock.lock()
        outData.append(out.fileHandleForReading.readDataToEndOfFile())
        errData.append(err.fileHandleForReading.readDataToEndOfFile())
        let result = ShellResult(status: p.terminationStatus,
                                 stdout: String(decoding: outData, as: UTF8.self),
                                 stderr: String(decoding: errData, as: UTF8.self))
        lock.unlock()
        return result
    }

    /// Starts a program that keeps running after we return; its output goes to `logFile`.
    @discardableResult
    public static func spawn(_ executable: String, _ args: [String] = [], env: [String: String] = [:],
                             cwd: URL? = nil, logFile: URL? = nil) throws -> Process {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: executable)
        p.arguments = args
        p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 }
        if let cwd { p.currentDirectoryURL = cwd }
        p.standardInput = FileHandle.nullDevice
        if let logFile {
            try FileManager.default.createDirectory(at: logFile.deletingLastPathComponent(), withIntermediateDirectories: true)
            FileManager.default.createFile(atPath: logFile.path, contents: nil)
            let h = try FileHandle(forWritingTo: logFile)
            p.standardOutput = h
            p.standardError = h
        } else {
            p.standardOutput = FileHandle.nullDevice
            p.standardError = FileHandle.nullDevice
        }
        try p.run()
        return p
    }

    /// Process ids whose command name is exactly `name` (Wine names a process after its exe).
    public static func pids(named name: String) -> [Int32] {
        run("/usr/bin/pgrep", ["-x", name]).stdout.split(separator: "\n").compactMap { Int32($0) }
    }

    /// Process ids whose full command line matches the pattern.
    public static func pids(matching pattern: String) -> [Int32] {
        run("/usr/bin/pgrep", ["-f", pattern]).stdout.split(separator: "\n").compactMap { Int32($0) }
    }

    /// Single-quotes a word for display in a shell command line.
    public static func quote(_ s: String) -> String {
        s.range(of: "^[A-Za-z0-9_./=:@%+,-]+$", options: .regularExpression) != nil ? s : "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
