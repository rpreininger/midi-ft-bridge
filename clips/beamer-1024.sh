#!/bin/bash
# Beamer versions of the stage clips: the top-left 128x128 (A_bigpanel) blown up
# to 1024x1024 by pixel repetition (factor 8, no smoothing).
#
#   ./beamer-1024.sh              # all clips
#   ./beamer-1024.sh Fly Moon     # just these
#
# Picture comes from the ProRes master (4:2:2, no H.264 artefacts to enlarge),
# audio is copied untouched from the stage mp4. The table below pins each stage
# clip to the master it was rendered from -- update it when a clip gets a new
# version. Output lands in beamer-1024/<name>.mp4 and replaces what is there.
#
# The Intro is the exception: its text is unreadable on the 128 grid, so the
# picture is the 16:9 YouTube cut (https://youtu.be/ScKLFUpKrjM) at 1024x576 with
# black bars. That cut starts 21 frames late and ends 7 frames early against the
# stage audio (measured, no drift), hence the trim and the held last frame.

set -uo pipefail
cd "$(dirname "$0")" || exit 1

MASTERS="
Intro|beamer-src/Intro_YouTube_720p.mp4
OYM|ProRes V4/OYM_ProRes_V4.mov
Fly|ProRes V3/FLY_PRORES_V3.mov
Magneto|ProRes V3/MAGNETO_PRORES_V3.mov
Shrink|ProRes V3/SHRINK_PRORES_V3.mov
Moon|Prores/ProRes V2/MOON_PRORES.mov
Pixel|Prores/ProRes V2/PIXL_PRORES.mov
Tennis|Prores/ProRes V2/TTENNIS_PRORES.mov
Axel|Prores/Axel_PRORES.mov
Black|Prores/Black_PRORES.mov
Jiffy|Prores/Jiffy_PRORES.mov
Neutrino|Prores/Neu_PRORES.mov
Shin|Prores/Shin_PRORES.mov
ZX89|Prores/ZX_PRORES.mov
"

# 8-bit before enlarging: any dither then lands on whole source pixels, not
# inside the 8x8 blocks.
VF="crop=128:128:0:0,format=yuv422p,scale=1024:1024:flags=neighbor,format=yuv420p"
VF_INTRO="trim=start_frame=21,setpts=PTS-STARTPTS,scale=1024:576:flags=area,pad=1024:1024:0:224,tpad=stop_mode=clone:stop=7,format=yuv420p"

FFMPEG=$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)
[ -x "$FFMPEG" ] || { echo "ffmpeg not found. brew install ffmpeg" >&2; exit 1; }

mkdir -p beamer-1024

ok=0
fail=0
while IFS='|' read -r name master; do
	[ -n "$name" ] || continue
	if [ $# -gt 0 ] && [[ " $* " != *" $name "* ]]; then continue; fi

	echo "$name  ($master)"
	vf=$VF
	[ "$name" = "Intro" ] && vf=$VF_INTRO
	# keyframe every second so a player can jump in without a visible delay;
	# no deblocking, it would soften the hard edges of the 8x8 pixels.
	# -nostdin: ffmpeg must not eat the clip table this loop is reading.
	if "$FFMPEG" -nostdin -y -i "$master" -i "mp4/$name.mp4" -map 0:v:0 -map 1:a:0 \
		-vf "$vf" -sws_flags neighbor \
		-c:v libx264 -preset slow -crf 14 -g 25 -x264-params no-deblock=1 \
		-c:a copy -movflags +faststart \
		"beamer-1024/$name.mp4" -hide_banner -loglevel error; then
		ok=$((ok + 1))
	else
		echo "  FAILED: $name" >&2
		fail=$((fail + 1))
	fi
done <<< "$MASTERS"

echo "$ok converted, $fail failed (1024x1024, audio copied)"
[ "$fail" -eq 0 ]
