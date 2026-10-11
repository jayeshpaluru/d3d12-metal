// SPDX-License-Identifier: LGPL-2.1-or-later
import Core
import SwiftUI

@main
struct D3D12MetalApp: App {
    @StateObject private var model = AppModel()

    init() {
        // `swift run` / a bare executable has no bundle: make it a regular, frontmost app.
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }

    var body: some Scene {
        WindowGroup(AppInfo.name) {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 900, minHeight: 600)
        }
        .defaultSize(width: 1120, height: 800)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Add Game...") { model.showAddGame = true }.keyboardShortcut("n")
                Button("Refresh Library") { model.refresh() }.keyboardShortcut("r")
            }
            CommandGroup(after: .appInfo) {
                Button("Setup Assistant...") { model.showSetup = true }
            }
        }
        Settings {
            PreferencesView().environmentObject(model)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        NavigationSplitView {
            List(SidebarItem.allCases, selection: $model.section) { s in
                Label(s.rawValue, systemImage: s.symbol).tag(s)
            }
            .navigationSplitViewColumnWidth(min: 160, ideal: 180)
            .safeAreaInset(edge: .bottom) {
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Circle().fill(model.steamRunning ? Color.green : Color.secondary).frame(width: 8, height: 8)
                        Text(model.steamRunning ? "Steam running" : "Steam stopped").font(.caption)
                    }
                    Text("\(AppInfo.name) \(AppInfo.version)").font(.caption2).foregroundStyle(.secondary)
                }
                .padding(10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .accessibilityElement(children: .combine)
            }
        } detail: {
            switch model.section ?? .library {
            case .library: LibraryView()
            case .steam: SteamView()
            case .bottles: BottlesView()
            case .health: HealthView()
            }
        }
        .overlay(alignment: .bottom) { if let busy = model.busy { BusyBanner(text: busy, progress: model.progress) } }
        .sheet(isPresented: $model.showSetup) { SetupView().environmentObject(model) }
        .sheet(isPresented: $model.showAddGame) { AddGameView().environmentObject(model) }
        .alert("Something went wrong", isPresented: Binding(get: { model.errorMessage != nil }, set: { if !$0 { model.errorMessage = nil } })) {
            Button("OK") { model.errorMessage = nil }
        } message: { Text(model.errorMessage ?? "") }
        .alert("Note", isPresented: Binding(get: { model.infoMessage != nil }, set: { if !$0 { model.infoMessage = nil } })) {
            Button("OK") { model.infoMessage = nil }
        } message: { Text(model.infoMessage ?? "") }
    }
}

struct BusyBanner: View {
    var text: String
    var progress: Double?
    var body: some View {
        HStack(spacing: 12) {
            if let progress { ProgressView(value: progress).frame(width: 140) } else { ProgressView().controlSize(.small) }
            Text(text).font(.callout)
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .background(.regularMaterial, in: Capsule())
        .shadow(radius: 6, y: 2)
        .padding(.bottom, 16)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Working: \(text)")
    }
}
