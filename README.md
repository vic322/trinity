# Crimson Desert Combat Menu (Beta)

A focused, single-player combat menu for **Crimson Desert**, built on [Trinity](https://github.com/mul0095/Trinity) by **XeTrinityz** with community updates by **Lian (mul0095)**.

Where upstream Trinity is a full mod menu (inventory, teleport, equipment, world/weather), this build deliberately exposes **only combat options** and pins itself to one verified game revision. It is smaller, starts every session with all cheats off, and refuses to install hooks on any executable it has not been validated against.

---

<div align="center">

# Works with · Crimson Desert 2.02.00
# Game executable · 1.0.0.2850

**Nothing else.** Any other build and the menu disables itself and tells you so on screen.

</div>

### How to check your version before installing

Right-click `CrimsonDesert.exe` in your game's `bin64` folder → **Properties** → **Details** → read **File version**. It must read exactly `1.0.0.2850`.

You don't have to do this by hand — the included installer reads it for you and refuses to install on anything else.

---

> **Beta.** Validated on one machine. A Crimson Desert patch changes the executable and this build stops working until it is retargeted.

> **Single-player only.** Do not use in online or anti-cheat-protected modes. Not affiliated with or endorsed by Pearl Abyss.

---

## Features

| Option | Behaviour |
| --- | --- |
| **God Mode** | Keeps tracked player health full and blocks incoming health damage. |
| **Infinite Stamina** | Keeps player stamina full for sprinting, dodging, and climbing. |
| **Damage Multiplier** | Scales damage from your attacks, 1x–100x. 1x is normal. |
| **Extreme Damage** | 10,000x outgoing damage, overriding the slider while enabled. |
| **Disable All** | Turns every cheat off and restores 1x damage. |

Scripted invulnerability and boss phase transitions can still prevent a one-hit kill.

## Controls

Launch the game normally through Steam and load a save. The menu needs a live player character, so nothing responds on the title screen.

- **Open / close:** tilde/backtick (<kbd>~</kbd> / <kbd>`</kbd>, the key below <kbd>Esc</kbd>, no Shift), or **LB + D-pad Down** on a controller.
- **Navigate:** arrow keys or D-pad. **Toggle:** <kbd>Enter</kbd> or <kbd>A</kbd>.

The panel header reports whether a player is being tracked yet. Every launch starts with all cheats **off** — settings are not persisted in this build.

## Install

Download the release archive, extract it anywhere, and **double-click `Install.bat`**.

It finds Crimson Desert through Steam on its own, checks your executable version, backs up anything already in `bin64`, and copies the two files in. If your game is a different version it stops and tells you, without changing anything.

If the game is somewhere the installer can't find, point it at the folder yourself:

```powershell
.\Install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Crimson Desert"
```

<details>
<summary>Installing by hand instead</summary>

Copy both files into your game's `bin64` folder, beside `CrimsonDesert.exe`:

```
<Steam>\steamapps\common\Crimson Desert\bin64
```

- `Trinity.asi` — this combat menu
- `winmm.dll` — [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) v9.7.4, which loads it at startup

</details>

No game executable or archive is modified or replaced. Back up your save folder before first use.

## Uninstall

**Double-click `Uninstall.bat`**, which removes both files. Or do it by hand: exit the game completely and rename `Trinity.asi` to `Trinity.asi.disabled` in `bin64`. Restoring the name re-enables it.

## If nothing happens in game

The menu tells you what is wrong rather than failing silently:

| What you see | What it means |
| --- | --- |
| A red **UNSUPPORTED GAME VERSION** panel | Your game build is not `1.0.0.2850`. Nothing was hooked and no game memory was touched. |
| Menu opens, but options do nothing | The game version matched but a hook failed. See `Trinity.log` in `bin64`. |
| Nothing at all, no panel | The loader never ran. Check that both files are in `bin64` beside `CrimsonDesert.exe`. |

`Trinity.log` is written next to the game executable on every launch and names the exact failure.

## Build from source

Requires Visual Studio 2022 C++ tools, a Windows SDK, CMake, and Git.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Output is `build/Release/Trinity.asi`.

### What differs from upstream

- `src/core/compat.h` — new; the one place the supported game build is named. Retargeting touches this file, and the installer, overlay warning, and menu header all follow.
- `src/gui/menu.cpp` — replaced with a single COMBAT tab, plus the full-screen unsupported-version panel.
- `src/core/mod.cpp` — hard revision gate on `2850`; installs only the player hooks and a player-only game-thread driver. The overlay is created *before* the version is judged, so a mismatch can be reported on screen instead of failing silently.
- `Install.ps1` / `Uninstall.ps1` — new; Steam auto-detection, an executable-version gate, and backups.
- `src/game/focused_tick.cpp` — new; drives player logic from a single guarded per-frame hook.
- `src/mem/hooks.h` — an ambiguous signature is now a hard failure instead of a warning that hooks the first match.
- `src/game/player.cpp` — PE 2850 stat guard, bounded stamina scanning for type 22, stricter character-manager resolution.
- `CMakeLists.txt` — pins MinHook to a known commit and widens its trampoline allocation range, since the game's low address space is crowded once a save loads.

The upstream sources for inventory, teleport, equipment, and world remain in-tree but are not built into the menu.

## Credits & licence

- **Trinity** — © XeTrinityz, MIT. Community updates by Lian (mul0095).
- **Ultimate ASI Loader** — © ThirteenAG, MIT.
- **Dear ImGui** — © Omar Cornut, MIT.
- **MinHook** — © Tsuda Kageyu, BSD 2-Clause.

Distributed under the MIT licence; see [LICENSE](LICENSE). Full third-party notices ship in the release archive.
