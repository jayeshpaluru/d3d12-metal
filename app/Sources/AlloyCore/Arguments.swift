// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// The command line of the `alloy` tool: positional words, `--flag`, `--option value` / `--option=value` (an option can repeat:
/// `--env A=1 --env B=2`), and everything after `--` untouched.
public struct Args {
    public var positional: [String] = []
    public var multi: [String: [String]] = [:]
    public var flags: Set<String> = []
    public var passthrough: [String] = []

    /// Options that take a value; everything else starting with "--" is a flag.
    public static let valueOptions: Set<String> = [
        "bottle", "prefix", "name", "folder", "exe", "appid", "version", "output", "lines", "hud", "gpu", "feature-level", "gdeflate",
        "log", "env", "args", "wine-root", "timeout", "profile",
    ]

    public init(_ argv: [String]) {
        var i = 0
        while i < argv.count {
            let a = argv[i]
            if a == "--" { passthrough = Array(argv[(i + 1)...]); break }
            if a.hasPrefix("--") {
                let key = String(a.dropFirst(2))
                if let eq = key.firstIndex(of: "=") {
                    multi[String(key[..<eq]), default: []].append(String(key[key.index(after: eq)...]))
                } else if Self.valueOptions.contains(key), i + 1 < argv.count {
                    i += 1
                    multi[key, default: []].append(argv[i])
                } else {
                    flags.insert(key.lowercased())
                }
            } else if a == "-h" {
                flags.insert("help")
            } else {
                positional.append(a)
            }
            i += 1
        }
    }

    public func has(_ flag: String) -> Bool { flags.contains(flag) }
    /// The last value of an option.
    public func option(_ name: String) -> String? { multi[name]?.last }
    /// Every value of a repeatable option, in order.
    public func values(_ name: String) -> [String] { multi[name] ?? [] }
}
