# Trinity — Crimson Desert (vic322 fork)

An in-game DirectX 12 mod menu for **Crimson Desert**, originally created by
**XeTrinityz**. This repository is a downstream fork of
[Romaconn/crimson-desert-combat-menu](https://github.com/Romaconn/crimson-desert-combat-menu),
which retargeted Trinity at the current game build.

> **Single-player only.** Do not use in online or anti-cheat-protected modes. Not affiliated
> with or endorsed by Pearl Abyss.

---

<div align="center">

# Works with · Crimson Desert 2.02.00
# Game executable · 1.0.0.2850

**Nothing else.** Any other build and the menu disables itself and tells you so on screen.

</div>

Right-click `CrimsonDesert.exe` in your game's `bin64` folder → **Properties** → **Details** →
read **File version**. It must read exactly `1.0.0.2850`. The included installer checks this for
you and refuses to install on anything else.

---

## About this fork

Trinity is capable, but in extended play it is not always dependable: features quietly stop
applying in some game states, and a game patch can take the whole menu offline until somebody
retargets it by hand. This fork is a **maintenance effort aimed at making it dependable enough to
just leave running** — fixing correctness bugs and removing single points of failure. The priority
is that the features already here work, every time, in every game state.

### Approach

Changes are driven by evidence, not guesswork. Every fix starts from something observable — a log
line, a captured fault address, a reproducible state — and the reasoning is written down so the
next person can check it. Where a cause cannot be established, the issue is recorded as an
observation rather than patched speculatively, and anything not actually verified in game is
labelled as such.

### What this fork changes

**1. The tracked player set is seeded with the body you are driving** — *implemented, not yet
observed firing*

The character-manager walk admits bodies whose class vtable matches an anchor taken from the first
player-class character in the list, which is Kliff whenever he is present: he keeps the SelfPlayer
tag even while a secondary protagonist is the one under your control. A protagonist whose class
vtable differs from his is then driven but never enumerated, and everything consuming those sets —
God Mode, the damage multipliers, the stat pins — goes quietly inert for that character, while
features that only act on whatever body is being driven keep working because they follow the
possessor instead.

The possessor's active pawn is authoritative about who is being driven, so it is taken before the
enumeration loop and the loop fills the remaining slots. Seeded rather than appended on purpose:
the loop stops at three, so appending would let two companions fill the set and leave out the
character you are actually playing.

*Honest status:* this was written against a build where the symptom reproduced, but on the current
manager the secondary protagonist passes the vtable gate on its own, so the new path has not been
seen to fire. It is correct as a guard either way; it has not been proven to fix anything here.

**2. The char-manager global is recovered by shape when the byte anchors go stale** — *verified*

Byte anchors key on the instructions *around* the global, so a Title Update that recompiles those
call sites takes them with it. On this build **one of the five anchors still matches** — there is
not much margin left — and a total miss used to abort the whole player subsystem with no way back
short of a new release.

The manager's shape outlives the code that reaches it:

```
global -> P -> mgr,  mgr+ListData = character*[],  mgr+ListCount
```

so the fallback walks the module's committed pages, treats every aligned qword as a candidate
global, and keeps whichever one's double dereference lands on that shape and holds a body whose
possessor round-trips. Reads are SEH-guarded, so following a garbage chain costs a failed read
rather than the process. It runs on its own thread, because nothing is possessed on the title
screen and a pass costs seconds.

Verified against TU 2.02.00 with every anchor dead: resolved in ~750 ms once in-world, with God
Mode, One-Hit Kill, the damage multipliers and the infinite-stat pins all working from it. It stays
dormant whenever the anchors resolve normally.

### Relationship to upstream

This fork intends to stay close to upstream. Both changes are in `src/game/player.cpp` and are
meant to go back as pull requests rather than to diverge.

---

## Controls

Launch the game normally through Steam and load a save. The menu needs a live player character, so
nothing responds on the title screen.

- **Open / close:** tilde/backtick (<kbd>~</kbd> / <kbd>`</kbd>, the key below <kbd>Esc</kbd>, no
  Shift), or **LB + D-pad Down** on a controller.
- **Navigate:** arrow keys or D-pad. **Toggle:** <kbd>Enter</kbd> or <kbd>A</kbd>.

## Install

Download the release archive, extract it anywhere, and **double-click `Install.bat`**.

It finds Crimson Desert through Steam on its own, checks your executable version, backs up anything
already in `bin64`, and copies the files in. If your game is a different version it stops and tells
you, without changing anything.

If the game is somewhere the installer can't find, point it at the folder yourself:

```powershell
.\Install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Crimson Desert"
```

<details>
<summary>Installing by hand instead</summary>

Copy `Trinity.asi` into your game's `bin64` folder, beside `CrimsonDesert.exe`:

```
<Steam>\steamapps\common\Crimson Desert\bin64
```

You also need an ASI loader — [Ultimate ASI
Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `winmm.dll` or `dinput8.dll` — if one
is not already there.

</details>

No game executable or archive is modified or replaced. Back up your save folder before first use.

### A note on updating

Mod sites encode the upload date in the download's folder name, so each update of an ASI mod
arrives as a *new* folder rather than replacing the old one. If two copies of `Trinity.asi` end up
under your mod manager's mods directory, the manager may deploy the older one, and you will be
running a build you did not intend — which looks exactly like "the update did nothing". If Trinity
behaves like an older version after an update, check that only one `Trinity.asi` exists in your
mods directory, and confirm the file actually deployed to `bin64\` is the size you expect.

## Uninstall

**Double-click `Uninstall.bat`**. Or do it by hand: exit the game completely and rename
`Trinity.asi` to `Trinity.asi.disabled` in `bin64`. Restoring the name re-enables it.

## If nothing happens in game

The menu tells you what is wrong rather than failing silently:

| What you see | What it means |
| --- | --- |
| A red **UNSUPPORTED GAME VERSION** panel | Your game build is not `1.0.0.2850`. Nothing was hooked and no game memory was touched. |
| Menu opens, but options do nothing | The game version matched but a hook failed. See `Trinity.log` in `bin64`. |
| Nothing at all, no panel | The loader never ran. Check that `Trinity.asi` and a loader are both in `bin64` beside `CrimsonDesert.exe`. |

`Trinity.log` is written next to the game executable on every launch and names the exact failure.

## Build from source

Requires Visual Studio 2022 C++ tools, a Windows SDK, CMake, and Git.

```powershell
powershell -ExecutionPolicy Bypass -File .\Build_Trinity.ps1
```

Output is `build/Release/Trinity.asi`.

## Credits & licence

Trinity is open source under the MIT licence. The lineage, in order:

- **XeTrinityz** — original Trinity creator and maintainer
  ([XeTrinityz/Trinity](https://github.com/XeTrinityz/Trinity))
- **Orcax1399** — research insights credited by the original project
- **Gugi96**, **namintelvn** — ASI/reference research used during compatibility work
- **slingblade2047** — Crimson Desert 1.17–2.01 compatibility work
  ([slingblade2047/Trinity](https://github.com/slingblade2047/Trinity))
- **Lian / ReXooGen (mul0095)** — vTweak features, localisation, maintenance
  ([ReXooGen/Trinity](https://github.com/ReXooGen/Trinity))
- **Romaconn** — retargeting at Crimson Desert 2.02.00 / PE 1.0.0.2850, version gating, installer
  ([Romaconn/crimson-desert-combat-menu](https://github.com/Romaconn/crimson-desert-combat-menu))
- **vic322** — this fork: stability and correctness maintenance
  ([vic322/trinity](https://github.com/vic322/trinity))

Everything in this repository other than the two changes described in *About this fork* is the work
of the maintainers listed above. The MIT copyright notice in `LICENSE` belongs to XeTrinityz and is
retained unchanged.

Third-party components:

- **Ultimate ASI Loader** — © ThirteenAG, MIT
- **Dear ImGui** — © Omar Cornut, MIT
- **MinHook** — © Tsuda Kageyu, BSD 2-Clause

Distributed under the MIT licence; see [LICENSE](LICENSE).

---

*Crimson Desert is a trademark of Pearl Abyss. This project is intended solely for single-player
modding and educational purposes.*
