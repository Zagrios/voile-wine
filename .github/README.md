# voile-wine

**The Wine inside [Voile](https://github.com/Zagrios/voile), the Windows runtime of [Obelisk](https://github.com/Zagrios/obelisk).** It is CrossOver's open-source Wine, plus Obelisk's changes. One source tree builds both of Voile's engines: Wine for macOS, under Rosetta, and Wine for Linux, which runs on Voile's Linux engine.

This page is voile-wine's own. Wine's README is [README.md](../README.md), unchanged.

## Contents

- [What changes from CrossOver's Wine](#what-changes-from-crossovers-wine)
- [Branches](#branches)
- [Building](#building)
- [Updating CrossOver](#updating-crossover)
- [License](#license)
- [Related repositories](#related-repositories)

## What changes from CrossOver's Wine

Each change is one commit on `main`; `git log crossover..main` lists them. Several come from Proton's Wine ([ValveSoftware/wine](https://github.com/ValveSoftware/wine)), and their commit messages name the original commits.

| Area | Changes |
|---|---|
| **Steam** | Valve's `steamclient` is redirected to Proton's `lsteamclient`, and the Steam client is registered in each prefix. |
| **Linux engine** | `wineobelisk.drv` (display and input) and `wineobeliskaudio.drv` (audio) talk to voile-lkd's wsd window server; `nptunix` is the unix side of Neptune's Direct3D driver. Re-showing a window that already has Wine foreground focus restores native key focus, while no-activate windows and inactive-app deferral remain protected. ntdll and wineserver run on larger host pages, a host address space that is not a power of two, and hosts without netlink. |
| **Gamepads** | `winebus.sys` serves the gamepads of Obelisk's input service (`bus_obelisk.c`), in both Wines. XInput and Windows.Gaming.Input keep controllers across hotplug and report them from startup. |
| **Games** | Fixes for fullscreen windows, keyboard state, audio mixing, the Visual C++ runtime version, crash dialogs and memory allocation, among others. |
| **Prefixes** | User folders stay inside the prefix instead of linking to the Mac home, and the registry is saved outside the wineserver. |
| **macOS** | `winemac.drv` tells Obelisk of Shift-Tab while a game has captured the displays, so its in-game overlay still opens. ntdll searches the folders in `WINEDLLPATH_PREPEND` before its own, so that the graphics backend a game runs with, D3DMetal or DXMT, replaces Wine's `d3d11` and `dxgi`. |
| **Build** | Modules such as `lsteamclient` build outside Wine's tree, `win32u` builds without Vulkan, and ntdll no longer loads CrossOver's compatibility database, which Voile does not ship. |

## Branches

- `crossover` holds CrossOver's Wine sources exactly as CodeWeavers publishes them, one commit per release, tagged `crossover-<version>`. It never carries Obelisk's changes.
- `main` is `crossover` plus Obelisk's changes, one commit each. Releases are tagged `v<crossover version>-<revision>`, such as `v26.3.0-1`. Voile's build pins one of these commits.

## Building

voile-wine is built by [Voile](https://github.com/Zagrios/voile), where it is the `wine` submodule: `scripts/build.sh` builds Wine for macOS and `scripts/build-linux.sh` Wine for Linux. Wine builds straight from that checkout, so a local edit there rebuilds it. Commit changes here, never as patches in Voile.

## Updating CrossOver

```sh
voile/check-crossover.sh                  # exits 10 when a newer release exists
voile/import-crossover.sh 26.4.0          # commits it on crossover, tags crossover-26.4.0
git rebase crossover-26.4.0 main          # replays Obelisk's changes, stopping at conflicts
```

Then tag the new `main` `v26.4.0-1`. In [Voile](https://github.com/Zagrios/voile), point the `wine` submodule at it, update the CrossOver archive in `sources.json` with the hash the import prints, and push a tag `v26.4.0-1` there to publish the runtime.

## License

Like Wine, voile-wine is licensed under the LGPL-2.1-or-later: see [COPYING.LIB](../COPYING.LIB) and [LICENSE](../LICENSE).

## Related repositories

| Repository | Role |
|---|---|
| [Obelisk](https://github.com/Zagrios/obelisk) | The Mac app: Steam, Epic Games and GOG libraries, downloads and launches |
| [Voile](https://github.com/Zagrios/voile) | The Windows runtime Obelisk installs, which builds this Wine |
| **voile-wine** | The Wine inside Voile, for both engines |
| [voile-lkd](https://github.com/Zagrios/voile-lkd) | The Linux engine: x86-64 Linux programs in native macOS processes |
| [Neptune](https://github.com/Zagrios/neptune) | Direct3D for the Linux engine |
| [FEX](https://github.com/Zagrios/FEX) | FEX-Emu with its macOS port, which voile-lkd translates x86-64 code with |
