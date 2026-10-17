#!/bin/bash
# Makes the PSP menu's art and music for the EBOOT from the game itself:
# psp/ICON0.PNG (144x80), the logo from the end of the intro, and
# psp/PIC1.PNG (480x272) and psp/SND0.AT3, the title screen's Rayquaza
# without the logo, and its music. They're made from the game, so don't share them;
# .gitignore leaves them out. `make psp` puts them in the EBOOT if they're here.
#
# usage: psp/make_art.sh [PC_BINARY] [ATRACDENC]
#   PC_BINARY: a PC build of this repository (make linux), default ./pokeemerald64
#   ATRACDENC: https://github.com/dcherednik/atracdenc release 0.2.3, for the
#              ATRAC3 music the PSP plays. A PSP 3001 played its files in the
#              menu but not those of later versions. Without it there's no
#              SND0.AT3.
# Needs ImageMagick and ffmpeg.
set -euo pipefail
cd "$(dirname "$0")/.."
game=$(realpath "${1:-./pokeemerald64}")
atracdenc=${2:-$(command -v atracdenc || true)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Skip the intro to the title screen, and record its music and pictures
# without the logo (BG2) and the sprites
printf '300 -\n2 START\n2700 -\n' > "$work/input.txt"
"$game" --save "$work/none.sav" --test-input "$work/input.txt" --test-audio "$work/title.wav" \
    --test-shots "$work" --test-shot-every 60 --test-hide-layers 0x1400 > /dev/null 2>&1

# The intro, without skipping it
mkdir "$work/intro"
printf '3600 -\n' > "$work/intro.txt"
"$game" --save "$work/none.sav" --test-input "$work/intro.txt" \
    --test-shots "$work/intro" --test-shot-every 20 > /dev/null 2>&1

# The background: the title screen's Rayquaza when its markings glow brightest
magick "$work/frame_001920.ppm" -filter point -resize 200% -crop 480x272+0+24 +repage psp/PIC1.PNG
# The icon: the logo at the end of the intro, which is on black, made clear
magick "$work/intro/frame_003600.ppm" -fuzz 2% -transparent black -trim +repage \
    -filter Lanczos -resize 144x80 -background none -gravity center -extent 144x80 psp/ICON0.PNG

# 18 seconds of the title music at 44100 Hz, about 300 KB, well inside the
# PSP menu's limit of about 500 KB.
if [ -n "$atracdenc" ]; then
    ffmpeg -hide_banner -loglevel error -y -i "$work/title.wav" -ss 6.5 -t 18 \
        -af "afade=t=in:d=0.3,afade=t=out:st=15.5:d=2.5,aresample=44100" -ac 2 -c:a pcm_s16le "$work/title44.wav"
    # atracdenc picks the RIFF container from the lowercase .at3. It writes the
    # RIFF size wrong, and the file that played had it set to the file's size
    # less 8, as RIFF has it.
    "$atracdenc" -e atrac3 -i "$work/title44.wav" -o "$work/snd0.at3" > /dev/null
    python3 -c 'import os, struct, sys
f = open(sys.argv[1], "r+b"); f.seek(4); f.write(struct.pack("<I", os.path.getsize(sys.argv[1]) - 8))' "$work/snd0.at3"
    cp "$work/snd0.at3" psp/SND0.AT3
else
    echo "atracdenc not found, so there's no SND0.AT3"
fi
ls -l psp/ICON0.PNG psp/PIC1.PNG psp/SND0.AT3 2>/dev/null || true
