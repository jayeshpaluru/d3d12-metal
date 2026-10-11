# Contributing

Thank you for helping. The project is licensed **LGPL-2.1-or-later**; by contributing you agree your work is licensed the same way.

## Before you start

- Read [docs/DEVELOPING.md](docs/DEVELOPING.md) (layout, building, tests) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- **Never** include game binaries, assets, shader dumps, captures or screenshots with game content, and no Apple Metal Shader Converter files
  ([docs/LICENSING.md](docs/LICENSING.md)). Do not copy code from sources with an incompatible licence; the layer is a clean-room implementation of the
  public API documentation. Facts about an interface (ids, structure layouts) are fine, with a note in `THIRD_PARTY_NOTICES.md`.
- Game-specific behaviour goes in `profiles/*.json`, not in `src/` or the tools.

## Making a change

1. Keep changes small and focused; match the surrounding style, naming and comment density. Reuse helpers.
2. Add or update tests with behaviour changes: native tests in `tests/` (portable tests also run under Wine), Swift tests in `app/Tests`.
3. Run `tools/check.sh` (add `--validation` for layer changes, and `tools/run-wine-tests.sh` / `tools/run-godot-test.sh` when the Wine build is affected).
4. Every source file starts with `SPDX-License-Identifier: LGPL-2.1-or-later` (`tools/check.sh` verifies it).
5. Commit messages are conventional (`feat:`, `fix:`, `docs:`, `test:`, `perf:`, `chore:`); explain why in the body when it is not obvious.

## Adding a game profile

See "Adding a game" in [docs/DEVELOPING.md](docs/DEVELOPING.md). Include the status you actually observed, which Mac and macOS version, and add a row to
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). Settings that make a game work belong in the profile's `conf`; if a fix needs layer code, make it generic (a conf option or a
correct implementation), not a check for the game's name.

## Reporting bugs

Use the app's **Report a Problem** (or `alloy report <game>`) and attach the zip; it has personal paths removed, but look inside first.
