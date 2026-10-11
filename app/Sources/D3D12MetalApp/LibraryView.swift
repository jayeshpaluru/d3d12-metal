// SPDX-License-Identifier: LGPL-2.1-or-later
import Core
import SwiftUI

struct ArtworkView: View {
    var url: URL?
    var title: String
    var body: some View {
        ZStack {
            if let url, let image = NSImage(contentsOf: url) {
                Image(nsImage: image).resizable().scaledToFill()
            } else {
                LinearGradient(colors: [.indigo.opacity(0.7), .purple.opacity(0.5)], startPoint: .topLeading, endPoint: .bottomTrailing)
                VStack(spacing: 6) {
                    Image(systemName: "gamecontroller.fill").font(.system(size: 34))
                    Text(title).font(.headline).multilineTextAlignment(.center).padding(.horizontal, 8)
                }
                .foregroundStyle(.white.opacity(0.9))
            }
        }
        .accessibilityHidden(true)
    }
}

struct StatusBadge: View {
    var text: String
    var color: Color
    var body: some View {
        Text(text).font(.caption2.weight(.semibold))
            .padding(.horizontal, 7).padding(.vertical, 2)
            .background(color.opacity(0.18), in: Capsule())
            .foregroundStyle(color)
    }
}

func layerBadge(_ s: GameState) -> (String, Color) {
    if s.running { return ("Running", .green) }
    if !s.layer.installed { return ("Layer not installed", .secondary) }
    return s.layer.upToDate ? ("Layer installed", .blue) : ("Update available", .orange)
}

struct LibraryView: View {
    @EnvironmentObject var model: AppModel
    @State private var search = ""
    @State private var listMode = false

    var filtered: [GameState] {
        search.isEmpty ? model.games : model.games.filter { $0.game.name.localizedCaseInsensitiveContains(search) }
    }

    var body: some View {
        NavigationStack(path: $model.path) {
            Group {
                if model.games.isEmpty {
                    EmptyLibrary()
                } else if listMode {
                    List(filtered) { s in
                        NavigationLink(value: s.id) { GameRow(state: s) }
                    }
                } else {
                    ScrollView {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 170, maximum: 220), spacing: 18)], spacing: 18) {
                            ForEach(filtered) { s in
                                NavigationLink(value: s.id) { GameCard(state: s) }.buttonStyle(.plain)
                            }
                        }
                        .padding(20)
                    }
                }
            }
            .navigationTitle("Library")
            .navigationDestination(for: String.self) { id in GameDetailView(gameID: id) }
            .searchable(text: $search, placement: .toolbar, prompt: "Search games")
            .toolbar {
                ToolbarItemGroup {
                    Picker("View", selection: $listMode) {
                        Image(systemName: "square.grid.2x2").tag(false)
                        Image(systemName: "list.bullet").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .help("Grid or list")
                    Button { model.refresh() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                        .help("Scan the bottles again")
                    Button { model.showAddGame = true } label: { Label("Add Game", systemImage: "plus") }
                        .help("Add a game that is not in Steam")
                }
            }
        }
    }
}

struct GameCard: View {
    var state: GameState
    var body: some View {
        let badge = layerBadge(state)
        VStack(alignment: .leading, spacing: 8) {
            ArtworkView(url: state.game.art, title: state.game.name)
                .aspectRatio(2.0 / 3.0, contentMode: .fit)
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(.separator))
            Text(state.game.name).font(.subheadline.weight(.medium)).lineLimit(2).frame(maxWidth: .infinity, alignment: .leading)
            StatusBadge(text: badge.0, color: badge.1)
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(state.game.name), \(badge.0)")
    }
}

struct GameRow: View {
    var state: GameState
    var body: some View {
        let badge = layerBadge(state)
        HStack(spacing: 12) {
            ArtworkView(url: state.game.art, title: "").frame(width: 34, height: 48).clipShape(RoundedRectangle(cornerRadius: 5))
            VStack(alignment: .leading) {
                Text(state.game.name).font(.body)
                Text(state.game.exe).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            StatusBadge(text: badge.0, color: badge.1)
        }
    }
}

struct EmptyLibrary: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "tray").font(.system(size: 44)).foregroundStyle(.secondary)
            Text("No games yet").font(.title2.weight(.semibold))
            Text("Install games in Steam (Steam section), or add a game that lives in a folder.")
                .foregroundStyle(.secondary).multilineTextAlignment(.center)
            HStack {
                Button("Open Setup Assistant") { model.showSetup = true }
                Button("Add Game...") { model.showAddGame = true }
            }
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
