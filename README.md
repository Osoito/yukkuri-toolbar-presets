# Yukkuri Toolbar Presets

A hotkey mod for *Yukkuri Hakkei* (ゆっくり八景), a closed-source Japanese simulation game built on the HSP3 runtime. The game's top launcher holds nine action slots that have to be reconfigured by hand every time you want to change how you behave. This mod stores eight full launcher layouts and can switch between them with a function key.

    F1 - F8          switch the launcher to preset 1-8
    Shift+F1 - F8    capture the current launcher into that preset
    F9               show or hide the preset panel

The game has no mod API, scripting hook or published format. So this mod works by reading the process.

The mod itself is a download. How it was built is not, and that is what this repository is for.

## What you can take from this

**The HSP3 variable table layout, in [`src/slots.c`](src/slots.c).** A 32-bit HSP3 `PVal` record is 48 bytes and every scalar int starts with the same 28 bytes, so those 28 bytes work as a needle that finds the whole variable table in a running process. That applies to any game built on HSP3, this one included. I have not found it written down anywhere in a form you can act on, which is why it is spelled out under Mechanics below.

**Locating a moving target by value fingerprint.** The base address changes every launch, so the mod reads 35 known integers out of the game's own save file and searches the process for a region where all 35 match at the expected stride. A match on 35 values at a fixed stride is not a coincidence. This beats a hardcoded offset for any game that saves the state you are looking for.

**Painting over a window you do not own, in [`src/overlay.c`](src/overlay.c).** Hooking `BitBlt` and `StretchBlt` in the game's import table and drawing after its own present call. Drawing from any other thread gets overwritten within a frame.

**Why the panel is keyboard only.** A worked example of a constraint that came out of reading the engine. HSP polls `GetAsyncKeyState`, which reports physical button state no matter how messages are routed, so a clickable panel would also feed every click to the game. See Mechanics.

## Build

Both binaries must be 32-bit, because the game process is. MSYS2 MinGW32:

```sh
MINGW32_BIN=/c/msys64/mingw32/bin ./build.sh
```

Produces `build/YukkuriToolbarMod.dll`, `build/YukkuriToolbarMod.exe` and `build/slots_test.exe`.

Sources are kept pure ASCII and all wide strings are written as `\uXXXX` escapes, so the build does not depend on the compiler's input charset guess.

## Install

Copy `YukkuriToolbarMod.exe` and `YukkuriToolbarMod.dll` next to `ゆっくり八景.exe`, then start the game through the mod executable. The mod is based on my environment where a Chinese-translation launcher runs the game, so the executable will run that and inject the DLL once the game window appears. Attaching to an already running game also works. Like clicking the mod executable after you already opened the game itself.

Tested against the Manba Chinese translation of Yukkuri Hakkei, `ゆっくり八景.exe` at 10,639,933 bytes. Other builds have different variable layouts and are not supported for now.

I'll work on making this available for the original Japanese build.

## Mechanics

##### How the toolbar is found

HSP3 keeps every script variable in a table of `PVal` records. In a 32-bit build each record is 48 bytes and starts with the same 28 bytes for any scalar int: type flag, allocation mode, five dimension lengths, element size.

`src/slots.c` uses those 28 bytes as a needle. It scans the game heap for a position where the prefix repeats at a 48-byte stride, which locates the whole variable table, then reads the nine `act_select_key_confing_1..9` values out of it.

The base address moves on every launch, so a fixed offset is useless. The run is identified by fingerprint instead. The 35 launcher-related integers are read from the game's own `key_save` file on disk, and the process is searched for a region where all 35 match at the expected stride. A match on 35 values at a fixed stride is not a coincidence, so this survives both a relaunch and a save reload. `MIN_FINGERPRINT` in `src/mod.c` sets how many distinct values are required before a candidate is accepted.

`src/slots.c` builds as a standalone test binary (`build/slots_test.exe`) that runs the fingerprint logic against a live game without loading anything into it, so the risky part can be debugged outside the injected DLL.

##### How the panel is drawn

The game owns its window and repaints it continuously. `src/overlay.c` hooks `BitBlt` and `StretchBlt` in the game's import table, and paints the preset panel after the game's own present call. Drawing from any other thread gets overwritten within a frame.

The panel colours are sampled from the game's own launcher artwork so the overlay sits in the same palette as the UI it belongs to.

##### Why it is keyboard only

HSP reads input with `GetAsyncKeyState`, which reports physical key and button state no matter how messages are routed. A clickable panel would therefore feed every click through to the game as well, which in this game means performing an action on a creature. Function keys are the one range the game never polls, so they are safe to take.

## Layout

| Path | What it is |
|---|---|
| `src/slots.c` | HSP variable table discovery, read and write of the nine slots |
| `src/presets.c` | Preset storage, `yk_presets.ini` parse and write, two shipped presets |
| `src/names.c` | Action id to display label, loaded from `yk_action_names.ini` |
| `src/overlay.c` | `BitBlt` / `StretchBlt` IAT hooks, overlay paint |
| `src/mod.c` | Hotkey loop, panel layout, toast messages |
| `src/inject.c` | Launcher: starts the game, waits for its window, `LoadLibraryW` in a remote thread |
| `src/slots_test.c` | Standalone harness for the discovery logic |
| `tools/probe.py` | Phase 0 reverse engineering probe: locate, read and write the slots from Python |
| `tools/hspv.py` | HSP `PVal` struct reader |
| `docs/使用说明.txt` | End user manual, Chinese |

## Notes

This repository contains only the mod source and no game files.

Three more repositories from the same run of game-mod work, each written up the same way. [isaac-voice-sfx-pipeline](https://github.com/Osoito/isaac-voice-sfx-pipeline) assigns 43 voice clips across 673 sound effect slots under stated constraints, [isaac-chiikawa-soundtrack](https://github.com/Osoito/isaac-chiikawa-soundtrack) fills 85 music slots from 16 masters, and [slot-assignment-scaffold](https://github.com/Osoito/slot-assignment-scaffold) is the scaffold those two have in common.
