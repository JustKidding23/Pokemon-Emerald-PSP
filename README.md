# Pokémon Emerald PSP

Pokémon Emerald running natively on the Sony PSP (not emulated). This is a fork of
[dnllnoe/pokeemerald-ps2](https://github.com/dnllnoe/pokeemerald-ps2), which builds on
[fuddlesworth/pokeemerald-native](https://github.com/fuddlesworth/pokeemerald-native), the PC
port of the [pret decompilation](https://github.com/pret/pokeemerald) that was originally made
by [NTx86](https://github.com/NTx86/pokeemerald-sdl2pc). The PSP work is on the `psp` branch.
It's made for a PSP 3001 with custom firmware and has so far been tested in PPSSPP.

**Built with the use of AI:** the changes in this fork were made with Claude (Anthropic's AI
assistant). AI-assisted code can contain mistakes, so review it before relying on it.

Bring your own game. Building needs no ROM, because pret's decompilation keeps the game's
graphics and sound in the repository, but an EBOOT built from it contains the game, so don't
share one. No built EBOOTs are published here.

Also by me: [SM64 PSP](https://github.com/JustKidding23/SM64-PSP) and
[Ocarina of Time PSP](https://github.com/JustKidding23/Ocarina-of-Time-PSP)

## PSP

Build it as described in [INSTALL_PC.md](INSTALL_PC.md#psp), then copy the
`EMERALD_PSP` folder from `build/psp32/PSP/GAME/` to `PSP/GAME/` on the memory stick.

| PSP | GBA |
|---|---|
| Cross, Circle | A, B (the menu can swap them) |
| D-pad or analog stick | D-pad |
| L, R, Start, Select | L, R, Start, Select |
| Square (hold) | Fast-forward, four times as fast |
| Triangle | The save file and settings menu |

### Save files

There are ten save files, `saves/slot1.sav` to `saves/slot10.sav` in the game's folder. Each
is an ordinary 128 KB Emerald save, the same as an emulator writes, so you can bring one over
by renaming it. The previous version of each save is kept as `slotN.sav.bak`. A
`pokeemerald.sav` in the game's folder becomes file 1 if there are no save files yet.

Triangle opens a menu that lists the files with the player's name, play time and badges.
Pick a file with Cross to play it, copy it to another file or delete it. Playing another file
starts the game over with it, which loses what you've done since you last saved, so the menu
asks first. It waits until the game has finished saving if it's in the middle of it. The
same menu sets the screen size, smoothing, which button is A, frameskip and a speed display.
Those settings are kept in `settings.ini`.

### Speed

The PSP draws the picture in software. In PPSSPP the overworld runs at a full 60 frames a
second. Battles take longer to draw, so with frameskip on (the default) the game keeps its
speed and draws fewer frames. "Show speed" in the menu shows how many frames a second the game
runs and draws, and how many milliseconds a frame takes.

## PC and PS2

The rest of this file is about the PC and PlayStation 2 builds this one comes from.
To set up the repository, see [INSTALL_PC.md](INSTALL_PC.md).

### Playing

| GBA | Keyboard | Controller |
|---|---|---|
| A | Z | A |
| B | X | B or X |
| Start | Enter | Start |
| Select | \ or Backspace | Back |
| L, R | A, S | LB, RB |
| D-pad | Arrow keys | D-pad or left stick |
| Fast-forward (hold) | Space | Right trigger |

Ctrl+R resets the game, like A+B+Start+Select, Ctrl+P pauses and F11 switches to
fullscreen.

The controls, the window size and fullscreen can be changed in `pokeemerald.ini`, which
the game writes with the defaults when it first starts. It's next to the save file, in
`%APPDATA%\pokeemerald` on Windows, `~/.local/share/pokeemerald` on Linux and
`~/Library/Application Support/pokeemerald` on macOS. If the
folder the game starts in has a `pokeemerald.sav`, where earlier versions saved, both
files are there instead. The game prints where they are when it starts, and
`--save FILE` and `--config FILE` use other files.

SDL knows most controllers. For others, put a `gamecontrollerdb.txt` from
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) in the same folder.

### PlayStation 2

`make ps2` builds `pokeemerald32.elf`, which runs on a PlayStation 2 or in PCSX2. How to
build it is in [INSTALL_PC.md](INSTALL_PC.md#playstation-2). No game data or binaries are
provided here.

The picture is drawn by the PS2's Graphics Synthesizer, from the GBA's VRAM, palettes and
OAM, at twice the GBA's size in the middle of an NTSC screen. Everything the game has
shown in testing, including battles, the title screen and the intro, is drawn this way
at 60 frames a second in PCSX2, and matches the software renderer pixel for pixel where
it was compared. Turned and scaled sprites and backgrounds are finer than the GBA's, and
some blended colors can be one step off. Anything the GS can't draw like the GBA yet
falls back to the software renderer.

Sound is mixed at 24000 Hz, which the PS2's sound driver plays as it is.

| GBA | PS2 controller |
|---|---|
| A | Cross |
| B | Circle |
| Start | Start |
| Select | Select |
| L, R | L1, R1 |
| D-pad | D-pad or left stick |

The save and `pokeemerald.ini` go in a `pokeemerald` folder in the folder the ELF is
started from, like `mass:/POKEEMERALD/pokeemerald/` for an ELF started from
`mass:/POKEEMERALD/` with wLaunchELF. In PCSX2, turn on Host Filesystem so the game can
save next to the ELF.

So far this has only been tested in PCSX2, not on a PS2. The video mode is NTSC only.
