# Pokémon Emerald PSP

Pokémon Emerald running natively on the Sony PSP (not emulated). This is a fork of
[dnllnoe/pokeemerald-ps2](https://github.com/dnllnoe/pokeemerald-ps2), which builds on
[fuddlesworth/pokeemerald-native](https://github.com/fuddlesworth/pokeemerald-native), the PC
port of the [pret decompilation](https://github.com/pret/pokeemerald) that was originally made
by [NTx86](https://github.com/NTx86/pokeemerald-sdl2pc). The PSP work is on the `psp` branch.
It's made for a PSP 3001 with custom firmware (tested on 6.61 PRO-C) and in PPSSPP.

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
| Cross, Circle | A, B (the settings can swap them) |
| D-pad or analog stick | D-pad |
| L, R, Start, Select | L, R, Start, Select |
| Square (hold) | Turbo, two or three times as fast |
| Triangle | The save files |
| The music note button | The settings (Select and Triangle where it can't be read) |

### Save files

There are three save files, `saves/slot1.sav` to `saves/slot3.sav` in the game's folder.
Each is an ordinary 128 KB Emerald save, the same as an emulator writes, so you can bring
one over by renaming it. The previous version of each save is kept as `slotN.sav.bak`. A
`pokeemerald.sav` in the game's folder becomes file 1 if there are no save files yet.

Triangle shows the files the way the game's CONTINUE screen does, with the player's name,
play time and badges. Choose one to play it, copy it to another file or delete it. Playing
another file starts the game over with it, which loses what you've done since you last
saved, so it asks first. If the game is in the middle of saving, the switch waits until
it's done.

### Settings

The music note button opens the settings: the screen size, smoothing, which button is A,
frameskip, how fast turbo is and a speed display. They're kept in `settings.ini`. The note
button can only be read by the kernel, so `kbuttons.prx`, next to the EBOOT, reads it for
the game.

Turbo runs the game two or three times as fast, but the music keeps its normal tempo.

### Speed

The PSP draws the picture in software. On a PSP 3001 the game itself runs at full speed,
and with frameskip on (the default) it draws every frame it has time for, which is about
half of them in the overworld. "Show speed" in the settings shows how many frames a second
the game runs and draws, and how many milliseconds a frame takes.

### The PSP menu's art and music

`psp/make_art.sh` makes the icon, background and music that the PSP's menu shows for the
game, from the title screen of a PC build of this repository. It needs ImageMagick, ffmpeg
and [atracdenc](https://github.com/dcherednik/atracdenc) for the music. `make psp` puts
them in the EBOOT when they're there. They come from the game, so they aren't in the
repository.

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
