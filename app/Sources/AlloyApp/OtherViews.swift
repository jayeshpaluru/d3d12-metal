// SPDX-License-Identifier: LGPL-2.1-or-later
import AlloyCore
import SwiftUI
import UniformTypeIdentifiers

/// Paths shown to the user start with ~ instead of the home directory.
func tilde(_ s: String) -> String { s.replacingOccurrences(of: NSHomeDirectory(), with: "~") }

// MARK: - Setup assistant

struct SetupStep: Identifiable {
    var id: String
    var title: String
    var detail: String
    var done: Bool
    var action: (title: String, run: () -> Void)?
}

struct SetupView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) var dismiss

    func check(_ id: String) -> Check? { model.checks.first { $0.id == id } }

    var steps: [SetupStep] {
        let wineOK = check("wine")?.level == .ok
        let hasBottle = model.ws.bottles.bottle(named: model.bottleName) != nil
        let steamOK = (try? model.ws.steam(bottleName: model.bottleName))?.isInstalled ?? false
        return [
            SetupStep(id: "rosetta", title: "Rosetta 2", detail: tilde(check("rosetta")?.detail ?? "Checking..."), done: check("rosetta")?.level == .ok,
                      action: check("rosetta")?.level == .error ? ("Copy install command", {
                          NSPasteboard.general.clearContents()
                          NSPasteboard.general.setString("softwareupdate --install-rosetta --agree-to-license", forType: .string)
                      }) : nil),
            SetupStep(id: "msc", title: "Apple Metal Shader Converter",
                      detail: check("msc")?.level == .ok ? "Found." : "Apple's license does not let us include it. Download it from Apple's developer site, install the package, then press Check Again.",
                      done: check("msc")?.level == .ok,
                      action: check("msc")?.level == .ok ? nil : ("Open Apple's page", { NSWorkspace.shared.open(Doctor.mscDownloadURL) })),
            SetupStep(id: "wine", title: "Wine \(model.ws.config.wineVersion)",
                      detail: wineOK ? tilde(check("wine")?.detail ?? "") : "Downloaded from the Gcenx macOS builds (about 190 MB) and checked against a pinned checksum.",
                      done: wineOK, action: wineOK ? nil : ("Download", { model.installWine() })),
            SetupStep(id: "bottle", title: "Bottle \"\(model.bottleName)\"", detail: hasBottle ? "A Wine prefix for Steam and your games." : "Creates a separate Windows environment for Steam and your games.",
                      done: hasBottle, action: (hasBottle || !wineOK) ? nil : ("Create", { model.createBottle(model.bottleName) })),
            SetupStep(id: "steam", title: "Steam", detail: steamOK ? "Installed in the bottle." : "Downloads Valve's installer; you accept Valve's terms in Steam.",
                      done: steamOK, action: (steamOK || !hasBottle) ? nil : ("Install", { model.installSteam() })),
            SetupStep(id: "signin", title: "Sign in to Steam", detail: model.steamRunning ? "Steam is running. Sign in in its window, then install your games." : "Starts Steam; sign in in its window and install your games.",
                      done: model.steamRunning, action: steamOK && !model.steamRunning ? ("Start Steam", { model.startSteam() }) : nil),
        ]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Welcome to \(AppInfo.name)").font(.largeTitle.weight(.semibold))
                Text("Play Windows Direct3D 12 games on your Mac. A few things to set up first.").foregroundStyle(.secondary)
            }
            VStack(spacing: 0) {
                ForEach(Array(steps.enumerated()), id: \.element.id) { i, step in
                    HStack(alignment: .top, spacing: 12) {
                        Image(systemName: step.done ? "checkmark.circle.fill" : "\(i + 1).circle")
                            .font(.title2).foregroundStyle(step.done ? .green : .secondary)
                            .accessibilityLabel(step.done ? "Done" : "To do")
                        VStack(alignment: .leading, spacing: 2) {
                            Text(step.title).font(.headline)
                            Text(step.detail).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                        Spacer()
                        if let a = step.action { Button(a.title) { a.run() }.disabled(model.busy != nil) }
                    }
                    .padding(.vertical, 10)
                    if i < steps.count - 1 { Divider() }
                }
            }
            .padding(.horizontal, 14)
            .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 10))
            if let busy = model.busy {
                HStack { if let p = model.progress { ProgressView(value: p) } else { ProgressView().controlSize(.small) }; Text(busy).font(.callout) }
            }
            HStack {
                Button("Check Again") { model.refresh(full: true) }
                Spacer()
                Button("Done") { model.finishSetup(); dismiss() }.buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
            }
        }
        .padding(28)
        .frame(width: 640)
        .onAppear { model.refresh(full: true) }
    }
}

// MARK: - Steam

