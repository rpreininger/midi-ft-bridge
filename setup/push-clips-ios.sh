#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Push clips (and config.json) into the iOS app's Documents container.
#
# WHY: the clip library is far too large to ship inside the app bundle, so the
# iOS app reads config.json and ./clips from its Documents directory
# (UIFileSharingEnabled — see ios-app/MidiFtBridgeIOS/IOSAppModel.swift).
# Dragging a folder in over Finder is unreliable: Finder will not merge into an
# existing subfolder, so a re-encoded clip cannot simply be dropped in place.
# `devicectl` writes straight into the container and skips unchanged files.
#
# Usage:
#   setup/push-clips-ios.sh                     # all clips -> every connected device
#   setup/push-clips-ios.sh Fly Magneto         # just these clips (name or file)
#   setup/push-clips-ios.sh --config            # ... and config.json too
#   setup/push-clips-ios.sh --config-only
#   setup/push-clips-ios.sh --device iFön Fly   # one device (name, UDID or id)
#   setup/push-clips-ios.sh --list              # show connected devices, copy nothing
#   setup/push-clips-ios.sh --dry-run
#
# The phone must be UNLOCKED — devicectl mounts a developer disk image first,
# and that fails with kAMDMobileImageMounterDeviceLocked on a locked device.
#
# Run from anywhere; paths are resolved relative to the repo.
# ---------------------------------------------------------------------------
set -uo pipefail

BUNDLE_ID="de.welt.midiftbridge.ios"
DEST_DIR="Documents/clips/mp4"          # matches clips_dir "./clips" + "mp4/<name>.mp4"
DEST_CONFIG="Documents/config.json"

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CLIP_DIR="$REPO/clips/mp4"
CONFIG="$REPO/config.json"

want_device=""
want_config=0
config_only=0
list_only=0
dry_run=0
clips=()

while [ $# -gt 0 ]; do
	case "$1" in
		--device|-d) want_device="${2:-}"; shift 2 ;;
		--config)    want_config=1; shift ;;
		--config-only) want_config=1; config_only=1; shift ;;
		--list)      list_only=1; shift ;;
		--dry-run|-n) dry_run=1; shift ;;
		-h|--help)   sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
		-*)          echo "unknown option: $1" >&2; exit 2 ;;
		*)           clips+=("$(basename "$1" .mp4)"); shift ;;
	esac
done

command -v xcrun >/dev/null || { echo "xcrun not found — install Xcode." >&2; exit 1; }

# --- which devices -------------------------------------------------------
tmp=$(mktemp -t devicectl) || exit 1
trap 'rm -f "$tmp"' EXIT
if ! xcrun devicectl list devices --json-output "$tmp" --quiet 2>/dev/null; then
	echo "devicectl could not list devices." >&2; exit 1
fi

# id<TAB>name<TAB>model, one paired device per line
devices=()
while IFS= read -r line; do
	[ -n "$line" ] && devices+=("$line")
done < <(python3 - "$tmp" "$want_device" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
want = sys.argv[2]
for d in data.get("result", {}).get("devices", []):
    if d.get("connectionProperties", {}).get("pairingState") != "paired":
        continue
    ident = d["identifier"]
    name = d.get("deviceProperties", {}).get("name", "?")
    hw = d.get("hardwareProperties", {})
    model, udid = hw.get("marketingName", "?"), hw.get("udid", "")
    if want and want not in (name, ident, udid):
        continue
    print("\t".join((ident, name, model)))
PY
)

if [ "${#devices[@]}" -eq 0 ]; then
	if [ -n "$want_device" ]; then
		echo "No paired device matching '$want_device'." >&2
	else
		echo "No paired iOS devices connected." >&2
	fi
	exit 1
fi

if [ "$list_only" -eq 1 ]; then
	printf '%s\n' "${devices[@]}" | while IFS=$'\t' read -r id name model; do
		printf '%-12s %-16s %s\n' "$name" "$model" "$id"
	done
	exit 0
fi

# --- which files ---------------------------------------------------------
sources=()
if [ "$config_only" -eq 0 ]; then
	if [ "${#clips[@]}" -eq 0 ]; then
		while IFS= read -r f; do sources+=("$f"); done < <(find "$CLIP_DIR" -maxdepth 1 -name '*.mp4' | sort)
	else
		for c in "${clips[@]}"; do
			f="$CLIP_DIR/$c.mp4"
			[ -f "$f" ] || { echo "no such clip: $f" >&2; exit 1; }
			sources+=("$f")
		done
	fi
	[ "${#sources[@]}" -gt 0 ] || { echo "no clips found in $CLIP_DIR" >&2; exit 1; }
fi
[ "$want_config" -eq 1 ] && [ ! -f "$CONFIG" ] && { echo "no config.json at $CONFIG" >&2; exit 1; }

# Devices usually attach over the local network rather than USB, and that link
# drops often enough mid-transfer (NWError 54, "Connection invalid", the device
# briefly vanishing) that a single attempt is not enough. Retry those; a locked
# phone is not going to fix itself, so fail fast on that one.
copy_to() { # id, local path, remote path
	[ "$dry_run" -eq 1 ] && return 0
	local err rc attempt
	for attempt in 1 2 3; do
		err=$(xcrun devicectl device copy to --device "$1" \
			--domain-type appDataContainer --domain-identifier "$BUNDLE_ID" --user mobile \
			--source "$2" --destination "$3" --quiet 2>&1)
		rc=$?
		[ $rc -eq 0 ] && { [ $attempt -gt 1 ] && printf '(retry %d) ' "$attempt"; return 0; }
		case "$err" in
			*DeviceLocked*)
				echo "LOCKED — unlock the phone and re-run." >&2
				return 1 ;;
		esac
		[ $attempt -lt 3 ] && sleep $((attempt * 3))
	done
	echo "$err" | tail -3 >&2
	return 1
}

# --- copy ----------------------------------------------------------------
okmsg=ok
[ "$dry_run" -eq 1 ] && okmsg="dry-run"
failed=0
for row in "${devices[@]}"; do
	IFS=$'\t' read -r id name model <<<"$row"
	echo "== $name ($model)"

	for f in ${sources[@]+"${sources[@]}"}; do
		base=$(basename "$f")
		printf '   %-16s ' "$base"
		if copy_to "$id" "$f" "$DEST_DIR/$base"; then echo "$okmsg"; else echo FAILED; failed=$((failed + 1)); fi
	done

	if [ "$want_config" -eq 1 ]; then
		printf '   %-16s ' "config.json"
		if copy_to "$id" "$CONFIG" "$DEST_CONFIG"; then echo "$okmsg"; else echo FAILED; failed=$((failed + 1)); fi
	fi
done

if [ "$failed" -gt 0 ]; then
	echo "$failed copy/copies failed." >&2
	exit 1
fi
echo "Done. Restart the engine on each device — clips are read at start."
