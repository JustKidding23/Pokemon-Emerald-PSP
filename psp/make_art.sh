#!/bin/bash
# Makes the PSP menu's art and music for the EBOOT from the game itself:
# psp/ICON0.PNG (144x80) and psp/PIC1.PNG (480x272) from the title screen, and
# psp/SND0.AT3 from its music. They're made from the game, so don't share them;
# .gitignore leaves them out. `make psp` puts them in the EBOOT if they're here.
#
# usage: psp/make_art.sh [PC_BINARY] [ATRACDENC]
#   PC_BINARY: a PC build of this repository (make linux), default ./pokeemerald64
#   ATRACDENC: https://github.com/dcherednik/atracdenc, for the ATRAC3 music the
#              PSP plays. Without it there's no SND0.AT3.
# Needs ImageMagick and ffmpeg.
set -euo pipefail
cd "$(dirname "$0")/.."
game=$(realpath "${1:-./pokeemerald64}")
atracdenc=${2:-$(command -v atracdenc || true)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Skip the intro to the title screen, and record it for 45 seconds
printf '300 -\n2 START\n2700 -\n' > "$work/input.txt"
"$game" --save "$work/none.sav" --test-input "$work/input.txt" --test-audio "$work/title.wav" \
    --test-shots "$work" --test-shot-every 60 > /dev/null 2>&1

# A frame with Rayquaza and the logo, while PRESS START blinks off
frame="$work/frame_001680.ppm"
magick "$frame" -filter point -resize 200% -crop 480x272+0+8 +repage psp/PIC1.PNG
magick "$frame" -crop 180x100+30+0 +repage -filter Lanczos -resize '144x80!' psp/ICON0.PNG

# 30 seconds of the title music at 44100 Hz. The PSP's limit for it is about 500 KB.
if [ -n "$atracdenc" ]; then
    ffmpeg -hide_banner -loglevel error -y -i "$work/title.wav" -ss 6.5 -t 30 \
        -af "afade=t=in:d=0.3,afade=t=out:st=27.5:d=2.5,aresample=44100" -ac 2 -c:a pcm_s16le "$work/title44.wav"
    "$atracdenc" -e atrac3 --container riff -i "$work/title44.wav" -o psp/SND0.AT3 > /dev/null
else
    echo "atracdenc not found, so there's no SND0.AT3"
fi
ls -l psp/ICON0.PNG psp/PIC1.PNG psp/SND0.AT3 2>/dev/null || true
