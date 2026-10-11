// SPDX-License-Identifier: LGPL-2.1-or-later
import AlloyCore
import SwiftUI

struct GameDetailView: View {
    @EnvironmentObject var model: AppModel
    var gameID: String
    @State private var settings = GameSettings()
    @State private var envText = ""
    @State private var loadedFor: String?
    @State private var pending: [Profile.Requirement] = []

    var state: GameState? { model.games.first { $0.id == gameID } }

    var body: some View {
        if let s = state {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    header(s)
                    if !pending.isEmpty { setupNote(s) }
                    settingsForm(s)
                    notes(s)
                    tools(s)
                }
                .padding(24)
                .frame(maxWidth: 760, alignment: .leading)
                .frame(maxWidth: .infinity)
            }
            .navigationTitle(s.game.name)
            .onAppear { load(s) }
            .onChange(of: s.running) { _, _ in pending = model.ws.pendingRequirements(for: s.game) }
        } else {
            Text("Game not found").foregroundStyle(.secondary)
        }
    }

    func load(_ s: GameState) {
        guard loadedFor != s.id else { return }
        settings = model.ws.settings(for: s.game)
        envText = settings.customEnv.map { "\($0.key)=\($0.value)" }.sorted().joined(separator: "\n")
        pending = model.ws.pendingRequirements(for: s.game)
        loadedFor = s.id
    }

    func header(_ s: GameState) -> some View {
        let badge = layerBadge(s)
        return HStack(alignment: .top, spacing: 20) {
            ArtworkView(url: s.game.art, title: s.game.name)
                .frame(width: 150, height: 225)
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).stroke(.separator))
            VStack(alignment: .leading, spacing: 10) {
                Text(s.game.name).font(.largeTitle.weight(.semibold))
                HStack {
                    StatusBadge(text: badge.0, color: badge.1)
                    StatusBadge(text: "Compatibility: \(s.profile.status)", color: s.profile.status == "playable" ? .green : .orange)
                    if let v = s.layer.installedVersion { StatusBadge(text: "Layer \(v)", color: .secondary) }
                }
                Text("\(s.game.exe) in \(s.game.bottle)").font(.callout).foregroundStyle(.secondary)
                Spacer(minLength: 6)
                HStack(spacing: 10) {
                    if s.running {
                        Button { model.stop(s) } label: { Label("Stop", systemImage: "stop.fill") }
                            .buttonStyle(.borderedProminent).tint(.red).controlSize(.large)
                    } else {
                        Button { model.play(s) } label: { Label("Play", systemImage: "play.fill") }
                            .buttonStyle(.borderedProminent).controlSize(.large)
                            .disabled(model.busy != nil)
                            .keyboardShortcut(.return, modifiers: [.command])
                    }
                    Menu {
                        Button(s.layer.installed ? "Update layer" : "Install layer") { model.install(s) }
                        Button("Uninstall layer", role: .destructive) { model.uninstall(s) }.disabled(!s.layer.installed)
                    } label: { Label("Layer", systemImage: "square.stack.3d.up") }
                        .controlSize(.large)
                        .menuStyle(.borderlessButton).fixedSize()
                }
            }
        }
    }

    func setupNote(_ s: GameState) -> some View {
        let items = pending
        return GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                Label("First launch setup", systemImage: "wrench.and.screwdriver").font(.headline)
                ForEach(items, id: \.id) { Text("• \($0.title)").font(.callout) }
                Text("Runs automatically the first time you press Play.").font(.caption).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    func settingsForm(_ s: GameState) -> some View {
        GroupBox {
            Form {
                Toggle("Performance HUD (FPS, frame time, GPU time)", isOn: $settings.hud)
                Picker("GPU identity", selection: $settings.gpuIdentity) {
                    ForEach(GPUIdentity.allCases, id: \.self) { Text($0.label).tag($0) }
                }
                .help("Some games refuse unknown GPUs. Try AMD or NVIDIA if a game complains.")
                Toggle("Report Direct3D feature level 12_1", isOn: $settings.featureLevel121)
                Toggle("GPU decompression of DirectStorage (GDeflate)", isOn: $settings.gdeflate)
                Picker("Logging", selection: $settings.logLevel) {
                    ForEach(LogLevel.allCases, id: \.self) { Text($0.label).tag($0) }
                }
                TextField("Extra launch arguments", text: $settings.customArgs, prompt: Text("-windowed"))
                VStack(alignment: .leading) {
                    Text("Extra environment (KEY=VALUE per line)").font(.callout)
                    TextEditor(text: $envText).font(.system(.body, design: .monospaced)).frame(height: 56)
                        .overlay(RoundedRectangle(cornerRadius: 5).stroke(.separator))
                }
                HStack {
                    Spacer()
                    Button("Revert") { loadedFor = nil; load(s) }
                    Button("Save") { commit(s) }.buttonStyle(.borderedProminent)
                }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
        } label: { Label("Settings", systemImage: "slider.horizontal.3").font(.headline) }
    }

    func commit(_ s: GameState) {
        var env: [String: String] = [:]
        for line in envText.split(separator: "\n") {
            let p = line.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            if p.count == 2, !p[0].isEmpty { env[p[0]] = p[1] }
        }
        settings.customEnv = env
        model.save(settings, for: s)
        model.infoMessage = s.layer.installed ? "Saved. The next start of the game uses the new settings." : "Saved. They apply when the layer is installed."
    }

    func notes(_ s: GameState) -> some View {
        Group {
            if !s.profile.notes.isEmpty {
                GroupBox {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(s.profile.notes, id: \.self) { Text("• \($0)").font(.callout).fixedSize(horizontal: false, vertical: true) }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                } label: { Label("Notes (\(s.profile.name))", systemImage: "info.circle").font(.headline) }
            }
        }
    }

    func tools(_ s: GameState) -> some View {
        HStack {
            Button { model.openLogs(s) } label: { Label("Open Logs", systemImage: "doc.text.magnifyingglass") }
            Button { model.report(for: s) } label: { Label("Report a Problem...", systemImage: "ladybug") }
            Button { NSWorkspace.shared.open(s.game.gameDir) } label: { Label("Show Game Folder", systemImage: "folder") }
        }
    }
}
