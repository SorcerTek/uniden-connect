#!/bin/sh
# Rebuilds the boot animation / logo that are built into the firmware.
#   Input : "Uniden Connect Bootup.gif" and "Uniden Connect boot logo.jpeg" (project folder)
#   Output: data/boot_anim.bin (JPEG frames, 480x320) and data/boot_logo.jpg (480x320)
#           data/tab5_boot_anim.bin and data/tab5_boot_logo.jpg (640x360, shown doubled on the Tab5)
# Needs ffmpeg and python3. Run from the project folder:  sh tools/make_boot_assets.sh
set -e
GIF="${1:-Uniden Connect Bootup.gif}"
LOGO="${2:-Uniden Connect boot logo.jpeg}"
Q="${JPEG_Q:-5}"   # ffmpeg JPEG quality, 2 (best) .. 31 (smallest)
TMP=$(mktemp -d)
# Scale to 320 px tall, then crop the sides to fill the 480x320 screen (the sides are empty background).
VF="scale=-2:320:flags=lanczos,crop=480:320,format=yuvj420p"   # 4:2:0 baseline JPEG (what the board decoder handles)
pack() {   # pack the JPEG frames in $1 into $2
python3 -I - "$1" "$FPS" "$2" <<'PY'
import sys, os, struct, glob, fractions
d, fps, out = sys.argv[1], sys.argv[2], sys.argv[3]
ms = max(20, round(1000 / float(fractions.Fraction(fps))))
frames = [open(f, 'rb').read() for f in sorted(glob.glob(os.path.join(d, 'f*.jpg')))]
# Header: "UCA1", u16 frame count, u16 ms per frame, then u32 offset + u32 length per frame.
hdr = b'UCA1' + struct.pack('<HH', len(frames), ms)
off = len(hdr) + 8 * len(frames)
table = b''
for f in frames:
    table += struct.pack('<II', off, len(f)); off += len(f)
open(out, 'wb').write(hdr + table + b''.join(frames))
print(f"{len(frames)} frames, {ms} ms each, {off} bytes -> {out}")
PY
}
FPS=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$GIF")

# FNK0104N: 480x320
ffmpeg -v error -i "$GIF" -vf "$VF" -q:v "$Q" "$TMP/f%03d.jpg"
ffmpeg -v error -y -i "$LOGO" -vf "$VF" -q:v 3 data/boot_logo.jpg
pack "$TMP" data/boot_anim.bin

# Tab5: 640x360 (16:9 like the screen; drawn at 2x)
VF5="scale=640:360:flags=lanczos,format=yuvj420p"
mkdir -p "$TMP/t5"
ffmpeg -v error -i "$GIF" -vf "$VF5" -q:v "$Q" "$TMP/t5/f%03d.jpg"
ffmpeg -v error -y -i "$LOGO" -vf "$VF5" -q:v 3 data/tab5_boot_logo.jpg
pack "$TMP/t5" data/tab5_boot_anim.bin
rm -rf "$TMP"
ls -l data/boot_logo.jpg data/boot_anim.bin data/tab5_boot_logo.jpg data/tab5_boot_anim.bin