struct SteamView: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let steam = try? model.ws.steam(bottleName: model.bottleName)
        Form {
            Section {
                LabeledContent("Bottle", value: model.bottleName)
                LabeledContent("Status") {
                    HStack(spacing: 6) {
                        Circle().fill(model.steamRunning ? Color.green : Color.secondary).frame(width: 9, height: 9)
                        Text(steam?.isInstalled == true ? (model.steamRunning ? "Running" : "Not running") : "Not installed")
                    }
                }
            }
            Section("Controls") {
                HStack {
                    if steam?.isInstalled != true {
                        Button("Install Steam") { model.installSteam() }.buttonStyle(.borderedProminent)
                    } else {
                        Button { model.startSteam() } label: { Label("Start", systemImage: "play.fill") }.disabled(model.steamRunning)
                        Button { model.stopSteam() } label: { Label("Stop", systemImage: "stop.fill") }.disabled(!model.steamRunning)
                        Button { model.restartSteam() } label: { Label("Restart", systemImage: "arrow.clockwise") }.disabled(!model.steamRunning)
                        Button(role: .destructive) { model.stopSteam(force: true) } label: { Label("Force Quit", systemImage: "xmark.octagon") }
                            .help("Stops every Windows process of this bottle")
                    }
                }
                .disabled(model.busy != nil)
            }
            Section("About") {
                Text("Steam runs inside the bottle with a small helper so its windows draw correctly, and with AVX2 enabled for games that need it. Sign in in the Steam window; your games appear in the Library once installed.")
                    .font(.callout).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .navigationTitle("Steam")
    }
}

// MARK: - Bottles

struct BottlesView: View {
    @EnvironmentObject var model: AppModel
    @State private var newName = ""
    @State private var confirmRemove: Bottle?

    var body: some View {
        VStack(alignment: .leading) {
            List {
                ForEach(model.bottles) { b in
                    HStack {
                        Image(systemName: "shippingbox").foregroundStyle(.secondary)
                        VStack(alignment: .leading) {
                            Text(b.name).font(.headline)
                            Text(tilde(b.prefixPath)).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                        }
                        Spacer()
                        if b.name == model.bottleName { StatusBadge(text: "Default", color: .blue) }
                        else { Button("Make Default") { model.updateConfig { $0.defaultBottle = b.name } } }
                        Button { NSWorkspace.shared.activateFileViewerSelecting([b.prefix]) } label: { Image(systemName: "folder") }
                            .help("Show in Finder").accessibilityLabel("Show in Finder")
                        Button(role: .destructive) { confirmRemove = b } label: { Image(systemName: "trash") }
                            .help("Remove").accessibilityLabel("Remove bottle")
                    }
                    .padding(.vertical, 4)
                }
                if model.bottles.isEmpty { Text("No bottles yet.").foregroundStyle(.secondary) }
            }
            HStack {
                TextField("New bottle name", text: $newName).frame(width: 220)
                Button("Create") { model.createBottle(newName); newName = "" }.disabled(newName.isEmpty || model.busy != nil)
                Button("Add Existing Prefix...") {
                    let p = NSOpenPanel()
                    p.canChooseDirectories = true; p.canChooseFiles = false
                    p.message = "Choose a Wine prefix folder (it contains system.reg)"
                    if p.runModal() == .OK, let url = p.url { model.addBottle(name: url.lastPathComponent, prefix: url) }
                }
            }
            .padding()
        }
        .navigationTitle("Bottles")
        .confirmationDialog("Remove bottle \"\(confirmRemove?.name ?? "")\"?", isPresented: Binding(get: { confirmRemove != nil }, set: { if !$0 { confirmRemove = nil } })) {
            if let b = confirmRemove {
                Button("Forget Bottle") { model.removeBottle(b, deleteFiles: false); confirmRemove = nil }
                if b.managed { Button("Delete Bottle and Its Files", role: .destructive) { model.removeBottle(b, deleteFiles: true); confirmRemove = nil } }
            }
        } message: { Text("Games installed inside the bottle are deleted only with \"Delete Bottle and Its Files\".") }
    }
}

// MARK: - Health

struct HealthView: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        List {
            ForEach(model.checks) { c in
                HStack(alignment: .top, spacing: 12) {
                    Image(systemName: icon(c.level)).foregroundStyle(color(c.level)).font(.title3)
                        .accessibilityLabel(c.level.rawValue)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(c.title).font(.headline)
                        Text(tilde(c.detail)).font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
                        if let fix = c.fix { Text(fix).font(.callout).foregroundStyle(color(c.level)).textSelection(.enabled) }
                    }
                }
                .padding(.vertical, 3)
            }
        }
        .navigationTitle("Health")
        .toolbar {
            ToolbarItemGroup {
                Button { model.refresh(full: true) } label: { Label("Check Again", systemImage: "arrow.clockwise") }
                Button { model.report(for: nil) } label: { Label("Report a Problem...", systemImage: "ladybug") }
                Button { model.showSetup = true } label: { Label("Setup Assistant", systemImage: "wand.and.stars") }
            }
        }
    }

    func icon(_ l: Check.Level) -> String {
        switch l { case .ok: return "checkmark.circle.fill"; case .warning: return "exclamationmark.triangle.fill"; case .error: return "xmark.octagon.fill"; case .info: return "info.circle.fill" }
    }
    func color(_ l: Check.Level) -> Color {
        switch l { case .ok: return .green; case .warning: return .orange; case .error: return .red; case .info: return .secondary }
    }
}

