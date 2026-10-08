// Finds a window of the Wine process by title and says whether it can be captured.
//
//   find_window "<title>"      prints one line:
//     ok <CGWindowID>            screencapture -l <id> can capture it
//     not-found                  no on-screen window has that title
//     blocked <reason>           the display is asleep, the session locked, or the
//                                terminal has no Screen Recording permission
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2 else {
    print("usage: find_window <title>")
    exit(2)
}
let title = CommandLine.arguments[1]

if CGDisplayIsAsleep(CGMainDisplayID()) != 0 {
    print("blocked display-asleep")
    exit(0)
}
if let session = CGSessionCopyCurrentDictionary() as? [String: Any],
   let locked = session["CGSSessionScreenIsLocked"] as? Bool, locked {
    print("blocked session-locked")
    exit(0)
}
if !CGPreflightScreenCaptureAccess() {
    print("blocked no-screen-recording-permission")
    exit(0)
}

let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
for window in windows {
    if let name = window[kCGWindowName as String] as? String, name.contains(title),
       let number = window[kCGWindowNumber as String] as? Int {
        print("ok \(number)")
        exit(0)
    }
}
print("not-found")
