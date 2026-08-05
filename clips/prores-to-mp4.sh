#!/bin/bash
# Command-line twin of the "ProRes to MP4" droplet — same rules, no Finder needed.
#
#   ./prores-to-mp4.sh "ProRes V3"/*.mov
#   ./prores-to-mp4.sh "ProRes V3"            # a folder works too (non-recursive)
#
# H.264 / yuv420p at 288x128 with uncompressed PCM audio, first video + first
# audio stream only. Sources already 288 wide but taller (the padded ProRes
# masters) are cropped to the top 288x128; anything else is scaled.
# Output lands next to the source as <name>.mp4, or <name>_288x128.mp4 if that
# name is taken.

set -uo pipefail

W=288
H=128

FFMPEG=$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)
FFPROBE=$(command -v ffprobe || echo /opt/homebrew/bin/ffprobe)
[ -x "$FFMPEG" ] || { echo "ffmpeg not found. brew install ffmpeg" >&2; exit 1; }

[ $# -gt 0 ] || { echo "usage: $0 <file-or-folder> [...]" >&2; exit 1; }

# expand folders into their video files
files=()
for arg in "$@"; do
	if [ -d "$arg" ]; then
		while IFS= read -r f; do files+=("$f"); done < <(
			find "$arg" -maxdepth 1 -type f \
				\( -iname '*.mov' -o -iname '*.mp4' -o -iname '*.m4v' \
				   -o -iname '*.avi' -o -iname '*.mkv' -o -iname '*.mxf' \) | sort)
	else
		files+=("$arg")
	fi
done

ok=0
fail=0
for in in "${files[@]}"; do
	out="${in%.*}.mp4"
	[ -e "$out" ] && out="${in%.*}_${W}x${H}.mp4"

	dims=$("$FFPROBE" -v error -select_streams v:0 \
		-show_entries stream=width,height -of csv=p=0:s=x "$in" 2>/dev/null)
	sw=${dims%x*}
	sh=${dims#*x}

	if [ "$sw" = "$W" ] && [ "$sh" = "$H" ]; then
		vf=(-vf null)                           # already correct — don't touch the pixels
		note="as-is"
	elif [ "$sw" = "$W" ] && [ "${sh:-0}" -gt "$H" ] 2>/dev/null; then
		vf=(-vf "crop=$W:$H:0:0")               # padded master — keep the top rows
		note="crop top"
	else
		vf=(-vf "scale=$W:$H")
		note="scale"
	fi

	printf '%s  (%s %s)\n' "$(basename "$in")" "${dims:-?}" "$note"
	if "$FFMPEG" -y -i "$in" "${vf[@]}" \
		-c:v libx264 -pix_fmt yuv420p -c:a pcm_s16le \
		-map 0:v:0 -map '0:a:0?' "$out" -hide_banner -loglevel error; then
		ok=$((ok + 1))
	else
		echo "  FAILED: $in" >&2
		fail=$((fail + 1))
	fi
done

echo "$ok converted, $fail failed (${W}x${H}, PCM audio)"
[ "$fail" -eq 0 ]
