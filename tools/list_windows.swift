// SPDX-License-Identifier: LGPL-2.1-or-later
// Lists the windows of Wine and Steam processes (all of them, on screen or not), one per line:
//   <CGWindowID> | <owner> | <title> | <width> x <height> | onscreen: <bool> | layer: <n>
// tools/run-game.sh picks the game's window from this list and captures it with `screencapture -x -o -l <id>`.
//
//   list_windows [owner-substring ...]     default owners: wine, steam (case-insensitive)
import CoreGraphics
import Foundation

var owners = Array(CommandLine.arguments.dropFirst()).map { $0.lowercased() }
if owners.isEmpty { owners = ["wine", "steam"] }

let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    if !owners.contains(where: { owner.lowercased().contains($0) }) { continue }
    let id = w[kCGWindowNumber as String] as? Int ?? 0
    let name = w[kCGWindowName as String] as? String ?? ""
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    let onscreen = w[kCGWindowIsOnscreen as String] as? Bool ?? false
    let layer = w[kCGWindowLayer as String] as? Int ?? 0
    print(id, "|", owner, "|", name, "|", b["Width"] ?? 0, "x", b["Height"] ?? 0, "| onscreen:", onscreen, "| layer:", layer)
}
