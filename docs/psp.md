# Notes on the PSP port

How the PSP version works, what was found on the way, and how to debug it on a real PSP.
It's for anyone working on this port or making their own PSP homebrew.

## How it fits together

The game code is pret's, shared with the GBA, PC and PS2 builds. The PSP version adds:

| File | What it does |
|---|---|
| `src/platform/psp.c` | The frame loop, the buttons, the save files, FILE SELECT and the settings |
| `src/platform/psp_hw.c` | Everything that talks to the PSP: the GU, sound, buttons, files, clock, watchdog and trace |
| `include/platform/psp_hw.h` | The line between them. Only standard C types cross it, so pspsdk's headers and the game's never meet |
| `psp/kbuttons/` | A kernel module that reads the music note button for the game |
| `psp/make_art.sh` | Makes the PSP menu's icon, background and music from a PC build |
| `psp/tools/` | `lwlr_hazards.py` and `trace.py` (see below) |

`make psp` builds the game as a PRX (like pspsdk's `BUILD_PRX=1`), packs it into
`EBOOT.PBP` and copies `kbuttons.prx` next to it. Everything PSP-only in pret's files is
inside `#if defined(PORTABLE) && defined(__PSP__)`, so the GBA, PC and PS2 builds are
unchanged.

### A frame

The game runs one frame for every frame the PSP displays (59.94 a second). The GBA's
picture is drawn in software (`src/platform/gba_easy_draw.c`) into a 240x160 image, which
the GU scales to the screen. When drawing falls behind, frames are run without drawing them
(`SkipFrame` still runs each line's interrupts and HBlank DMAs), so the game keeps its speed.
Turbo runs two or three frames for each one shown and the sound engine only once, so the
music keeps its tempo.

The sound engine mixes at 24000 Hz. A sound thread resamples that to the 44100 Hz the PSP
plays, a little faster or slower as its buffer fills or empties, so the game's frame rate
and the PSP's audio clock never drift apart enough to be heard.

### Saves

The game's flash save is a 128 KB array (`FLASH_BASE`). It's read from `saves/slotN.sav` at
startup and written whole every time the game saves: to `slotN.sav.tmp` first, then the old
file becomes `slotN.sav.bak` and the new one takes its place, since `sceIoRename` doesn't
overwrite. Switching files waits until the game allows a soft reset (it doesn't while
saving), reads the other file and resets. FILE SELECT reads the name, play time and badges
straight from each file's sectors, from the newer of the two copies the game keeps.

### FILE SELECT and the settings

They're drawn over the game, which keeps running without the buttons. FILE SELECT is drawn
like the main menu's CONTINUE window, with the game's own font (`gFontNormalLatinGlyphs`),
the player's window frame (`GetWindowFrameTilesPal`) and the main menu's colors and
darkening. Buttons held when a screen closes are kept from the game until they're let go,
so closing it with B doesn't also close the start menu behind it.

The music note button can only be read by the kernel, so `kbuttons.prx` is loaded into
kernel memory and a thread there writes the buttons into a variable the game gave it. The
game stops that thread before it exits.

## Things found on real hardware

### The PSP's CPU and lwl/lwr

The game froze, and the PSP turned itself off about 20 seconds later, a second after the
player left the moving truck. PPSSPP ran the same build fine. PSPLink caught it: a bus error
at `0x48030408`, called from `FieldEffectCmd_callnative`. The field effect script holds a
function pointer at an odd address, which pret's code reads a byte at a time. GCC merged
those into an `lwl`/`lwr` pair and put the `jalr` straight after the `lwr`:

```
lwl  v1, 4(v0)
...
lwr  v1, 1(v0)
jalr v1        # jumped to 0x48030408, the aligned word lwr loaded
```

After the crash, `v1` held the right pointer, `0x089DA048`. So on the PSP's Allegrex CPU an
instruction that uses an `lwl`/`lwr` result immediately gets the loaded word before it's
merged. A result used later is fine, which is why memcpy, which uses these pairs all the
time, works.

The PSP build avoids it with three GCC flags (see the Makefile):

- `-fdisable-tree-bswap`: GCC's pass that merges byte-by-byte reads into wider ones
- `-fno-store-merging`: the same for writes (`swl`/`swr`)
- `-fno-tree-vectorize`: the vectorizer copies two halfwords as one unaligned word

It also helps to declare arrays with their real alignment: the emulated GBA registers were
declared `extern u8 REG_BASE[]`, so every 32-bit register access was an `lwl`/`lwr` pair
too.

`psp/tools/lwlr_hazards.py pokeemerald32.elf` lists any `lwl`/`lwr` whose result is used by
the next instruction. What's left in this port is in code that needs a link cable.

### The PSP menu's music

The PSP menu plays `SND0.AT3` from the EBOOT when the game is selected. It's ATRAC3 in a
RIFF file, under about 500 KB. Encoded with atracdenc, files from its current development
version didn't play on a PSP 3001 with 6.61 PRO-C, with or without a loop chunk like Sony's
at3tool writes. A file from atracdenc release 0.2.3 did, once its RIFF size (which that
version writes wrong) was set to the file's size less 8. `psp/make_art.sh` does that.
atracdenc 0.2.3 needs `-DCMAKE_CXX_FLAGS="-include cstdint"` to build with a current GCC,
and picks the RIFF container from a lowercase `.at3` file name.

### Other things that behave differently on a PSP

- When a homebrew program crashes, the PSP freezes and turns itself off after about 20
  seconds, without a message. A kernel module calling `pspDebugInstallErrorHandler` didn't
  catch anything on 6.61 PRO-C. PSPLink does.
- PPSSPP doesn't model the CPU's cache, so programs run slower on a PSP than its timing
  suggests. Drawing the overworld takes about 11 ms a frame by PPSSPP's clock and 17.6 ms on
  a PSP 3001.
- The analog stick, the note button and the files on the memory stick need nothing special,
  but every file is opened with `sceIo` and an absolute path (`ms0:/...`), which also works
  when the game is started from PSPLink (`host0:/...`).

## Debugging on a PSP

### PSPLink

[PSPLink](https://github.com/pspdev/psplinkusb) starts a program on the PSP over USB and
shows a crash's registers. pspdev includes the PC side (`usbhostfs_pc` and `pspsh`).

1. Unzip PSPLink's `psplink.zip` into `PSP/GAME/` and start it on the PSP with the USB cable
   in. On Linux, install pspdev's `share/psplinkusb/50-psplink.rules` into
   `/etc/udev/rules.d/` once, so it doesn't need root.
2. Copy `build/psp32/pokeemerald.prx` into a folder and run `usbhostfs_pc` there. That
   folder is `host0:/` on the PSP, and the game keeps its saves and settings there.
3. Run `pspsh` and type `./pokeemerald.prx`. `reset` stops it, `scrshot x.bmp` saves what's
   on the screen.

A crash prints the exception, `EPC`, `RA` and the registers. `modinfo` with the game's UID
gives `TextAddr`, where it was loaded: take that from an address and look it up with
`psp-addr2line -e pokeemerald32.elf <address>`. `memprot off` and `savemem` save memory to a
file, for looking at what was there when it crashed.

### In the game

- **The watchdog.** If the game stops responding for 5 seconds, a blue screen shows the
  step of the frame it was in, the game's main callbacks and its running tasks.
- **The trace.** With `trace = 1` in `settings.ini`, the game writes the same to `trace.bin`
  before every step, task and callback. That's slow, but after a freeze it says what the
  game was doing last: `psp/tools/trace.py trace.bin pokeemerald32.elf`.

### In PPSSPP

`make psp PSP_TEST=1 BUILD_DIR=build-test` builds a version for `PPSSPPHeadless` that plays
the PSP buttons in `test_input.txt` (one `<frames> <buttons>` line each, like `30 CROSS+UP`)
and saves the screen after lines with `SHOT` in them. INSTALL_PC.md has more.
`PSP_UBSAN=1` adds a report of every misaligned access, which PPSSPP allows but a PSP might
not.

The PC build's headless tests (`test/headless/`) check that a change to shared code gives
exactly the same frames and sound as before: `python3 test/headless/compare.py OLD NEW`.
The renderer and the Game Boy sound channels were made about four times faster on PC that
way, without changing a pixel or a sample.

## What could come next

- Drawing the overworld in time for every frame. Drawing the backgrounds front to back and
  skipping what's covered would save most of the bottom layer, or the GU could draw the
  tiles, like the PS2 port's GS renderer.
- Link cable and wireless play aren't there.
