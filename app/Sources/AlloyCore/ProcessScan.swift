// SPDX-License-Identifier: LGPL-2.1-or-later
import Darwin
import Foundation

/// Finds which bottle a Wine process belongs to. Wine renames its processes and rewrites their environment, but every process
/// of a prefix keeps files open in that prefix's wineserver directory (/tmp/.wine-<uid>/server-<device>-<inode>/, named after the
/// device and inode of the prefix folder).
public enum ProcessScan {
    /// "server-1000019-11e126" for a prefix folder; nil if it cannot be read.
    public static func serverDirectoryName(prefix: URL) -> String? {
        var st = stat()
        guard stat(prefix.path, &st) == 0 else { return nil }
        return "server-" + String(UInt32(bitPattern: st.st_dev), radix: 16) + "-" + String(st.st_ino, radix: 16)
    }

    /// True when the process has a file open in the bottle's wineserver directory.
    public static func process(_ pid: Int32, belongsTo bottle: Bottle) -> Bool {
        guard let server = serverDirectoryName(prefix: bottle.prefix) else { return false }
        let r = Shell.run("/usr/sbin/lsof", ["-p", String(pid), "-Fn"], timeout: 10)
        return r.stdout.contains("/" + server + "/")
    }

    /// The pids of `candidates` that belong to the bottle (all of them when no bottle is given).
    public static func filter(_ candidates: [Int32], in bottle: Bottle?) -> [Int32] {
        guard let bottle else { return candidates }
        return candidates.filter { process($0, belongsTo: bottle) }
    }
}
