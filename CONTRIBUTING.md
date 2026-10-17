# Contributing to Pokémon Emerald PSP

Thanks for wanting to help. You don't need to be a programmer: playing the game on your PSP
and reporting what goes wrong helps just as much.

## Reporting a problem

[Open an issue](https://github.com/JustKidding23/Pokemon-Emerald-PSP/issues/new/choose) and pick
"Something's wrong". It asks for your PSP model and firmware, how you got the game, and what
happened. A photo of the screen helps a lot. Please never attach a ROM, a built game or
anything taken from the game.

For an idea or an improvement, pick "Idea or improvement" instead.

## What would help

- **Speed in the overworld.** The game runs at full speed, but a PSP 3001 only has time to
  draw about half of the overworld's frames. [docs/psp.md](docs/psp.md) has two ideas for
  drawing faster.
- **Testing** on other PSP models (1000, 2000, Go, Street) and other custom firmware, and
  reports of anything that looks or sounds wrong compared with the GBA.
- **FILE SELECT in the Mystery Gift versions of the main menu**, which don't have it yet.
- **Link cable and wireless play**, which aren't there at all.

## Changing the code

1. Fork the repository and make a branch from `psp`.
2. Make your change, and test it (see below).
3. Open a pull request into `psp` that says what it changes, why, and how you tested
   it.

The owner of this repository reviews each pull request and tries it on a PSP when possible,
so it can take a little while.

### Building

You need Linux (or WSL) and the [pspdev](https://github.com/pspdev/pspdev) toolchain in
`~/pspdev`. `make psp` builds the game into `build/psp32/PSP/GAME/EMERALD_PSP/`.
[INSTALL_PC.md](INSTALL_PC.md#psp) has the details, and [docs/psp.md](docs/psp.md) explains
how the port works.

### Testing

- **On a PSP.** Please try your change on a real PSP if you can, and say in the pull request
  what you tested. PPSSPP is a good first check, but this port has already had a crash that
  only happened on a real PSP (see [docs/psp.md](docs/psp.md)). With
  [PSPLink](https://github.com/pspdev/psplinkusb) you can start a build over USB and see
  crash details; docs/psp.md shows how.
- **In PPSSPP.** `make psp PSP_TEST=1 BUILD_DIR=build-test` builds a version that plays a
  list of button presses and saves screenshots, for testing without touching anything.
- **Shared game code.** pret's code is shared with the GBA, PC and PS2 builds. Keep PSP-only
  changes inside `#if defined(PORTABLE) && defined(__PSP__)`. For anything else, build the
  PC version (`make linux`) before and after your change and check that
  `python3 test/headless/compare.py OLD NEW` reports every scenario as identical, unless the
  change is meant to change what the game does.

## Ground rules

- **No game files.** Don't commit or attach ROMs, built games (`EBOOT.PBP`), saves, or
  graphics, sound or text taken from the game.
- **Say how you tested it.** "Played to the first gym on a PSP 3001" is perfect. "Only built
  it" is fine too, as long as it says so.
- **Keep changes focused.** One fix or feature per pull request is much easier to review.
- Run `psp/tools/lwlr_hazards.py pokeemerald32.elf` after changing compiler flags or
  low-level code, since the PSP mishandles some code that PPSSPP runs fine.

### Using AI

This project was made with Claude, Anthropic's AI assistant, and it says so openly. Changes
made with AI tools are welcome on the same terms: say in the pull request that you used one
(and which), and make sure you've read and tested what it wrote. You're responsible for your
pull request either way.

## Upstream

Fixes to the decompilation itself belong in [pret/pokeemerald](https://github.com/pret/pokeemerald),
and fixes to the PC port in [fuddlesworth/pokeemerald-native](https://github.com/fuddlesworth/pokeemerald-native),
so that everyone gets them.