// MARK: - Preferences

struct PreferencesView: View {
    @EnvironmentObject var model: AppModel
    @State private var wineRoot = ""
    @State private var layerDir = ""
    @State private var cacheSize: Int64 = 0

    var body: some View {
        Form {
            Section("Wine") {
                LabeledContent("Release", value: model.ws.config.wineVersion)
                TextField("Custom Wine folder (contains bin/wine)", text: $wineRoot, prompt: Text("Managed download"))
                    .onSubmit { model.updateConfig { $0.wineRoot = wineRoot.isEmpty ? nil : wineRoot } }
                if let rt = RuntimeManager.locate(paths: model.ws.paths, config: model.ws.config) {
                    Text(tilde(rt.root.path)).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
            Section("Layer") {
                TextField("Layer build folder", text: $layerDir, prompt: Text("Bundled build"))
                    .onSubmit { model.updateConfig { $0.layerDir = layerDir.isEmpty ? nil : layerDir } }
                Text(model.ws.layerBuild.map { "\($0.version) at \(tilde($0.dir.path))" } ?? "No layer build found").font(.caption).foregroundStyle(.secondary)
            }
            Section("Shader cache") {
                LabeledContent("Size on disk", value: ByteCountFormatter.string(fromByteCount: cacheSize, countStyle: .file))
                Button("Clear Shader Cache") { model.clearShaderCache(); cacheSize = model.shaderCacheSize() }
            }
            Section("Files") {
                LabeledContent("Data folder") {
                    HStack {
                        Text(tilde(model.ws.paths.home.path)).font(.caption).textSelection(.enabled)
                        Button("Show") { NSWorkspace.shared.open(model.ws.paths.home) }
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 560, height: 480)
        .onAppear {
            wineRoot = model.ws.config.wineRoot ?? ""
            layerDir = model.ws.config.layerDir ?? ""
            cacheSize = model.shaderCacheSize()
        }
    }
}

// MARK: - Add a game

struct AddGameView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) var dismiss
    @State private var folder: URL?
    @State private var candidates: [ExeCandidate] = []
    @State private var exe = ""
    @State private var name = ""
    @State private var bottle = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Add a Game").font(.title2.weight(.semibold))
            Text("Pick the folder of a Windows game that is not in Steam. The game runs in the chosen bottle.").foregroundStyle(.secondary)
            HStack {
                Text(folder.map { tilde($0.path) } ?? "No folder chosen").lineLimit(1).truncationMode(.middle).foregroundStyle(folder == nil ? .secondary : .primary)
                Spacer()
                Button("Choose Folder...") {
                    let p = NSOpenPanel()
                    p.canChooseDirectories = true; p.canChooseFiles = false
                    if p.runModal() == .OK, let url = p.url {
                        folder = url
                        name = url.lastPathComponent
                        let found = ExeFinder.candidates(in: url)
                        candidates = found
                        exe = found.first.map { $0.url.path.replacingOccurrences(of: url.path + "/", with: "") } ?? ""
                    }
                }
            }
            if !candidates.isEmpty {
                Picker("Executable", selection: $exe) {
                    ForEach(candidates, id: \.url) { c in
                        let rel = c.url.path.replacingOccurrences(of: (folder?.path ?? "") + "/", with: "")
                        Text(rel + (c.usesD3D12 ? "  (Direct3D 12)" : "")).tag(rel)
                    }
                }
            }
            TextField("Name", text: $name)
            Picker("Bottle", selection: $bottle) { ForEach(model.bottles) { Text($0.name).tag($0.name) } }
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }
                Button("Add") {
                    guard let folder else { return }
                    let exeURL = folder.appendingPathComponent(exe)
                    do {
                        try model.ws.library.addCustom(name: name, bottle: bottle.isEmpty ? model.bottleName : bottle,
                                                       folder: exeURL.deletingLastPathComponent(), exe: exeURL.lastPathComponent)
                        model.refresh()
                        dismiss()
                    } catch { model.errorMessage = "\(error)" }
                }
                .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                .disabled(folder == nil || exe.isEmpty || name.isEmpty)
            }
        }
        .padding(24)
        .frame(width: 520)
        .onAppear { bottle = model.bottleName }
    }
}
