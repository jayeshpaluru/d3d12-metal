# User guide

## What this does

Windows games talk to the graphics card through Direct3D 12. Macs have Metal instead. Alloy supplies the Direct3D 12 side as
replacement `d3d12.dll` and `dxgi.dll` files that turn the game's calls into Metal, and runs the game inside Wine (a Windows
compatibility layer) with Rosetta 2. The app manages the pieces:

- **Wine**: downloaded once (a pinned release, checked against a SHA-256).
- **Bottles**: separate Windows environments (Wine prefixes). The default bottle "Steam" holds the Windows Steam client and your Steam games.
- **The layer** is copied next to a game's executable, with a backup of any file it replaces. Uninstalling puts everything back.
- **Profiles** hold what is known about a game (launch options, setup it needs, notes).

## First run

1. Open the app. The Setup Assistant lists six steps; each shows a green check when done.
   - *Rosetta 2*: if missing, copy the shown command into Terminal.
   - *Metal Shader Converter*: download it from Apple (button), install the package, press **Check Again**.
   - *Wine*: **Download** (about 190 MB).
   - *Bottle*: **Create**.
   - *Steam*: **Install**. This downloads Valve's installer; you accept Valve's terms in Steam.
   - *Sign in*: **Start Steam**, sign in to the Steam window and install your games (the Mac cannot run games that need Windows-only anti-cheat).
2. Press **Done**. Games that Steam finished installing appear in **Library**. Press **Refresh** after an install finishes.

Reopen the assistant any time from the **Health** page or the app menu.

## Playing

Open a game and press **Play**. The first time, the app installs the layer, runs any first-launch setup the game's profile lists (for example the
PlayStation PC SDK runtime of Spider-Man 2), and starts the game through Steam or directly, as the profile says. **Stop** ends just the game; Steam keeps running.

The first start of a game compiles its shaders: it can take several minutes and the window may look frozen. Later starts use the cache.

### Settings (per game)

| Setting | Use |
|---|---|
| Performance HUD | FPS, frame time and GPU time per frame in the top-left corner |
| GPU identity | what the game is told the GPU is: Apple (default), AMD Radeon RX 6800, NVIDIA GeForce RTX 3080. Try another if a game rejects the GPU |
| Feature level 12_1 | report Direct3D feature level 12_1 (with ordered views); a few games need it |
| DirectStorage GPU decompression | on by default; turn off if a game that streams assets misbehaves |
| Logging | errors only, frame statistics, API trace, everything. Trace makes the game slower |
| Extra arguments, environment | passed to the game |

Press **Save**; the settings are written to the game's `d3d12metal.conf` and used at the next start.

### Games that are not in Steam

**File > Add Game** (or `alloy game add`): pick the game's folder and its `.exe`. The game runs in the chosen bottle. The app shows which executables use Direct3D 12.
Games without a profile get the generic one, which makes no game-specific changes.

## Steam, bottles, preferences

- **Steam** page: start, stop, restart, force quit (stops every Windows process of the bottle).
- **Bottles** page: create, add an existing Wine prefix (it is never deleted unless you ask), choose the default.
- **Preferences** (Cmd-,): a custom Wine folder, the layer build, shader cache size and clearing, the data folder
  (`~/Library/Application Support/Alloy`).

## Command line

`alloy --help` lists everything. `--json` makes every command print JSON (progress goes to stderr). `<game>` is a Steam app id, part of a name,
or an id like `steam:1817070` or `custom:my-game`.

| Command | |
|---|---|
| `setup`, `doctor`, `runtime`, `bottle`, `steam` | environment |
| `library`, `scan`, `game add/remove`, `info` | games |
| `install`, `update`, `uninstall`, `config`, `launch`, `stop` | per game |
| `logs`, `report`, `profiles` | diagnostics |

## When something goes wrong

1. **Health** page / `alloy doctor`: it names what is missing and the fix.
2. **Open Logs**: `d3d12metal.log` is the layer's log (errors; more with a higher logging level), `wine.log` Wine's output for directly started games.
3. **Report a Problem** collects `doctor` output, the game's settings and conf and its logs into a zip in Downloads. User name, home path, e-mail addresses and Steam ids are replaced;
   open the zip and check before you share it. Game files and shaders are not included.

Common causes: Metal Shader Converter not installed; a game that needs AVX2 on macOS 14 (update to 15+); a game whose anti-cheat does not run under Wine;
a game that wants a GPU vendor (try the GPU identity setting).

## Removing things

- A game's layer: **Layer > Uninstall layer** (restores the original files, removes the registry overrides).
- Everything else: delete `~/Library/Application Support/Alloy` (Wine, bottles and logs) and `~/Library/Caches/d3d12metal` (shader cache), then the app. Bottles you added
  from elsewhere are only forgotten, not deleted.
