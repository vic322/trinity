# Crimson Desert Combat Menu (Beta)

A focused, single-player combat menu for **Crimson Desert**, built on [Trinity](https://github.com/mul0095/Trinity) by **XeTrinityz** with community updates by **Lian (mul0095)**.

Where upstream Trinity is a full mod menu (inventory, teleport, equipment, world/weather), this build deliberately exposes **only combat options** and pins itself to one verified game revision. It is smaller, starts every session with all cheats off, and refuses to install hooks on any executable it has not been validated against.

> **Beta.** Validated against Crimson Desert `2.02.00`, executable `1.0.0.2850`, on one machine. Any other game build is unsupported and the mod will decline to hook rather than guess.

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

Download the release archive and copy both files into your game's `bin64` folder, beside `CrimsonDesert.exe`:

```
<Steam>\steamapps\common\Crimson Desert\bin64
```

- `Trinity.asi` — this combat menu
- `winmm.dll` — [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) v9.7.4, which loads it at startup

No game executable or archive is modified or replaced. Back up your save folder before first use.

## Uninstall

Exit the game completely, then rename `Trinity.asi` to `Trinity.asi.disabled` in `bin64`. To remove the loader as well, rename `winmm.dll` to `winmm.dll.disabled`. Restoring the original names re-enables them.

## Build from source

Requires Visual Studio 2022 C++ tools, a Windows SDK, CMake, and Git.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Output is `build/Release/Trinity.asi`.

### What differs from upstream

- `src/gui/menu.cpp` — replaced with a single COMBAT tab.
- `src/core/mod.cpp` — hard revision gate on `2850`; installs only the player hooks and a player-only game-thread driver.
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
